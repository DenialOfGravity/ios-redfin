/*
 * venusd: video stage 4a (docs/features/video.md "Stage 4 spec", research/notes/venus-stage4-design-2026-11-03.md):
 * the root daemon that owns the Venus video engine for clients without /dev/pmem. Protocol: venus_proto.h.
 *   venusd [--log FILE]    default /var/local/venusd.log (cut to 0 past 1 MB; all of venus_core's output), one line
 *                          per bring-up, shutdown and session also to /dev/console (the live log)
 * Venus code: venus_core.c, venusctl's steps in tools/sunfish-venus-test.sh decode's order: power on (with the CVP
 * domain, as the script's --cvp), boot --keep, init --phys VENUSD_PHYS, then per session the decoder steps with the
 * buffer window right after the queue block (VENUSD_BUF_MB, 160: the v4a0 memtest window 0xa5500000+160 MB). Down
 * (idle VENUSD_IDLE_S = 60 s, SIGTERM, a firmware sys error) = venusctl shutdown (PAS shutdown first), then power
 * off; a failed shutdown is LEFT POWERED: no power off (a collapse under live firmware hangs the bus), venusd stays
 * up refusing OPEN with -EIO until a reboot. venus_core's STATE file is the record: a STATE left by a venusd that
 * died is undone at the next start. Don't run venusctl while venusd runs (same STATE).
 * Polling, no IRQ: the message queue every 1 ms while a session runs, nothing while idle. One session at a time
 * (a second OPEN: -EBUSY). Frames go back over the socket (copy-out through the kernel160 sweetpmemwc=2 mapping);
 * or straight into a credited IOSurface (vdcredit below).
 * Switches: /var/local/venus-off (OPEN -> -ENODEV), VENUSD_VERBOSE=1 (venusctl's per-buffer lines in the log),
 * VENUSD_IDLE_S, VENUSD_PHYS (redfin 0xa2600000), VENUSD_BUF_MB (160), VENUSD_IN_KB (input buffer floor, 1024),
 * VENUSD_DEBUG (firmware debug mask, 0x18); the VENUSCTL_* decoder switches work as in venusctl.
 * Cold start (vdcold, video.md vdboot: the first decode after boot stalls up to ~13 s): until a session's first
 * picture, socket reads/writes that would block and the 3 s no-progress rule get VENUSD_COLD_S (20) instead of 2/3 s
 * (0: the old rule). VENUSD_WARMUP=1 (default): right after the socket is up, power Venus, decode one built-in
 * 320x240 IDR, power off again, so the first-use stall happens at boot, not in Safari (0: off).
 * vdstop (research/notes/venus-wpt-2026-11-03.md section 4; vdcold: a clock/GDSC read off once at a doorbell and
 * venusd exited): a venus_core STOP (a clock or GDSC off in 3 reads 1 ms apart, a blocked write) longjmps back to the
 * main loop instead of exiting: the client gets VD_ERROR -EIO (an OPEN in bring-up: VD_OPEN -EIO), the SFR is
 * printed, then Venus down as downstream handle_sys_error (power on first if the clocks still read off: PAS shutdown
 * needs it), the next OPEN brings it up again; a failed shutdown is LEFT POWERED as above. VENUSD_RECOVER=0: exit(1)
 * as before. Test: /var/local/venusd-test-clkoff (or VENUSD_TEST_CLKOFF) "F[:K]": at the session's frame F the next
 * K clock checks read off (K 3 = one STOP, normal teardown; 4 = the power-on-first path; 1-2 = the re-read passes;
 * 5 = power on "fails": the LEFT POWERED path, reboot after);
 * the file is deleted when read (one shot per file), the env var fires once per venusd. Firmware debug lines during a
 * session carry "[frame N]" (VENUSD_FW_FRAME=0: not).
 * 4e: OPEN with codec VD_CODEC_HEVC opens an HEVC session (8-bit 4:2:0 Main; another SPS: ERROR -ENOTSUP at the first
 * FRAME); codec 0 or VD_CODEC_H264 is H.264 as before. VENUSD_HEVC=0: HEVC OPENs refused (-EINVAL).
 * vdleak (the 15-min Safari soak, video.md 2026-11-03: 4 slots held by dead vtvenus instances, then "too many
 * clients"): up to MAXCL (16, was 4) connections; one is dropped on EOF, a read/write error, POLLHUP/POLLERR/POLLNVAL,
 * or after VENUSD_IDLE_CLIENT_S (30; 0: never) without a message while it has no session or one with nothing to do
 * (opened without a FRAME, EOS done, failed: such a held session refused every other OPEN); each connect/drop logs
 * "client fd N connected|gone (reason), K connected".
 * vdnopay (copy-cost test): /var/local/venusd-nopayload present at an OPEN: that session's frames are still copied out
 * of Venus, but DECODED goes with len 0 (no pixels; vtvenus emits an unfilled pool buffer). Absent: as before.
 * vdcredit (2026-11-03, video.md's copy saving): CREDIT gives venusd an IOSurface ID of the client's (vtvenus: VT's
 * pixel-buffer pool); while one is queued and fits the picture (2 planes 420v/420f, plane 0 the visible width x
 * height, rows wide enough, x crop a multiple of 4), the picture's visible rect is pread from /dev/pmem straight into
 * its planes (one read per plane when bytes per row == Venus's stride, else per row) and DECODED goes with len 0 +
 * surface. No credit: the copy-out buffer and the payload as before; a credit that doesn't fit goes back (UNCREDIT
 * d->c, -EINVAL).
 * Credits end at UNCREDIT, EOS_DONE, ERROR, CLOSE and a disconnect (never written after); looked-up surfaces (at most
 * VSURF_MAX, least recently used out) are released at the session's end. One log line per session for each path.
 * VENUSD_SURF=0: every CREDIT handed back with -ENOTSUP (the client stops crediting), the socket path only.
 * IOSurface lookups by ID work across processes here (SweetSurface sel 4 has no owner check).
 * vdstamp: every line of the log (venus_core's too) and of the console starts "[ssss.mmm] ": seconds since boot from
 * mach_absolute_time, the clock (and format) of the kernel's per-boot log lines, so they line up; the first line after
 * a minute without one, the start and each session open add "[wall YYYY-MM-DD HH:MM:SS] ". VENUSD_STAMP=0: no stamps.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <IOSurface/IOSurfaceRef.h>
#include "pmem.h"
#include "venus_core.h"
#include "venus_hevc.h"
#include "venus_proto.h"

#define LOG_MAX		(1 << 20)
#define MAXCL		16	/* vdleak: was 4 */
#define VSURF_MAX	16	/* vdcredit: looked-up surfaces kept per session */
#ifndef LOCKF	/* -D for a host test build */
#define LOCKF		"/var/run/venusd.pid"
#define OFF_FILE	"/var/local/venus-off"
/* redfin (track V): off unless this file exists: venusd exits at start without it (the launchd job's KeepAlive
 * PathState waits for it), and OPEN is refused while it is missing (rm = off without a restart) */
#define ON_FILE		"/var/local/venus.on"
#define TEST_FILE	"/var/local/venusd-test-clkoff"
#endif
#ifndef NOPAY_FILE
#define NOPAY_FILE	"/var/local/venusd-nopayload"
#endif

enum { S_NONE, S_OPEN, S_RUN, S_EOS, S_DONE, S_FAILED };	/* the session (owner connection) */
static const char *const sname[] = { "none", "open", "running", "eos sent", "eos done", "failed" };

static struct client { int fd, gone; double t_msg; } cl[MAXCL];	/* gone: a write failed, dropped by the main loop;
								 * t_msg: connect or last message (vdleak) */
static int owner = -1, sess = S_NONE, hfi, up, fault, logfd = -1, confd = -1;
static volatile sig_atomic_t term;
static const char *fw;
static uint64_t phys = 0xa2600000ull, buf_mb = 160;	/* redfin: queues 0xa2600000, window 0xa2a00000 + 160 MB (xnu 1057) */
static uint32_t debug = 0x18, in_min = 1024u << 10;
static unsigned idle_s = 60, nsess, idle_cl_s = 30;	/* idle_cl_s: VENUSD_IDLE_CLIENT_S (vdleak) */
static double idle_since, t_open, t_first, t_last_frame, t_eos, t_prog;
static uint32_t sent, hint_w, hint_h;
static unsigned last_acts;
static uint8_t *au;
static unsigned cold_s = 20;	/* vdcold: VENUSD_COLD_S */
static int sess_hevc, hevc_ok = 1;	/* 4e: the session's codec; VENUSD_HEVC */
static jmp_buf stop_jb, rec_jb;	/* vdstop: the main loop; recover() itself */
static int stop_armed, recovering, opening = -1, ending, warmed;
static int nopay;	/* vdnopay: NOPAY_FILE at this session's OPEN */
static unsigned nstops, test_frame, test_k;	/* VENUSD_TEST_CLKOFF / TEST_FILE: frame F, K reads */
/* vdcredit: the owner's credits (FIFO), the surfaces looked up for them, the one a frame is being read into */
static int surf_on = 1;	/* VENUSD_SURF */
static uint32_t cred[VENUSD_MAX_CREDITS];
static unsigned cred_h, cred_n, n_placed, n_copied, n_returned;
static unsigned path_logged;	/* this session's path lines: 1 credit, 2 socket copy, 4 handed back */
static struct { uint32_t id; IOSurfaceRef s; unsigned long use; } vsurf[VSURF_MAX];
static unsigned long vsurf_clock;
static IOSurfaceRef placed_s;
static uint32_t placed_id;

/* vdcold: the warm-up stream, one 320x240 grey IDR (SPS, PPS, slice; ffmpeg color=gray, libx264 constrained baseline,
 * SEI removed; 320x240 is a size stage 3 decoded bit-exact) */
static const uint8_t warm_au[] = {
	0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0d, 0xdc, 0x14, 0x1f, 0xb0, 0x11, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
	0x00, 0x03, 0x00, 0x3c, 0x0f, 0x14, 0x2b, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x0f, 0x2c, 0x80, 0x00, 0x00,
	0x01, 0x65, 0x88, 0x84, 0x04, 0xbc, 0x98, 0xa0, 0x00, 0x20, 0x2f, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27,
	0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x27, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
	0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
	0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
	0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
	0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
	0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
	0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
	0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
	0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d,
	0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75,
	0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7,
	0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x80
};

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* vdstamp: "[ssss.mmm] " (the kernel log's mach_absolute_time stamp), "[wall ...] " too when due; its length */
static int stamp_on = 1;
static double wall_due;	/* uptime of the next wall-clock stamp (0: the next line) */

static size_t stamp(char *b, size_t n)
{
	static mach_timebase_info_data_t tb;
	if (!stamp_on)
		return b[0] = 0, 0;
	if (!tb.denom)
		mach_timebase_info(&tb);
	uint64_t ms = mach_absolute_time() * tb.numer / tb.denom / 1000000u;
	int k = snprintf(b, n, "[%4llu.%03llu] ", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
	if (ms / 1e3 >= wall_due && k > 0 && (size_t)k < n) {
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		k += (int)strftime(b + k, n - (size_t)k, "[wall %Y-%m-%d %H:%M:%S] ", &tm);
		wall_due = ms / 1e3 + 60;
	}
	return k > 0 ? (size_t)k : 0;
}

/* vdstamp: stdout/stderr's writer (funopen): the stamp at each line start, to the log fd */
static int log_write(void *c, const char *p, int n)
{
	static int bol = 1;
	for (int i = 0; i < n;) {
		if (bol) {
			char pre[64];
			size_t k = stamp(pre, sizeof pre);
			if (k)
				(void)!write(logfd, pre, k);
			bol = 0;
		}
		const char *nl = memchr(p + i, '\n', (size_t)(n - i));
		int len = nl ? (int)(nl - (p + i)) + 1 : n - i;
		(void)!write(logfd, p + i, (size_t)len);
		i += len;
		bol = nl != NULL;
	}
	return n;
}

/* one line to the log (stdout) and the live log (/dev/console, as the bridge's sm2 lines); the same stamp in both */
static void note(const char *fmt, ...)
{
	char s[512], pre[64];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof s, fmt, ap);
	va_end(ap);
	fflush(stdout);	/* every line ends in a newline: the next starts a line */
	stamp(pre, sizeof pre);
	dprintf(logfd, "%s%s\n", pre, s);
	if (confd < 0)
		confd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	if (confd >= 0)
		dprintf(confd, "%s%s\n", pre, s);
}

/* vdcold: how long a socket read/write may wait in all: the cold-start allowance until the session's first picture
 * (the first decode after boot stalls), else the 2 s of SO_SNDTIMEO/SO_RCVTIMEO */
static double io_limit(void)
{
	int cold = owner >= 0 && (sess == S_OPEN || sess == S_RUN || sess == S_EOS) && !t_first;
	return cold && cold_s > 2 ? cold_s : 2;
}

/* partial transfers and EAGAIN (the 2 s socket timeouts) retried until io_limit() has passed */
static int wr_full(int fd, const void *b, size_t n)
{
	const uint8_t *p = b;
	double t0 = mono(), lim = io_limit();
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && mono() - t0 < lim) {
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

static int rd_full(int fd, void *b, size_t n)
{
	uint8_t *p = b;
	double t0 = mono(), lim = io_limit();
	while (n) {
		ssize_t k = read(fd, p, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && mono() - t0 < lim) {
			struct pollfd q = { .fd = fd, .events = POLLIN };
			poll(&q, 1, 100);
			continue;
		}
		if (k <= 0) {
			if (!k)
				errno = 0;	/* vdleak: EOF, the peer closed */
			return -1;
		}
		p += k, n -= (size_t)k;
	}
	return 0;
}

static void send_msg(int i, struct venusd_msg *m, const void *payload)
{
	if (i < 0 || cl[i].fd < 0 || cl[i].gone)
		return;
	m->magic = VENUSD_MAGIC;
	m->version = VENUSD_VERSION;
	/* sink() runs inside venus_core's queue loop: no session teardown from here, the main loop drops it */
	if (wr_full(cl[i].fd, m, sizeof *m) || (m->len && wr_full(cl[i].fd, payload, m->len))) {
		printf("venusd: client fd %d: write failed (%s): dropping it\n", cl[i].fd, strerror(errno));
		cl[i].gone = 1;
	}
}

static void reply(int i, int type, int32_t status, uint32_t seq)
{
	struct vdec_status st;
	vdec_status(&st);
	struct venusd_msg m = { .type = (uint16_t)type, .status = status, .seq = seq,
		.frames = i == owner ? (uint32_t)st.frames : 0 };
	send_msg(i, &m, NULL);
}

/* ---- vdcredit: the client's IOSurfaces ---- */

/* a credit back to the owner unused (UNCREDIT d->c): status why, w x h the picture it was meant for */
static void cred_return(uint32_t id, int32_t status, uint32_t w, uint32_t h)
{
	struct venusd_msg m = { .type = VD_UNCREDIT, .status = status, .surface = id, .width = w, .height = h };
	n_returned++;
	send_msg(owner, &m, NULL);
}

/* every queued credit void (UNCREDIT, EOS_DONE, ERROR, CLOSE, disconnect, STOP); how many there were */
static unsigned cred_drop(void)
{
	unsigned n = cred_n;
	if (placed_s)	/* a STOP unwound a placed read: the surface stays the client's, its credit is gone */
		IOSurfaceUnlock(placed_s, 0, NULL), placed_s = NULL;
	cred_h = cred_n = 0;
	return n;
}

/* the looked-up surfaces released (the session's end) */
static void vsurf_drop(void)
{
	for (int i = 0; i < VSURF_MAX; i++)
		if (vsurf[i].s)
			CFRelease(vsurf[i].s), vsurf[i].s = NULL, vsurf[i].id = 0;
}

static IOSurfaceRef vsurf_get(uint32_t id)
{
	int lru = 0;
	for (int i = 0; i < VSURF_MAX; i++) {
		if (vsurf[i].s && vsurf[i].id == id) {
			vsurf[i].use = ++vsurf_clock;
			return vsurf[i].s;
		}
		if (!vsurf[i].s || (vsurf[lru].s && vsurf[i].use < vsurf[lru].use))
			lru = i;
	}
	IOSurfaceRef s = IOSurfaceLookup(id);
	if (!s)
		return NULL;
	if (vsurf[lru].s)
		CFRelease(vsurf[lru].s);
	vsurf[lru].id = id, vsurf[lru].s = s, vsurf[lru].use = ++vsurf_clock;
	return s;
}

#define FCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* NULL if surface s can take picture f as venus_core's place_io writes it, else why not */
static const char *vsurf_misfit(IOSurfaceRef s, const struct vdec_frame *f)
{
	uint32_t fmt = IOSurfaceGetPixelFormat(s);
	size_t w4 = ((size_t)f->cw + 3) & ~(size_t)3;
	if (IOSurfaceGetPlaneCount(s) != 2 || (fmt != FCC('4', '2', '0', 'v') && fmt != FCC('4', '2', '0', 'f')))
		return "not 2-plane 420v/420f";
	if (IOSurfaceGetWidthOfPlane(s, 0) != (size_t)f->cw || IOSurfaceGetHeightOfPlane(s, 0) != (size_t)f->ch ||
	    IOSurfaceGetHeightOfPlane(s, 1) < ((size_t)f->ch + 1) / 2)
		return "size";
	if (IOSurfaceGetBytesPerRowOfPlane(s, 0) < w4 || IOSurfaceGetBytesPerRowOfPlane(s, 1) < w4)
		return "bytes per row";
	if (f->cx & 3 || f->cx + w4 > f->stride)
		return "x crop";
	return NULL;
}

/* venus_core's vdec_place: the oldest credit that fits, locked, its planes in d; misfits go back. f NULL: the read
 * into the placed surface failed (no DECODED for that frame; the credit goes back) */
static int place(const struct vdec_frame *f, struct vdec_dest *d)
{
	if (!f) {
		if (placed_s) {
			IOSurfaceUnlock(placed_s, 0, NULL);
			placed_s = NULL;
			cred_return(placed_id, -EIO, 0, 0);
		}
		return 0;
	}
	if (owner < 0 || nopay)
		return 0;
	while (cred_n) {
		uint32_t id = cred[cred_h];
		cred_h = (cred_h + 1) % VENUSD_MAX_CREDITS, cred_n--;
		IOSurfaceRef s = vsurf_get(id);
		const char *why = !s ? "lookup failed" : vsurf_misfit(s, f);
		if (!why && IOSurfaceLock(s, 0, NULL))
			why = "lock failed";
		if (!why) {
			d->y = IOSurfaceGetBaseAddressOfPlane(s, 0), d->c = IOSurfaceGetBaseAddressOfPlane(s, 1);
			d->ybpr = IOSurfaceGetBytesPerRowOfPlane(s, 0), d->cbpr = IOSurfaceGetBytesPerRowOfPlane(s, 1);
			if (d->y && d->c) {
				placed_s = s, placed_id = id;
				return 1;
			}
			IOSurfaceUnlock(s, 0, NULL);
			why = "no plane address";
		}
		if (!(path_logged & 4)) {
			path_logged |= 4;
			printf("venusd: session %u: credited IOSurface %u handed back: %s (picture %dx%d at %d,%d, stride %u;"
			       " surface %zux%zu, %zu planes, bytes per row %zu/%zu)\n", nsess, id, why, f->cw, f->ch, f->cx,
			       f->cy, f->stride, s ? IOSurfaceGetWidthOfPlane(s, 0) : 0, s ? IOSurfaceGetHeightOfPlane(s, 0) : 0,
			       s ? IOSurfaceGetPlaneCount(s) : 0, s ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : 0,
			       s ? IOSurfaceGetBytesPerRowOfPlane(s, 1) : 0);
		}
		cred_return(id, -EINVAL, (uint32_t)f->cw, (uint32_t)f->ch);
	}
	return 0;
}

/* CREDIT from the owner: queued while the session can still output pictures */
static void do_credit(int i, const struct venusd_msg *m)
{
	if (i != owner || (sess != S_OPEN && sess != S_RUN && sess != S_EOS) || !m->surface)
		return;	/* the client released its credits with the session's end */
	if (!surf_on) {
		cred_return(m->surface, -ENOTSUP, 0, 0);
		return;
	}
	if (cred_n == VENUSD_MAX_CREDITS) {
		cred_return(m->surface, -ENOSPC, 0, 0);
		return;
	}
	cred[(cred_h + cred_n++) % VENUSD_MAX_CREDITS] = m->surface;
}

/* the decoder's frames (venus_core frame_out) -> DECODED to the session's client */
static void sink(const struct vdec_frame *f)
{
	struct venusd_msg m = { .type = VD_DECODED, .len = (uint32_t)f->len, .pts = f->ts, .flags = f->flags,
		.width = (uint32_t)f->cw, .height = (uint32_t)f->ch, .crop_x = (uint32_t)f->cx, .crop_y = (uint32_t)f->cy,
		.stride = f->stride, .scanlines = f->scanlines, .filled = f->filled, .pic_type = f->pic_type,
		.seq = (uint32_t)f->index, .fbd_w = f->fbd_w, .fbd_h = f->fbd_h, .fbd_x = f->fbd_x, .fbd_y = f->fbd_y,
		.copy_us = (uint32_t)(f->copy_ms * 1e3) };
	if (nopay)
		m.len = 0;	/* vdnopay: header only */
	if (f->placed) {	/* vdcredit: the pixels are in the credited surface */
		IOSurfaceUnlock(placed_s, 0, NULL);
		m.len = 0, m.surface = placed_id;
		placed_s = NULL;
		n_placed++;
		if (!(path_logged & 1)) {
			path_logged |= 1;
			printf("venusd: session %u: frame %d into credited IOSurface %u (%.2f ms read)\n", nsess, f->index,
			       m.surface, f->copy_ms);
		}
	} else {
		n_copied++;
		if (!(path_logged & 2)) {
			path_logged |= 2;
			printf("venusd: session %u: frame %d over the socket (%s)\n", nsess, f->index,
			       nopay ? "nopayload test" : !surf_on ? "VENUSD_SURF=0" : "no credit queued");
		}
	}
	if (!f->index)
		t_first = mono();
	t_last_frame = mono();
	send_msg(owner, &m, f->buf);
	if (test_frame && (unsigned)f->index + 1 >= test_frame) {	/* vdstop test: the next gated access STOPs */
		note("venusd: test: frame %d: the next %u clock check(s) read off", f->index, test_k);
		vc_fake_off = (int)test_k;
		test_frame = 0;
	}
}

/* vdstop test: TEST_FILE (deleted when read) or VENUSD_TEST_CLKOFF "F[:K]" (K 3) */
static void test_arm(const char *spec, const char *from)
{
	unsigned f = 0, k = 3;
	if (!spec || sscanf(spec, "%u:%u", &f, &k) < 1 || !f)
		return;
	test_frame = f, test_k = k ? k : 3;
	note("venusd: test (%s): at frame %u the next %u clock check(s) read off", from, test_frame, test_k);
}

static void test_file(void)
{
	char b[32] = "";
	FILE *f = fopen(TEST_FILE, "r");
	if (!f)
		return;
	if (!fgets(b, sizeof b, f))
		b[0] = 0;
	fclose(f);
	unlink(TEST_FILE);
	test_arm(b, TEST_FILE);
}

/* ---- Venus up/down: tools/sunfish-venus-test.sh decode's steps and its LEFT POWERED rule ---- */

static void down(const char *why)
{
	if (fault)
		return;
	note("venusd: Venus down (%s)", why);
	if (shutdown_cmd()) {
		fault = 1;
		note("venusd: shutdown FAILED: no power off (the firmware may still run): LEFT POWERED, reboot needed");
		return;
	}
	if (power_off())
		note("venusd: power off reported a failure (see %s)", "the venusd log");
	up = 0;
	vdec_fw_reset();
}

static int bring_up(void)
{
	double t0 = mono();
	struct stat sb;
	cvp = 1;	/* the test script's --cvp on every power on/off (pre-run review 8.5) */
	if (power_on()) {	/* undoes itself */
		note("venusd: power on failed");
		return -1;
	}
	if (boot(fw, 1) || init(phys)) {
		note("venusd: boot/init failed: undoing");
		down("bring-up failed");
		return -1;
	}
	if (stat(STATE, &sb)) {
		note("venusd: no %s after init: refusing to go on", STATE);
		return -1;
	}
	up = 1;
	note("venusd: Venus up in %.0f ms (firmware %s, queues %#llx)", 1e3 * (mono() - t0), fw, (unsigned long long)phys);
	return 0;
}

/* bring_up for an OPEN from connection i (vdstop: a STOP in it answers that OPEN from recover()) */
static int up_for(int i)
{
	opening = i;
	int r = bring_up();
	opening = -1;
	return r;
}

/* ---- the session ---- */

static void sess_log(const char *how)
{
	struct vdec_status st;
	vdec_status(&st);
	double span = t_last_frame > t_first ? t_last_frame - t_first : 0;
	note("venusd: session %u %s: %u AUs in, %d frames, %.2f s from OPEN, first frame %.0f ms after it, %.1f fps"
	     " (frame 0 -> %d), copy-out mean %.2f ms max %.2f ms, error %#x%s; credited %u, socket %u, handed back %u",
	     nsess, how, sent, st.frames,
	     mono() - t_open, st.frames ? 1e3 * (t_first - t_open) : 0.0, st.frames > 1 && span > 0 ? (st.frames - 1) / span : 0.0,
	     st.frames ? st.frames - 1 : 0, st.frames ? 1e3 * st.cp_sum / st.frames : 0.0, 1e3 * st.cp_max, st.err,
	     st.fw_dead ? ", firmware sys error" : "", n_placed, n_copied, n_returned);
}

/* the HFI session down (if it started), the firmware too after a sys error */
static void sess_stop(void)
{
	struct vdec_status st;
	int bad = hfi && vdec_close();
	hfi = 0;
	vdec_status(&st);
	if (st.fw_dead)
		down("firmware sys error");
	else if (bad)	/* a half-closed firmware session would refuse the next SESSION_INIT: start clean */
		down("session close incomplete");
}

static void sess_fail(int32_t status, uint32_t seq)
{
	printf("venusd: session %u failed: status %d (%#x) at frame %u\n", nsess, status, (unsigned)status, seq);
	cred_drop();	/* vdcredit: the client lets go of them at the ERROR */
	if (owner >= 0)
		reply(owner, VD_ERROR, status, seq);
	sess = S_FAILED;	/* vdstop: before sess_stop, so a STOP in it sends no second VD_ERROR */
	sess_stop();
	sess_log("failed");
}

static void sess_end(const char *how)
{
	cred_drop();	/* vdcredit: before the decoder's last pictures (they go over the socket) */
	if (sess != S_FAILED) {
		ending = 1;	/* vdstop: a STOP in here: recover() finishes the end */
		sess_stop();
		ending = 0;
		sess_log(how);
	}
	vsurf_drop();
	sess = S_NONE;
	owner = -1;
	idle_since = mono();
}

static int nclients(void)
{
	int n = 0;
	for (int i = 0; i < MAXCL; i++)
		n += cl[i].fd >= 0;
	return n;
}

static void drop_client(int i, const char *why)
{
	if (cl[i].fd < 0)
		return;
	int fd = cl[i].fd;
	close(cl[i].fd);
	cl[i].fd = -1;
	cl[i].gone = 0;
	printf("venusd: client fd %d gone (%s), %d connected\n", fd, why, nclients());	/* vdleak */
	if (i == owner)
		sess_end("closed (client gone)");
}

/* vdleak: why a read failed (rd_full: errno 0 = EOF) */
static const char *rd_why(void)
{
	return !errno ? "EOF" : errno == EAGAIN || errno == EWOULDBLOCK ? "read timeout" : strerror(errno);
}

static void do_open(int i, const struct venusd_msg *m)
{
	struct stat sb;
	int32_t rc = 0;
	if (owner >= 0 && owner != i)
		rc = -EBUSY, printf("venusd: OPEN from fd %d refused: one session at a time (fd %d has it)\n", cl[i].fd,
				  cl[owner].fd);
	else if (owner == i)
		rc = -EINVAL, printf("venusd: second OPEN on fd %d refused (session %s)\n", cl[i].fd, sname[sess]);
	else if (m->codec && m->codec != VD_CODEC_H264 && (m->codec != VD_CODEC_HEVC || !hevc_ok))
		rc = -EINVAL, printf("venusd: OPEN codec %u refused: H.264 (%u)%s only\n", m->codec, VD_CODEC_H264,
				     hevc_ok ? " or HEVC (2)" : " (VENUSD_HEVC=0)");
	else if (!stat(OFF_FILE, &sb))
		rc = -ENODEV, printf("venusd: OPEN refused: %s exists\n", OFF_FILE);
	else if (stat(ON_FILE, &sb))
		rc = -ENODEV, printf("venusd: OPEN refused: no %s\n", ON_FILE);
	else if (fault)
		rc = -EIO, printf("venusd: OPEN refused: Venus was left powered after a failed shutdown (reboot)\n");
	else if (!up && up_for(i))
		rc = -EIO;
	if (!rc) {
		owner = i;
		sess = S_OPEN;
		nsess++;
		sent = 0;
		hint_w = m->width, hint_h = m->height;
		sess_hevc = m->codec == VD_CODEC_HEVC;
		t_open = mono();
		t_first = t_last_frame = 0;
		cred_drop();	/* vdcredit */
		n_placed = n_copied = n_returned = path_logged = 0;
		wall_due = 0;	/* vdstamp: the wall clock on this line */
		note("venusd: session %u opened (fd %d, %s, hint %ux%u)", nsess, cl[i].fd, sess_hevc ? "HEVC" : "H.264", hint_w,
		     hint_h);
		test_file();
		nopay = !stat(NOPAY_FILE, &sb);	/* vdnopay */
		if (nopay)
			note("venusd: nopayload test: frames sent without pixels");
	}
	reply(i, VD_OPEN, rc, 0);
}

/* the first FRAME: the size from its SPS (else the OPEN hint, else 1920x1088 as venusctl), then the decoder start */
static int sess_start(const uint8_t *p, uint32_t len)
{
	uint32_t off[4], alen[4];
	struct h264_sps sps = { 0, 0, 0, 0, 0, 0 };
	struct hevc_sps hs = { 0 };
	int nau = sess_hevc ? split_aus_hevc(p, len, off, alen, 4, &sps, &hs) : split_aus(p, len, off, alen, 4, &sps);
	if (sess_hevc && nau >= 0 && !hevc_main8(&hs)) {	/* 4e: Venus here decodes 8-bit 4:2:0 Main only */
		printf("venusd: HEVC profile %d, chroma format %d, %d/%d bit refused: 8-bit 4:2:0 Main only\n", hs.profile,
		       hs.chroma, hs.depth_y, hs.depth_c);
		return -ENOTSUP;
	}
	if (nau < 0) {
		uint32_t w = hint_w ? hint_w : 1920, h = hint_h ? hint_h : 1080;
		sps = (struct h264_sps){ .w = (int)((w + 15) & ~15u), .h = (int)((h + 15) & ~15u), .cw = (int)w, .ch = (int)h };
		printf("venusd: no SPS in the first access unit: %dx%d (visible %dx%d), the firmware's SEQUENCE_CHANGED decides\n",
		       sps.w, sps.h, sps.cw, sps.ch);
	}
	if (vdec_prepare(debug, 0, buf_mb, INT_MAX, &sps, sess_hevc))
		return -EIO;
	int r = vdec_start(in_min);
	if (r) {
		struct vdec_status st;
		vdec_status(&st);
		hfi = r == 2;	/* a session to close (sess_stop) */
		return st.err ? st.err : -EIO;
	}
	sess = S_RUN;
	hfi = 1;
	t_prog = mono();
	return 0;
}

/* one message from connection i (it is readable) */
static void command(int i)
{
	struct venusd_msg m;
	if (rd_full(cl[i].fd, &m, sizeof m)) {
		drop_client(i, rd_why());
		return;
	}
	if (m.magic != VENUSD_MAGIC || m.len > VENUSD_MAX_AU) {
		printf("venusd: client fd %d: bad message (magic %#x, len %u): dropped\n", cl[i].fd, m.magic, m.len);
		drop_client(i, "bad message");
		return;
	}
	if (m.len && rd_full(cl[i].fd, au, m.len)) {
		drop_client(i, rd_why());
		return;
	}
	cl[i].t_msg = mono();
	if (m.type == VD_OPEN) {
		do_open(i, &m);
		return;
	}
	if (m.type == VD_CREDIT) {	/* vdcredit: before the owner and state checks: never an ERROR */
		do_credit(i, &m);
		return;
	}
	if (m.type == VD_UNCREDIT) {
		struct venusd_msg r = { .type = VD_UNCREDIT, .frames = i == owner ? cred_drop() : 0 };
		send_msg(i, &r, NULL);
		return;
	}
	if (i != owner) {
		reply(i, VD_ERROR, -EINVAL, m.seq);
		return;
	}
	if (m.type == VD_CLOSE) {
		cred_drop();	/* vdcredit: none written after the reply */
		reply(i, VD_CLOSE, 0, 0);
		sess_end("closed");
		return;
	}
	if (sess == S_FAILED) {
		reply(i, VD_ERROR, -EIO, m.seq);
		return;
	}
	if (m.type == VD_FRAME) {
		if (sess == S_EOS || sess == S_DONE) {
			reply(i, VD_ERROR, -EINVAL, m.seq);
			return;
		}
		if (sess == S_OPEN) {
			int r = sess_start(au, m.len);
			if (r) {
				sess_fail(r, m.seq);
				return;
			}
		}
		int r = vdec_etb(au, m.len, m.pts, 0, (int)m.seq);
		if (r == -2) {
			struct vdec_status st;
			vdec_status(&st);
			printf("venusd: frame %u: %u bytes, input buffers are %u: dropped\n", m.seq, m.len, st.in_size);
			reply(i, VD_ERROR, -E2BIG, m.seq);
		} else if (r) {
			sess_fail(-EIO, m.seq);
		} else {
			sent++;
		}
		return;
	}
	if (m.type == VD_EOS) {
		if (sess == S_OPEN) {	/* nothing decoded: nothing to drain */
			cred_drop();	/* vdcredit: they end with EOS_DONE */
			reply(i, VD_EOS_DONE, 0, 0);
			sess = S_DONE;
			return;
		}
		if (sess != S_RUN || vdec_etb(NULL, 0, 0, 1, (int)sent)) {
			sess_fail(sess == S_RUN ? -EIO : -EINVAL, m.seq);
			return;
		}
		sess = S_EOS;
		t_eos = t_prog = mono();
		return;
	}
	reply(i, VD_ERROR, -EINVAL, m.seq);
}

/* while a session runs: the queues, then what came of them */
static void sess_poll(void)
{
	if (sess != S_RUN && sess != S_EOS)
		return;
	if (vdec_poll(0)) {
		sess_fail(-EIO, sent);
		return;
	}
	struct vdec_status st;
	vdec_status(&st);
	double t = mono();
	if (st.acts != last_acts)
		last_acts = st.acts, t_prog = t;
	if (st.err || st.fw_dead) {
		sess_fail(st.err ? st.err : -EIO, sent);
		return;
	}
	if (sess == S_EOS && st.eos) {
		cred_drop();	/* vdcredit: they end with EOS_DONE */
		reply(owner, VD_EOS_DONE, 0, 0);
		sess = S_DONE;
		printf("venusd: EOS done %.0f ms after EOS, %d frames\n", 1e3 * (t - t_eos), st.frames);
		return;
	}
	/* venusctl decode's 3 s without a message; while the inputs are all queued too (the client can't send).
	 * vdcold: VENUSD_COLD_S before the session's first picture */
	double quiet = !t_first && cold_s > 3 ? cold_s : 3;
	if (t - t_prog > quiet && (sess == S_EOS || !vdec_can_etb())) {
		printf("venusd: no firmware progress for %.0f s (%s)\n", quiet, sname[sess]);
		if (sess == S_EOS) {
			cred_drop();
			reply(owner, VD_EOS_DONE, -ETIMEDOUT, 0);
			sess = S_DONE;
		} else {
			sess_fail(-ETIMEDOUT, sent);
		}
	}
}

/* vdcold: one tiny decode at start (VENUSD_WARMUP), then Venus down again: the first-use stall happens here */
static void warm_up(void)
{
	struct vdec_status st = { 0 };
	double t0 = mono(), t_up, lim = cold_s > 30 ? cold_s : 30;
	int r = 0, eos = 0;
	note("venusd: warm-up: Venus up, one %zu-byte 320x240 IDR, Venus down", sizeof warm_au);
	if (bring_up()) {
		note("venusd: warm-up: bring-up failed (OPEN brings it up again)");
		return;
	}
	t_up = mono();
	t_open = t_up, t_first = t_last_frame = 0;
	sess_hevc = 0;	/* the warm-up stream is H.264 */
	memcpy(au, warm_au, sizeof warm_au);
	r = sess_start(au, sizeof warm_au);
	if (!r && vdec_etb(au, sizeof warm_au, 0, 0, 0))
		r = -EIO;
	while (!r && !term) {
		if (!eos && vdec_can_etb()) {	/* the EOS once an input is free (a reconfig may hold them) */
			if (vdec_etb(NULL, 0, 0, 1, 1)) {
				r = -EIO;
				break;
			}
			eos = 1;
		}
		if (vdec_poll(1)) {
			r = -EIO;
			break;
		}
		vdec_status(&st);
		if (st.err || st.fw_dead)
			r = st.err ? st.err : -EIO;
		else if (st.eos)
			break;
		else if (mono() - t_up > lim)
			r = -ETIMEDOUT;
	}
	vdec_status(&st);
	note("venusd: warm-up %s (status %d): %d frame(s), first frame %.0f ms after the decoder start, %.0f ms in all"
	     " (bring-up %.0f ms)", r ? "FAILED" : "done", r, st.frames, t_first ? 1e3 * (t_first - t_up) : 0.0,
	     1e3 * (mono() - t0), 1e3 * (t_up - t0));
	sess_stop();
	sess = S_NONE;
	t_first = t_last_frame = 0;
	if (up)
		down("warm-up done");
}

/* vdstop: venus_core's STOP hook: back to the main loop (stop_jb) or out of recover() (rec_jb); unarmed: venus_core
 * exits as before */
static void on_stop(const char *what)
{
	if (!stop_armed)
		return;
	stop_armed = 0;
	note("venusd: venus_core STOP at %s: unwinding", what);
	longjmp(recovering ? rec_jb : stop_jb, 1);
}

/* vdstop: after a STOP unwound the current operation: the client told, the SFR, Venus down (downstream
 * handle_sys_error -> core_release: PAS shutdown + power off), the next OPEN runs bring-up */
static void recover(void)
{
	struct stat sb;
	nstops++;
	cred_drop();	/* vdcredit: the client gets an ERROR (or the session ends) below */
	note("venusd: recovering from STOP %u (session %u %s, %s)", nstops, nsess, sname[sess],
	     opening >= 0 ? "in an OPEN's bring-up" : ending ? "while ending it" : owner >= 0 ? "client attached" : "no client");
	if (opening >= 0) {
		reply(opening, VD_OPEN, -EIO, 0);
		opening = -1;
	} else if (ending) {	/* the client closed or went: finish sess_end */
		sess_log("ended (STOP while closing)");
		sess = S_NONE, owner = -1, ending = 0;
	} else if (owner >= 0 && (sess == S_OPEN || sess == S_RUN || sess == S_EOS)) {
		reply(owner, VD_ERROR, -EIO, sent);
		sess_log("failed (STOP)");
		sess = S_FAILED;
	} else if (owner < 0) {	/* warm-up, idle down */
		sess = S_NONE;
	}
	hfi = 0;	/* the HFI session goes with the firmware */
	vdec_sfr();
	vdec_abort();
	recovering = 1;
	if (setjmp(rec_jb)) {
		fault = 1;
		note("venusd: STOP again while recovering: no power off (the firmware may still run): LEFT POWERED,"
		     " reboot needed");
	} else {
		stop_armed = 1;
		cvp = 1;
		if (!stat(STATE, &sb)) {
			if (!vc_core_ok()) {
				note("venusd: recovery: a clock or GDSC still reads off: power on first (PAS shutdown needs it)");
				if (power_on()) {
					fault = 1;
					note("venusd: recovery: power on failed: no PAS shutdown, no power off: LEFT POWERED,"
					     " reboot needed");
				}
			}
			if (!fault) {
				up = 1;
				down("recovery after a STOP");	/* LEFT POWERED (fault) if the PAS shutdown fails */
			}
		} else if (!stat(AHB_STATE, &sb)) {
			note("venusd: recovery: %s without %s: powered, no firmware: power off", AHB_STATE, STATE);
			power_off();
			up = 0;
		} else {
			up = 0;
		}
	}
	stop_armed = 0;
	recovering = 0;
	vc_fake_off = 0;	/* a test's leftover fake reads */
	vdec_fw_reset();
	idle_since = mono();
	note("venusd: recovery done: %s", fault ? "LEFT POWERED, OPEN refused" : "Venus down, the next OPEN brings it up");
}

static void on_term(int s)
{
	(void)s;
	term = 1;
}

static int sock_create(void)	/* camd's sock_create */
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", VENUSD_SOCK);
	unlink(VENUSD_SOCK);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || chmod(VENUSD_SOCK, 0666) || listen(s, 4)) {
		perror(VENUSD_SOCK);
		return -1;
	}
	return s;
}

/* libsystem_kernel: jetsam control (root may change its own limits), as wland's auto_up() */
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);
struct memlimit_properties { int32_t active; uint32_t active_attr; int32_t inactive; uint32_t inactive_attr; };

int main(int argc, char **argv)
{
	const char *logp = "/var/local/venusd.log";
	if (argc == 3 && !strcmp(argv[1], "--log"))
		logp = argv[2];
	else if (argc != 1) {
		fprintf(stderr, "usage: venusd [--log FILE]\n");
		return 2;
	}
	struct stat onsb;
	if (stat(ON_FILE, &onsb)) {	/* redfin: off by default */
		fprintf(stderr, "venusd: %s missing: Venus stays off\n", ON_FILE);
		return 0;
	}
	int lk = open(LOCKF, O_RDWR | O_CREAT, 0644);
	if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) {
		fprintf(stderr, "venusd: already running (%s locked)\n", LOCKF);
		return 0;	/* not a crash: launchd leaves it */
	}
	if (!ftruncate(lk, 0))
		dprintf(lk, "%d\n", getpid());
	logfd = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (logfd < 0) {
		perror(logp);
		return 1;
	}
	dup2(logfd, 1);
	dup2(logfd, 2);
	stamp_on = !getenv("VENUSD_STAMP") || atoi(getenv("VENUSD_STAMP"));
	FILE *lf = stamp_on ? funopen(NULL, NULL, log_write, NULL, NULL) : NULL;	/* vdstamp */
	if (lf)
		stdout = stderr = lf;
	setvbuf(stdout, NULL, _IOLBF, 0);
	vc_quiet = !(getenv("VENUSD_VERBOSE") && atoi(getenv("VENUSD_VERBOSE")));
	if (getenv("VENUSD_IDLE_S"))
		idle_s = (unsigned)atoi(getenv("VENUSD_IDLE_S"));
	if (getenv("VENUSD_PHYS"))
		phys = strtoull(getenv("VENUSD_PHYS"), NULL, 0);
	if (getenv("VENUSD_BUF_MB"))
		buf_mb = strtoull(getenv("VENUSD_BUF_MB"), NULL, 0);
	if (getenv("VENUSD_IN_KB"))
		in_min = (uint32_t)strtoul(getenv("VENUSD_IN_KB"), NULL, 0) << 10;
	if (getenv("VENUSD_DEBUG"))
		debug = (uint32_t)strtoul(getenv("VENUSD_DEBUG"), NULL, 0);
	if (getenv("VENUSD_IDLE_CLIENT_S"))
		idle_cl_s = (unsigned)atoi(getenv("VENUSD_IDLE_CLIENT_S"));
	if (getenv("VENUSD_COLD_S"))
		cold_s = (unsigned)atoi(getenv("VENUSD_COLD_S"));
	int warm = !getenv("VENUSD_WARMUP") || atoi(getenv("VENUSD_WARMUP"));
	hevc_ok = !getenv("VENUSD_HEVC") || atoi(getenv("VENUSD_HEVC"));
	surf_on = !getenv("VENUSD_SURF") || atoi(getenv("VENUSD_SURF"));	/* vdcredit */
	if (!getenv("VENUSD_RECOVER") || atoi(getenv("VENUSD_RECOVER")))
		vc_stop_hook = on_stop;
	vc_frame_tag = !getenv("VENUSD_FW_FRAME") || atoi(getenv("VENUSD_FW_FRAME"));
	test_arm(getenv("VENUSD_TEST_CLKOFF"), "VENUSD_TEST_CLKOFF");
	struct stat sb;
	fw = stat("/vendor/firmware/venus.mdt", &sb) == 0 ? "/vendor/firmware" : "/var/local/fw/venus";	/* venusctl's */
	/* Each credited IOSurface lookup maps the whole surface and charges it to our footprint (iokit_mapped): launchd's
	 * daemon limit killed the first one (crA/crD: jetsam reason 7); JetsamProperties in the job is ignored, as for
	 * wland. 512 MB covers 16 surfaces at 4K. VENUSD_MEM_MB=0 leaves launchd's limit. */
	int mem_mb = getenv("VENUSD_MEM_MB") ? atoi(getenv("VENUSD_MEM_MB")) : 512;
	if (mem_mb > 0) {
		struct memlimit_properties old = { 0 }, ml = { mem_mb, 1, mem_mb, 1 };	/* MB, fatal */
		memorystatus_control(8, getpid(), 0, &old, sizeof(old));	/* GET_MEMLIMIT_PROPERTIES */
		int mrc = memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));	/* SET_MEMLIMIT_PROPERTIES */
		note("venusd: memory limit %d/%d MB -> %d MB: %d", old.active, old.inactive, mem_mb, mrc);
	}
	{	/* redfin xnu 1057: how /dev/pmem maps the Venus buffer window (0 or missing: device memory, ~14 MB/s copy-out) */
		int kv = -1;
		size_t kn = sizeof(kv);
		if (sysctlbyname("kern.sweet_venus", &kv, &kn, NULL, 0))
			kv = -1;
		note("venusd: kern.sweet_venus %d: %s", kv, kv == 2 ? "cached window + cache maintenance" :
		     kv == 1 ? "write-combined window" : kv == 0 ? "device mapping (slow copy-out; boot-arg sweetvenus=1|2)" :
		     "no xnu 1057 (device mapping, slow copy-out)");
	}
	au = malloc(VENUSD_MAX_AU);
	if (!au)
		return 1;
	note("venusd: start, pid %d, idle %u s, idle client %u s, max %d clients, queues %#llx, window %llu MB, inputs >= %u KB,"
	     " quiet %d, cold %u s, warm-up %d, recover %d, hevc %d, surf %d", getpid(), idle_s, idle_cl_s, MAXCL,
	     (unsigned long long)phys, (unsigned long long)buf_mb, in_min >> 10, vc_quiet, cold_s, warm, vc_stop_hook != NULL,
	     hevc_ok, surf_on);
	pm_init();
	stale_drop(STATE);
	stale_drop(AHB_STATE);
	if (!stat(STATE, &sb))	/* a venusd (or venusctl) of this boot died with Venus up */
		down("STATE left from an earlier run");
	else if (!stat(AHB_STATE, &sb)) {
		note("venusd: %s without %s: powered, no firmware: power off", AHB_STATE, STATE);
		cvp = 1;
		power_off();
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	vdec_sink = sink;
	vdec_place = place;	/* vdcredit */
	int ls = sock_create();
	if (ls < 0)
		return 1;
	for (int i = 0; i < MAXCL; i++)
		cl[i].fd = -1, cl[i].gone = 0;
	if (setjmp(stop_jb))	/* vdstop: a venus_core STOP anywhere below unwinds to here */
		recover();
	stop_armed = vc_stop_hook != NULL;
	if (warm && !fault && !warmed++)	/* clients that connect meanwhile wait in the listen backlog */
		warm_up();
	idle_since = mono();
	while (!term) {
		struct pollfd p[MAXCL + 1];
		int n = 0, idx[MAXCL];
		p[n].fd = ls, p[n].events = POLLIN, p[n++].revents = 0;
		for (int i = 0; i < MAXCL; i++) {
			/* the owner's next message only once it can be taken: an input buffer free (backpressure) */
			if (cl[i].fd < 0 || cl[i].gone || (i == owner && sess == S_RUN && !vdec_can_etb()))
				continue;
			idx[n - 1] = i, p[n].fd = cl[i].fd, p[n].events = POLLIN, p[n++].revents = 0;
		}
		int active = sess == S_RUN || sess == S_EOS;
		/* videoprof 2026-11-03: a 1 ms tick while a session runs was ~40 % of a core (1 kHz wakeups); the msgq has no
		 * IRQ, so poll it every VENUSD_TICK_MS (default 4: 30 fps frames are 33 ms apart, 18 DPBs queued) */
		static int tick = -1;
		if (tick < 0) {
			const char *e = getenv("VENUSD_TICK_MS");
			tick = e && atoi(e) > 0 ? atoi(e) : 4;
		}
		if (poll(p, (nfds_t)n, active ? tick : 1000) > 0) {
			if (p[0].revents & POLLIN) {
				int fd = accept(ls, NULL, NULL), k;
				for (k = 0; fd >= 0 && k < MAXCL && cl[k].fd >= 0; k++)
					;
				if (fd >= 0 && k == MAXCL) {
					printf("venusd: too many clients (%d connected): fd %d refused\n", MAXCL, fd);
					close(fd);
				} else if (fd >= 0) {
					struct timeval tv = { 2, 0 };	/* a stuck client can't hold the daemon for long */
					int sz = 4 << 20;
					setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
					setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
					setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
					cl[k].fd = fd, cl[k].gone = 0, cl[k].t_msg = mono();
					printf("venusd: client fd %d connected, %d connected\n", fd, nclients());
				}
			}
			for (int j = 1; j < n; j++)
				if (p[j].revents & POLLNVAL && cl[idx[j - 1]].fd >= 0)
					drop_client(idx[j - 1], "POLLNVAL");	/* vdleak; HUP/ERR: the read in command() */
				else if (p[j].revents & (POLLIN | POLLHUP | POLLERR) && cl[idx[j - 1]].fd >= 0)
					command(idx[j - 1]);
		}
		sess_poll();
		for (int i = 0; i < MAXCL; i++)
			if (cl[i].fd >= 0 && cl[i].gone)
				drop_client(i, "write failed");
		/* vdleak: a connection that says nothing without a session, or with one that has nothing to do (opened, EOS
		 * done, failed): a vtvenus instance its host never released (a held session refused every other OPEN) */
		for (int i = 0; i < MAXCL && idle_cl_s; i++)
			if (cl[i].fd >= 0 && opening != i && (i != owner || sess == S_OPEN || sess == S_DONE || sess == S_FAILED) &&
			    mono() - cl[i].t_msg > idle_cl_s) {
				char why[64];
				snprintf(why, sizeof why, "idle %u s, %s", idle_cl_s, i == owner ? sname[sess] : "no session");
				drop_client(i, why);
			}
		if (up && owner < 0 && mono() - idle_since > idle_s)
			down("idle");
		if (!fstat(logfd, &sb) && sb.st_size > LOG_MAX && !ftruncate(logfd, 0))
			printf("venusd: log cut at %d B\n", LOG_MAX);
	}
	if (owner >= 0)
		sess_end("closed (venusd stopping)");
	if (up)
		down("venusd stopping");
	note("venusd: exit (signal)%s", fault ? ": LEFT POWERED" : "");
	unlink(VENUSD_SOCK);
	return 1;	/* as camd: launchd (KeepAlive SuccessfulExit false) starts it again after a kill */
}
