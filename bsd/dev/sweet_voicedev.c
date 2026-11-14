/*
 * redfin (Pixel 5): /dev/redwood-voice, a call's PCM for userland (the RedwoodAudio plug-in's "Baseband Voice" device,
 * BBA stage 4). Patch 1091. Only used with kern.sweet_voice_proxy 2 (sweet_voice.c, read at a call's start): the
 * downlink frames the DSP renders on AFE RT proxy port 0x2000 come out of read() here instead of being played by the
 * kernel, and what write() takes goes to the uplink port 0x2001 instead of the microphone (sweet_vproxy.c dl_frame()/
 * ul_frame()). Modes 0 and 1 never call into this file; the node exists from boot and is idle without a mode-2 call.
 *
 *   read()   downlink PCM: 16-bit signed mono, native endian, at the call's rate; whole 10 ms frames (rate / 100
 *            samples) only (a buffer smaller than one frame: EINVAL). Blocks until a frame is there, at most 200 ms
 *            (then EAGAIN; O_NONBLOCK: EAGAIN at once). No call (or the call ended while blocked): 0 bytes at once.
 *            A reader more than 200 ms behind skips to the newest 20 ms (skipped samples: dl_dropped).
 *   write()  uplink PCM, same format, any even byte count (odd: EINVAL); more than 200 ms queued: the excess is
 *            dropped (ul_dropped) and the write still succeeds. Without a mode-2 call: taken and discarded.
 *            What is missing when the DSP wants a frame goes as silence (ul_underruns, counted once data came).
 *   ioctl    the numbers below; the plug-in copies them (research/cc/BBA-stage1-notes.md has them too).
 *   One opener at a time (EBUSY), uid/gid 501 (mobile: mediaserverd) and root, mode 0660.
 * Nobody has it open during a mode-2 call: silence both ways, logged once per call.
 * Nothing here sleeps or allocates on the proxy's data thread: fixed rings (single producer / single consumer, indices
 * only moved by their owner), thread_wakeup() for a blocked reader. Log lines: "SWEET VOICE DEV: ...".
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define SAY(...)                printf("SWEET VOICE DEV: " __VA_ARGS__)

/* ---- the interface (stable: the plug-in has a copy) ---- */
struct redwood_voice_state {            /* 32 bytes */
	uint32_t call_up;               /* 1: a mode-2 call runs on the proxy ports (frames move) */
	uint32_t rate;                  /* the running call's rate, else the rate the next call will use (8000/16000) */
	uint32_t frame_samples;         /* rate / 100 */
	uint32_t mode;                  /* kern.sweet_voice_proxy of the running call (2), 0 none */
	uint32_t dl_queued;             /* downlink samples waiting for read() */
	uint32_t ul_queued;             /* uplink samples written, not yet sent */
	uint32_t call_count;            /* mode-2 calls since boot (changes at each call start) */
	uint32_t reserved;
};
struct redwood_voice_time {             /* 24 bytes */
	uint64_t sample;                /* downlink samples the DSP gave this call (end of the latest frame) */
	uint64_t mach_time;             /* mach_absolute_time() of that frame's READ_V2 ack; 0 = no frame yet */
	uint32_t rate;
	uint32_t call_count;
};
struct redwood_voice_counters {         /* 32 bytes, this call (zeroed at call start) */
	uint32_t dl_frames;             /* frames from the DSP */
	uint32_t ul_frames;             /* frames to the DSP */
	uint32_t dl_dropped;            /* samples lost: reader > 200 ms behind, or the ring full */
	uint32_t ul_dropped;            /* samples written beyond 200 ms of queue */
	uint32_t ul_underruns;          /* samples sent as silence (after the first write of the call) */
	uint32_t dl_unread;             /* frames that came while nobody had the device open */
	uint32_t slips;                 /* reserved for +-1 sample slips (0: none here; sweet_vproxy has its own) */
	uint32_t reserved;
};
#define REDWOOD_VOICE_GET_STATE         _IOR('v', 1, struct redwood_voice_state)
#define REDWOOD_VOICE_SET_RATE          _IOW('v', 2, uint32_t)          /* 8000 / 16000: the next call's rate */
#define REDWOOD_VOICE_GET_TIME          _IOR('v', 3, struct redwood_voice_time)
#define REDWOOD_VOICE_GET_COUNTERS      _IOR('v', 4, struct redwood_voice_counters)

/* ---- kernel side (sweet_vproxy.c) ---- */
void sweet_voicedev_init(void);
void sweet_voicedev_call(bool up, uint32_t rate, int mode);
void sweet_voicedev_dl(const int16_t *s, uint32_t n, uint64_t mach);
void sweet_voicedev_ul(int16_t *out, uint32_t n);
uint32_t sweet_voicedev_rate_wanted(void);

#define RING                    8192u           /* samples per direction, a power of two (512 ms at 16 kHz) */
#define WAIT_MS                 200
#define MOBILE_ID               501

static int16_t dl_ring[RING], ul_ring[RING];
static volatile uint32_t dl_head, dl_tail;      /* head: the data thread; tail: the reader */
static volatile uint32_t ul_head, ul_tail;      /* head: the writer; tail: the data thread */
static volatile uint32_t dl_call_start;         /* dl_head at the call's start (the reader skips what was before) */
static uint32_t reader_call;                    /* call_count the reader last saw */
static volatile uint32_t call_up, call_rate, call_mode, call_count, rate_wanted;
static volatile uint32_t ul_got_data;           /* a write() came during this call */
static volatile SInt32 opened;
static volatile bool said_nobody;
static volatile uint32_t tseq;                  /* even: stable */
static volatile uint64_t t_sample, t_mach;
static struct redwood_voice_counters cnt;
static int dl_chan;                             /* wait channel of a blocked reader */
static int vd_major = -1;

static uint32_t
frame_samples(void)
{
	return (call_rate ? call_rate : 16000) / 100;
}

uint32_t
sweet_voicedev_rate_wanted(void)
{
	return rate_wanted;
}

/* From sweet_vproxy.c on the voice thread: after the data thread was started (up) / after it stopped (down). */
void
sweet_voicedev_call(bool up, uint32_t rate, int mode)
{
	if (up) {
		bzero(&cnt, sizeof(cnt));
		tseq++;
		OSMemoryBarrier();
		t_sample = 0;
		t_mach = 0;
		OSMemoryBarrier();
		tseq++;
		call_rate = rate;
		call_mode = (uint32_t)mode;
		ul_got_data = 0;
		said_nobody = false;
		dl_call_start = dl_head;
		ul_tail = ul_head;      /* stale uplink of an earlier call: never sent */
		OSMemoryBarrier();
		call_count++;
		call_up = 1;
		SAY("call up: mode %d, %u Hz, %u-sample frames; /dev/redwood-voice %s\n", mode, rate, rate / 100,
		    opened ? "open: PCM to/from userland" : "NOT OPEN: silence both ways until it is");
		if (!opened) {
			said_nobody = true;
		}
		return;
	}
	if (!call_up) {
		return;
	}
	call_up = 0;
	OSMemoryBarrier();
	thread_wakeup(&dl_chan);        /* a blocked reader returns 0 bytes */
	SAY("call down: dl %u frames (%u unread, %u samples dropped), ul %u frames (%u underrun samples, %u dropped)\n",
	    cnt.dl_frames, cnt.dl_unread, cnt.dl_dropped, cnt.ul_frames, cnt.ul_underruns, cnt.ul_dropped);
}

/* Data thread: one downlink frame (n samples) the DSP gave at mach. Never blocks. */
void
sweet_voicedev_dl(const int16_t *s, uint32_t n, uint64_t mach)
{
	cnt.dl_frames++;
	tseq++;
	OSMemoryBarrier();
	t_sample += n;
	t_mach = mach;
	OSMemoryBarrier();
	tseq++;
	if (!opened) {
		cnt.dl_unread++;
		if (!said_nobody) {
			said_nobody = true;
			SAY("mode 2 call with nobody on /dev/redwood-voice: silence both ways (once per call)\n");
		}
		return;
	}
	uint32_t head = dl_head, tail = __atomic_load_n(&dl_tail, __ATOMIC_ACQUIRE);
	if (head - tail + n > RING) {
		__atomic_fetch_add(&cnt.dl_dropped, n, __ATOMIC_RELAXED);      /* the reader is far behind: this frame is lost */
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		dl_ring[(head + i) & (RING - 1)] = s[i];
	}
	__atomic_store_n(&dl_head, head + n, __ATOMIC_RELEASE);
	thread_wakeup(&dl_chan);
}

/* Data thread: the next n uplink samples for the DSP. Never blocks; what is missing is silence. */
void
sweet_voicedev_ul(int16_t *out, uint32_t n)
{
	uint32_t got = 0;

	cnt.ul_frames++;
	if (opened) {
		uint32_t tail = ul_tail, head = __atomic_load_n(&ul_head, __ATOMIC_ACQUIRE), avail = head - tail;

		got = avail < n ? avail : n;
		for (uint32_t i = 0; i < got; i++) {
			out[i] = ul_ring[(tail + i) & (RING - 1)];
		}
		__atomic_store_n(&ul_tail, tail + got, __ATOMIC_RELEASE);
	} else if (!said_nobody) {
		said_nobody = true;
		SAY("mode 2 call with nobody on /dev/redwood-voice: silence both ways (once per call)\n");
	}
	if (got < n && ul_got_data) {
		cnt.ul_underruns += n - got;
	}
	for (uint32_t i = got; i < n; i++) {
		out[i] = 0;
	}
}

/* ---- the device ---- */
static int
vd_open(__unused dev_t dev, __unused int flags, __unused int devtype, proc_t p)
{
	if (!OSCompareAndSwap(0, 1, &opened)) {
		return EBUSY;
	}
	reader_call = call_count;
	if (call_up) {
		/* opened during a call: start from what is there now */
		__atomic_store_n(&dl_tail, dl_head, __ATOMIC_RELEASE);
	}
	SAY("opened by pid %d %s%s\n", proc_pid(p), p ? proc_best_name(p) : "?", call_up ? " (during a mode-2 call)" : "");
	return 0;
}

static int
vd_close(__unused dev_t dev, __unused int flags, __unused int devtype, proc_t p)
{
	SAY("closed (pid %d)%s\n", p ? proc_pid(p) : -1, call_up ? " during a mode-2 call: silence both ways from now" : "");
	OSMemoryBarrier();
	opened = 0;
	thread_wakeup(&dl_chan);
	return 0;
}

static int
vd_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	for (;;) {
		uint32_t fs = frame_samples(), most = fs * (WAIT_MS / 10);
		uint32_t tail = dl_tail, head = __atomic_load_n(&dl_head, __ATOMIC_ACQUIRE);
		uint32_t want = (uint32_t)(uio_resid(uio) / 2 / fs) * fs;

		if (want == 0) {
			return EINVAL;
		}
		if (reader_call != call_count) {
			/* a new call: what was left of the earlier one is not this call's audio */
			reader_call = call_count;
			uint32_t start = dl_call_start;
			if ((int32_t)(start - tail) > 0) {
				tail = start;
			}
		}
		uint32_t avail = head - tail;
		if (avail > most) {
			uint32_t keep = 2 * fs, skip = avail - keep;
			__atomic_fetch_add(&cnt.dl_dropped, skip, __ATOMIC_RELAXED);
			tail += skip;
			avail = keep;
		}
		__atomic_store_n(&dl_tail, tail, __ATOMIC_RELEASE);
		if (avail >= fs) {
			uint32_t n = avail - avail % fs;
			if (n > want) {
				n = want;
			}
			uint32_t idx = tail & (RING - 1), first = RING - idx < n ? RING - idx : n;
			int e = uiomove((caddr_t)&dl_ring[idx], (int)(first * 2), uio);
			if (e == 0 && n > first) {
				e = uiomove((caddr_t)&dl_ring[0], (int)((n - first) * 2), uio);
			}
			__atomic_store_n(&dl_tail, tail + n, __ATOMIC_RELEASE);
			return e;
		}
		if (!call_up) {
			return 0;
		}
		if (ioflag & IO_NDELAY) {
			return EAGAIN;
		}
		assert_wait_timeout((event_t)&dl_chan, THREAD_ABORTSAFE, WAIT_MS, NSEC_PER_MSEC);
		if (__atomic_load_n(&dl_head, __ATOMIC_ACQUIRE) != head || !call_up || !opened) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			continue;
		}
		wait_result_t w = thread_block(THREAD_CONTINUE_NULL);
		if (w == THREAD_INTERRUPTED) {
			return EINTR;
		}
		if (w == THREAD_TIMED_OUT && __atomic_load_n(&dl_head, __ATOMIC_ACQUIRE) == head && call_up) {
			return EAGAIN;
		}
	}
}

static int
vd_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	user_ssize_t resid = uio_resid(uio);

	if (resid & 1) {
		return EINVAL;
	}
	if (!call_up) {
		uio_update(uio, resid);         /* no mode-2 call: taken and discarded */
		return 0;
	}
	ul_got_data = 1;
	uint32_t most = frame_samples() * (WAIT_MS / 10);
	while (uio_resid(uio) > 0) {
		uint32_t head = ul_head, tail = __atomic_load_n(&ul_tail, __ATOMIC_ACQUIRE), queued = head - tail;
		uint32_t n = (uint32_t)(uio_resid(uio) / 2);

		if (queued >= most) {
			cnt.ul_dropped += n;
			uio_update(uio, uio_resid(uio));
			break;
		}
		if (n > most - queued) {
			n = most - queued;
		}
		uint32_t idx = head & (RING - 1), first = RING - idx < n ? RING - idx : n;
		int e = uiomove((caddr_t)&ul_ring[idx], (int)(first * 2), uio);
		if (e == 0 && n > first) {
			e = uiomove((caddr_t)&ul_ring[0], (int)((n - first) * 2), uio);
		}
		if (e != 0) {
			return e;
		}
		__atomic_store_n(&ul_head, head + n, __ATOMIC_RELEASE);
	}
	return 0;
}

static int
vd_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused proc_t p)
{
	switch (cmd) {
	case REDWOOD_VOICE_GET_STATE: {
		struct redwood_voice_state *s = (struct redwood_voice_state *)data;
		bzero(s, sizeof(*s));
		s->call_up = call_up;
		s->rate = call_up ? call_rate : (rate_wanted ? rate_wanted : 16000);
		s->frame_samples = s->rate / 100;
		s->mode = call_up ? call_mode : 0;
		s->dl_queued = dl_head - dl_tail;
		s->ul_queued = ul_head - ul_tail;
		s->call_count = call_count;
		return 0;
	}
	case REDWOOD_VOICE_SET_RATE: {
		uint32_t r = *(uint32_t *)data;
		if (r != 8000 && r != 16000) {
			return EINVAL;
		}
		rate_wanted = r;
		SAY("rate %u Hz for the next mode-2 call%s\n", r, call_up ? " (the running call keeps its rate)" : "");
		return 0;
	}
	case REDWOOD_VOICE_GET_TIME: {
		struct redwood_voice_time *t = (struct redwood_voice_time *)data;
		uint32_t s1, s2;
		do {
			s1 = tseq;
			OSMemoryBarrier();
			t->sample = t_sample;
			t->mach_time = t_mach;
			OSMemoryBarrier();
			s2 = tseq;
		} while (s1 != s2 || (s1 & 1));
		t->rate = call_up ? call_rate : 0;
		t->call_count = call_count;
		return 0;
	}
	case REDWOOD_VOICE_GET_COUNTERS:
		memcpy(data, &cnt, sizeof(cnt));
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw vd_cdevsw = {
	.d_open       = vd_open,
	.d_close      = vd_close,
	.d_read       = vd_read,
	.d_write      = vd_write,
	.d_ioctl      = vd_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

void
sweet_voicedev_init(void)
{
	if (vd_major >= 0) {
		return;
	}
	vd_major = cdevsw_add(-1, &vd_cdevsw);
	void *n = vd_major < 0 ? NULL : devfs_make_node(makedev(vd_major, 0), DEVFS_CHAR, MOBILE_ID, MOBILE_ID, 0660,
	    "redwood-voice");
	SAY("/dev/redwood-voice %s (major %d; used only with kern.sweet_voice_proxy 2)\n", n ? "ready" : "FAILED", vd_major);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
