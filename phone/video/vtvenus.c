/*
 * vtvenus: video stage 4b (docs/features/video.md "Stage 4 spec", research/notes/venus-stage4-design-2026-11-03.md
 * sections 3-4): a VideoToolbox H.264 decoder that is a thin client of venusd (phone/wifi/venus_proto.h).
 * Installed as the flat Mach-O /System/Library/VideoDecoders/ExampleIPB.videodecoder (tools/sunfish-install-vtvenus.sh),
 * the free slot _VTLoadVideoDecoders dlopens in every VideoToolbox process; ExampleIPBDecoderRegister only registers
 * 'avc1' (VTRegisterVideoDecoderWithInfo: hardware, as AVD's VTRegisterVideoDecoder), nothing else at load.
 *  - CreateInstance refuses (kVTCouldNotFindVideoDecoderErr: VT moves on to the software decoder) when
 *    /var/local/venus-off exists or venusd's socket doesn't connect. One line per process (getprogname) to the log.
 *  - StartSession: avcC parameter sets -> Annex B (sent before the first FRAME of each venusd session), pool
 *    attributes '420v' at the visible size, IOSurface, rows aligned to 64. OPEN (the size as the hint) at the first
 *    DecodeFrame (VTVENUS_LAZY_OPEN below).
 *  - DecodeFrame: AVCC length prefixes -> start codes, FRAME with pts = tag * 33333 (tag = the instance's frame
 *    number; venusc's spacing) and seq = tag; returns asynchronous. A reader thread takes DECODED (display order),
 *    copies Y and CbCr row by row from the padded Venus NV12 into a buffer of VT's pool and emits frame[tag]. Frames
 *    venusd never returns are emitted as dropped: 24 tags behind a returned one, at EOS_DONE, on a session error,
 *    at Invalidate. Backpressure: at most 32 frames in flight (DecodeFrame waits; 2 s without any frame back drops
 *    the oldest; cold: below), and venusd stops reading while its inputs are full.
 *  - FinishDelayedFrames: EOS, wait for EOS_DONE (10 s without a message ends it; cold: below), leftovers dropped;
 *    the next DecodeFrame CLOSEs and OPENs a new venusd session (FRAME after EOS is refused). Invalidate: CLOSE,
 *    leftovers dropped, reader stopped. A session ERROR (not -E2BIG): pending dropped, reopened at the next frame
 *    (5 reopens without a picture: off).
 * Never crashes its host (mediaserverd holds audio too): every failure returns a VT error and logs, no asserts.
 * The reader's state is a separately allocated, reference-counted struct, so a release from inside an emit can't
 * free it under the thread.
 * Cold start (vdcold, video.md vdboot: the first decode after boot stalls up to ~13 s in venusd, which then reads
 * nothing; the old 2 s timeouts left the plugin dead for the clip). Switches: env or KEY=VALUE lines in
 * /var/local/vtvenus-env (read once per process; the file wins):
 *  - VTVENUS_COLD_MS (20000): OPEN's answer, and until the venusd session's first picture: FRAME sends, EOS_DONE,
 *    "none back" for the in-flight limit. VTVENUS_WAIT_MS (10000): EOS_DONE/CLOSE after that, a FRAME send's
 *    deadline. Sends that would block (EAGAIN, partial writes) are retried until the deadline. 4000/2000 ~ the old.
 *  - VTVENUS_LAZY_OPEN (1): no OPEN at StartSession, only at the first DecodeFrame (an instance that never decodes
 *    doesn't power Venus). 0: OPEN at StartSession as before.
 *  - VTVENUS_RECONNECT (1): a lost connection (or a send past its deadline, an OPEN not answered) -> pending dropped,
 *    reconnect once and OPEN a fresh venusd session; a second loss before a picture came back: errors from then on.
 *    0: unusable after a loss, as before.
 *  - VTVENUS_RESYNC (1): a fresh venusd session that would start on a non-IDR frame (after FinishDelayedFrames, an
 *    error, a reconnect) first gets the parameter sets and the access units since the last IDR/I frame again (up to
 *    256 / 16 MB; their pictures are discarded: pts tag * 33333 + 1), else frames are dropped until the next IDR/I
 *    frame (300 at most, then sent anyway). 0: sent as they come, as before.
 *  - VTVENUS_RECON_WAIT (1; vdstop): a reconnect that finds venusd restarting (connection refused, no socket) tries
 *    again every 250 ms up to VTVENUS_COLD_MS. 0: one try, as before.
 *  - VTVENUS_ERR_GAP_MS (5000; vdstop): a venusd session error (venusd recovers Venus after a STOP and answers
 *    VD_ERROR -EIO) reopens with resync as above, once per venusd session; a second session failing within this many
 *    ms of the last error: errors for this instance from then on. 0: no such limit (MAX_FAILS reopens only).
 *  - VTVENUS_HEVC (1; video 4e): also register 'hvc1' (OPEN with VD_CODEC_HEVC): hvcC's VPS/SPS/PPS -> Annex B as for
 *    avc1; StartSession/CanAccept refuse an SPS that isn't 8-bit 4:2:0 Main (Main10, 4:2:2: VT's software decoder),
 *    resync from the last IRAP access unit (IDR, CRA, BLA). 0: avc1 only, as before.
 *  - VTVENUS_REVIVE_MS (10000; vdleak, the 15-min soak: instances that gave up kept their venusd connection, their
 *    hosts never released them, venusd's 4 slots filled): an instance that gives up (errors from now on) closes its
 *    venusd connection at once; Invalidate closes it too (not only at the last release). After this many ms a
 *    given-up instance gets one fresh try at the next frame (reconnect, OPEN, resync), as a new instance would.
 *    0: given up stays given up (the connection is still closed).
 * vdnopay (copy-cost test, venusd's /var/local/venusd-nopayload): a DECODED with len 0 gets a pool buffer emitted
 * unfilled (stale pixels, no copy); one log line per venusd session. len > 0: as before.
 *  - VTVENUS_SURF (1; vdcredit, venus_proto.h CREDIT): VTVENUS_CREDITS (6, 1-16) buffers of VT's pool are held and
 *    their IOSurface IDs credited to venusd, which reads the picture straight into one and sends DECODED len 0 +
 *    surface: that buffer is emitted as it is (no lock, no copy) and the next DecodeFrame credits a fresh one before
 *    its FRAME (only that thread writes: the reader never blocks on a send). Held buffers are released when their
 *    credits end (venusd's EOS_DONE, ERROR, CLOSE reply, UNCREDIT reply, a lost connection, Invalidate); a buffer
 *    venusd hands back (UNCREDIT d->c: didn't fit) is released at once. A new pool (VT made another) gets UNCREDIT
 *    first, then fresh credits; the pool must be the picture's size first. -ENOTSUP back, or 2 x VTVENUS_CREDITS
 *    handed back without a picture through a surface: no credits for the rest of that venusd session. A DECODED with
 *    pixels (len > 0: venusd had no credit) is copied as before. 0: no credits, the socket path only, as before.
 * Log: /var/local/vtvenus.log (0666, cut past 1 MB) + os_log. Off: touch /var/local/venus-off (both this and venusd
 * refuse), or tools/sunfish-install-vtvenus.sh remove.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <fcntl.h>
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
#include "venus_hevc.h"

#define LOG_PATH	"/var/local/vtvenus.log"
#define LOG_MAX		(1 << 20)
#define OFF_FILE	"/var/local/venus-off"
#define ON_FILE		"/var/local/venus.on"	/* redfin (track V): software decoders unless it exists */
#define PTS_STEP	33333u		/* tag spacing in the Venus timestamp (venusc's) */
#define NSLOT		64
#define MAX_INFLIGHT	32
#define STALE		24		/* pending this many tags behind a returned picture: dropped */
#define IO_MS		2000		/* socket timeout slices; the in-flight limit's "none back" once decoding */
#define ENV_FILE	"/var/local/vtvenus-env"
#define GOP_MAX		256		/* vdcold: access units kept for a resync (libx264 keyint 250) */
#define GOP_BYTES	(16u << 20)
#define SYNC_WAIT_MAX	300		/* frames dropped waiting for an IDR/I frame before sending anyway */
#define MAX_FAILS	5
#define RX_MAX		(32u << 20)
#define LOG_ERR_MAX	50		/* error lines per instance */

/* VideoToolbox's decoder plug-in ABI (AVD.videodecoder's _kH264VideoDecoderVTable; stage 1 notes section 4) */
typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;
typedef struct OpaqueVTVideoDecoderSession *VTVideoDecoderSession;
typedef struct OpaqueVTVideoDecoderFrame *VTVideoDecoderFrame;

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
struct decoder_class {	/* VTVideoDecoderClass; VT reads +0x30/+0x38 only for version >= 2 */
	uint64_t version;	/* 3, as AVD */
	OSStatus (*start_session)(CMBaseObjectRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);
	OSStatus (*decode_frame)(CMBaseObjectRef, VTVideoDecoderFrame, CMSampleBufferRef, VTDecodeFrameFlags,
				 VTDecodeInfoFlags *);
	OSStatus (*copy_supported_property_dictionary)(CMBaseObjectRef, CFDictionaryRef *);
	void *set_properties, *copy_serializable_properties;
	Boolean (*can_accept_format_description)(CMBaseObjectRef, CMFormatDescriptionRef);
	OSStatus (*finish_delayed_frames)(CMBaseObjectRef);
	void *start_tile_session, *decode_tile;
};
struct vtable {
	const void *reserved;
	const struct base_class *base;
	const struct decoder_class *derived;
};

extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
extern void *CMBaseObjectGetDerivedStorage(CMBaseObjectRef);
extern CMBaseClassID VTVideoDecoderGetClassID(void);
extern OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode, CFDictionaryRef, void *);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession, CFDictionaryRef);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession, VTVideoDecoderFrame, OSStatus, VTDecodeInfoFlags,
						 CVImageBufferRef);
extern const CFStringRef kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder;	/* macOS-only in the SDK */

enum {	/* VTErrors.h */
	ERR_PROPERTY = -12900, ERR_PARAM = -12902, ERR_INVALID = -12903, ERR_NO_DECODER = -12906, ERR_BAD_DATA = -12909,
	ERR_FORMAT = -12910, ERR_MALFUNCTION = -12911, ERR_NOT_NOW = -12913,
};

struct slot { int used; uint64_t tag; VTVideoDecoderFrame frame; int no_out; };

/* vdcold switches (header) */
static struct {
	int cold_ms, wait_ms, lazy, reconnect, resync, recon_wait, err_gap_ms, hevc, revive_ms, surf, credits;
} cfg = { 20000, 10000, 1, 1, 1, 1, 5000, 1, 10000, 1, 6 };	/* surf, credits: vdcredit */
static pthread_once_t cfg_once = PTHREAD_ONCE_INIT;

static void cfg_set(const char *k, const char *val)
{
	int n = atoi(val);
	if (!strcmp(k, "VTVENUS_COLD_MS") && n > 0)
		cfg.cold_ms = n;
	else if (!strcmp(k, "VTVENUS_WAIT_MS") && n > 0)
		cfg.wait_ms = n;
	else if (!strcmp(k, "VTVENUS_LAZY_OPEN"))
		cfg.lazy = n;
	else if (!strcmp(k, "VTVENUS_RECONNECT"))
		cfg.reconnect = n;
	else if (!strcmp(k, "VTVENUS_RESYNC"))
		cfg.resync = n;
	else if (!strcmp(k, "VTVENUS_RECON_WAIT"))
		cfg.recon_wait = n;
	else if (!strcmp(k, "VTVENUS_ERR_GAP_MS") && n >= 0)
		cfg.err_gap_ms = n;
	else if (!strcmp(k, "VTVENUS_HEVC"))
		cfg.hevc = n;
	else if (!strcmp(k, "VTVENUS_REVIVE_MS") && n >= 0)
		cfg.revive_ms = n;
	else if (!strcmp(k, "VTVENUS_SURF"))	/* vdcredit */
		cfg.surf = n;
	else if (!strcmp(k, "VTVENUS_CREDITS") && n > 0)
		cfg.credits = n > VENUSD_MAX_CREDITS ? VENUSD_MAX_CREDITS : n;
}

static void cfg_load(void)
{
	static const char *const keys[] = { "VTVENUS_COLD_MS", "VTVENUS_WAIT_MS", "VTVENUS_LAZY_OPEN", "VTVENUS_RECONNECT",
					    "VTVENUS_RESYNC", "VTVENUS_RECON_WAIT", "VTVENUS_ERR_GAP_MS", "VTVENUS_HEVC",
					    "VTVENUS_REVIVE_MS", "VTVENUS_SURF", "VTVENUS_CREDITS" };
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
		if (getenv(keys[i]))
			cfg_set(keys[i], getenv(keys[i]));
	FILE *f = fopen(ENV_FILE, "r");
	char line[160];
	while (f && fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		cfg_set(line, eq + 1);
	}
	if (f)
		fclose(f);
}

/* everything the reader thread touches; refs: the decoder object + the reader */
struct vd {
	int refs;
	pthread_mutex_t mu, txmu;
	pthread_cond_t cv;
	int fd, stop, dead, invalid, emitting, reader_on, opened, sent_cfg, drained, reopen, fails, nerr;
	int hevc;		/* 4e: an hvc1 instance */
	int lost, parked, recon_used, just_recon;	/* vdcold: connection lost (reader parked), reconnect done */
	int got_pic, need_sync, sync_wait;		/* a picture in this venusd session; resync before the next frame */
	unsigned err_sess;				/* vdstop: the venusd session of the last error counted */
	double last_err;				/* and when (0: none yet) */
	int hung;					/* vdleak: given up, the connection shut down (hang_up) */
	double t_hung;
	pthread_t reader;
	VTVideoDecoderSession session;
	CMVideoFormatDescriptionRef fmt;
	uint8_t *ps;		/* SPS + PPS (HEVC: VPS + SPS + PPS), Annex B */
	size_t ps_len;
	int nal_len, w, h, pool_w, pool_h;
	uint64_t next_tag;
	struct slot slot[NSLOT];
	int inflight;
	int got_open, got_close, got_eos;
	int32_t open_status, eos_status;
	double last_rx;
	uint8_t *rx, *tx, *in;	/* reader's payload; DecodeFrame's FRAME and sample copy */
	size_t rx_cap, tx_cap, in_cap;
	unsigned n_in, n_out, n_drop, n_sess;
	unsigned nopay_sess;	/* vdnopay: the venusd session last logged (0: none) */
	/* vdcredit: the pool buffers whose surfaces venusd may fill, the pool they come from (retained), UNCREDIT
	 * answer due, off for this venusd session, handed back since the last surface picture, the last picture size */
	struct { CVPixelBufferRef pb; uint32_t id; } cr[VENUSD_MAX_CREDITS];
	int cr_n, cr_uncred, cr_off, cr_miss, pic_w, pic_h;
	CVPixelBufferPoolRef cr_pool;
	unsigned cr_logged, n_surf;	/* cr_logged: the venusd session whose paths were logged (bits in cr_bits) */
	int cr_bits;
	uint8_t *gop, *rp;	/* vdcold: the access units since the last IDR/I frame (HEVC: IRAP; no parameter sets); replay */
	size_t gop_len, gop_cap, rp_cap;
	uint32_t gop_off[GOP_MAX], gop_alen[GOP_MAX];
	uint64_t gop_tag[GOP_MAX];
	int gop_n, gop_ok;
};
struct storage { struct vd *v; };

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int process_logged;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char line[400], out[480];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	int n = snprintf(out, sizeof out, "vtvenus: %d %s %ld: %s\n", getpid(), getprogname(), (long)time(NULL), line);
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
			(void)ftruncate(fd, 0);
		(void)fchmod(fd, 0666);
		(void)write(fd, out, (size_t)n);
		close(fd);
	}
	pthread_mutex_unlock(&log_lock);
}

/* error lines: LOG_ERR_MAX per instance, then quiet */
#define ERRLOG(v, ...) do { if ((v)->nerr++ < LOG_ERR_MAX) say(__VA_ARGS__); } while (0)

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void wait_ms(struct vd *v, int ms)	/* v->mu held */
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	pthread_cond_timedwait_relative_np(&v->cv, &v->mu, &ts);
}

/* vdcold: partial transfers and EAGAIN (the 2 s SO_SNDTIMEO/SO_RCVTIMEO) retried until ms have passed */
static int wr_full(int fd, const void *b, size_t n, int ms)
{
	const uint8_t *p = b;
	double t0 = mono();
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && (mono() - t0) * 1e3 < ms) {
			struct pollfd q = { .fd = fd, .events = POLLOUT };
			poll(&q, 1, 100);
			continue;
		}
		if (k <= 0)
			return -1;
		p += k, n -= (size_t)k;
	}
	return 0;
}

static int rd_full(int fd, void *b, size_t n, int ms)
{
	uint8_t *p = b;
	double t0 = mono();
	while (n) {
		ssize_t k = read(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && (mono() - t0) * 1e3 < ms) {
			struct pollfd q = { .fd = fd, .events = POLLIN };
			poll(&q, 1, 100);
			continue;
		}
		if (k <= 0)
			return -1;
		p += k, n -= (size_t)k;
	}
	return 0;
}

/* vdcold: how long a send or an answer may take: the cold-start allowance until the venusd session's first picture */
static int io_ms(struct vd *v)
{
	return v->got_pic ? cfg.wait_ms : cfg.cold_ms;
}

static int grow(uint8_t **b, size_t *cap, size_t want)
{
	if (want <= *cap)
		return 0;
	uint8_t *n = realloc(*b, want);
	if (!n)
		return -1;
	*b = n, *cap = want;
	return 0;
}

static int send_msg(struct vd *v, int type, uint64_t pts, uint32_t seq, const void *payload, uint32_t len)
{
	struct venusd_msg m = { .magic = VENUSD_MAGIC, .type = (uint16_t)type, .version = VENUSD_VERSION, .len = len,
		.pts = pts, .seq = seq, .codec = type == VD_OPEN ? (v->hevc ? VD_CODEC_HEVC : VD_CODEC_H264) : 0,
		.width = type == VD_OPEN ? (uint32_t)v->w : 0, .height = type == VD_OPEN ? (uint32_t)v->h : 0 };
	int ms = io_ms(v);
	pthread_mutex_lock(&v->txmu);
	int r = v->fd < 0 || wr_full(v->fd, &m, sizeof m, ms) || (len && wr_full(v->fd, payload, len, ms)) ? -1 : 0;
	pthread_mutex_unlock(&v->txmu);
	return r;
}

static void vd_unref(struct vd *v)
{
	if (__atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL))
		return;
	if (v->fd >= 0)
		close(v->fd);
	if (v->fmt)
		CFRelease(v->fmt);
	for (int i = 0; i < v->cr_n; i++)	/* vdcredit: the reader is gone, so is the connection */
		CFRelease(v->cr[i].pb);
	if (v->cr_pool)
		CFRelease(v->cr_pool);
	free(v->ps);
	free(v->rx);
	free(v->tx);
	free(v->in);
	free(v->gop);
	free(v->rp);
	pthread_mutex_destroy(&v->mu);
	pthread_mutex_destroy(&v->txmu);
	pthread_cond_destroy(&v->cv);
	free(v);
}

/* ---- pending frames ---- */

/* takes (removes) pending frames into out[]: all (keep_from = UINT64_MAX), or those with tag + STALE < keep_from;
 * v->mu held. Returns how many; emitting is raised by one for the caller's emit_dropped. */
static int take(struct vd *v, uint64_t keep_from, int all, VTVideoDecoderFrame *out)
{
	int n = 0;
	for (int i = 0; i < NSLOT; i++) {
		struct slot *s = &v->slot[i];
		if (s->used && (all || s->tag + STALE < keep_from)) {
			out[n++] = s->frame;
			s->used = 0;
			v->inflight--;
		}
	}
	v->emitting++;
	return n;
}

static void emit_dropped(struct vd *v, VTVideoDecoderFrame *f, int n)	/* after take(); v->mu not held */
{
	if (!v->invalid)
		for (int i = 0; i < n; i++)
			VTDecoderSessionEmitDecodedFrame(v->session, f[i], noErr, kVTDecodeInfo_FrameDropped, NULL);
	pthread_mutex_lock(&v->mu);
	v->n_drop += (unsigned)n;
	v->emitting--;
	pthread_cond_broadcast(&v->cv);
	pthread_mutex_unlock(&v->mu);
}

static void drop_all(struct vd *v)
{
	VTVideoDecoderFrame f[NSLOT];
	pthread_mutex_lock(&v->mu);
	int n = take(v, 0, 1, f);
	pthread_mutex_unlock(&v->mu);
	emit_dropped(v, f, n);
}

/* ---- vdcredit: VT's pool buffers lent to venusd ---- */

/* the held buffer of surface id, out of the table (the caller gets its reference), or NULL; v->mu held */
static CVPixelBufferRef cr_take(struct vd *v, uint32_t id)
{
	for (int i = 0; i < v->cr_n; i++)
		if (v->cr[i].id == id) {
			CVPixelBufferRef pb = v->cr[i].pb;
			v->cr[i] = v->cr[--v->cr_n];
			return pb;
		}
	return NULL;
}

/* every held buffer released: their credits ended (venusd writes none of them from now on); v->mu not held */
static void cr_release(struct vd *v)
{
	CVPixelBufferRef pb[VENUSD_MAX_CREDITS];
	pthread_mutex_lock(&v->mu);
	int n = v->cr_n;
	for (int i = 0; i < n; i++)
		pb[i] = v->cr[i].pb;
	v->cr_n = 0;
	pthread_mutex_unlock(&v->mu);
	for (int i = 0; i < n; i++)
		CFRelease(pb[i]);
}

/* the paths of this venusd session, one line each: bit 1 surface, 2 socket copy, 4 credits off; v->mu not held */
static void cr_log(struct vd *v, int bit, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cr_log(struct vd *v, int bit, const char *fmt, ...)
{
	char b[200];
	pthread_mutex_lock(&v->mu);
	if (v->cr_logged != v->n_sess)
		v->cr_logged = v->n_sess, v->cr_bits = 0;
	int now = !(v->cr_bits & bit);
	v->cr_bits |= bit;
	pthread_mutex_unlock(&v->mu);
	if (!now)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	say("venusd session %u: %s", v->n_sess, b);
}

static int send_credit(struct vd *v, int type, uint32_t id, uint32_t w, uint32_t h, uint32_t bpr)
{
	struct venusd_msg m = { .magic = VENUSD_MAGIC, .type = (uint16_t)type, .version = VENUSD_VERSION, .surface = id,
		.width = w, .height = h, .stride = bpr };
	int ms = io_ms(v);
	pthread_mutex_lock(&v->txmu);
	int r = v->fd < 0 || wr_full(v->fd, &m, sizeof m, ms) ? -1 : 0;
	pthread_mutex_unlock(&v->txmu);
	return r;
}

static int cr_usable(struct vd *v)	/* credits may go out now; v->mu held */
{
	return cfg.surf && v->opened && !v->dead && !v->lost && !v->invalid && !v->drained && !v->reopen && !v->cr_off &&
	       !v->cr_uncred && v->session;
}

static void send_broke(struct vd *v);

/* DecodeFrame's thread, before a FRAME: credits up to VTVENUS_CREDITS from VT's pool (once the pool has the picture's
 * size); VT made a new pool: UNCREDIT first (the reader releases the old ones at its answer) */
static void cr_topup(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	CVPixelBufferPoolRef pool = cr_usable(v) ? VTDecoderSessionGetPixelBufferPool(v->session) : NULL;
	int sized = !v->pic_w || (v->pic_w == v->pool_w && v->pic_h == v->pool_h);
	int uncred = pool && v->cr_n && v->cr_pool && pool != v->cr_pool;
	int want = pool && sized && !uncred ? cfg.credits - v->cr_n : 0;
	CVPixelBufferPoolRef old = NULL;
	if (uncred)
		v->cr_uncred = 1;
	else if (want > 0 && pool != v->cr_pool)
		old = v->cr_pool, v->cr_pool = (CVPixelBufferPoolRef)CFRetain(pool);
	pthread_mutex_unlock(&v->mu);
	if (old)
		CFRelease(old);
	if (uncred) {
		say("venusd session %u: VT's pixel buffer pool changed: UNCREDIT, fresh credits after the answer", v->n_sess);
		if (send_credit(v, VD_UNCREDIT, 0, 0, 0, 0))
			send_broke(v);
		return;
	}
	for (int k = 0; k < want; k++) {
		CVPixelBufferRef pb = NULL;
		IOSurfaceRef ios = NULL;
		CVReturn r = CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
		if (!r && pb)
			ios = CVPixelBufferGetIOSurface(pb);
		uint32_t id = ios ? IOSurfaceGetID(ios) : 0;
		if (!id) {
			if (pb)
				CFRelease(pb);
			pthread_mutex_lock(&v->mu);
			v->cr_off = 1;
			pthread_mutex_unlock(&v->mu);
			cr_log(v, 4, "no IOSurface-backed buffer from VT's pool (CVReturn %d, buffer %p): no credits", r, (void *)pb);
			return;
		}
		pthread_mutex_lock(&v->mu);
		int ok = cr_usable(v) && v->cr_n < VENUSD_MAX_CREDITS;
		if (ok)
			v->cr[v->cr_n].pb = pb, v->cr[v->cr_n++].id = id;	/* held before venusd can use it */
		pthread_mutex_unlock(&v->mu);
		if (!ok) {
			CFRelease(pb);
			return;
		}
		if (send_credit(v, VD_CREDIT, id, (uint32_t)CVPixelBufferGetWidth(pb), (uint32_t)CVPixelBufferGetHeight(pb),
				(uint32_t)CVPixelBufferGetBytesPerRowOfPlane(pb, 0))) {
			ERRLOG(v, "CREDIT: send failed (%s)%s", strerror(errno), cfg.reconnect ? ": reconnect" : "");
			send_broke(v);	/* the reader releases them with the lost connection */
			return;
		}
	}
}

/* UNCREDIT from venusd: one credit handed back (surface != 0), or the answer to ours (0: all of them ended) */
static void cr_returned(struct vd *v, const struct venusd_msg *m)
{
	if (!m->surface) {
		cr_release(v);
		pthread_mutex_lock(&v->mu);
		v->cr_uncred = 0;
		pthread_mutex_unlock(&v->mu);
		return;
	}
	pthread_mutex_lock(&v->mu);
	CVPixelBufferRef pb = cr_take(v, m->surface);
	if (m->width)
		v->pic_w = (int)m->width, v->pic_h = (int)m->height;
	int off = m->status == -ENOTSUP || ++v->cr_miss >= 2 * cfg.credits;
	int was = v->cr_off;
	v->cr_off |= off;
	pthread_mutex_unlock(&v->mu);
	if (pb)
		CFRelease(pb);
	if (off && !was)
		cr_log(v, 4, "credit IOSurface %u handed back (%d%s): no credits for the rest of this session", m->surface,
		       m->status, m->status == -ENOTSUP ? ", venusd's credit path is off" : ", too many in a row");
}

/* ---- the pool and the copy ---- */

static CFDictionaryRef pb_attrs(int w, int h)
{
	int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, align = 64;
	CFNumberRef nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt), nw = CFNumberCreate(NULL, kCFNumberIntType, &w),
		    nh = CFNumberCreate(NULL, kCFNumberIntType, &h), na = CFNumberCreate(NULL, kCFNumberIntType, &align);
	CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
						 &kCFTypeDictionaryValueCallBacks);
	const void *k[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey,
			    kCVPixelBufferBytesPerRowAlignmentKey, kCVPixelBufferIOSurfacePropertiesKey };
	const void *val[] = { nf, nw, nh, na, ios };
	CFDictionaryRef d = nf && nw && nh && na && ios ? CFDictionaryCreate(NULL, k, val, 5, &kCFTypeDictionaryKeyCallBacks,
								&kCFTypeDictionaryValueCallBacks) : NULL;
	const void *all[] = { nf, nw, nh, na, ios };
	for (int i = 0; i < 5; i++)
		if (all[i])
			CFRelease(all[i]);
	return d;
}

static int set_pool(struct vd *v, int w, int h)
{
	CFDictionaryRef a = pb_attrs(w, h);
	OSStatus st = a ? VTDecoderSessionSetPixelBufferAttributes(v->session, a) : ERR_MALFUNCTION;
	if (a)
		CFRelease(a);
	if (st) {
		ERRLOG(v, "pixel buffer attributes %dx%d: OSStatus %d", w, h, (int)st);
		return -1;
	}
	v->pool_w = w, v->pool_h = h;
	return 0;
}

/* DECODED payload (Venus OUTPUT2: Y stride * scanlines, then CbCr stride * scanlines / 2) -> a pool buffer.
 * vdcredit: surface != 0: spb, the held buffer venusd wrote (its reference passes to the caller) */
static CVPixelBufferRef picture(struct vd *v, const struct venusd_msg *m, CVPixelBufferRef spb)
{
	size_t ylen = (size_t)m->stride * m->scanlines;
	if (m->surface) {
		if (!spb) {
			ERRLOG(v, "frame %u: written into IOSurface %u, which isn't one of ours: dropped", m->seq, m->surface);
			return NULL;
		}
		if (((int)m->width != v->pool_w || (int)m->height != v->pool_h)) {	/* the pool follows the picture */
			say("picture size %ux%u, pool %dx%d: new pool attributes", m->width, m->height, v->pool_w, v->pool_h);
			set_pool(v, (int)m->width, (int)m->height);
		}
		cr_log(v, 1, "frames through credited IOSurfaces (%d credits): emitted without a copy", cfg.credits);
		return spb;
	}
	if (cfg.surf)
		cr_log(v, 2, "frame %u copied from the socket (venusd had no credit)", m->seq);
	int nopay = !m->len;	/* vdnopay: venusd sent no pixels */
	if (nopay && v->nopay_sess != v->n_sess) {
		v->nopay_sess = v->n_sess;
		say("venusd session %u: nopayload test: frames emitted unfilled", v->n_sess);
	}
	if (!m->width || !m->height || (!nopay && m->len < ylen + ylen / 2) || m->crop_x + m->width > m->stride ||
	    m->crop_y + m->height > m->scanlines) {
		ERRLOG(v, "frame %u: bad layout (len %u, stride %u, scanlines %u, %ux%u at %u,%u)", m->seq, m->len, m->stride,
		       m->scanlines, m->width, m->height, m->crop_x, m->crop_y);
		return NULL;
	}
	if (((int)m->width != v->pool_w || (int)m->height != v->pool_h)) {
		say("picture size %ux%u, pool %dx%d: new pool attributes", m->width, m->height, v->pool_w, v->pool_h);
		if (set_pool(v, (int)m->width, (int)m->height))
			return NULL;
	}
	CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(v->session);
	CVPixelBufferRef pb = NULL;
	CVReturn r = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) : kCVReturnInvalidArgument;
	if (r || !pb) {
		ERRLOG(v, "frame %u: no pixel buffer from VT's pool (pool %p, CVReturn %d)", m->seq, (void *)pool, r);
		return NULL;
	}
	if (CVPixelBufferGetPlaneCount(pb) != 2 || CVPixelBufferLockBaseAddress(pb, 0)) {
		ERRLOG(v, "frame %u: pool buffer not a lockable 2-plane buffer", m->seq);
		CFRelease(pb);
		return NULL;
	}
	uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
	size_t ybpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cbpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
	size_t w = m->width, h = m->height, pw = CVPixelBufferGetWidthOfPlane(pb, 0), ph = CVPixelBufferGetHeightOfPlane(pb, 0);
	size_t ch = CVPixelBufferGetHeightOfPlane(pb, 1);
	if (w > pw)
		w = pw;
	if (w > ybpr)
		w = ybpr;
	if (h > ph)
		h = ph;
	size_t cw = (w + 1) & ~(size_t)1, crows = (h + 1) / 2;
	if (cw > cbpr)
		cw = cbpr;
	if (crows > ch)
		crows = ch;
	const uint8_t *src = v->rx;
	if (y && c && !nopay) {
		for (size_t r0 = 0; r0 < h; r0++)
			memcpy(y + r0 * ybpr, src + (size_t)(r0 + m->crop_y) * m->stride + m->crop_x, w);
		for (size_t r0 = 0; r0 < crows; r0++)
			memcpy(c + r0 * cbpr, src + ylen + (size_t)(r0 + m->crop_y / 2) * m->stride + (m->crop_x & ~1u), cw);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	if (!y || !c) {
		ERRLOG(v, "frame %u: pool buffer without plane addresses", m->seq);
		CFRelease(pb);
		return NULL;
	}
	return pb;
}

/* one DECODED: frame[tag] gets the picture; pending far behind it are dropped */
static void decoded(struct vd *v, const struct venusd_msg *m)
{
	VTVideoDecoderFrame stale[NSLOT], frame = NULL;
	int no_out = 0;
	uint64_t tag = m->pts / PTS_STEP;
	pthread_mutex_lock(&v->mu);
	v->got_pic = 1, v->recon_used = 0;	/* vdcold: Venus decodes in this session */
	CVPixelBufferRef spb = m->surface ? cr_take(v, m->surface) : NULL;	/* vdcredit: ours now, venusd is done */
	if (spb)
		v->cr_miss = 0, v->n_surf++;
	v->pic_w = (int)m->width, v->pic_h = (int)m->height;
	if (m->pts % PTS_STEP == 1) {	/* a resync access unit's picture: shown before already */
		pthread_mutex_unlock(&v->mu);
		if (spb)
			CFRelease(spb);
		return;
	}
	struct slot *s = &v->slot[tag % NSLOT];
	if (m->pts % PTS_STEP == 0 && s->used && s->tag == tag) {
		frame = s->frame, no_out = s->no_out;
		s->used = 0;
		v->inflight--;
	}
	int n = take(v, tag, 0, stale);
	pthread_mutex_unlock(&v->mu);
	if (n)
		ERRLOG(v, "%d frame(s) more than %d behind tag %llu never came back: dropped", n, STALE, (unsigned long long)tag);
	emit_dropped(v, stale, n);
	if (!frame) {
		ERRLOG(v, "DECODED pts %llu (seq %u): no pending frame for it", (unsigned long long)m->pts, m->seq);
		if (spb)
			CFRelease(spb);
		return;
	}
	pthread_mutex_lock(&v->mu);
	v->emitting++;
	pthread_mutex_unlock(&v->mu);
	CVPixelBufferRef pb = no_out || v->invalid ? NULL : picture(v, m, spb);
	if (spb && (no_out || v->invalid))
		CFRelease(spb);
	if (!v->invalid) {
		if (pb)
			VTDecoderSessionEmitDecodedFrame(v->session, frame, noErr, 0, pb);
		else
			VTDecoderSessionEmitDecodedFrame(v->session, frame, noErr, kVTDecodeInfo_FrameDropped, NULL);
	}
	if (pb)
		CFRelease(pb);
	pthread_mutex_lock(&v->mu);
	if (pb)
		v->n_out++, v->fails = 0;	/* decoding again: the reopen count starts over */
	else
		v->n_drop++;
	v->emitting--;
	pthread_cond_broadcast(&v->cv);
	pthread_mutex_unlock(&v->mu);
}

static void hang_up(struct vd *v);

static void *reader(void *arg)
{
	struct vd *v = arg;
	for (;;) {
		pthread_mutex_lock(&v->mu);
		int stop = v->stop, fd = v->fd, ms = io_ms(v);
		pthread_mutex_unlock(&v->mu);
		if (stop)
			break;
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r = poll(&p, 1, 250);
		if (r == 0 || (r < 0 && errno == EINTR))
			continue;
		struct venusd_msg m = { 0 };
		if (r < 0 || rd_full(fd, &m, sizeof m, ms) || m.magic != VENUSD_MAGIC || m.len > RX_MAX ||
		    grow(&v->rx, &v->rx_cap, m.len) || (m.len && rd_full(fd, v->rx, m.len, ms))) {
			pthread_mutex_lock(&v->mu);
			stop = v->stop;
			pthread_mutex_unlock(&v->mu);
			pthread_mutex_lock(&v->mu);
			int dead = v->dead;
			pthread_mutex_unlock(&v->mu);
			if (stop)
				break;
			if (dead) {	/* vdleak: hang_up shut it down: park (a revive reconnects) */
				cr_release(v);	/* vdcredit */
				pthread_mutex_lock(&v->mu);
				v->lost = 1, v->parked = 1;
				pthread_cond_broadcast(&v->cv);
				while (v->lost && !v->stop)
					wait_ms(v, 100);
				v->parked = 0;
				pthread_mutex_unlock(&v->mu);
				continue;
			}
			ERRLOG(v, "venusd connection lost or bad message (magic %#x, len %u): %s", m.magic, m.len,
			       cfg.reconnect ? "pending dropped, reconnect at the next frame" : "decoder unusable");
			if (!cfg.reconnect)
				break;
			/* vdcold: park until DecodeFrame has a new connection (reconnect) or the instance ends */
			pthread_mutex_lock(&v->mu);
			v->lost = 1;
			pthread_cond_broadcast(&v->cv);
			pthread_mutex_unlock(&v->mu);
			drop_all(v);
			cr_release(v);	/* vdcredit: the credits went with the connection */
			pthread_mutex_lock(&v->mu);
			v->parked = 1;
			pthread_cond_broadcast(&v->cv);
			while (v->lost && !v->stop)
				wait_ms(v, 100);
			v->parked = 0;
			pthread_mutex_unlock(&v->mu);
			continue;
		}
		pthread_mutex_lock(&v->mu);
		v->last_rx = mono();
		pthread_mutex_unlock(&v->mu);
		switch (m.type) {
		case VD_DECODED:
			decoded(v, &m);
			break;
		case VD_ERROR:
			if (m.status == -E2BIG) {
				VTVideoDecoderFrame f[1];
				int n = 0;
				pthread_mutex_lock(&v->mu);
				struct slot *s = &v->slot[m.seq % NSLOT];
				if (s->used && (uint32_t)s->tag == m.seq)
					f[n++] = s->frame, s->used = 0, v->inflight--;
				v->emitting++;
				pthread_mutex_unlock(&v->mu);
				ERRLOG(v, "frame %u longer than venusd's input buffers: dropped", m.seq);
				emit_dropped(v, f, n);
			} else {
				/* vdstop: one error per venusd session counts (venusd answers every FRAME after it with one
				 * too); a second failed session within VTVENUS_ERR_GAP_MS: give up */
				double t = mono();
				pthread_mutex_lock(&v->mu);
				int counted = v->last_err > 0 && v->err_sess == v->n_sess;
				int quit = !counted && cfg.err_gap_ms > 0 && v->last_err > 0 &&
					   (t - v->last_err) * 1e3 < cfg.err_gap_ms;
				if (!counted)
					v->err_sess = v->n_sess, v->last_err = t;
				if (quit)
					v->dead = 1;
				pthread_mutex_unlock(&v->mu);
				if (quit)
					ERRLOG(v, "venusd session error %d (%#x) at frame %u, again within %d ms: giving up"
					       " (errors from now on)", m.status, (unsigned)m.status, m.seq, cfg.err_gap_ms);
				if (quit)
					hang_up(v);	/* vdleak */
				else if (!counted)
					ERRLOG(v, "venusd session error %d (%#x) at frame %u: pending dropped, reopen at the"
					       " next frame", m.status, (unsigned)m.status, m.seq);
				pthread_mutex_lock(&v->mu);
				v->reopen = 1;
				v->got_eos = 1, v->eos_status = m.status;	/* an EOS waiter ends too */
				pthread_mutex_unlock(&v->mu);
				drop_all(v);
				cr_release(v);	/* vdcredit: venusd dropped them before the ERROR */
			}
			break;
		case VD_OPEN:
			pthread_mutex_lock(&v->mu);
			v->got_open = 1, v->open_status = m.status;
			pthread_cond_broadcast(&v->cv);
			pthread_mutex_unlock(&v->mu);
			break;
		case VD_EOS_DONE:
			cr_release(v);	/* vdcredit: they end with EOS_DONE */
			pthread_mutex_lock(&v->mu);
			v->got_eos = 1, v->eos_status = m.status;
			pthread_cond_broadcast(&v->cv);
			pthread_mutex_unlock(&v->mu);
			break;
		case VD_CLOSE:
			cr_release(v);	/* vdcredit */
			pthread_mutex_lock(&v->mu);
			v->got_close = 1;
			pthread_cond_broadcast(&v->cv);
			pthread_mutex_unlock(&v->mu);
			break;
		case VD_UNCREDIT:	/* vdcredit */
			cr_returned(v, &m);
			break;
		default:
			ERRLOG(v, "unexpected venusd message type %u", m.type);
		}
	}
	pthread_mutex_lock(&v->mu);
	v->dead = 1;
	v->reader_on = 0;
	pthread_cond_broadcast(&v->cv);
	pthread_mutex_unlock(&v->mu);
	drop_all(v);
	cr_release(v);	/* vdcredit */
	vd_unref(v);
	return NULL;
}

/* ---- venusd session ---- */

/* waits (v->mu held) until *flag, the reader's end, or ms without any message from venusd */
static int wait_reply(struct vd *v, int *flag, int ms)
{
	double t0 = mono();
	while (!*flag && !v->dead && !v->lost) {
		double since = mono() - (v->last_rx > t0 ? v->last_rx : t0);
		if (since * 1e3 >= ms)
			return -1;
		wait_ms(v, 100);
	}
	return *flag ? 0 : -1;
}

/* vdcold: the stream is broken (a send past its deadline or partial, an answer missing): a new connection at the
 * next try (lost), or none (dead: VTVENUS_RECONNECT=0, or dead already); v->mu held */
static void mark_broken(struct vd *v)
{
	v->dead = v->dead || !cfg.reconnect;
	v->lost = !v->dead;
	pthread_cond_broadcast(&v->cv);
}

static void send_broke(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	mark_broken(v);
	pthread_mutex_unlock(&v->mu);
}

/* vdleak: a given-up instance (dead) lets go of its venusd connection now, not at the last release (its host may
 * keep it for long): shutdown, so venusd sees EOF and frees the slot; the fd itself goes at Invalidate/release, the
 * reader sees the end and parks. Once per give-up; v->mu not held */
static void hang_up(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	int now = v->dead && !v->hung;
	if (now)
		v->hung = 1, v->t_hung = mono();
	pthread_mutex_unlock(&v->mu);
	if (!now)
		return;
	pthread_mutex_lock(&v->txmu);
	if (v->fd >= 0)
		shutdown(v->fd, SHUT_RDWR);
	pthread_mutex_unlock(&v->txmu);
	say("given up: venusd connection closed (%u frames in, %u out, %u dropped, %u venusd session(s))%s", v->n_in,
	    v->n_out, v->n_drop, v->n_sess, cfg.reconnect && cfg.revive_ms ? ", a fresh try after VTVENUS_REVIVE_MS" : "");
}

static int venus_open(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	v->got_open = 0;
	pthread_mutex_unlock(&v->mu);
	double t0 = mono();
	v->got_pic = 0;	/* the cold allowance for OPEN's send too */
	if (send_msg(v, VD_OPEN, 0, 0, NULL, 0)) {
		ERRLOG(v, "OPEN: send failed (%s)%s", strerror(errno), cfg.reconnect ? ": reconnect" : "");
		send_broke(v);
		return ERR_MALFUNCTION;
	}
	pthread_mutex_lock(&v->mu);
	int r = wait_reply(v, &v->got_open, cfg.cold_ms);
	int32_t st = v->open_status;
	if (r)	/* an OPEN answered later would leave the state unknown: a new connection (vdcold) or none */
		mark_broken(v);
	else if (!st)
		v->opened = 1, v->sent_cfg = 0, v->drained = 0, v->reopen = 0, v->got_eos = 0, v->n_sess++,
		v->need_sync = 1, v->sync_wait = 0, v->cr_off = 0, v->cr_miss = 0, v->cr_uncred = 0;	/* vdcredit */
	pthread_mutex_unlock(&v->mu);
	if (r) {
		ERRLOG(v, "OPEN: no answer from venusd in %d ms", cfg.cold_ms);
		return ERR_MALFUNCTION;
	}
	if (st) {
		ERRLOG(v, "OPEN refused by venusd: %d (%s)", st, st < 0 ? strerror(-st) : "firmware");
		return st == -EBUSY ? ERR_NOT_NOW : ERR_MALFUNCTION;
	}
	say("venusd session %u open for %dx%d in %.0f ms", v->n_sess, v->w, v->h, 1e3 * (mono() - t0));
	return 0;
}

static void venus_close(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	int opened = v->opened && !v->dead && !v->lost;
	v->got_close = 0;
	v->opened = 0;
	pthread_mutex_unlock(&v->mu);
	if (!opened) {
		cr_release(v);	/* vdcredit: no session or no connection: no credits */
		return;
	}
	if (send_msg(v, VD_CLOSE, 0, 0, NULL, 0)) {
		ERRLOG(v, "CLOSE: send failed (%s)", strerror(errno));
		send_broke(v);	/* half a message on the wire */
		cr_release(v);
		return;
	}
	pthread_mutex_lock(&v->mu);
	int ms = cfg.wait_ms;
	if (wait_reply(v, &v->got_close, ms))
		ERRLOG(v, "CLOSE: no answer from venusd in %d ms", ms);
	pthread_mutex_unlock(&v->mu);
	cr_release(v);	/* vdcredit: the reader did at the answer; after a timeout too (worst case one garbled frame) */
}

static int venusd_connect(void);

/* vdcold: a new connection after a loss, once until a picture comes back; the reader parks meanwhile. 0 = ok */
static int reconnect(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	int again = v->recon_used, off = !cfg.reconnect || v->stop || v->invalid;
	if (again || off)
		v->dead = 1;
	else
		v->recon_used = 1, v->lost = 1;
	pthread_mutex_unlock(&v->mu);
	if (again || off) {
		ERRLOG(v, "venusd connection lost again before a picture came back: giving up (errors from now on)");
		return -1;
	}
	pthread_mutex_lock(&v->txmu);
	int old = v->fd;
	pthread_mutex_unlock(&v->txmu);
	if (old >= 0)
		shutdown(old, SHUT_RDWR);	/* the reader sees the end and parks */
	pthread_mutex_lock(&v->mu);
	for (double t0 = mono(); v->reader_on && !v->parked && mono() - t0 < cfg.wait_ms / 1e3;)
		wait_ms(v, 50);
	int ok = !v->reader_on || v->parked;
	if (!ok)
		v->dead = 1;
	pthread_mutex_unlock(&v->mu);
	if (!ok) {
		ERRLOG(v, "reconnect: the reader thread did not stop: giving up");
		return -1;
	}
	int fd = venusd_connect(), e = errno, tries = 1;
	/* vdstop: venusd restarting (no socket, nobody listening): every 250 ms up to VTVENUS_COLD_MS */
	for (double t0 = mono(); fd < 0 && cfg.recon_wait && (e == ECONNREFUSED || e == ENOENT) &&
	     mono() - t0 < cfg.cold_ms / 1e3;) {
		pthread_mutex_lock(&v->mu);
		int stop = v->stop || v->invalid;
		pthread_mutex_unlock(&v->mu);
		if (stop)
			break;
		usleep(250000);
		fd = venusd_connect(), e = errno, tries++;
	}
	if (fd >= 0 && tries > 1)
		say("reconnect: venusd answered after %d tries (250 ms apart)", tries);
	pthread_mutex_lock(&v->txmu);
	if (old >= 0)
		close(old);
	v->fd = fd;
	pthread_mutex_unlock(&v->txmu);
	pthread_mutex_lock(&v->mu);
	if (fd < 0)
		v->dead = 1;
	else
		v->lost = 0, v->opened = 0, v->drained = 0, v->reopen = 0, v->just_recon = 1;
	pthread_cond_broadcast(&v->cv);
	pthread_mutex_unlock(&v->mu);
	if (fd < 0) {
		ERRLOG(v, "reconnect to venusd failed (%s): giving up (errors from now on)", strerror(e));
		return -1;
	}
	say("reconnected to venusd: a fresh session at the next frame");
	return 0;
}

/* ---- the decoder's functions ---- */

static struct vd *vd_of(CMBaseObjectRef o)
{
	struct storage *s = o ? CMBaseObjectGetDerivedStorage(o) : NULL;
	return s ? s->v : NULL;
}

static OSStatus dec_invalidate(CMBaseObjectRef o)
{
	struct vd *v = vd_of(o);
	if (!v)
		return 0;
	pthread_mutex_lock(&v->mu);
	int was = v->invalid;
	pthread_mutex_unlock(&v->mu);
	if (was)
		return 0;
	venus_close(v);
	drop_all(v);
	pthread_mutex_lock(&v->mu);
	int self = v->reader_on && pthread_equal(pthread_self(), v->reader);
	int join = v->reader_on && !self;
	v->invalid = 1, v->stop = 1;
	for (double t0 = mono(); v->emitting > (self ? 1 : 0) && mono() - t0 < 2;)
		wait_ms(v, 50);
	pthread_mutex_unlock(&v->mu);
	if (v->fd >= 0)
		shutdown(v->fd, SHUT_RDWR);
	if (join)
		pthread_join(v->reader, NULL);
	else if (self)
		pthread_detach(v->reader);
	if (!self) {	/* vdleak: the connection goes now, not at the last release (no reader left to use it) */
		pthread_mutex_lock(&v->txmu);
		if (v->fd >= 0)
			close(v->fd);
		v->fd = -1;
		pthread_mutex_unlock(&v->txmu);
	}
	cr_release(v);	/* vdcredit */
	say("invalidated: %u frames in, %u out (%u through credited surfaces), %u dropped, %u venusd session(s)", v->n_in,
	    v->n_out, v->n_surf, v->n_drop, v->n_sess);
	return 0;
}

static void dec_finalize(CMBaseObjectRef o)
{
	struct storage *s = o ? CMBaseObjectGetDerivedStorage(o) : NULL;
	if (!s || !s->v)
		return;
	dec_invalidate(o);
	vd_unref(s->v);
	s->v = NULL;
}

static CFStringRef dec_description(CMBaseObjectRef o)
{
	struct vd *v = vd_of(o);
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<vtvenus %p: Venus %s via venusd, %u in, %u out, %u dropped>"),
					(void *)o, v && v->hevc ? "HEVC" : "H.264", v ? v->n_in : 0, v ? v->n_out : 0,
					v ? v->n_drop : 0);
}

static OSStatus dec_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef a, void *out)
{
	if (out && key && CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
		*(CFTypeRef *)out = CFRetain(kCFBooleanTrue);
		return 0;
	}
	return ERR_PROPERTY;
}

static OSStatus dec_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	return ERR_PROPERTY;
}

static OSStatus dec_supported(CMBaseObjectRef o, CFDictionaryRef *out)
{
	if (!out)
		return ERR_PARAM;
	const void *pk[] = { kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey };
	const void *pv[] = { kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly };
	CFDictionaryRef p = CFDictionaryCreate(NULL, pk, pv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *k[] = { kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder };
	const void *val[] = { p };
	*out = p ? CFDictionaryCreate(NULL, k, val, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
	if (p)
		CFRelease(p);
	return *out ? 0 : ERR_MALFUNCTION;
}

/* the format's SPS/PPS (4e hvc1: VPS/SPS/PPS from hvcC) as Annex B, its NAL length size and size; 0 = usable */
typedef OSStatus (*getps_fn)(CMFormatDescriptionRef, size_t, const uint8_t **, size_t *, size_t *, int *);
static OSStatus parse_format(struct vd *v, CMFormatDescriptionRef fmt, uint8_t **ps, size_t *ps_len, int *nal_len,
			     int *w, int *h)
{
	size_t count = 0;
	int nl = 0;
	FourCharCode want = v->hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
	getps_fn getps = v->hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex :
				   CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
	if (!fmt || CMFormatDescriptionGetMediaSubType(fmt) != want || getps(fmt, 0, NULL, NULL, &count, &nl) || !count ||
	    (nl != 1 && nl != 2 && nl != 4)) {
		ERRLOG(v, "format: not %s with parameter sets (count %zu, NAL length size %d)", v->hevc ? "hvc1" : "avc1",
		       count, nl);
		return ERR_FORMAT;
	}
	size_t total = 0;
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = NULL;
		size_t n = 0;
		if (getps(fmt, i, &p, &n, NULL, NULL) || !p || !n)
			return ERR_FORMAT;
		total += 4 + n;
	}
	uint8_t *b = malloc(total), *q = b;
	if (!b)
		return ERR_MALFUNCTION;
	int profile = -1, main8 = !v->hevc;
	struct hevc_sps hs = { 0 };
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = NULL;
		size_t n = 0;
		getps(fmt, i, &p, &n, NULL, NULL);
		if (!v->hevc && (p[0] & 0x1f) == 7 && n > 1 && profile < 0)
			profile = p[1];
		if (v->hevc && ((p[0] >> 1) & 0x3f) == 33 && profile < 0 && !hevc_sps_parse(p, n, &hs))	/* 4e */
			profile = hs.profile, main8 = hevc_main8(&hs);
		memcpy(q, "\0\0\0\1", 4);
		memcpy(q + 4, p, n);
		q += 4 + n;
	}
	CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fmt);
	int big = d.width > d.height ? d.width : d.height, small = d.width > d.height ? d.height : d.width;
	/* 8-bit 4:2:0 only (High 10 110, High 4:2:2 122, High 4:4:4 244; HEVC: Main / Main Still Picture, an SPS that
	 * says 4:2:0 8-bit, none parsed = refused), up to 1920x1088 */
	if (profile == 110 || profile == 122 || profile == 244 || !main8 || d.width <= 0 || d.height <= 0 || big > 1920 ||
	    small > 1088) {
		if (v->hevc)
			ERRLOG(v, "format: hvc1 profile %d (chroma format %d, %d/%d bit), %dx%d: not for Venus here", profile,
			       hs.chroma, hs.depth_y, hs.depth_c, d.width, d.height);
		else
			ERRLOG(v, "format: profile %d, %dx%d: not for Venus here", profile, d.width, d.height);
		free(b);
		return ERR_FORMAT;
	}
	*ps = b, *ps_len = total, *nal_len = nl, *w = d.width, *h = d.height;
	return 0;
}

static OSStatus dec_start_session(CMBaseObjectRef o, VTVideoDecoderSession session, CMVideoFormatDescriptionRef fmt)
{
	struct vd *v = vd_of(o);
	if (!v || !session)
		return ERR_PARAM;
	if (v->session || v->invalid || v->dead)
		return ERR_INVALID;
	OSStatus st = parse_format(v, fmt, &v->ps, &v->ps_len, &v->nal_len, &v->w, &v->h);
	if (st)
		return st;
	v->fmt = CFRetain(fmt);
	v->session = session;
	if (set_pool(v, v->w, v->h))
		return ERR_MALFUNCTION;
	__atomic_add_fetch(&v->refs, 1, __ATOMIC_ACQ_REL);
	v->reader_on = 1;
	if (pthread_create(&v->reader, NULL, reader, v)) {
		v->reader_on = 0;
		__atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL);
		ERRLOG(v, "no reader thread");
		return ERR_MALFUNCTION;
	}
	st = cfg.lazy ? 0 : venus_open(v);	/* vdcold: lazy, the first DecodeFrame OPENs */
	if (!st)
		say("session started: %s %dx%d, NAL length %d, parameter sets %zu B%s", v->hevc ? "hvc1" : "avc1", v->w, v->h,
		    v->nal_len, v->ps_len, cfg.lazy ? ", venusd OPEN at the first frame" : "");
	return st;
}

static Boolean dec_can_accept(CMBaseObjectRef o, CMFormatDescriptionRef fmt)
{
	struct vd *v = vd_of(o);
	uint8_t *ps = NULL;
	size_t ps_len = 0;
	int nl = 0, w = 0, h = 0;
	if (!v || v->invalid || parse_format(v, fmt, &ps, &ps_len, &nl, &w, &h))
		return false;
	if (w != v->w || h != v->h) {	/* a new size: VT makes a new instance */
		free(ps);
		return false;
	}
	/* same size, maybe new parameter sets: they go before the next frame */
	if (ps_len != v->ps_len || memcmp(ps, v->ps, ps_len))	/* vdcold: the kept GOP went with the old sets */
		v->gop_ok = 0, v->gop_n = 0, v->gop_len = 0;
	free(v->ps);
	v->ps = ps, v->ps_len = ps_len, v->nal_len = nl;
	v->sent_cfg = 0;
	if (v->fmt)
		CFRelease(v->fmt);
	v->fmt = CFRetain(fmt);
	return true;
}

/* vdcold: a NAL a fresh decoder can start on: IDR, or a slice with slice_type I/SI (first_mb_in_slice, slice_type:
 * the first two ue(v) after the header; emulation prevention bytes skipped) */
static int sync_nal(const uint8_t *p, size_t n)
{
	int type = n ? p[0] & 0x1f : 0;
	if (type == 5)
		return 1;
	if (type != 1)
		return 0;
	uint8_t b[16];
	size_t nb = 0;
	for (size_t i = 1, z = 0; i < n && nb < sizeof b; i++) {
		if (z >= 2 && p[i] == 3) {
			z = 0;
			continue;
		}
		z = p[i] ? 0 : z + 1;
		b[nb++] = p[i];
	}
	size_t bit = 0;
	uint32_t val[2];
	for (int k = 0; k < 2; k++) {
		int lz = 0;
		while (bit < nb * 8 && !(b[bit / 8] >> (7 - bit % 8) & 1) && lz < 31)
			lz++, bit++;
		bit++;
		uint32_t x = 0;
		for (int j = 0; j < lz; j++, bit++)
			x = x << 1 | (bit < nb * 8 ? (b[bit / 8] >> (7 - bit % 8) & 1) : 0);
		if (bit > nb * 8)
			return 0;
		val[k] = (1u << lz) - 1 + x;
	}
	return val[1] % 5 == 2 || val[1] % 5 == 4;
}

/* the sample's AVCC NALs -> Annex B in v->tx (after the parameter sets when they are due); its length, or -1.
 * *sync: the access unit has an IDR, or its first slice is I (vdcold); 4e HEVC: it has an IRAP NAL (IDR, CRA, BLA) */
static ssize_t to_annexb(struct vd *v, CMSampleBufferRef sb, int with_ps, int *sync)
{
	CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
	size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
	if (!len || grow(&v->in, &v->in_cap, len) || CMBlockBufferCopyDataBytes(bb, 0, len, v->in))
		return -1;
	size_t nal = 0, out = with_ps ? v->ps_len : 0;
	int vcl = 0;
	*sync = 0;
	for (size_t i = 0; i < len;) {	/* sizes first */
		size_t n = 0;
		if (i + (size_t)v->nal_len > len)
			return -1;
		for (int k = 0; k < v->nal_len; k++)
			n = n << 8 | v->in[i + k];
		i += (size_t)v->nal_len;
		if (!n || n > len - i)
			return -1;
		if (v->hevc) {
			*sync |= hevc_irap((v->in[i] >> 1) & 0x3f);
		} else {
			int t = v->in[i] & 0x1f;
			if (t == 5 || (t == 1 && !vcl))
				*sync |= sync_nal(v->in + i, n);
			vcl |= t == 1 || t == 5;
		}
		i += n, out += 4 + n, nal++;
	}
	if (!nal || out > VENUSD_MAX_AU || grow(&v->tx, &v->tx_cap, out))
		return -1;
	uint8_t *q = v->tx;
	if (with_ps)
		memcpy(q, v->ps, v->ps_len), q += v->ps_len;
	for (size_t i = 0; i < len;) {
		size_t n = 0;
		for (int k = 0; k < v->nal_len; k++)
			n = n << 8 | v->in[i + k];
		i += (size_t)v->nal_len;
		memcpy(q, "\0\0\0\1", 4);
		memcpy(q + 4, v->in + i, n);
		q += 4 + n, i += n;
	}
	return (ssize_t)out;
}

#define RETRY	1	/* vdcold, submit(): the connection broke while sending: reconnect, then this frame again */

/* vdcold: the access units since the last IDR/I frame, for a resync (resync()) */
static void gop_add(struct vd *v, const uint8_t *au, size_t n, uint64_t tag, int sync)
{
	if (!cfg.resync)
		return;
	if (sync)
		v->gop_n = 0, v->gop_len = 0, v->gop_ok = 1;
	if (!v->gop_ok)
		return;
	size_t want = v->gop_len + n;
	if (v->gop_n >= GOP_MAX || want > GOP_BYTES ||
	    grow(&v->gop, &v->gop_cap, want > v->gop_cap ? (want > GOP_BYTES / 2 ? GOP_BYTES : want * 2) : want)) {
		v->gop_ok = 0, v->gop_n = 0, v->gop_len = 0;	/* too long: the next IDR/I frame starts over */
		return;
	}
	memcpy(v->gop + v->gop_len, au, n);
	v->gop_off[v->gop_n] = (uint32_t)v->gop_len, v->gop_alen[v->gop_n] = (uint32_t)n, v->gop_tag[v->gop_n] = tag;
	v->gop_n++, v->gop_len = want;
}

/* vdcold: a fresh venusd session gets the parameter sets and the GOP so far again; pts tag * PTS_STEP + 1 marks
 * their pictures for decoded() to discard. 0, RETRY or an OSStatus */
static OSStatus resync(struct vd *v)
{
	double t0 = mono();
	for (int i = 0; i < v->gop_n; i++) {
		size_t pre = i ? 0 : v->ps_len, want = pre + v->gop_alen[i];
		if (grow(&v->rp, &v->rp_cap, want))
			return ERR_MALFUNCTION;
		memcpy(v->rp, v->ps, pre);
		memcpy(v->rp + pre, v->gop + v->gop_off[i], v->gop_alen[i]);
		if (send_msg(v, VD_FRAME, v->gop_tag[i] * PTS_STEP + 1, (uint32_t)v->gop_tag[i], v->rp, (uint32_t)want)) {
			ERRLOG(v, "resync: FRAME %llu (again) send failed (%s)", (unsigned long long)v->gop_tag[i], strerror(errno));
			send_broke(v);
			return cfg.reconnect ? RETRY : ERR_MALFUNCTION;
		}
	}
	v->sent_cfg = 1;
	say("resync: parameter sets + %d access unit(s) from tag %llu sent again in %.0f ms (their pictures discarded)",
	    v->gop_n, (unsigned long long)v->gop_tag[0], 1e3 * (mono() - t0));
	return 0;
}

/* vdcold: a venusd session to send into: reconnect after a loss; OPEN at the first frame, after EOS or an error */
static OSStatus ensure_session(struct vd *v)
{
	pthread_mutex_lock(&v->mu);
	/* vdleak: a given-up instance gets a fresh try after VTVENUS_REVIVE_MS (needs the parked reader) */
	int revive = v->dead && v->hung && !v->invalid && !v->stop && cfg.reconnect && cfg.revive_ms && v->reader_on &&
		     (mono() - v->t_hung) * 1e3 >= cfg.revive_ms;
	if (revive)
		v->dead = 0, v->hung = 0, v->lost = 1, v->recon_used = 0, v->fails = 0, v->last_err = 0;
	int dead = v->dead, lost = v->lost, reopen = v->drained || v->reopen || !v->opened;
	pthread_mutex_unlock(&v->mu);
	if (revive)
		say("given up %d ms ago: a fresh try (reconnect, OPEN)", cfg.revive_ms);
	if (dead)
		return ERR_MALFUNCTION;
	if (lost) {
		if (reconnect(v))
			return ERR_MALFUNCTION;
		reopen = 1;
	}
	if (!reopen)
		return 0;
	/* after EOS (FinishDelayedFrames) or a session error: a new venusd session */
	pthread_mutex_lock(&v->mu);
	int fails = ++v->fails;	/* back to 0 with the next picture (decoded) */
	if (fails > MAX_FAILS)
		v->dead = 1;
	int recon = v->just_recon;
	v->just_recon = 0;
	pthread_mutex_unlock(&v->mu);
	if (fails > MAX_FAILS) {
		ERRLOG(v, "%d venusd sessions failed or were reopened in a row: giving up", MAX_FAILS);
		return ERR_MALFUNCTION;
	}
	venus_close(v);
	drop_all(v);
	OSStatus st = venus_open(v);
	/* just reconnected: venusd may not have dropped the old connection yet (-EBUSY for a moment) */
	for (double t0 = mono(); st == ERR_NOT_NOW && recon && mono() - t0 < 3;) {
		usleep(250000);
		st = venus_open(v);
	}
	return st;
}

static OSStatus submit(struct vd *v, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags,
		       VTDecodeInfoFlags *info)
{
	/* backpressure: MAX_INFLIGHT frames out; IO_MS (before the session's first picture: VTVENUS_COLD_MS) without
	 * anything back drops the oldest */
	pthread_mutex_lock(&v->mu);
	double t0 = mono();
	while (v->inflight >= MAX_INFLIGHT && !v->dead && !v->invalid && !v->lost) {
		double last = v->last_rx > t0 ? v->last_rx : t0;
		int lim = v->got_pic ? IO_MS : cfg.cold_ms;
		if (mono() - last > lim / 1e3) {
			VTVideoDecoderFrame f[1];
			int n = 0;
			uint64_t old = UINT64_MAX;
			struct slot *os = NULL;
			for (int i = 0; i < NSLOT; i++)
				if (v->slot[i].used && v->slot[i].tag < old)
					old = v->slot[i].tag, os = &v->slot[i];
			if (os)
				f[n++] = os->frame, os->used = 0, v->inflight--;
			v->emitting++;
			pthread_mutex_unlock(&v->mu);
			ERRLOG(v, "%d frames in flight, none back for %d ms: oldest (tag %llu) dropped", MAX_INFLIGHT, lim,
			       (unsigned long long)old);
			emit_dropped(v, f, n);
			pthread_mutex_lock(&v->mu);
			t0 = mono();
			continue;
		}
		wait_ms(v, 100);
	}
	int dead = v->dead || v->invalid, lost = v->lost;
	pthread_mutex_unlock(&v->mu);
	if (dead)
		return ERR_MALFUNCTION;
	if (lost)
		return RETRY;
	if (cfg.surf) {	/* vdcredit: credits go out ahead of the FRAME, from this thread only */
		cr_topup(v);
		pthread_mutex_lock(&v->mu);
		lost = v->lost, dead = v->dead;
		pthread_mutex_unlock(&v->mu);
		if (dead)
			return ERR_MALFUNCTION;
		if (lost)
			return RETRY;
	}
	int sync = 0, with_ps = !v->sent_cfg;
	ssize_t len = to_annexb(v, sb, with_ps, &sync);
	if (len < 0) {
		ERRLOG(v, "frame %llu: sample not AVCC with %d-byte lengths (or too long): bad data",
		       (unsigned long long)v->next_tag, v->nal_len);
		return ERR_BAD_DATA;
	}
	if (v->need_sync && cfg.resync && !sync) {	/* vdcold: a fresh session would start on a non-IDR frame */
		if (v->gop_ok && v->gop_n) {
			OSStatus st = resync(v);
			if (st)
				return st;
		} else if (v->sync_wait < SYNC_WAIT_MAX) {
			if (!v->sync_wait++)
				ERRLOG(v, "frame %llu: fresh venusd session, nothing kept to resync on: dropped until an IDR/I frame",
				       (unsigned long long)v->next_tag);
			VTVideoDecoderFrame f[1] = { frame };
			pthread_mutex_lock(&v->mu);
			v->emitting++;
			pthread_mutex_unlock(&v->mu);
			emit_dropped(v, f, 1);
			v->n_in++;
			return 0;
		} else {
			ERRLOG(v, "no IDR/I frame in %d frames: frame %llu sent anyway", SYNC_WAIT_MAX,
			       (unsigned long long)v->next_tag);
		}
	}
	VTVideoDecoderFrame stale[1];
	int ns = 0;
	pthread_mutex_lock(&v->mu);
	uint64_t tag = v->next_tag++;
	struct slot *s = &v->slot[tag % NSLOT];
	if (s->used)	/* NSLOT tags back and never returned */
		stale[ns++] = s->frame, v->inflight--;
	*s = (struct slot){ .used = 1, .tag = tag, .frame = frame, .no_out = !!(flags & kVTDecodeFrame_DoNotOutputFrame) };
	v->inflight++;
	v->emitting++;
	pthread_mutex_unlock(&v->mu);
	emit_dropped(v, stale, ns);
	if (send_msg(v, VD_FRAME, tag * PTS_STEP, (uint32_t)tag, v->tx, (uint32_t)len)) {
		int e = errno;
		pthread_mutex_lock(&v->mu);
		if (s->used && s->tag == tag)
			s->used = 0, v->inflight--;
		pthread_mutex_unlock(&v->mu);
		send_broke(v);
		ERRLOG(v, "FRAME %llu: send failed within %d ms (%s): %s", (unsigned long long)tag, io_ms(v), strerror(e),
		       cfg.reconnect ? "reconnect" : "decoder unusable");
		return cfg.reconnect ? RETRY : ERR_MALFUNCTION;
	}
	v->sent_cfg = 1;
	v->need_sync = 0;
	v->n_in++;
	gop_add(v, v->tx + (with_ps ? v->ps_len : 0), (size_t)len - (with_ps ? v->ps_len : 0), tag, sync);
	if (info)
		*info = kVTDecodeInfo_Asynchronous;
	return 0;
}

static OSStatus decode_try(struct vd *v, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags,
			   VTDecodeInfoFlags *info)
{
	/* vdcold: a lost connection gets one more try right away (reconnect, OPEN, resync, this frame) */
	for (int attempt = 0;; attempt++) {
		OSStatus st = ensure_session(v);
		if (st) {
			pthread_mutex_lock(&v->mu);
			int again = v->lost && !v->dead && !attempt;
			pthread_mutex_unlock(&v->mu);
			if (again)
				continue;
			return st;
		}
		st = submit(v, frame, sb, flags, info);
		if (st != RETRY)
			return st;
		if (attempt)
			return ERR_MALFUNCTION;	/* lost again: the next frame's reconnect gives up unless a picture came */
	}
}

static OSStatus dec_decode_frame(CMBaseObjectRef o, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags,
				 VTDecodeInfoFlags *info)
{
	struct vd *v = vd_of(o);
	if (info)
		*info = 0;
	if (!v || !frame || !sb)
		return ERR_PARAM;
	pthread_mutex_lock(&v->mu);
	int bad = v->invalid || !v->session;
	pthread_mutex_unlock(&v->mu);
	if (bad)
		return ERR_INVALID;
	OSStatus st = decode_try(v, frame, sb, flags, info);
	if (st)
		hang_up(v);	/* vdleak: given up (dead): the venusd slot goes now */
	return st;
}

static OSStatus dec_finish_delayed(CMBaseObjectRef o)
{
	struct vd *v = vd_of(o);
	if (!v)
		return ERR_PARAM;
	pthread_mutex_lock(&v->mu);
	int skip = v->invalid || v->dead || v->lost || !v->opened || v->drained || v->reopen || !v->sent_cfg;
	v->got_eos = 0;
	int ms = io_ms(v);	/* vdcold: no picture yet in this session: the cold allowance */
	pthread_mutex_unlock(&v->mu);
	if (skip) {	/* nothing sent in this venusd session: nothing to drain */
		drop_all(v);
		hang_up(v);	/* vdleak: only if given up */
		return v->dead ? ERR_MALFUNCTION : 0;
	}
	double t0 = mono();
	if (send_msg(v, VD_EOS, 0, (uint32_t)v->next_tag, NULL, 0)) {
		ERRLOG(v, "EOS: send failed (%s)", strerror(errno));
		send_broke(v);
		drop_all(v);
		return cfg.reconnect ? 0 : ERR_MALFUNCTION;
	}
	pthread_mutex_lock(&v->mu);
	int r = wait_reply(v, &v->got_eos, ms);
	int32_t st = v->eos_status;
	v->drained = 1;
	pthread_mutex_unlock(&v->mu);
	if (r || st)
		ERRLOG(v, "EOS: %s (status %d) after %.0f ms", r ? "no EOS_DONE" : "EOS_DONE", st, 1e3 * (mono() - t0));
	drop_all(v);	/* what the firmware never returned */
	return 0;
}

static const struct base_class dec_base = {
	1, sizeof(struct storage), NULL, dec_invalidate, dec_finalize, dec_description, dec_copy_property, dec_set_property,
	NULL, NULL, NULL
};
static const struct decoder_class dec_class = {
	3, dec_start_session, dec_decode_frame, dec_supported, NULL, NULL, dec_can_accept, dec_finish_delayed, NULL, NULL
};
static const struct vtable dec_vtable = { NULL, &dec_base, &dec_class };

static int venusd_connect(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", VENUSD_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	struct timeval tv = { IO_MS / 1000, 0 };
	int sz = 4 << 20, one = 1;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);	/* a dead venusd must not SIGPIPE the host */
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

static OSStatus create_instance(FourCharCode codec, CFAllocatorRef alloc, CMBaseObjectRef *out)
{
	struct stat sb;
	char why[96] = "";
	int fd = -1;
	if (!out)
		return ERR_PARAM;
	*out = NULL;
	pthread_once(&cfg_once, cfg_load);
	if (codec != kCMVideoCodecType_H264 && (codec != kCMVideoCodecType_HEVC || !cfg.hevc))
		snprintf(why, sizeof why, "codec %#x", (unsigned)codec);
	else if (!stat(OFF_FILE, &sb))
		snprintf(why, sizeof why, "%s exists", OFF_FILE);
	else if (stat(ON_FILE, &sb))
		snprintf(why, sizeof why, "no %s", ON_FILE);
	else if ((fd = venusd_connect()) < 0)
		snprintf(why, sizeof why, "no venusd at %s (%s)", VENUSD_SOCK, strerror(errno));
	if (!__atomic_exchange_n(&process_logged, 1, __ATOMIC_ACQ_REL))
		say("first CreateInstance in this process: %s%s (cold %d ms, wait %d ms, lazy open %d, reconnect %d, resync %d,"
		    " reconnect wait %d, error gap %d ms, revive %d ms, surf %d, credits %d)",
		    why[0] ? "refused, software decoder: " : "venusd connected", why, cfg.cold_ms, cfg.wait_ms, cfg.lazy,
		    cfg.reconnect, cfg.resync, cfg.recon_wait, cfg.err_gap_ms, cfg.revive_ms, cfg.surf, cfg.credits);
	if (why[0])
		return ERR_NO_DECODER;
	struct vd *v = calloc(1, sizeof *v);
	CMBaseObjectRef o = NULL;
	if (!v || CMDerivedObjectCreate(alloc, &dec_vtable, VTVideoDecoderGetClassID(), &o) || !o) {
		say("CreateInstance: no memory or no decoder object");
		free(v);
		close(fd);
		return ERR_MALFUNCTION;
	}
	v->refs = 1, v->fd = fd, v->hevc = codec == kCMVideoCodecType_HEVC;
	pthread_mutex_init(&v->mu, NULL);
	pthread_mutex_init(&v->txmu, NULL);
	pthread_cond_init(&v->cv, NULL);
	((struct storage *)CMBaseObjectGetDerivedStorage(o))->v = v;
	*out = o;
	return 0;
}

/* _VTLoadVideoDecoders: dlopen(RTLD_LOCAL), then this with no meaningful arguments. Registration only. */
__attribute__((visibility("default"))) void ExampleIPBDecoderRegister(void)
{
	const void *k[] = { CFSTR("CMClassImplementationID") };
	const void *val[] = { CFSTR("org.iosonpixel.venus") };
	CFDictionaryRef info = CFDictionaryCreate(NULL, k, val, 1, &kCFTypeDictionaryKeyCallBacks,
						  &kCFTypeDictionaryValueCallBacks);
	/* hardware (VT's default) at rating 0: hardware wins a tie (_vtSortRegistryItemsByRating) */
	OSStatus st = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info, (void *)create_instance);
	if (st)
		say("VTRegisterVideoDecoderWithInfo avc1: OSStatus %d", (int)st);
	pthread_once(&cfg_once, cfg_load);
	if (cfg.hevc && (st = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_HEVC, info, (void *)create_instance)))
		say("VTRegisterVideoDecoderWithInfo hvc1: OSStatus %d", (int)st);	/* 4e; VTVENUS_HEVC=0: not registered */
	if (info)
		CFRelease(info);
}
