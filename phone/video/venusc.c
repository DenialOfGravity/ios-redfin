/*
 * venusc: video stage 4a test client for venusd (venus_proto.h): decodes an Annex B H.264 file through the daemon.
 *   venusc IN.h264 OUT.nv12 [--frames N]    the first N access units (default all), EOS, then CLOSE; OUT gets the
 *                                           frames cropped to the visible size as plain NV12, as venusctl decode's
 *   venusc IN.h264 [OUT] --null [--frames N] the frames are discarded (no crop, CRC or write): speed
 *   ... --surf [K]                          vdcredit: frames into K credited IOSurfaces (below)
 * Access units from venusctl's splitter (venus_h264.c); pts = n * 33333 us as venusctl's ETBs, so the per-frame lines
 * (venusctl's format: FBD flags, filled, ts, size, pic type, luma CRC32) compare 1:1 with venusctl decode's and
 * tools/sunfish-venus-yuvcmp.py. Then a summary: frames, wall time (first FRAME to EOS_DONE), fps, round trip per
 * frame (FRAME sent to its DECODED back, by pts), venusd's copy-out. Needs no /dev/pmem: runs as any user.
 * Env: VENUSD_SOCK (default /var/run/venusd.sock). Exit 0: every access unit came back as a frame, no error.
 * 4e: IN may be HEVC Annex B (venus_hevc.c splitter, OPEN codec VD_CODEC_HEVC), told apart by its first NAL header;
 * --hevc / --h264 force the codec.
 * vdcredit: --surf [K] (K 6): K NV12 ('420v') IOSurface-backed CVPixelBuffers at the SPS's visible size are credited
 * to venusd after OPEN; a DECODED into one (len 0 + surface) is read from it (cropped already) and written to OUT
 * like a socket frame, then that surface is credited again before the next FRAME (or while waiting for EOS_DONE).
 * DECODED with pixels (no credit was queued) as before. The summary counts frames by path.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include "venus_h264.h"
#include "venus_hevc.h"
#include "venus_proto.h"

#define PTS_STEP	33333u

static int fd = -1, nullout, want, hevc = -1;	/* 4e: -1 = from the stream */
static FILE *out;
static double *sent_t;		/* when FRAME n went out */
static volatile int stop, eos_done, closed, fatal;
static int frames, eos_status, cw, ch;
static double rt_sum, rt_max, cp_sum, t_eos_done;
static uint8_t *fbuf, *vis;
static size_t fbuf_len;
/* vdcredit (--surf): the buffers, their surface IDs, the ones to credit again (reader -> main thread) */
#define NSURF	VENUSD_MAX_CREDITS
static int nsurf;
static CVPixelBufferRef spb[NSURF];
static uint32_t sid[NSURF], owed[NSURF];
static int nowed, n_surf, n_sock, n_back, scw, sch;	/* scw x sch: the surfaces' size */
static pthread_mutex_t owe_mu = PTHREAD_MUTEX_INITIALIZER;

static double now_s(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec + t.tv_usec / 1e6;
}

static int wr_full(const void *b, size_t n)
{
	const uint8_t *p = b;
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		p += k, n -= (size_t)k;
	}
	return 0;
}

static int rd_full(void *b, size_t n)
{
	uint8_t *p = b;
	while (n) {
		ssize_t k = read(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		p += k, n -= (size_t)k;
	}
	return 0;
}

static int send_msg(int type, uint64_t pts, uint32_t seq, const void *payload, uint32_t len, uint32_t w, uint32_t h)
{
	struct venusd_msg m = { .magic = VENUSD_MAGIC, .type = (uint16_t)type, .version = VENUSD_VERSION, .len = len,
		.pts = pts, .seq = seq, .codec = type == VD_OPEN ? (hevc ? VD_CODEC_HEVC : VD_CODEC_H264) : 0, .width = w,
		.height = h };
	return wr_full(&m, sizeof m) || (len && wr_full(payload, len)) ? -1 : 0;
}

static const char *errname(int32_t s)
{
	static char b[48];
	if (s < 0)
		return strerror(-s);
	snprintf(b, sizeof b, "firmware session error %#x", (unsigned)s);
	return b;
}

static int send_credit(uint32_t id, uint32_t w, uint32_t h)	/* vdcredit */
{
	struct venusd_msg m = { .magic = VENUSD_MAGIC, .type = VD_CREDIT, .version = VENUSD_VERSION, .surface = id,
		.width = w, .height = h };
	return wr_full(&m, sizeof m);
}

/* vdcredit: the surfaces venusd is done with, credited again (main thread: the only writer while frames go) */
static void credit_owed(void)
{
	uint32_t ids[NSURF];
	pthread_mutex_lock(&owe_mu);
	int n = nowed;
	memcpy(ids, owed, sizeof(uint32_t) * (size_t)n);
	nowed = 0;
	pthread_mutex_unlock(&owe_mu);
	for (int i = 0; i < n; i++)
		if (send_credit(ids[i], (uint32_t)scw, (uint32_t)sch))
			printf("venusc: CREDIT %u: send failed\n", ids[i]);
}

/* vdcredit: venusd is done with surface id: credit it again */
static void from_surface_done(uint32_t id)
{
	pthread_mutex_lock(&owe_mu);
	if (nowed < NSURF)
		owed[nowed++] = id;
	pthread_mutex_unlock(&owe_mu);
}

/* vdcredit: a DECODED written into credited surface m->surface: its visible rect (the whole buffer) into vis */
static int from_surface(const struct venusd_msg *m)
{
	int k;
	for (k = 0; k < nsurf && sid[k] != m->surface; k++)
		;
	if (k == nsurf) {
		printf("venusc: frame %d: written into IOSurface %u, not one of ours\n", frames, m->surface);
		return -1;
	}
	CVPixelBufferRef pb = spb[k];
	int ok = CVPixelBufferGetWidth(pb) == (size_t)cw && CVPixelBufferGetHeight(pb) == (size_t)ch &&
		 !CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	if (ok) {
		const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
		size_t ybpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cbpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
		for (int r = 0; y && c && r < ch; r++)
			memcpy(vis + (size_t)r * cw, y + (size_t)r * ybpr, (size_t)cw);
		for (int r = 0; y && c && r < ch / 2; r++)
			memcpy(vis + (size_t)cw * ch + (size_t)r * cw, c + (size_t)r * cbpr, (size_t)cw);
		ok = y && c;
		CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
	}
	if (!ok)
		printf("venusc: frame %d: surface %u unreadable or not %dx%d (picture %ux%u)\n", frames, m->surface, cw, ch,
		       m->width, m->height);
	from_surface_done(m->surface);
	return ok ? 0 : -1;
}

/* one DECODED: crop as venusctl frame_out (vdcredit: or the credited surface's rect), CRC32 of the visible luma,
 * append to OUT */
static void decoded(const struct venusd_msg *m)
{
	double t = now_s();
	uint64_t n = m->pts / PTS_STEP;
	if (m->pts % PTS_STEP == 0 && n < (uint64_t)want && sent_t[n] > 0) {
		double rt = t - sent_t[n];
		rt_sum += rt;
		if (rt > rt_max)
			rt_max = rt;
	}
	cp_sum += m->copy_us / 1e3;
	if (m->surface) {	/* vdcredit */
		n_surf++;
		int bad = 0;
		if (!nullout) {
			if ((int)m->width != cw || (int)m->height != ch || !vis)
				free(vis), cw = (int)m->width, ch = (int)m->height, vis = malloc((size_t)cw * ch * 3 / 2);
			bad = !vis || from_surface(m);
		} else {
			from_surface_done(m->surface);
		}
		if (nullout || bad) {
			frames++;
			return;
		}
	} else {
		if (m->len)
			n_sock++;
		if (nullout || !m->len) {	/* vdnopay: venusd's no-pixels test sends len 0 */
			frames++;
			return;
		}
		size_t ylen = (size_t)m->stride * m->scanlines;
		if (m->len < ylen + ylen / 2 || m->crop_x + m->width > m->stride || m->crop_y + m->height > m->scanlines) {
			printf("venusc: frame %d: bad layout (len %u, stride %u, scanlines %u, %ux%u at %u,%u)\n", frames, m->len,
			       m->stride, m->scanlines, m->width, m->height, m->crop_x, m->crop_y);
			frames++;
			return;
		}
		cw = (int)m->width, ch = (int)m->height;
		free(vis);
		vis = malloc((size_t)cw * ch * 3 / 2);
		if (!vis)
			return;
		for (int y = 0; y < ch; y++)
			memcpy(vis + (size_t)y * cw, fbuf + (size_t)(y + m->crop_y) * m->stride + m->crop_x, (size_t)cw);
		for (int y = 0; y < ch / 2; y++)
			memcpy(vis + (size_t)cw * ch + (size_t)y * cw,
			       fbuf + ylen + (size_t)(y + m->crop_y / 2) * m->stride + (m->crop_x & ~1u), (size_t)cw);
	}
	uint32_t crc = crc32_buf(vis, (size_t)cw * ch);
	if (out)
		fwrite(vis, 1, (size_t)cw * ch * 3 / 2, out);
	printf("venus: frame %d: FBD flags %#x filled %u ts %llu us, %ux%u at %u,%u, pic type %#x, luma crc32 %08x\n",
	       frames, m->flags, m->filled, (unsigned long long)m->pts, m->fbd_w, m->fbd_h, m->fbd_x, m->fbd_y, m->pic_type,
	       crc);
	frames++;
}

/* vdcredit: nsurf NV12 IOSurface-backed buffers of w x h, credited; 0 = ok */
static int make_surfaces(int w, int h)
{
	CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
						 &kCFTypeDictionaryValueCallBacks);
	const void *k[] = { kCVPixelBufferIOSurfacePropertiesKey };
	const void *val[] = { ios };
	CFDictionaryRef a = ios ? CFDictionaryCreate(NULL, k, val, 1, &kCFTypeDictionaryKeyCallBacks,
						     &kCFTypeDictionaryValueCallBacks) : NULL;
	scw = w, sch = h;
	for (int i = 0; i < nsurf; i++) {
		CVReturn r = a ? CVPixelBufferCreate(NULL, (size_t)w, (size_t)h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
						     a, &spb[i]) : kCVReturnError;
		IOSurfaceRef s = r ? NULL : CVPixelBufferGetIOSurface(spb[i]);
		sid[i] = s ? IOSurfaceGetID(s) : 0;
		if (!sid[i]) {
			printf("venusc: --surf: no IOSurface-backed %dx%d NV12 buffer (CVReturn %d)\n", w, h, r);
			nsurf = i + (r ? 0 : 1);
			return -1;
		}
		if (send_credit(sid[i], (uint32_t)w, (uint32_t)h))
			return -1;
	}
	if (a)
		CFRelease(a);
	if (ios)
		CFRelease(ios);
	printf("venusc: --surf: %d IOSurfaces %dx%d NV12 credited (bytes per row %zu/%zu, ids %u..)\n", nsurf, w, h,
	       CVPixelBufferGetBytesPerRowOfPlane(spb[0], 0), CVPixelBufferGetBytesPerRowOfPlane(spb[0], 1), sid[0]);
	return 0;
}

/* everything venusd sends after OPEN, until CLOSE's reply or the connection's end */
static void *reader(void *arg)
{
	(void)arg;
	for (;;) {
		struct venusd_msg m;
		if (rd_full(&m, sizeof m) || m.magic != VENUSD_MAGIC) {
			if (!closed)
				printf("venusc: venusd connection lost\n");
			break;
		}
		if (m.len > fbuf_len) {
			free(fbuf);
			fbuf = malloc(m.len);
			fbuf_len = fbuf ? m.len : 0;
			if (!fbuf)
				break;
		}
		if (m.len && rd_full(fbuf, m.len))
			break;
		if (m.type == VD_DECODED) {
			decoded(&m);
		} else if (m.type == VD_ERROR) {
			printf("venusc: ERROR at frame %u: %d (%s)%s\n", m.seq, m.status, errname(m.status),
			       m.status == -E2BIG ? ", frame dropped" : ", session closed");
			if (m.status != -E2BIG) {
				fatal = 1;
				stop = 1;
			}
		} else if (m.type == VD_EOS_DONE) {
			eos_status = m.status;
			t_eos_done = now_s();
			eos_done = 1;
			if (m.status)
				printf("venusc: EOS_DONE status %d (%s)\n", m.status, errname(m.status));
		} else if (m.type == VD_UNCREDIT) {	/* vdcredit: handed back unused (the sizes are fixed: not again) */
			if (!n_back++)
				printf("venusc: credit IOSurface %u handed back: %d (%s), picture %ux%u\n", m.surface, m.status,
				       strerror(-m.status), m.width, m.height);
		} else if (m.type == VD_CLOSE) {
			printf("venusc: CLOSE: %u frames in the session\n", m.frames);
			closed = 1;
			break;
		} else {
			printf("venusc: unexpected message type %u\n", m.type);
		}
	}
	stop = 1;
	eos_done = 1;
	return NULL;
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 4);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	const char *in = NULL, *outp = NULL;
	int maxf = -1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--null"))
			nullout = 1;
		else if (!strcmp(argv[i], "--surf")) {	/* vdcredit */
			nsurf = 6;
			if (i + 1 < argc && atoi(argv[i + 1]) > 0)
				nsurf = atoi(argv[++i]);
			if (nsurf > NSURF)
				nsurf = NSURF;
		}
		else if (!strcmp(argv[i], "--hevc"))
			hevc = 1;
		else if (!strcmp(argv[i], "--h264"))
			hevc = 0;
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			maxf = atoi(argv[++i]);
		else if (!in)
			in = argv[i];
		else if (!outp)
			outp = argv[i];
		else
			goto usage;
	}
	if (!in || (!outp && !nullout) || maxf == 0)
		goto usage;
	size_t len = 0;
	uint8_t *s = slurp(in, &len);
	if (!s || !len) {
		printf("venusc: can't read %s\n", in);
		return 1;
	}
	int cap = (int)(len / 4 + 1);	/* no AU is shorter than a start code + NAL header */
	uint32_t *off = malloc(sizeof(uint32_t) * (size_t)cap), *alen = malloc(sizeof(uint32_t) * (size_t)cap);
	struct h264_sps sps = { 0, 0, 0, 0, 0, 0 };
	if (hevc < 0)
		hevc = hevc_detect(s, len);
	int nau = !off || !alen ? 0 : hevc ? split_aus_hevc(s, len, off, alen, cap, &sps, NULL) :
					      split_aus(s, len, off, alen, cap, &sps);
	if (nau < 0) {
		nau = -nau - 1;
		printf("venusc: no SPS parsed: venusd uses its default size\n");
	}
	want = maxf > 0 && maxf < nau ? maxf : nau;
	printf("venusc: %s: %zu bytes, %d access units%s, sending %d\n", in, len, nau, hevc ? " (HEVC)" : "", want);
	if (!want)
		return 1;
	sent_t = calloc((size_t)want, sizeof(double));
	if (!nullout && !(out = fopen(outp, "wb"))) {
		printf("venusc: can't write %s\n", outp);
		return 1;
	}
	const char *sp = getenv("VENUSD_SOCK") ? getenv("VENUSD_SOCK") : VENUSD_SOCK;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", sp);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
		printf("venusc: can't connect to %s: %s\n", sp, strerror(errno));
		return 1;
	}
	int sz = 4 << 20;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);

	/* OPEN, answered before anything else (Venus may boot: ~1.1 s) */
	double t0 = now_s();
	struct venusd_msg m;
	if (send_msg(VD_OPEN, 0, 0, NULL, 0, (uint32_t)sps.w, (uint32_t)sps.h) || rd_full(&m, sizeof m) ||
	    m.magic != VENUSD_MAGIC || m.type != VD_OPEN) {
		printf("venusc: OPEN: no answer from venusd\n");
		return 1;
	}
	double t_open = now_s() - t0;
	if (m.status) {
		printf("venusc: OPEN refused: %d (%s)\n", m.status, errname(m.status));
		return 1;
	}
	printf("venusc: OPEN ok in %.0f ms (hint %dx%d)\n", 1e3 * t_open, sps.w, sps.h);
	if (nsurf && (!sps.cw || !sps.ch || make_surfaces(sps.cw, sps.ch))) {	/* before the reader: no reply comes */
		printf("venusc: --surf: %s\n", sps.cw ? "credits failed" : "needs the visible size from an SPS");
		return 1;
	}
	pthread_t th;
	if (pthread_create(&th, NULL, reader, NULL)) {
		printf("venusc: no reader thread\n");
		return 1;
	}
	double t1 = now_s();
	int n;
	for (n = 0; n < want && !stop; n++) {
		if (nsurf)
			credit_owed();	/* vdcredit: ahead of the FRAME */
		sent_t[n] = now_s();
		if (send_msg(VD_FRAME, (uint64_t)n * PTS_STEP, (uint32_t)n, s + off[n], alen[n], 0, 0)) {
			printf("venusc: FRAME %d: send failed\n", n);
			break;
		}
	}
	if (!stop && send_msg(VD_EOS, 0, (uint32_t)n, NULL, 0, 0, 0))
		printf("venusc: EOS: send failed\n");
	while (!eos_done) {
		if (nsurf)
			credit_owed();	/* vdcredit: venusd takes credits while it drains */
		usleep(1000);
	}
	double wall = (t_eos_done > 0 ? t_eos_done : now_s()) - t1;
	send_msg(VD_CLOSE, 0, 0, NULL, 0, 0, 0);
	pthread_join(th, NULL);
	close(fd);
	if (out)
		fclose(out);
	printf("venusc: %d frames of %d sent%s%s; wall %.2f s (first FRAME to EOS_DONE) = %.1f fps\n", frames, n,
	       eos_status ? ", EOS timeout" : "", fatal ? ", session error" : "", wall, wall > 0 ? frames / wall : 0.0);
	if (frames)
		printf("venusc: round trip per frame mean %.2f ms, max %.2f ms; venusd copy-out mean %.2f ms; OPEN %.0f ms\n",
		       1e3 * rt_sum / frames, 1e3 * rt_max, cp_sum / frames, 1e3 * t_open);
	if (nsurf) {
		printf("venusc: --surf: %d frames through credited surfaces, %d over the socket, %d credit(s) handed back\n",
		       n_surf, n_sock, n_back);
		for (int i = 0; i < nsurf; i++)
			if (spb[i])
				CFRelease(spb[i]);
	}
	if (!nullout)
		printf("venus: wrote %d frames %dx%d NV12 (Y, then interleaved CbCr) to %s\n", frames, cw, ch, outp);
	return frames == want && !fatal && !eos_status ? 0 : 1;
usage:
	fprintf(stderr, "usage: venusc IN.h264|IN.h265 OUT.nv12 [--frames N] [--hevc|--h264] [--surf [K]] | venusc IN [OUT]"
		" --null [--frames N] [--surf [K]]\n");
	return 2;
}
