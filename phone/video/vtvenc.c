/*
 * vtvenc (ws-vt-venc, 2026-11-20): a VideoToolbox HARDWARE ENCODER for 'avc1' (H.264) and 'hvc1' (HEVC) on Venus, a thin
 * client of venusd's encoder sessions (venus_proto.h VD_ENC_*). Design: logs/research/vt-venus-encoder-2026-11-20.md.
 * A dylib (/var/local/lib/vtvenc.dylib); a host process registers it by calling VTVenusEncRegister() (RedwoodAudio's
 * inject_media.c in mediaserverd, vtenc_test for tests): VTRegisterVideoEncoderWithInfo with IsHardwareAccelerated,
 * as the iPhone's AVE encoders are registered, so VideoToolbox (and CMCapture's movie recording) takes it.
 * The encoder object is a CMDerivedObject of VTVideoEncoderGetClassID() with the iOS 16.7 VTVideoEncoderClass slots
 * taken from VCPHEVC.videocodec's encoder class (HEVCVideoEncoder_CreateInstance -> its vtable, version 3):
 *   1 StartSession(enc, session, CMVideoDimensions, CFDictionaryRef)   2 EncodeFrame(enc, token, image, pts, dur, opts)
 *   3 CompleteFrames(enc, CMTime until)                                4 CopySupportedPropertyDictionary(enc, &dict)
 *   (5.. setProperties, copySerializableProperties, ..., tiles: not implemented, NULL as VCPHEVC's 5-16)
 * Data path: EncodeFrame sends the frame's IOSurface ID (ENC_FRAME; the CVPixelBuffer is held until venusd's
 * ENC_INDONE), venusd copies or maps it (zero-copy when it is in Venus's layout), Venus encodes (constant QP from the
 * bit rate), a reader thread turns each ENC_DATA (Annex B) into a CMSampleBuffer of 4-byte length-prefixed NAL units
 * with the format description (avcC/hvcC from the parameter sets) and VTEncoderSessionEmitEncodedFrame()s it.
 * The venusd session opens lazily at the first frame (the properties are set by then), closes at Invalidate.
 * Never crashes its host: every failure is a VT error or a dropped frame and a log line.
 * Switches: env or KEY=VALUE lines in /var/local/vtvenc-env (read once per process; the file wins):
 *   VTVENC_HEVC (1): register hvc1 too.   VTVENC_ATTR (0): 1 = ask VT for buffers in Venus's layout (bytes per row
 *   aligned to 128, rows to 32) instead of passing any 420v/420f through.  VTVENC_QP=I[:P]: fixed QP (else from the
 *   bit rate).  VTVENC_SEQHDR (0): 1 = parameter sets in band before every IDR.  VTVENC_INFLIGHT (12): frames out
 *   at once before EncodeFrame waits.  VTVENC_LOG (0): 1 = a line per frame.  VTVENC_RC (1): host rate control: the
 *   constant QP follows the measured bit rate toward AverageBitRate (DataRateLimits as a cap), ENC_QP to venusd;
 *   0 = the QP from the bit rate guess stays (venusd's mapping).  VTVENC_EARLY_MS (40): the venusd session opens this
 *   long after StartSession (in the background), not at the first frame; -1 = at the first frame.
 * Off: /var/local/vtvenc-off (nothing registered) or no /var/local/venus.on.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <os/log.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "venus_proto.h"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"	/* CVBufferGetAttachment: iOS 16.7 has it */

#define LOG_PATH	"/var/local/vtvenc.log"
#define LOG_MAX		(1 << 20)
#define ENV_FILE	"/var/local/vtvenc-env"
#define OFF_FILE	"/var/local/vtvenc-off"
#define ON_FILE		"/var/local/venus.on"
#define NPEND		64
#define RX_MAX		(16u << 20)

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;
typedef struct OpaqueVTEncoderSession *VTEncoderSessionRef;

struct base_class {
	uint64_t version;	/* 1 */
	uint64_t storage_size;
	Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
	OSStatus (*invalidate)(CMBaseObjectRef);
	void (*finalize)(CMBaseObjectRef);
	CFStringRef (*copy_debug_description)(CMBaseObjectRef);
	OSStatus (*copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus (*set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	OSStatus (*notification_barrier)(CMBaseObjectRef);
	const void *protocols, *reserved;
};
struct encoder_class {	/* VTVideoEncoderClass as VCPHEVC.videocodec's (iOS 16.7), version 3; 21 slots after version */
	uint64_t version;
	OSStatus (*start_session)(CMBaseObjectRef, VTEncoderSessionRef, CMVideoDimensions, CFDictionaryRef);
	OSStatus (*encode_frame)(CMBaseObjectRef, void *, CVImageBufferRef, CMTime, CMTime, CFDictionaryRef);
	OSStatus (*complete_frames)(CMBaseObjectRef, CMTime);
	OSStatus (*copy_supported_property_dictionary)(CMBaseObjectRef, CFDictionaryRef *);
	void *rest[17];
};
struct vtable {
	const void *reserved;
	const struct base_class *base;
	const struct encoder_class *derived;
};

extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
extern void *CMBaseObjectGetDerivedStorage(CMBaseObjectRef);
extern CMBaseClassID VTVideoEncoderGetClassID(void);
extern OSStatus VTRegisterVideoEncoderWithInfo(FourCharCode, CFDictionaryRef, void *);
extern OSStatus VTEncoderSessionSetPixelBufferAttributes(VTEncoderSessionRef, CFDictionaryRef);
extern OSStatus VTEncoderSessionEmitEncodedFrame(VTEncoderSessionRef, void *, OSStatus, uint32_t, CMSampleBufferRef);

enum {	/* VTErrors.h */
	ERR_PROPERTY = -12900, ERR_PARAM = -12902, ERR_INVALID = -12903, ERR_NO_ENCODER = -12908, ERR_MALFUNCTION = -12911,
	ERR_FORMAT = -12910,
};
#define INFO_DROPPED	2u	/* kVTEncodeInfo_FrameDropped */

static struct { int hevc, attr, qp_i, qp_p, seqhdr, inflight, log, hw, rating, id, rc, early_ms; } cfg = { 1, 0, 0, 0, 0, 12, 0, 1, 1000, 1, 1, 40 };
static pthread_once_t cfg_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char line[480], out[560];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	int n = snprintf(out, sizeof out, "vtvenc: %d %s %ld: %s\n", getpid(), getprogname(), (long)time(NULL), line);
	if (n <= 0)
		return;
	if (n >= (int)sizeof out)
		n = (int)sizeof out - 1, out[n - 1] = '\n';
	os_log(OS_LOG_DEFAULT, "%{public}s", out);
	pthread_mutex_lock(&log_lock);
	int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	if (fd >= 0) {
		struct stat st;
		if (!fstat(fd, &st) && st.st_size > LOG_MAX)
			(void)!ftruncate(fd, 0);
		(void)fchmod(fd, 0666);
		(void)!write(fd, out, (size_t)n);
		close(fd);
	}
	pthread_mutex_unlock(&log_lock);
}

static void cfg_set(const char *k, const char *v)
{
	int n = atoi(v);
	if (!strcmp(k, "VTVENC_HEVC"))
		cfg.hevc = n;
	else if (!strcmp(k, "VTVENC_ATTR"))
		cfg.attr = n;
	else if (!strcmp(k, "VTVENC_QP"))
		sscanf(v, "%d:%d", &cfg.qp_i, &cfg.qp_p);
	else if (!strcmp(k, "VTVENC_SEQHDR"))
		cfg.seqhdr = n;
	else if (!strcmp(k, "VTVENC_INFLIGHT"))
		cfg.inflight = n < 1 ? 1 : n > NPEND - 4 ? NPEND - 4 : n;
	else if (!strcmp(k, "VTVENC_LOG"))
		cfg.log = n;
	else if (!strcmp(k, "VTVENC_HW"))
		cfg.hw = n;
	else if (!strcmp(k, "VTVENC_RATING"))
		cfg.rating = n;
	else if (!strcmp(k, "VTVENC_ID"))
		cfg.id = n;
	else if (!strcmp(k, "VTVENC_RC"))
		cfg.rc = n;
	else if (!strcmp(k, "VTVENC_EARLY_MS"))
		cfg.early_ms = n;
}

static void cfg_load(void)
{
	static const char *const keys[] = { "VTVENC_HEVC", "VTVENC_ATTR", "VTVENC_QP", "VTVENC_SEQHDR", "VTVENC_INFLIGHT",
		"VTVENC_LOG", "VTVENC_HW", "VTVENC_RATING", "VTVENC_ID",
		"VTVENC_RC", "VTVENC_EARLY_MS" };
	for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
		if (getenv(keys[i]))
			cfg_set(keys[i], getenv(keys[i]));
	FILE *f = fopen(ENV_FILE, "r");
	char l[160];
	while (f && fgets(l, sizeof l, f)) {
		char *eq = strchr(l, '=');
		if (!eq || l[0] == '#')
			continue;
		*eq = 0;
		eq[strcspn(eq + 1, "\r\n") + 1] = 0;
		cfg_set(l, eq + 1);
	}
	if (f)
		fclose(f);
}

static double mono(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- the encoder instance ---- */

struct pend {
	int used;
	uint32_t seq;
	void *token;
	CMTime pts, dur;
	CVPixelBufferRef pb;	/* held until ENC_INDONE */
	int done_in, emitted;
};

struct ve {
	int refs;
	pthread_mutex_t mu, txmu;
	pthread_cond_t cv;
	pthread_t reader;
	int reader_on, stop, fd, failed, opened, open_wait, invalid;
	int32_t open_status;
	VTEncoderSessionRef session;
	FourCharCode codec;
	int w, h;
	/* properties */
	double bitrate, fps, key_dur, peak;
	/* host rate control (Venus's own needs the CVP core): bytes of the frames since the last QP change */
	int qp, qpi, rc_n, rc_changes;
	double rc_bytes;
	int key_int, realtime, reorder, prof, level, entropy_cavlc;
	int prim, transfer, matrix, full;
	CFStringRef prim_s, transfer_s, matrix_s;
	/* stream */
	uint8_t ps[3][256];	/* VPS (HEVC), SPS, PPS */
	size_t ps_len[3];
	CMFormatDescriptionRef fmt;
	struct pend pend[NPEND];
	int inflight;
	uint32_t seq;
	unsigned n_in, n_out, n_drop, n_err, n_zc;
	double t_first, t_open, bytes;
	int force_next, early;
};

struct storage {
	struct ve *v;
};

static void ve_unref(struct ve *v)
{
	pthread_mutex_lock(&v->mu);
	int last = --v->refs == 0;
	pthread_mutex_unlock(&v->mu);
	if (!last)
		return;
	if (v->fmt)
		CFRelease(v->fmt);
	if (v->prim_s)
		CFRelease(v->prim_s);
	if (v->transfer_s)
		CFRelease(v->transfer_s);
	if (v->matrix_s)
		CFRelease(v->matrix_s);
	if (v->fd >= 0)
		close(v->fd);
	pthread_mutex_destroy(&v->mu);
	pthread_mutex_destroy(&v->txmu);
	pthread_cond_destroy(&v->cv);
	free(v);
}

static struct ve *ve_of(CMBaseObjectRef o)
{
	struct storage *s = o ? CMBaseObjectGetDerivedStorage(o) : NULL;
	return s ? s->v : NULL;
}

static int wr_all(int fd, const void *b, size_t n)
{
	const uint8_t *p = b;
	double t0 = mono();
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && mono() - t0 < 5) {
			struct pollfd q = { .fd = fd, .events = POLLOUT };
			poll(&q, 1, 50);
			continue;
		}
		if (k <= 0)
			return -1;
		p += k, n -= (size_t)k;
	}
	return 0;
}

static int rd_all(int fd, void *b, size_t n, int *stop)
{
	uint8_t *p = b;
	while (n) {
		ssize_t k = read(fd, p, n);
		if (k < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (*stop)
				return -1;
			continue;
		}
		if (k <= 0)
			return -1;
		p += k, n -= (size_t)k;
	}
	return 0;
}

static int send_msg(struct ve *v, struct venusd_msg *m, const void *payload)
{
	m->magic = VENUSD_MAGIC;
	m->version = VENUSD_VERSION;
	pthread_mutex_lock(&v->txmu);
	int r = v->fd < 0 || wr_all(v->fd, m, sizeof *m) || (m->len && wr_all(v->fd, payload, m->len));
	pthread_mutex_unlock(&v->txmu);
	return r ? -1 : 0;
}

/* (under mu) emit frame p as dropped or failed and forget it */
static void pend_drop(struct ve *v, struct pend *p, OSStatus st)
{
	if (!p->used)
		return;
	if (!p->emitted && v->session)
		VTEncoderSessionEmitEncodedFrame(v->session, p->token, st, st ? 0 : INFO_DROPPED, NULL);
	if (p->pb)
		CVPixelBufferRelease(p->pb);
	memset(p, 0, sizeof *p);
	v->inflight--;
	v->n_drop++;
	pthread_cond_broadcast(&v->cv);
}

static struct pend *pend_of(struct ve *v, uint32_t seq)
{
	for (int i = 0; i < NPEND; i++)
		if (v->pend[i].used && v->pend[i].seq == seq)
			return &v->pend[i];
	return NULL;
}

/* ---- Annex B -> parameter sets + length-prefixed NAL units ---- */

static int nal_type(const struct ve *v, const uint8_t *n)
{
	return v->codec == kCMVideoCodecType_HEVC ? (n[0] >> 1) & 0x3f : n[0] & 0x1f;
}

/* the NAL units of an Annex B buffer: calls f(v, nal, len, ctx) for each */
static void each_nal(struct ve *v, const uint8_t *b, size_t n, void (*f)(struct ve *, const uint8_t *, size_t, void *),
		     void *ctx)
{
	size_t i = 0, start = (size_t)-1;
	while (i + 3 <= n) {
		if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
			if (start != (size_t)-1) {
				size_t end = i;
				while (end > start && b[end - 1] == 0)
					end--;
				f(v, b + start, end - start, ctx);
			}
			i += 3;
			start = i;
		} else {
			i++;
		}
	}
	if (start != (size_t)-1 && start < n)
		f(v, b + start, n - start, ctx);
}

struct conv { uint8_t *out; size_t len, cap; int sync, ps_changed; };

static void conv_nal(struct ve *v, const uint8_t *nal, size_t len, void *c_)
{
	struct conv *c = c_;
	if (!len)
		return;
	int t = nal_type(v, nal), slot = -1;
	if (v->codec == kCMVideoCodecType_HEVC)
		slot = t == 32 ? 0 : t == 33 ? 1 : t == 34 ? 2 : -1;
	else
		slot = t == 7 ? 1 : t == 8 ? 2 : -1;
	if (slot >= 0) {	/* a parameter set: kept for the format description, not in the sample */
		if (len <= sizeof v->ps[0] && (v->ps_len[slot] != len || memcmp(v->ps[slot], nal, len))) {
			memcpy(v->ps[slot], nal, len);
			v->ps_len[slot] = len;
			c->ps_changed = 1;
		}
		return;
	}
	if (v->codec == kCMVideoCodecType_HEVC ? (t >= 16 && t <= 21) : t == 5)
		c->sync = 1;
	if (v->codec == kCMVideoCodecType_H264 && t == 9)
		return;	/* access unit delimiter: not in MP4 samples */
	if (v->codec == kCMVideoCodecType_HEVC && t == 35)
		return;
	if (!c->out)
		return;
	if (c->len + 4 + len > c->cap)
		return;
	c->out[c->len++] = (uint8_t)(len >> 24);
	c->out[c->len++] = (uint8_t)(len >> 16);
	c->out[c->len++] = (uint8_t)(len >> 8);
	c->out[c->len++] = (uint8_t)len;
	memcpy(c->out + c->len, nal, len);
	c->len += len;
}

static void dset_int(CFMutableDictionaryRef d, CFStringRef k, int v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
	CFDictionarySetValue(d, k, n);
	CFRelease(n);
}

/* (under mu) the format description from the parameter sets */
static int make_fmt(struct ve *v)
{
	int hevc = v->codec == kCMVideoCodecType_HEVC;
	if (!v->ps_len[1] || !v->ps_len[2] || (hevc && !v->ps_len[0]))
		return -1;
	CFMutableDictionaryRef ext = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							       &kCFTypeDictionaryValueCallBacks);
	if (v->prim_s)
		CFDictionarySetValue(ext, kCMFormatDescriptionExtension_ColorPrimaries, v->prim_s);
	if (v->transfer_s)
		CFDictionarySetValue(ext, kCMFormatDescriptionExtension_TransferFunction, v->transfer_s);
	if (v->matrix_s)
		CFDictionarySetValue(ext, kCMFormatDescriptionExtension_YCbCrMatrix, v->matrix_s);
	if (v->full)
		CFDictionarySetValue(ext, kCMFormatDescriptionExtension_FullRangeVideo, kCFBooleanTrue);
	CFDictionarySetValue(ext, kCMFormatDescriptionExtension_FormatName, hevc ? CFSTR("HEVC") : CFSTR("H.264"));
	CMFormatDescriptionRef f = NULL;
	OSStatus st;
	if (hevc) {
		const uint8_t *p[3] = { v->ps[0], v->ps[1], v->ps[2] };
		size_t s[3] = { v->ps_len[0], v->ps_len[1], v->ps_len[2] };
		st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, p, s, 4, ext, &f);
	} else {
		const uint8_t *p[2] = { v->ps[1], v->ps[2] };
		size_t s[2] = { v->ps_len[1], v->ps_len[2] };
		st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, p, s, 4, &f);
		if (!st && f && CFDictionaryGetCount(ext)) {	/* the H.264 call takes no extensions: add ours */
			CFDictionaryRef old = CMFormatDescriptionGetExtensions(f);
			CFMutableDictionaryRef all = old ? CFDictionaryCreateMutableCopy(NULL, 0, old) :
				CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFIndex n = CFDictionaryGetCount(ext);
			const void **k = malloc(sizeof(void *) * (size_t)n), **val = malloc(sizeof(void *) * (size_t)n);
			if (k && val) {
				CFDictionaryGetKeysAndValues(ext, k, val);
				for (CFIndex i = 0; i < n; i++)
					CFDictionarySetValue(all, k[i], val[i]);
			}
			free(k);
			free(val);
			CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f);
			CMFormatDescriptionRef g = NULL;
			if (!CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_H264, d.width, d.height, all, &g) && g) {
				CFRelease(f);
				f = g;
			}
			CFRelease(all);
		}
	}
	CFRelease(ext);
	if (st || !f) {
		say("format description from the parameter sets failed: %d", (int)st);
		return -1;
	}
	if (v->fmt)
		CFRelease(v->fmt);
	v->fmt = f;
	CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f);
	say("format description: %s %dx%d (SPS %zu B, PPS %zu B%s)", hevc ? "hvc1" : "avc1", d.width, d.height,
	    v->ps_len[1], v->ps_len[2], hevc ? ", VPS" : "");
	return 0;
}

/* (under mu) host rate control: after RC_MIN frames at the current QP, the bit rate they made against the target
 * (AverageBitRate, capped by DataRateLimits); 6 QP per doubling (H.264/HEVC), half the step at a time, at most +4/-3,
 * QP 12..51; dead band 15 %. Returns the new P QP to send (I = P - 2), or 0 */
#define RC_MIN	8
static int rc_step(struct ve *v, size_t len)
{
	double target = v->bitrate > 0 ? v->bitrate : 0;
	if (v->peak > 0 && (!target || v->peak * 0.9 < target))
		target = v->peak * 0.9;
	if (!cfg.rc || cfg.qp_i || target <= 0 || !v->qp)
		return 0;
	v->rc_bytes += (double)len;
	if (++v->rc_n < RC_MIN)
		return 0;
	double fps = v->fps > 0 ? v->fps : 30, actual = v->rc_bytes * 8 * fps / v->rc_n, ratio = actual / target;
	if (ratio < 1.15 && ratio > 0.87) {
		v->rc_n = 0, v->rc_bytes = 0;
		return 0;
	}
	int step = (int)lround(3.0 * log2(ratio));
	step = step > 4 ? 4 : step < -3 ? -3 : step;
	if (!step)
		step = ratio > 1 ? 1 : -1;
	int q = v->qp + step;
	q = q < 12 ? 12 : q > 51 ? 51 : q;
	v->rc_n = 0, v->rc_bytes = 0;
	if (q == v->qp)
		return 0;
	if (cfg.log || v->rc_changes < 12)
		say("rate control: %.0f kbit/s against %.0f over the last frames: QP %d -> %d", actual / 1e3, target / 1e3, v->qp, q);
	v->rc_changes++;
	v->qp = q;
	return q;
}

/* (reader) one ENC_DATA */
static void on_data(struct ve *v, const struct venusd_msg *m, const uint8_t *data)
{
	struct conv c = { 0 };
	pthread_mutex_lock(&v->mu);
	if (m->flags & 0x80u) {	/* codec config: parameter sets only */
		each_nal(v, data, m->len, conv_nal, &c);
		if (c.ps_changed && !make_fmt(v))
			;
		pthread_mutex_unlock(&v->mu);
		return;
	}
	struct pend *p = pend_of(v, m->seq);
	if (!p) {
		pthread_mutex_unlock(&v->mu);
		say("encoded frame for seq %u that is not pending: dropped", m->seq);
		return;
	}
	c.cap = m->len + 64;
	c.out = malloc(c.cap);
	each_nal(v, data, m->len, conv_nal, &c);
	if ((c.ps_changed || !v->fmt) && make_fmt(v)) {
		pthread_mutex_unlock(&v->mu);
		free(c.out);
		say("frame %u: no parameter sets yet: dropped", m->seq);
		pthread_mutex_lock(&v->mu);
		pend_drop(v, p, 0);
		pthread_mutex_unlock(&v->mu);
		return;
	}
	int sync = c.sync || (m->flags & 0x20u);
	CMBlockBufferRef bb = NULL;
	CMSampleBufferRef sb = NULL;
	OSStatus st = c.out && c.len ? CMBlockBufferCreateWithMemoryBlock(NULL, c.out, c.len, kCFAllocatorMalloc, NULL, 0,
									c.len, 0, &bb) : ERR_MALFUNCTION;
	if (st)
		free(c.out);
	if (!st) {
		CMSampleTimingInfo ti = { p->dur, p->pts, p->pts };
		size_t sz = c.len;
		st = CMSampleBufferCreateReady(NULL, bb, v->fmt, 1, 1, &ti, 1, &sz, &sb);
	}
	if (!st && sb) {
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, true);
		CFMutableDictionaryRef a = att && CFArrayGetCount(att) ? (CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0) : NULL;
		if (a) {
			CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, sync ? kCFBooleanFalse : kCFBooleanTrue);
			CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, sync ? kCFBooleanFalse : kCFBooleanTrue);
		}
	}
	void *token = p->token;
	VTEncoderSessionRef s = v->session;
	if (!v->n_out++)
		v->t_first = mono();
	v->bytes += c.len;
	p->emitted = 1;
	int newqp = rc_step(v, c.len);
	pthread_mutex_unlock(&v->mu);
	if (newqp) {
		struct venusd_msg q = { .type = VD_ENC_QP, .crop_x = (uint32_t)(newqp - 2), .crop_y = (uint32_t)newqp };
		send_msg(v, &q, NULL);
	}
	/* emit outside the lock: VT's callback may call back into us (CompleteFrames) */
	if (s)
		VTEncoderSessionEmitEncodedFrame(s, token, st ? ERR_MALFUNCTION : 0, 0, st ? NULL : sb);
	if (cfg.log)
		say("frame %u: %zu bytes%s, pic type %#x", m->seq, c.len, sync ? " sync" : "", m->pic_type);
	if (sb)
		CFRelease(sb);
	if (bb)
		CFRelease(bb);
	pthread_mutex_lock(&v->mu);
	if (p->used && p->seq == m->seq) {
		if (p->done_in) {	/* its surface is back too: done */
			memset(p, 0, sizeof *p);
			v->inflight--;
		}
	}
	pthread_cond_broadcast(&v->cv);
	pthread_mutex_unlock(&v->mu);
}

/* (reader) ENC_INDONE: the surface is free; a refused frame (status < 0) is dropped */
static void on_indone(struct ve *v, const struct venusd_msg *m)
{
	pthread_mutex_lock(&v->mu);
	struct pend *p = pend_of(v, m->seq);
	if (p) {
		if (p->pb)
			CVPixelBufferRelease(p->pb);
		p->pb = NULL;
		p->done_in = 1;
		if (m->flags & 1)
			v->n_zc++;
		if (m->status < 0) {
			v->n_err++;
			say("frame %u refused by venusd (%d): dropped", m->seq, m->status);
			pend_drop(v, p, 0);
		} else if (p->emitted) {
			memset(p, 0, sizeof *p);
			v->inflight--;
		}
	}
	pthread_cond_broadcast(&v->cv);
	pthread_mutex_unlock(&v->mu);
}

/* (under mu) every frame still out: failed (st) or dropped (0) */
static void drop_all(struct ve *v, OSStatus st)
{
	for (int i = 0; i < NPEND; i++)
		if (v->pend[i].used)
			pend_drop(v, &v->pend[i], st);
}

static void *reader(void *arg)
{
	struct ve *v = arg;
	uint8_t *buf = malloc(RX_MAX);
	struct venusd_msg m;
	while (buf && !v->stop) {
		struct pollfd q = { .fd = v->fd, .events = POLLIN };
		int pr = poll(&q, 1, 250);
		if (v->stop)
			break;
		if (pr <= 0)
			continue;
		if (rd_all(v->fd, &m, sizeof m, &v->stop) || m.magic != VENUSD_MAGIC || m.len > RX_MAX ||
		    (m.len && rd_all(v->fd, buf, m.len, &v->stop))) {
			if (!v->stop)
				say("venusd connection lost or bad message: frames out are dropped, errors from now on");
			pthread_mutex_lock(&v->mu);
			v->failed = 1;
			v->open_wait = 0;
			if (!v->opened)
				v->open_status = -EIO;
			drop_all(v, ERR_MALFUNCTION);
			pthread_cond_broadcast(&v->cv);
			pthread_mutex_unlock(&v->mu);
			break;
		}
		switch (m.type) {
		case VD_ENC_OPEN:
			pthread_mutex_lock(&v->mu);
			v->open_status = m.status;
			v->opened = !m.status;
			v->qpi = (int)m.fbd_w, v->qp = (int)m.fbd_h, v->rc_n = 0, v->rc_bytes = 0;
			v->open_wait = 0;
			pthread_cond_broadcast(&v->cv);
			pthread_mutex_unlock(&v->mu);
			say("venusd encoder session %s (%d): Venus layout stride %u, scanlines %u, CbCr at %u, input %u bytes",
			    m.status ? "REFUSED" : "open", m.status, m.stride, m.scanlines, m.crop_x, m.filled);
			break;
		case VD_ENC_DATA:
			on_data(v, &m, buf);
			break;
		case VD_ENC_INDONE:
			on_indone(v, &m);
			break;
		case VD_ERROR:
			say("venusd: session error %d at frame %u: frames out failed, errors from now on", m.status, m.seq);
			pthread_mutex_lock(&v->mu);
			v->failed = 1;
			drop_all(v, ERR_MALFUNCTION);
			pthread_mutex_unlock(&v->mu);
			break;
		case VD_EOS_DONE:
		case VD_CLOSE:
			break;
		default:
			break;
		}
	}
	free(buf);
	return NULL;
}

static int venusd_connect(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", VENUSD_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct timeval tv = { 2, 0 };
	int sz = 4 << 20, one = 1;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

/* CoreVideo colour strings -> H.264/HEVC VUI codes */
static int vui_code(CFStringRef s, int what)
{
	if (!s)
		return 0;
	char b[64] = "";
	CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8);
	if (strstr(b, "709"))
		return 1;
	if (strstr(b, "2020"))
		return what == 1 ? 14 : 9;	/* transfer: BT.2020 10-bit; primaries/matrix 9 */
	if (strstr(b, "601") || strstr(b, "SMPTE_C"))
		return 6;
	if (strstr(b, "P3"))
		return what == 0 ? 12 : 0;
	if (strstr(b, "sRGB") || strstr(b, "IEC_sRGB"))
		return what == 1 ? 13 : 0;
	return 0;
}

/* (under mu) the venusd session, at the first frame */
static int open_session(struct ve *v, CVPixelBufferRef pb)
{
	struct venusd_enc_open eo = { 0 };
	double fps = v->fps > 0 ? v->fps : 30;
	eo.fps_q16 = (uint32_t)lround(fps * 65536.0);
	eo.bitrate = v->bitrate > 0 ? (uint32_t)v->bitrate : 0;
	int gop = v->key_int > 0 ? v->key_int : 0;
	if (v->key_dur > 0) {
		int g = (int)lround(v->key_dur * fps);
		if (g < 1)
			g = 1;
		if (!gop || g < gop)
			gop = g;
	}
	eo.gop = gop ? gop : (int)lround(fps);	/* the iPhone's default: an IDR a second */
	eo.profile = v->prof, eo.level = v->level;
	eo.qp_i = (uint32_t)cfg.qp_i, eo.qp_p = (uint32_t)cfg.qp_p;
	eo.seqhdr = cfg.seqhdr;
	OSType f = pb ? CVPixelBufferGetPixelFormatType(pb) : 0;
	v->full = f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
	eo.full_range = v->full;
	if (!v->prim_s && pb) {	/* the buffer's own colour, if the client set none */
		CFTypeRef p = CVBufferGetAttachment(pb, kCVImageBufferColorPrimariesKey, NULL),
			t = CVBufferGetAttachment(pb, kCVImageBufferTransferFunctionKey, NULL),
			mx = CVBufferGetAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
		if (p && CFGetTypeID(p) == CFStringGetTypeID())
			v->prim_s = CFRetain(p);
		if (t && CFGetTypeID(t) == CFStringGetTypeID() && !v->transfer_s)
			v->transfer_s = CFRetain(t);
		if (mx && CFGetTypeID(mx) == CFStringGetTypeID() && !v->matrix_s)
			v->matrix_s = CFRetain(mx);
	}
	eo.primaries = vui_code(v->prim_s, 0), eo.transfer = vui_code(v->transfer_s, 1), eo.matrix = vui_code(v->matrix_s, 2);
	if (!eo.primaries)
		eo.transfer = eo.matrix = 0;
	else if (!eo.transfer || !eo.matrix)
		eo.transfer = eo.transfer ? eo.transfer : 1, eo.matrix = eo.matrix ? eo.matrix : 1;
	struct venusd_msg m = { .type = VD_ENC_OPEN, .codec = v->codec == kCMVideoCodecType_HEVC ? VD_CODEC_HEVC :
		VD_CODEC_H264, .width = (uint32_t)v->w, .height = (uint32_t)v->h, .len = sizeof eo };
	v->open_wait = 1, v->open_status = 0, v->opened = 0;
	v->t_open = mono();
	pthread_mutex_unlock(&v->mu);
	int r = send_msg(v, &m, &eo);
	pthread_mutex_lock(&v->mu);
	if (r) {
		v->open_wait = 0;
		return -1;
	}
	struct timespec ts;
	while (v->open_wait && !v->failed && !v->stop && mono() - v->t_open < 20) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100000000;
		if (ts.tv_nsec >= 1000000000)
			ts.tv_sec++, ts.tv_nsec -= 1000000000;
		pthread_cond_timedwait(&v->cv, &v->mu, &ts);
	}
	say("%s %dx%d %.2f fps, %.0f bit/s, gop %d, profile %d level %d, colour %d/%d/%d%s: %s in %.0f ms",
	    v->codec == kCMVideoCodecType_HEVC ? "HEVC" : "H.264", v->w, v->h, fps, v->bitrate, eo.gop, eo.profile, eo.level,
	    eo.primaries, eo.transfer, eo.matrix, v->full ? " full range" : "", v->opened ? "open" : "FAILED",
	    1e3 * (mono() - v->t_open));
	return v->opened ? 0 : -1;
}

/* early open: the venusd session (Venus resume + HFI session setup, ~0.3-1 s) starts VTVENC_EARLY_MS after StartSession
 * (the client sets its properties right after it), so the first EncodeFrame does not wait for all of it */
static void *early_open(void *arg)
{
	struct ve *v = arg;
	usleep((useconds_t)cfg.early_ms * 1000u);
	pthread_mutex_lock(&v->mu);
	if (!v->opened && !v->open_wait && !v->failed && !v->invalid && !v->n_in) {
		v->early = 1;
		if (open_session(v, NULL))
			say("early open failed: the first frame tries again");
	}
	pthread_mutex_unlock(&v->mu);
	ve_unref(v);
	return NULL;
}

/* ---- the encoder's functions ---- */

static OSStatus enc_start_session(CMBaseObjectRef o, VTEncoderSessionRef session, CMVideoDimensions dims, CFDictionaryRef d)
{
	struct ve *v = ve_of(o);
	if (!v || !session)
		return ERR_PARAM;
	if (dims.width < 32 || dims.height < 32 || dims.width > 4096 || dims.height > 2304 || (dims.width & 1) ||
	    (dims.height & 1)) {
		say("StartSession %dx%d: outside Venus's range (even, 32..4096 x 32..2304): refused", dims.width, dims.height);
		return ERR_NO_ENCODER;
	}
	pthread_mutex_lock(&v->mu);
	v->session = session;
	v->w = dims.width, v->h = dims.height;
	pthread_mutex_unlock(&v->mu);
	/* the buffers VT gives us: 420v/420f in IOSurfaces; VTVENC_ATTR=1: in Venus's layout (zero-copy) */
	int fmts[2] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange };
	CFNumberRef f0 = CFNumberCreate(NULL, kCFNumberIntType, &fmts[0]), f1 = CFNumberCreate(NULL, kCFNumberIntType, &fmts[1]);
	const void *fv[2] = { f0, f1 };
	CFArrayRef fa = CFArrayCreate(NULL, fv, 2, &kCFTypeArrayCallBacks);
	CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef a = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(a, kCVPixelBufferPixelFormatTypeKey, fa);
	CFDictionarySetValue(a, kCVPixelBufferIOSurfacePropertiesKey, empty);
	dset_int(a, kCVPixelBufferWidthKey, dims.width);
	dset_int(a, kCVPixelBufferHeightKey, dims.height);
	if (cfg.attr) {
		dset_int(a, kCVPixelBufferBytesPerRowAlignmentKey, 128);
		dset_int(a, kCVPixelBufferExtendedPixelsBottomKey, ((dims.height + 31) & ~31) - dims.height);
		dset_int(a, kCVPixelBufferPlaneAlignmentKey, 4096);
	}
	OSStatus st = VTEncoderSessionSetPixelBufferAttributes(session, a);
	CFRelease(a);
	CFRelease(empty);
	CFRelease(fa);
	CFRelease(f0);
	CFRelease(f1);
	if (cfg.early_ms >= 0) {
		pthread_t t;
		pthread_mutex_lock(&v->mu);
		v->refs++;
		pthread_mutex_unlock(&v->mu);
		if (pthread_create(&t, NULL, early_open, v))
			ve_unref(v);
		else
			pthread_detach(t);
	}
	say("StartSession %s %dx%d (session %p, spec %p): pixel buffer attributes %s: %d",
	    v->codec == kCMVideoCodecType_HEVC ? "hvc1" : "avc1", dims.width, dims.height, (void *)session, (const void *)d,
	    cfg.attr ? "Venus layout" : "420v/420f IOSurface", (int)st);
	return 0;
}

static OSStatus enc_encode_frame(CMBaseObjectRef o, void *token, CVImageBufferRef image, CMTime pts, CMTime dur,
				 CFDictionaryRef opts)
{
	struct ve *v = ve_of(o);
	if (!v || !image)
		return ERR_PARAM;
	IOSurfaceRef surf = CVPixelBufferGetIOSurface(image);
	pthread_mutex_lock(&v->mu);
	if (v->failed || v->invalid) {
		pthread_mutex_unlock(&v->mu);
		return ERR_MALFUNCTION;
	}
	if (!surf) {
		pthread_mutex_unlock(&v->mu);
		if (v->n_err++ < 3)
			say("frame without an IOSurface: dropped");
		VTEncoderSessionEmitEncodedFrame(v->session, token, 0, INFO_DROPPED, NULL);
		return 0;
	}
	while (v->open_wait && !v->failed) {	/* the early open is under way */
		struct timespec tw;
		clock_gettime(CLOCK_REALTIME, &tw);
		tw.tv_nsec += 20000000;
		if (tw.tv_nsec >= 1000000000)
			tw.tv_sec++, tw.tv_nsec -= 1000000000;
		pthread_cond_timedwait(&v->cv, &v->mu, &tw);
	}
	if (v->opened && v->early && !v->n_in && CVPixelBufferGetPixelFormatType(image) ==
	    kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
		say("note: opened early as video range, the frames are full range (420f): the VUI says video range");
	if (!v->opened && open_session(v, image)) {
		v->failed = 1;
		pthread_mutex_unlock(&v->mu);
		return ERR_MALFUNCTION;
	}
	/* backpressure: VTVENC_INFLIGHT frames out at most (Venus does one in ~8 ms; a stall shows in the log) */
	double t0 = mono();
	struct timespec ts;
	while (v->inflight >= cfg.inflight && !v->failed && mono() - t0 < 2) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20000000;
		if (ts.tv_nsec >= 1000000000)
			ts.tv_sec++, ts.tv_nsec -= 1000000000;
		pthread_cond_timedwait(&v->cv, &v->mu, &ts);
	}
	struct pend *p = NULL;
	for (int i = 0; i < NPEND && !p; i++)
		if (!v->pend[i].used)
			p = &v->pend[i];
	if (v->failed || !p || v->inflight >= cfg.inflight) {
		int f = v->failed;
		v->n_drop++;
		pthread_mutex_unlock(&v->mu);
		if (v->n_drop < 5 || !(v->n_drop % 100))
			say("frame dropped: %s (%d in flight, %u dropped so far)", f ? "session failed" : "encoder behind",
			    v->inflight, v->n_drop);
		if (f)
			return ERR_MALFUNCTION;
		VTEncoderSessionEmitEncodedFrame(v->session, token, 0, INFO_DROPPED, NULL);
		return 0;
	}
	int force = v->force_next;
	CFTypeRef fk = opts ? CFDictionaryGetValue(opts, CFSTR("ForceKeyFrame")) : NULL;
	if (fk == kCFBooleanTrue)
		force = 1;
	v->force_next = 0;
	*p = (struct pend){ 1, ++v->seq, token, pts, dur, CVPixelBufferRetain(image), 0, 0 };
	v->inflight++;
	v->n_in++;
	uint32_t seq = p->seq;
	pthread_mutex_unlock(&v->mu);
	struct venusd_msg m = { .type = VD_ENC_FRAME, .surface = IOSurfaceGetID(surf), .seq = seq, .flags = force ? 1u : 0u,
		.pts = CMTIME_IS_NUMERIC(pts) ? (uint64_t)(CMTimeGetSeconds(pts) * 1e6) : 0 };
	if (send_msg(v, &m, NULL)) {
		say("frame %u: send to venusd failed: errors from now on", seq);
		pthread_mutex_lock(&v->mu);
		v->failed = 1;
		drop_all(v, ERR_MALFUNCTION);
		pthread_mutex_unlock(&v->mu);
		return ERR_MALFUNCTION;
	}
	return 0;
}

static OSStatus enc_complete_frames(CMBaseObjectRef o, CMTime until)
{
	struct ve *v = ve_of(o);
	if (!v)
		return ERR_PARAM;
	double t0 = mono();
	pthread_mutex_lock(&v->mu);
	for (;;) {
		int left = 0;
		for (int i = 0; i < NPEND; i++)
			if (v->pend[i].used && !v->pend[i].emitted &&
			    (!CMTIME_IS_NUMERIC(until) || CMTimeCompare(v->pend[i].pts, until) <= 0))
				left++;
		if (!left || v->failed)
			break;
		if (mono() - t0 > 3) {
			say("CompleteFrames: %d frames not back after 3 s: dropped", left);
			for (int i = 0; i < NPEND; i++)
				if (v->pend[i].used && !v->pend[i].emitted &&
				    (!CMTIME_IS_NUMERIC(until) || CMTimeCompare(v->pend[i].pts, until) <= 0))
					pend_drop(v, &v->pend[i], 0);
			break;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20000000;
		if (ts.tv_nsec >= 1000000000)
			ts.tv_sec++, ts.tv_nsec -= 1000000000;
		pthread_cond_timedwait(&v->cv, &v->mu, &ts);
	}
	pthread_mutex_unlock(&v->mu);
	return 0;
}

/* the properties offered (CMCapture's and AVAssetWriter's); read-write unless said */
static const char *const props[] = { "AverageBitRate", "DataRateLimits", "ExpectedFrameRate", "MaxKeyFrameInterval",
	"MaxKeyFrameIntervalDuration", "ProfileLevel", "RealTime", "AllowFrameReordering", "H264EntropyMode",
	"ColorPrimaries", "TransferFunction", "YCbCrMatrix", "Quality", "PrioritizeEncodingSpeedOverQuality", "Priority",
	"QuantizationScalingMatrixPreset", "MaxFrameDelayCount", "AllowTemporalCompression", "ExpectedDuration",
	"SourceFrameCount", "AllowOpenGOP", "MoreFramesBeforeStart", "MoreFramesAfterEnd", "FieldCount", "CleanAperture",
	"PixelAspectRatio", "ICCProfile", "MaximizePowerEfficiency", "ConstantBitRate", "EnableLowLatencyRateControl",
	"MinAllowedFrameQP", "MaxAllowedFrameQP", "UsingHardwareAcceleratedVideoEncoder", "NumberOfPendingFrames" };

static OSStatus enc_supported(CMBaseObjectRef o, CFDictionaryRef *out)
{
	if (!out)
		return ERR_PARAM;
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
	for (unsigned i = 0; i < sizeof props / sizeof props[0]; i++) {
		CFStringRef k = CFStringCreateWithCString(NULL, props[i], kCFStringEncodingUTF8);
		int ro = !strcmp(props[i], "UsingHardwareAcceleratedVideoEncoder") || !strcmp(props[i], "NumberOfPendingFrames");
		const void *pk[] = { CFSTR("ReadWriteStatus") };
		const void *pv[] = { ro ? CFSTR("ReadOnly") : CFSTR("ReadWrite") };
		CFDictionaryRef pd = CFDictionaryCreate(NULL, pk, pv, 1, &kCFTypeDictionaryKeyCallBacks,
							&kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(d, k, pd);
		CFRelease(pd);
		CFRelease(k);
	}
	*out = d;
	return 0;
}

static double num(CFTypeRef v)
{
	double d = 0;
	if (v && CFGetTypeID(v) == CFNumberGetTypeID())
		CFNumberGetValue(v, kCFNumberDoubleType, &d);
	else if (v && CFGetTypeID(v) == CFBooleanGetTypeID())
		d = v == kCFBooleanTrue;
	return d;
}

static int key_is(CFStringRef k, const char *s)
{
	char b[80];
	return CFStringGetCString(k, b, sizeof b, kCFStringEncodingUTF8) && !strcmp(b, s);
}

/* ProfileLevel strings -> HFI profile/level (level 0: the firmware's choice) */
static void profile_of(struct ve *v, CFStringRef s)
{
	char b[64] = "";
	CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8);
	if (v->codec == kCMVideoCodecType_HEVC) {
		v->prof = strstr(b, "Main10") ? 0x2 : 0x1;
		v->level = 0;
		return;
	}
	v->prof = strstr(b, "ConstrainedBaseline") ? 0x20 : strstr(b, "Baseline") ? 0x1 : strstr(b, "ConstrainedHigh") ? 0x40 :
		strstr(b, "Main") ? 0x2 : 0x4;
	v->level = 0;
	static const struct { const char *s; int hfi; } lv[] = { { "_3_0", 0x100 }, { "_3_1", 0x200 }, { "_3_2", 0x400 },
		{ "_4_0", 0x800 }, { "_4_1", 0x1000 }, { "_4_2", 0x2000 }, { "_5_0", 0x4000 }, { "_5_1", 0x8000 },
		{ "_5_2", 0x10000 } };
	for (unsigned i = 0; i < sizeof lv / sizeof lv[0]; i++)
		if (strstr(b, lv[i].s))
			v->level = lv[i].hfi;
}

static OSStatus enc_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct ve *v = ve_of(o);
	if (!v || !key)
		return ERR_PARAM;
	char k[80] = "";
	CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8);
	pthread_mutex_lock(&v->mu);
	int known = 1;
	if (!strcmp(k, "AverageBitRate"))
		v->bitrate = num(value);
	else if (!strcmp(k, "DataRateLimits") && value && CFGetTypeID(value) == CFArrayGetTypeID() &&
		 CFArrayGetCount(value) >= 2) {
		double bytes = num(CFArrayGetValueAtIndex(value, 0)), secs = num(CFArrayGetValueAtIndex(value, 1));
		v->peak = secs > 0 ? bytes * 8 / secs : 0;
	} else if (!strcmp(k, "ExpectedFrameRate"))
		v->fps = num(value);
	else if (!strcmp(k, "MaxKeyFrameInterval"))
		v->key_int = (int)num(value);
	else if (!strcmp(k, "MaxKeyFrameIntervalDuration"))
		v->key_dur = num(value);
	else if (!strcmp(k, "RealTime"))
		v->realtime = (int)num(value);
	else if (!strcmp(k, "AllowFrameReordering"))
		v->reorder = (int)num(value);
	else if (!strcmp(k, "ProfileLevel") && value && CFGetTypeID(value) == CFStringGetTypeID())
		profile_of(v, value);
	else if ((!strcmp(k, "ColorPrimaries") || !strcmp(k, "TransferFunction") || !strcmp(k, "YCbCrMatrix")) && value &&
		 CFGetTypeID(value) == CFStringGetTypeID()) {
		CFStringRef *slot = k[0] == 'C' ? &v->prim_s : k[0] == 'T' ? &v->transfer_s : &v->matrix_s;
		if (*slot)
			CFRelease(*slot);
		*slot = CFRetain(value);
	} else
		known = 0;
	int opened = v->opened;
	pthread_mutex_unlock(&v->mu);
	if (!known && strcmp(k, "Priority") && strcmp(k, "Quality"))
		say("property %s: accepted, not used (constant-QP Venus encoder)", k);
	if (opened && !strcmp(k, "AverageBitRate"))
		say("AverageBitRate %.0f after the start: the QP stays (rate control off)", num(value));
	return 0;
}

static OSStatus enc_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	struct ve *v = ve_of(o);
	if (!v || !key || !out)
		return ERR_PARAM;
	CFTypeRef r = NULL;
	if (key_is(key, "UsingHardwareAcceleratedVideoEncoder"))
		r = CFRetain(kCFBooleanTrue);
	else if (key_is(key, "AllowFrameReordering"))
		r = CFRetain(kCFBooleanFalse);
	else if (key_is(key, "RealTime"))
		r = CFRetain(v->realtime ? kCFBooleanTrue : kCFBooleanFalse);
	else if (key_is(key, "AverageBitRate") && v->bitrate > 0) {
		int64_t n = (int64_t)v->bitrate;
		r = CFNumberCreate(NULL, kCFNumberSInt64Type, &n);
	} else if (key_is(key, "ExpectedFrameRate") && v->fps > 0)
		r = CFNumberCreate(NULL, kCFNumberDoubleType, &v->fps);
	else if (key_is(key, "NumberOfPendingFrames")) {
		int n = v->inflight;
		r = CFNumberCreate(NULL, kCFNumberIntType, &n);
	} else if (key_is(key, "MaxFrameDelayCount")) {
		int n = cfg.inflight;
		r = CFNumberCreate(NULL, kCFNumberIntType, &n);
	}
	if (!r)
		return ERR_PROPERTY;
	*(CFTypeRef *)out = r;
	return 0;
}

static OSStatus enc_invalidate(CMBaseObjectRef o)
{
	struct ve *v = ve_of(o);
	if (!v)
		return 0;
	pthread_mutex_lock(&v->mu);
	if (v->invalid) {
		pthread_mutex_unlock(&v->mu);
		return 0;
	}
	v->invalid = 1;
	int opened = v->opened;
	pthread_mutex_unlock(&v->mu);
	if (opened) {	/* what is out comes back first (no reordering: nothing is held), then CLOSE */
		enc_complete_frames(o, kCMTimeInvalid);
		struct venusd_msg m = { .type = VD_CLOSE };
		send_msg(v, &m, NULL);
	}
	pthread_mutex_lock(&v->mu);
	v->stop = 1;
	drop_all(v, 0);
	pthread_mutex_unlock(&v->mu);
	if (v->fd >= 0)
		shutdown(v->fd, SHUT_RDWR);
	if (v->reader_on && !pthread_equal(pthread_self(), v->reader))
		pthread_join(v->reader, NULL);
	else if (v->reader_on)
		pthread_detach(v->reader);
	v->reader_on = 0;
	double span = v->n_out > 1 ? mono() - v->t_first : 0;
	say("invalidated: %u frames in, %u out (%.1f fps from the first), %u zero-copy, %u dropped, %u refused, %.0f kbit/s",
	    v->n_in, v->n_out, span > 0 ? (v->n_out - 1) / span : 0.0, v->n_zc, v->n_drop, v->n_err,
	    span > 0 ? v->bytes * 8 / span / 1e3 : 0.0);
	return 0;
}

static void enc_finalize(CMBaseObjectRef o)
{
	struct storage *s = o ? CMBaseObjectGetDerivedStorage(o) : NULL;
	if (!s || !s->v)
		return;
	enc_invalidate(o);
	ve_unref(s->v);
	s->v = NULL;
}

static CFStringRef enc_description(CMBaseObjectRef o)
{
	struct ve *v = ve_of(o);
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<vtvenc %p: Venus %s via venusd, %u in, %u out, %u dropped>"),
					(void *)o, v && v->codec == kCMVideoCodecType_HEVC ? "HEVC" : "H.264",
					v ? v->n_in : 0, v ? v->n_out : 0, v ? v->n_drop : 0);
}

static const struct base_class enc_base = {
	1, sizeof(struct storage), NULL, enc_invalidate, enc_finalize, enc_description, enc_copy_property, enc_set_property,
	NULL, NULL, NULL
};
static const struct encoder_class enc_class = {
	3, enc_start_session, enc_encode_frame, enc_complete_frames, enc_supported, { NULL }
};
static const struct vtable enc_vtable = { NULL, &enc_base, &enc_class };

static OSStatus create_instance(FourCharCode codec, CFAllocatorRef alloc, CMBaseObjectRef *out)
{
	struct stat sb;
	char why[120] = "";
	int fd = -1;
	if (!out)
		return ERR_PARAM;
	*out = NULL;
	pthread_once(&cfg_once, cfg_load);
	if (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC)
		snprintf(why, sizeof why, "codec %#x", (unsigned)codec);
	else if (!stat(OFF_FILE, &sb))
		snprintf(why, sizeof why, "%s exists", OFF_FILE);
	else if (stat(ON_FILE, &sb))
		snprintf(why, sizeof why, "no %s", ON_FILE);
	else if ((fd = venusd_connect()) < 0)
		snprintf(why, sizeof why, "no venusd at %s (%s)", VENUSD_SOCK, strerror(errno));
	say("CreateInstance %c%c%c%c: %s%s", codec >> 24, codec >> 16 & 255, codec >> 8 & 255, codec & 255,
	    why[0] ? "refused: " : "venusd connected", why);
	if (why[0])
		return ERR_NO_ENCODER;
	struct ve *v = calloc(1, sizeof *v);
	CMBaseObjectRef o = NULL;
	if (!v || CMDerivedObjectCreate(alloc, &enc_vtable, VTVideoEncoderGetClassID(), &o) || !o) {
		say("CreateInstance: no memory or no encoder object");
		free(v);
		close(fd);
		return ERR_MALFUNCTION;
	}
	v->refs = 1, v->fd = fd, v->codec = codec, v->realtime = 1;
	pthread_mutex_init(&v->mu, NULL);
	pthread_mutex_init(&v->txmu, NULL);
	pthread_cond_init(&v->cv, NULL);
	if (!pthread_create(&v->reader, NULL, reader, v))
		v->reader_on = 1;
	((struct storage *)CMBaseObjectGetDerivedStorage(o))->v = v;
	*out = o;
	return 0;
}

/* registration: 'avc1' (+ 'hvc1' unless VTVENC_HEVC=0) as hardware encoders; the number registered */
__attribute__((visibility("default"))) int VTVenusEncRegister(void)
{
	struct stat sb;
	pthread_once(&cfg_once, cfg_load);
	if (!stat(OFF_FILE, &sb) || stat(ON_FILE, &sb)) {
		say("not registered: %s", !stat(OFF_FILE, &sb) ? OFF_FILE " exists" : "no " ON_FILE);
		return 0;
	}
	int n = 0;
	for (int i = 0; i < 2; i++) {
		if (i == 1 && !cfg.hevc)
			break;
		FourCharCode c = i ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
		/* VTRegisterVideoEncoderWithInfo's registry keys (iOS 16.7 VideoToolbox: CMClassImplementationID = the
		 * encoder ID, else "anon-N"; kVTVideoEncoder_IsHardwareAccelerated "VTIsHardwareAccelerated",
		 * kVTVideoEncoder_Rating "VTRating" (default 0), kVTVideoEncoder_EncoderNameKey "VTEncoderName"): the
		 * selection (vtSortRegistryItemsByVideoEncoderRating) takes the higher rating, then hardware */
		int rating = cfg.rating;
		CFNumberRef rn = CFNumberCreate(NULL, kCFNumberIntType, &rating);
		CFMutableDictionaryRef info = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
									&kCFTypeDictionaryValueCallBacks);
		if (cfg.id)
			CFDictionarySetValue(info, CFSTR("CMClassImplementationID"), i ? CFSTR("com.iosonpixel.videoencoder.hevc.venus") :
					     CFSTR("com.iosonpixel.videoencoder.h264.venus"));
		CFDictionarySetValue(info, CFSTR("VTEncoderName"), i ? CFSTR("HEVC (Venus)") : CFSTR("H.264 (Venus)"));
		if (cfg.hw)
			CFDictionarySetValue(info, CFSTR("VTIsHardwareAccelerated"), kCFBooleanTrue);
		if (cfg.rating)
			CFDictionarySetValue(info, CFSTR("VTRating"), rn);
		CFRelease(rn);
		OSStatus st = VTRegisterVideoEncoderWithInfo(c, info, (void *)create_instance);
		CFRelease(info);
		say("registered for %s: %d", i ? "hvc1" : "avc1", (int)st);
		n += !st;
	}
	return n;
}
