/*
 * redfin (Pixel 5): a call's PCM through the AP, over the audio DSP's AFE real-time proxy ports. Patch 1090
 * (experiment E1 / S1b of research/cc/BBA-design-study.md, sections B1-B3).
 *
 * With kern.sweet_voice_proxy 1 (sweet_voice.c; 0 = default = nothing here runs) the call's voice processor (CVP)
 * uses the AFE RT proxy ports instead of the TDM ports: RX RT_PROXY_PORT_001_RX 0x2000 (the DSP renders the downlink
 * there and the AP reads it) and TX RT_PROXY_PORT_001_TX 0x2001 (the AP writes the uplink, the DSP captures it).
 * This file is the AP's side of those ports, and in mode 1 the kernel loops the PCM straight back to today's hardware
 * paths: downlink frames to the speakers' playback stream (sweet_sound.c, in its call mode of 3 x 10 ms periods, to
 * QUIN_TDM_RX_0 0x9040 and the call's amplifier), the microphone ring (sweet_mic.c, PRI_TDM_TX_0 0x9001, 48 kHz)
 * averaged down to the proxy's rate and written to 0x2001.
 *
 * Reference: the redbull kernel techpack/audio: dsp/q6afe.c afe_register_get_events() (AFE_SERVICE_CMD_REGISTER_RT_PORT
 * _DRIVER, src/dest port 1), afe_cmd_memory_map() (AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS, pool SHMEM8_4K 3),
 * afe_rt_proxy_port_read()/_write() (AFE_PORT_DATA_CMD_RT_PROXY_PORT_READ_V2/WRITE_V2: the frames live in mapped
 * memory, not in the APR packet -- what msm-pcm-afe-v2.c does; there is no in-band variant), afe_callback() (the data
 * acks are APR_BASIC_RSP_RESULT with payload[0] = the opcode; AFE_EVENT_RT_PROXY_PORT_STATUS payload u16 port, u16
 * event); include/dsp/apr_audio-v2.h struct afe_param_id_rt_proxy_port_cfg (AFE_PARAM_ID_RT_PROXY_CONFIG 0x10213),
 * asoc/msm-dai-q6-v2.c (frame_size = the period, jitter_allowance = half of it, water marks 0; 16-bit only).
 *
 * Data path: two READ_V2 kept outstanding on 0x2000 and two WRITE_V2 on 0x2001 (10 ms frames: 320 bytes at 16 kHz,
 * 160 at 8 kHz), each sent again from this file's thread when its ack comes (the DSP paces both ways; the AFE's
 * handler only marks the slot done: sweet_adsp.c redwood_adsp_apr_events_proxy(), on the link's thread, never waits).
 * The tokens are 0x51xxxxxx (read) / 0x52xxxxxx (write), never a waiter's (those stay below 0x01000000).
 * Slip (+-1 sample, the clocks share the 19.2 MHz crystal): once a second, if the call ring stayed over 20 ms the
 * next second's downlink frames lose one 48 kHz frame each, after underruns they gain one; the same for the mic ring
 * (over 60 ms: one sample more consumed per frame, after underruns one less).
 *
 * 1091: kern.sweet_voice_proxy 2 = the same ports and thread, but the PCM goes to userland: downlink frames to
 * /dev/redwood-voice's read() (sweet_voicedev_dl) instead of the speakers' call ring, uplink frames from its write()
 * (sweet_voicedev_ul) instead of the mic ring; the call mode stays on (iOS's own playback in 10 ms periods). The rate
 * is the one set with REDWOOD_VOICE_SET_RATE, else opts 2 as in mode 1. Nobody on the device: silence both ways.
 *
 * Log lines: "SWEET VOICE PROXY: ..." (the study's B2 list), 1 s statistics for the first 10 s of a call, then every
 * 10 s, and the totals at the call's end; kern.sweet_voice_proxy_stats (read only) has them too.
 *
 * 1095 (pacing): the DSP answers READ_V2/WRITE_V2 at once (APR_BASIC_RSP_RESULT within ~0.6 ms, whatever the rate);
 * the answer is not a "10 ms of data is ready" signal. 1090 re-sent each read/write on its answer, so ~1700 frames a
 * second went each way (17 x real time): the downlink was read out of the DSP's jitter buffer in fragments and
 * overfilled the call ring, the uplink drained the mic ring. msm-pcm-afe-v2.c (mmap mode) paces both ports with an
 * hrtimer of one period (poll_time = period bytes / bytes per second): each tick one READ_V2 / WRITE_V2 at the next
 * period of a multi-period buffer (dsp_cnt walks 0..periods-1), and the answer only means "that period is done"
 * (snd_pcm_period_elapsed). kern.sweet_voice_proxy_pace 1 (default; read at call start) does the same here: a
 * thread on an absolute 10 ms deadline (mach time, no drift), PACE_PERIODS periods per direction laid out back to
 * back in the mapped buffer, one read on 0x2000 and one write on 0x2001 per tick at the advancing period, a period
 * used again only after its answer came (else "busy", counted); the downlink frame is taken when its read is
 * answered, in issue order. The call ring is held at 30-60 ms: a frame that would take it past 60 ms is dropped
 * ("trim"), over 30 ms at the least for a second -> -1 sample per frame for the next second, underruns -> +1.
 * kern.sweet_voice_proxy_pace 0: exactly 1090's data path. kern.sweet_voice_proxy_gain_dl / _ul: dB (-24..+24,
 * default 0 = untouched), live. 1 s lines for the first 30 s of a paced call (the ringback from ALERTING on).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/redwood_audio.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t thread_terminate(thread_t thread);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa);

bool sweet_vproxy_prepare(int mode, uint32_t opts);
bool sweet_vproxy_run(void);
void sweet_vproxy_stop(const char *why);
void sweet_vproxy_dtmf(uint32_t low, uint32_t high);
uint32_t sweet_vproxy_rate(void);
/* 1091: mode 2, the call's PCM to/from userland (bsd/dev/sweet_voicedev.c, /dev/redwood-voice) */
extern void sweet_voicedev_call(bool up, uint32_t rate, int mode);
extern void sweet_voicedev_dl(const int16_t *s, uint32_t n, uint64_t mach);
extern void sweet_voicedev_ul(int16_t *out, uint32_t n);
extern uint32_t sweet_voicedev_rate_wanted(void);

#define SAY(...)                printf("SWEET VOICE PROXY: " __VA_ARGS__)

#define APR_SVC_AFE                                     4
#define APR_BASIC_RSP_RESULT                            0x000110e8u
#define APR_WAIT_MS                                     1000
#define AFE_SERVICE_CMD_REGISTER_RT_PORT_DRIVER         0x000100e0u
#define AFE_SERVICE_CMD_UNREGISTER_RT_PORT_DRIVER       0x000100e1u
#define AFE_PORT_CMD_DEVICE_START                       0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP                        0x000100e6u
#define AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS          0x000100eau
#define AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS       0x000100ebu
#define AFE_SERVICE_CMD_SHARED_MEM_UNMAP_REGIONS        0x000100ecu
#define AFE_PORT_DATA_CMD_RT_PROXY_PORT_WRITE_V2        0x000100edu
#define AFE_PORT_DATA_CMD_RT_PROXY_PORT_READ_V2         0x000100eeu
#define AFE_PORT_CMD_SET_PARAM_V2                       0x000100efu
#define AFE_EVENT_RT_PROXY_PORT_STATUS                  0x00010105u
#define AFE_MODULE_AUDIO_DEV_INTERFACE                  0x0001020cu
#define AFE_PARAM_ID_RT_PROXY_CONFIG                    0x00010213u
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL                  3

#define PORT_RX                 0x2000          /* RT_PROXY_PORT_001_RX: the DSP renders, the AP reads (downlink) */
#define PORT_TX                 0x2001          /* RT_PROXY_PORT_001_TX: the AP writes, the DSP captures (uplink) */
#define AUDIO_SID_BITS          (1ull << 32)    /* sweet_sound.c: stream 0x1401's low bits, which the DSP strips */
#define MEM_BYTES               (32 * 1024)     /* 4 download pages, 4 upload pages */
#define SLOTS                   4
#define IN_FLIGHT               2
#define UL_OFFSET               (16 * 1024)
#define DL_TOKEN                0x51000000u
#define UL_TOKEN                0x52000000u
#define OPT_8K                  2u              /* kern.sweet_voice_proxy_opts (sweet_voice.c) */
#define OUT_RATE                48000
#define MIC_KEEP                1920            /* 40 ms behind the mic ring's writer */
#define MIC_MOST                4800            /* more than 100 ms behind: skipped forward (overrun) */
/* 1095: the paced data path */
#define PACE_PERIODS            4               /* periods per direction, back to back (msm-pcm-afe-v2.c's dsp_cnt ring) */
#define PACE_LOST_TICKS         4               /* a read/write not answered within 4 periods: given up (counted "lost") */
#define DL_RING_HIGH            2880            /* 60 ms at 48 kHz: a frame that would go past it is dropped (trim) */
#define DL_RING_SLIP            1440            /* 30 ms: the least fill over a second above it -> -1 sample per frame */
#define PACED_LINES_S           30              /* 1 s lines for the first 30 s (ringback included) */

static LCK_GRP_DECLARE(vproxy_lock_group, "sweet-vproxy");
static LCK_MTX_DECLARE(stats_lock, &vproxy_lock_group);

static struct {
	vm_offset_t va;
	uint64_t pa;
	uint32_t handle, rate, samples, bytes;  /* frame: samples (10 ms), bytes */
	int mode;
	bool space, hooked, mapped, reg[2], started[2];
} px;
static volatile uint32_t dl_done, ul_done, dl_bad, ul_bad;      /* slots acked (bits), and acked with an error */
static volatile uint64_t dl_at[SLOTS];          /* mach time of each read's ack */
static volatile bool stop_req, thread_runs, running;
static int px_wake;
static volatile uint32_t ev_start, ev_stop, ev_lwm, ev_hwm;
/* 1095 */
static int vp_pace = 1;                         /* kern.sweet_voice_proxy_pace: 1 paced (default), 0 1090's path */
static int vp_gain_db[2];                       /* kern.sweet_voice_proxy_gain_dl / _ul, dB, -24..+24 */
static bool paced;                              /* this call's data path (vp_pace at its start) */
static volatile uint32_t ev_wm[2][2];           /* [0x2000, 0x2001][LOW_WM, HIGH_WM] */
static int16_t dl_last[160];                    /* the previous downlink frame (duplicate check) */
static bool dl_last_ok;
static const uint32_t gain_q12[49] = {          /* round(4096 * 10^(dB / 20)), -24..+24 dB */
	258, 290, 325, 365, 410, 460, 516, 579, 649, 728, 817, 917, 1029, 1154, 1295, 1453, 1631, 1830, 2053, 2303, 2584, 2900,
	3254, 3651, 4096, 4596, 5157, 5786, 6492, 7284, 8173, 9170, 10289, 11544, 12953, 14533, 16306, 18296, 20529, 23034,
	25844, 28997, 32536, 36506, 40960, 45958, 51566, 57858, 64917,
};

/* statistics: per window (1 s) and per call */
struct win {
	uint32_t dl, dl_fail, ul, ul_fail, dl_peak, ul_peak;
	uint32_t gap_min, gap_max, gaps;
	uint64_t gap_sum;
	uint32_t dl_ring_min, dl_ring_max, ul_ring_min, ul_ring_max, rings, dl_rings;
	uint64_t dl_ring_sum, ul_ring_sum;
	uint32_t dl_under, ul_under, dl_over, ul_over;
	int32_t dl_slip, ul_slip;
	uint32_t dl_busy, ul_busy, late, dup, zero, trim, lost; /* 1095 */
	uint32_t wm[4];                                 /* 1095: AFE LOW_WM/HIGH_WM events, rx lo, rx hi, tx lo, tx hi */
};
static struct win cw, ct;              /* this second, this call */
static uint64_t call_began_us, last_dl_mach;
static int dl_slip_mode, ul_slip_mode;          /* -1 drop one frame/sample per frame, +1 add one, 0 none */
static uint64_t ul_cursor;
static bool ul_primed;
static int16_t dl_prev;
static uint32_t sound_underruns_seen;
static int32_t dtmf_last_ms = 0x7fffffff;
static char stats_text[512];

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static uint64_t
mach_us(uint64_t t)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(t, &ns);
	return ns / 1000;
}

static const char *
dsp_error(uint32_t status)
{
	static const char *const names[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected", "panic",
		                             "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted",
		                             "preempted", "continue", "immediate", "not implemented", "need more", "no memory",
		                             "not there" };

	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown error";
}

/* One AFE command and its answer (the shared waiter: setup and teardown only, on the voice thread). */
static bool
afe(uint16_t port, uint32_t opcode, const void *payload, uint32_t length, uint32_t wanted, struct redwood_apr_answer *answer,
    const char **err)
{
	struct redwood_apr_answer local;

	if (answer == NULL) {
		answer = &local;
	}
	bzero(answer, sizeof(*answer));
	*err = "ok";
	if (!redwood_adsp_apr(APR_SVC_AFE, port, 0, opcode, payload, length, APR_WAIT_MS, answer)) {
		*err = "no answer";
		return false;
	}
	if (answer->opcode == APR_BASIC_RSP_RESULT && answer->words[0] == opcode) {
		*err = dsp_error(answer->words[1]);
		return answer->words[1] == 0 && wanted == 0;
	}
	if (wanted != 0 && answer->opcode == wanted) {
		return true;
	}
	*err = "unexpected answer";
	return false;
}

/* ---- the AFE's events and data acks: on the link's thread, never waits ---- */

static bool
px_events(__unused uint8_t service, __unused uint16_t port, uint32_t opcode, uint32_t token, const uint32_t *payload,
    uint32_t length)
{
	if (opcode == AFE_EVENT_RT_PROXY_PORT_STATUS && length >= 4) {
		uint32_t p = payload[0] & 0xffff, e = payload[0] >> 16;
		static const char *const names[] = { "START", "STOP", "LOW_WM", "HIGH_WM" };

		if (p != PORT_RX && p != PORT_TX) {
			return false;
		}
		if (e == 0) {
			ev_start++;
		} else if (e == 1) {
			ev_stop++;
		} else if (e == 2) {
			ev_lwm++;
			ev_wm[p == PORT_TX][0]++;
		} else if (e == 3) {
			ev_hwm++;
			ev_wm[p == PORT_TX][1]++;
		}
		if (e < 2 || ev_lwm + ev_hwm <= 10) {
			SAY("AFE event: port %#x %s\n", p, e < 4 ? names[e] : "?");
		}
		return true;
	}
	if (opcode != APR_BASIC_RSP_RESULT || length < 8) {
		return false;
	}
	if ((token & 0xff000000u) == DL_TOKEN && payload[0] == AFE_PORT_DATA_CMD_RT_PROXY_PORT_READ_V2 && (token & 0xff) < SLOTS) {
		dl_at[token & 0xff] = mach_absolute_time();
		if (payload[1] != 0) {
			OSBitOrAtomic(1u << (token & 0xff), &dl_bad);
		}
		OSBitOrAtomic(1u << (token & 0xff), &dl_done);
		thread_wakeup(&px_wake);
		return true;
	}
	if ((token & 0xff000000u) == UL_TOKEN && payload[0] == AFE_PORT_DATA_CMD_RT_PROXY_PORT_WRITE_V2 && (token & 0xff) < SLOTS) {
		if (payload[1] != 0) {
			OSBitOrAtomic(1u << (token & 0xff), &ul_bad);
		}
		OSBitOrAtomic(1u << (token & 0xff), &ul_done);
		thread_wakeup(&px_wake);
		return true;
	}
	return false;
}

/* ---- setup and teardown (the voice thread) ---- */

static void
undo(void)
{
	const char *err;

	for (int i = 1; i >= 0; i--) {
		uint16_t port = i ? PORT_TX : PORT_RX;
		uint16_t p[2] = { port, 0 };

		if (px.started[i]) {
			(void)afe(0, AFE_PORT_CMD_DEVICE_STOP, p, sizeof(p), 0, NULL, &err);
			SAY("AFE: port %#x stopped: %s\n", port, err);
			px.started[i] = false;
		}
	}
	if (px.mapped) {
		uint32_t h = px.handle;

		(void)afe(0, AFE_SERVICE_CMD_SHARED_MEM_UNMAP_REGIONS, &h, sizeof(h), 0, NULL, &err);
		SAY("AFE: buffers unmapped: %s\n", err);
		px.mapped = false;
	}
	for (int i = 1; i >= 0; i--) {
		uint16_t port = i ? PORT_TX : PORT_RX;
		uint16_t p[2] = { port, 0 };

		if (px.reg[i]) {
			(void)afe(1, AFE_SERVICE_CMD_UNREGISTER_RT_PORT_DRIVER, p, sizeof(p), 0, NULL, &err);
			SAY("AFE: RT driver %#x unregistered: %s\n", port, err);
			px.reg[i] = false;
		}
	}
	if (px.hooked) {
		redwood_adsp_apr_events_proxy(NULL);
		px.hooked = false;
	}
	if (px.space) {
		(void)redwood_sound_dsp_space(false);
		px.space = false;
	}
}

/*
 * The AFE side, before the CVP is put on the ports: the RT driver registered for both ports, the buffers mapped, both
 * ports configured (rate, mono, 16 bit, 10 ms frames, jitter half a frame) and started. False (everything undone):
 * the call stays on today's ports.
 */
bool
sweet_vproxy_prepare(int mode, uint32_t opts)
{
	struct redwood_apr_answer answer;
	const char *err = "?";

	if (running) {
		SAY("prepare: the proxy runs already (a call that did not end?): not again\n");
		return false;
	}
	bzero(&px.reg, sizeof(px.reg));
	bzero(&px.started, sizeof(px.started));
	px.mode = mode;
	px.rate = (opts & OPT_8K) ? 8000 : 16000;
	if (mode == 2 && sweet_voicedev_rate_wanted() != 0) {
		px.rate = sweet_voicedev_rate_wanted();         /* 1091: the plug-in's REDWOOD_VOICE_SET_RATE */
	}
	px.samples = px.rate / 100;
	px.bytes = px.samples * 2;
	if (px.va == 0) {
		/* kept for the whole run: whatever the DSP still writes after a call lands in our own pages */
		if (sweet_gpu_pool_alloc(MEM_BYTES, &px.pa) != 0 || px.pa == 0 || px.pa + MEM_BYTES > 0x100000000ull) {
			SAY("FALLBACK: no GPU-pool memory below 4 GB for the proxy's buffers\n");
			return false;
		}
		px.va = ml_io_map_unmappable(px.pa, MEM_BYTES, VM_WIMG_WCOMB);
		if (px.va == 0) {
			SAY("FALLBACK: the proxy's buffers cannot be mapped\n");
			return false;
		}
	}
	bzero((void *)px.va, MEM_BYTES);
	if (!redwood_sound_dsp_space(true)) {
		SAY("FALLBACK: the DSP's view of memory (SMMU stream 0x1401) is not to be had\n");
		return false;
	}
	px.space = true;
	redwood_adsp_apr_events_proxy(px_events);
	px.hooked = true;
	ev_start = ev_stop = ev_lwm = ev_hwm = 0;
	for (int i = 0; i < 2; i++) {
		uint16_t port = i ? PORT_TX : PORT_RX;
		uint16_t p[2] = { port, 0 };
		bool ok = afe(1, AFE_SERVICE_CMD_REGISTER_RT_PORT_DRIVER, p, sizeof(p), 0, NULL, &err);

		if (!ok && strcmp(err, "no answer") == 0) {
			/* q6afe.c afe_register_get_events() does not wait for an answer: none is not taken as a refusal */
			SAY("AFE: RT driver %#x registered: ok (no answer within %u ms; Android does not wait for one)\n", port, APR_WAIT_MS);
			ok = true;
		} else {
			SAY("AFE: RT driver %#x registered: %s%s\n", port, ok ? "" : "err ", err);
		}
		if (!ok) {
			goto fail;
		}
		px.reg[i] = true;
	}
	{
		uint64_t address = px.pa | AUDIO_SID_BITS;
		struct {
			uint16_t pool, regions;
			uint32_t property;
			uint32_t low, high, size;
		} __attribute__((packed)) map = { ADSP_MEMORY_MAP_SHMEM8_4K_POOL, 1, 0, (uint32_t)address, (uint32_t)(address >> 32),
			                          MEM_BYTES };

		if (!afe(0, AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map), AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS, &answer,
		    &err)) {
			SAY("AFE: buffers mapped: FAILED: %s\n", err);
			goto fail;
		}
		px.handle = answer.words[0];
		px.mapped = true;
		SAY("AFE: buffers mapped: handle %#x, %u bytes (DSP address %#llx)\n", px.handle, MEM_BYTES, address);
	}
	for (int i = 0; i < 2; i++) {
		uint16_t port = i ? PORT_TX : PORT_RX;
		struct {
			uint16_t port, size;
			uint32_t lsw, msw, handle;      /* in band */
			uint32_t module, param;
			uint16_t psize, reserved;
			uint32_t version;
			uint16_t bits, interleaved, frame, jitter, low_wm, high_wm;
			uint32_t rate;
			uint16_t channels, reserved2;
		} __attribute__((packed)) cfg;
		uint16_t start[2] = { port, 0 };
		const char *err2 = "not tried";
		bool ok;

		bzero(&cfg, sizeof(cfg));
		cfg.port = port;
		cfg.size = (uint16_t)(sizeof(cfg) - 16);
		cfg.module = AFE_MODULE_AUDIO_DEV_INTERFACE;
		cfg.param = AFE_PARAM_ID_RT_PROXY_CONFIG;
		cfg.psize = (uint16_t)(sizeof(cfg) - 28);
		cfg.version = 1;
		cfg.bits = 16;
		cfg.interleaved = 1;
		cfg.frame = (uint16_t)px.bytes;
		cfg.jitter = (uint16_t)(px.bytes / 2);
		cfg.rate = px.rate;
		cfg.channels = 1;
		ok = afe(0, AFE_PORT_CMD_SET_PARAM_V2, &cfg, sizeof(cfg), 0, NULL, &err);
		if (ok) {
			ok = afe(0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0, NULL, &err2);
			px.started[i] = ok;
		}
		SAY("AFE: port %#x config %u Hz %u ch 16 bit frame %u B jitter %u B: %s; started: %s\n", port, px.rate, 1, px.bytes,
		    px.bytes / 2, err, err2);
		if (!ok) {
			goto fail;
		}
	}
	return true;
fail:
	SAY("FALLBACK: the AFE proxy ports did not come up: the call stays on today's ports (0x9001/0x9040)\n");
	undo();
	return false;
}

uint32_t
sweet_vproxy_rate(void)
{
	return running || px.started[0] ? px.rate : 0;
}

/* ---- DTMF latency: the CVS's RX DTMF event (sweet_voice.c, link thread) against the tone in the proxy frames ---- */

static const uint16_t dtmf_hz[8] = { 697, 770, 852, 941, 1209, 1336, 1477, 1633 };
static const int32_t goertzel_q14[2][8] = {
	{ 27980, 26956, 25701, 24219, 19073, 16325, 13085, 9315 },      /* 8 kHz: 2 cos(2 pi f / fs), Q14 */
	{ 31548, 31281, 30951, 30556, 29144, 28361, 27409, 26258 },     /* 16 kHz */
};
static volatile uint64_t dtmf_event_mach;      /* 0: none pending */
static volatile uint32_t dtmf_event_low, dtmf_event_high;
static uint64_t tone_onset_mach;
static uint32_t tone_low, tone_high;
static bool tone_on;

void
sweet_vproxy_dtmf(uint32_t low, uint32_t high)
{
	if (!running) {
		return;
	}
	dtmf_event_low = low;
	dtmf_event_high = high;
	OSMemoryBarrier();
	dtmf_event_mach = mach_absolute_time();
}

static uint64_t
goertzel(const int16_t *x, uint32_t n, int32_t c)
{
	int64_t s1 = 0, s2 = 0;

	for (uint32_t i = 0; i < n; i++) {
		int64_t s = x[i] + ((c * s1) >> 14) - s2;

		s2 = s1;
		s1 = s;
	}
	return (uint64_t)(s1 * s1 + s2 * s2 - ((c * s1) >> 14) * s2);
}

static void
dtmf_say(uint64_t event, uint64_t seen, uint32_t lo, uint32_t hi)
{
	int64_t d = (int64_t)mach_us(seen) - (int64_t)mach_us(event);

	dtmf_last_ms = (int32_t)(d / 1000);
	SAY("DTMF %u/%u Hz: DSP event at %llu us, seen in proxy data at %llu us (%+d ms)%s (tone found %u/%u Hz)\n", dtmf_event_low,
	    dtmf_event_high, mach_us(event) - call_began_us, mach_us(seen) - call_began_us, dtmf_last_ms,
	    d < 0 ? " -- before the event" : "", lo, hi);
}

static void
dtmf_frame(const int16_t *x, uint32_t n, uint64_t at)
{
	uint64_t energy = 0, best[2] = { 0, 0 };
	uint32_t which[2] = { 0, 0 };
	int r = px.rate == 16000;
	bool present;
	uint64_t ev;

	for (uint32_t i = 0; i < n; i++) {
		energy += (uint64_t)((int32_t)x[i] * x[i]);
	}
	if (energy > 33856ull * n) {    /* louder than about -45 dBFS */
		for (int k = 0; k < 8; k++) {
			uint64_t p = goertzel(x, n, goertzel_q14[r][k]);

			if (p > best[k / 4]) {
				best[k / 4] = p;
				which[k / 4] = k;
			}
		}
	}
	/* each tone over 20 % of the frame's energy (a pure tone gives P ~ E * N / 2) */
	present = energy > 33856ull * n && best[0] * 10 > energy * n && best[1] * 10 > energy * n;
	if (present && !tone_on) {
		tone_onset_mach = at;
		tone_low = dtmf_hz[which[0]];
		tone_high = dtmf_hz[which[1]];
	}
	tone_on = present;
	ev = dtmf_event_mach;
	if (ev == 0) {
		return;
	}
	if (tone_onset_mach != 0 && tone_onset_mach < ev && mach_us(ev) - mach_us(tone_onset_mach) < 300000 && tone_on) {
		dtmf_say(ev, tone_onset_mach, tone_low, tone_high);     /* the tone was in the frames before the event */
		dtmf_event_mach = 0;
	} else if (tone_on && tone_onset_mach >= ev) {
		dtmf_say(ev, tone_onset_mach, tone_low, tone_high);
		dtmf_event_mach = 0;
	} else if (mach_us(at) > mach_us(ev) + 1000000) {
		SAY("DTMF %u/%u Hz: DSP event at %llu us, NOT seen in the proxy data within 1 s\n", dtmf_event_low, dtmf_event_high,
		    mach_us(ev) - call_began_us);
		dtmf_event_mach = 0;
	}
}

/* ---- the data thread ---- */

static void
win_reset(struct win *x)
{
	bzero(x, sizeof(*x));
	x->gap_min = x->dl_ring_min = x->ul_ring_min = 0xffffffffu;
}

static void
win_add(struct win *t, const struct win *x)
{
	t->dl += x->dl;
	t->dl_fail += x->dl_fail;
	t->ul += x->ul;
	t->ul_fail += x->ul_fail;
	t->dl_peak = t->dl_peak > x->dl_peak ? t->dl_peak : x->dl_peak;
	t->ul_peak = t->ul_peak > x->ul_peak ? t->ul_peak : x->ul_peak;
	t->gap_min = t->gap_min < x->gap_min ? t->gap_min : x->gap_min;
	t->gap_max = t->gap_max > x->gap_max ? t->gap_max : x->gap_max;
	t->gaps += x->gaps;
	t->gap_sum += x->gap_sum;
	t->dl_ring_min = t->dl_ring_min < x->dl_ring_min ? t->dl_ring_min : x->dl_ring_min;
	t->dl_ring_max = t->dl_ring_max > x->dl_ring_max ? t->dl_ring_max : x->dl_ring_max;
	t->ul_ring_min = t->ul_ring_min < x->ul_ring_min ? t->ul_ring_min : x->ul_ring_min;
	t->ul_ring_max = t->ul_ring_max > x->ul_ring_max ? t->ul_ring_max : x->ul_ring_max;
	t->rings += x->rings;
	t->dl_rings += x->dl_rings;
	t->dl_ring_sum += x->dl_ring_sum;
	t->ul_ring_sum += x->ul_ring_sum;
	t->dl_under += x->dl_under;
	t->ul_under += x->ul_under;
	t->dl_over += x->dl_over;
	t->ul_over += x->ul_over;
	t->dl_slip += x->dl_slip;
	t->ul_slip += x->ul_slip;
	t->dl_busy += x->dl_busy;
	t->ul_busy += x->ul_busy;
	t->late += x->late;
	t->dup += x->dup;
	t->zero += x->zero;
	t->trim += x->trim;
	t->lost += x->lost;
	for (int i = 0; i < 4; i++) {
		t->wm[i] += x->wm[i];
	}
}

/* fills, ms (ring frames at 48 kHz -> ms) */
#define MS48(f)                 ((f) / 48)

static void
stats_format(char *b, size_t n, const char *head, const struct win *x, uint32_t queued_frames)
{
	uint32_t gaps = x->gaps ? x->gaps : 1, ur = x->rings ? x->rings : 1, dr = x->dl_rings ? x->dl_rings : 1;
	uint32_t dl_avg = (uint32_t)(x->dl_ring_sum / dr), ul_avg = (uint32_t)(x->ul_ring_sum / ur);

	if (paced) {
		/* 1095: periods actually done each way (~100/s), their spacing (~10000 us), what was lost or repeated */
		snprintf(b, n, "%s: PACED dl %u periods (fail %u lost %u busy %u, gap min/avg/max %u/%u/%u us) ul %u periods (fail %u "
		    "busy %u) late %u | dl dup %u zero %u peak %u ul peak %u | ring dl %u ms (min/max %u/%u) ul %u ms (min/max %u/%u) "
		    "dsp queued %u ms | under dl %u ul %u, over dl %u ul %u, trim %u, slips dl %+d ul %+d | wm rx %u/%u tx %u/%u", head,
		    x->dl, x->dl_fail, x->lost, x->dl_busy, x->gaps ? x->gap_min : 0, (uint32_t)(x->gap_sum / gaps), x->gap_max, x->ul,
		    x->ul_fail, x->ul_busy, x->late, x->dup, x->zero, x->dl_peak, x->ul_peak, MS48(dl_avg),
		    x->dl_rings ? MS48(x->dl_ring_min) : 0, MS48(x->dl_ring_max), MS48(ul_avg), x->rings ? MS48(x->ul_ring_min) : 0,
		    MS48(x->ul_ring_max), MS48(queued_frames), x->dl_under, x->ul_under, x->dl_over, x->ul_over, x->trim, x->dl_slip,
		    x->ul_slip, x->wm[0], x->wm[1], x->wm[2], x->wm[3]);
		return;
	}
	snprintf(b, n, "%s: dl %u frames (fail %u, gap min/avg/max %u/%u/%u us) ul %u frames (fail %u) dl peak %u ul peak %u | "
	    "ring dl %u ms ul %u ms | dsp queued %u ms | ring min/avg/max dl %u/%u/%u ms ul %u/%u/%u ms; under dl %u ul %u, "
	    "over dl %u ul %u, slips dl %+d ul %+d", head, x->dl, x->dl_fail, x->gaps ? x->gap_min : 0, (uint32_t)(x->gap_sum / gaps),
	    x->gap_max, x->ul, x->ul_fail, x->dl_peak, x->ul_peak, MS48(dl_avg), MS48(ul_avg), MS48(queued_frames),
	    x->dl_rings ? MS48(x->dl_ring_min) : 0, MS48(dl_avg), MS48(x->dl_ring_max), x->rings ? MS48(x->ul_ring_min) : 0,
	    MS48(ul_avg), MS48(x->ul_ring_max), x->dl_under, x->ul_under, x->dl_over, x->ul_over, x->dl_slip, x->ul_slip);
}

/* 1095: where a slot lives in the mapped buffer: paced = periods back to back (msm-pcm-afe-v2.c), 1090 = 4 KB pages */
static uint64_t
dl_off(unsigned slot)
{
	return paced ? (uint64_t)slot * px.bytes : 4096ull * slot;
}

static uint64_t
ul_off(unsigned slot)
{
	return UL_OFFSET + (paced ? (uint64_t)slot * px.bytes : 4096ull * slot);
}

/* 1095: v * gain (Q12), saturated */
static int16_t
gain_apply(int32_t v, uint32_t q12)
{
	v = (int32_t)(((int64_t)v * q12) >> 12);
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static uint32_t
gain_now(int dir)
{
	int db = vp_gain_db[dir];

	return gain_q12[(db < -24 ? -24 : db > 24 ? 24 : db) + 24];
}

static bool
dl_issue(unsigned slot)
{
	uint64_t a = (px.pa + dl_off(slot)) | AUDIO_SID_BITS;
	struct {
		uint16_t port, reserved;
		uint32_t lsw, msw, handle, bytes;
	} __attribute__((packed)) rd = { PORT_RX, 0, (uint32_t)a, (uint32_t)(a >> 32), px.handle, px.bytes };

	return redwood_adsp_apr(APR_SVC_AFE, 0, DL_TOKEN | slot, AFE_PORT_DATA_CMD_RT_PROXY_PORT_READ_V2, &rd, sizeof(rd), 0, NULL);
}

static bool
ul_issue(unsigned slot)
{
	uint64_t a = (px.pa + ul_off(slot)) | AUDIO_SID_BITS;
	struct {
		uint16_t port, reserved;
		uint32_t lsw, msw, handle, bytes;
	} __attribute__((packed)) wr = { PORT_TX, 0, (uint32_t)a, (uint32_t)(a >> 32), px.handle, px.bytes };

	return redwood_adsp_apr(APR_SVC_AFE, 0, UL_TOKEN | slot, AFE_PORT_DATA_CMD_RT_PROXY_PORT_WRITE_V2, &wr, sizeof(wr), 0, NULL);
}

/* A downlink frame came: to the speakers' call ring (mode 1), 16/8 kHz mono -> 48 kHz stereo, linear */
static void
dl_frame(unsigned slot)
{
	const volatile int16_t *src = (const volatile int16_t *)(px.va + dl_off(slot));
	int16_t x[160], out[2 * 482];
	uint32_t n = px.samples, f = OUT_RATE / px.rate, o = 0, gain = redwood_sound_call_gain_q15(), peak = 0, ring, queued, under,
	    periods, g12 = gain_now(0);
	uint64_t at = dl_at[slot];
	bool same = dl_last_ok;

	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < n; i++) {
		int16_t v = src[i];
		uint32_t a = v < 0 ? (uint32_t)-(int32_t)v : (uint32_t)v;

		x[i] = v;
		peak = a > peak ? a : peak;
		same = same && dl_last[i] == v;
		dl_last[i] = v;
	}
	dl_last_ok = true;
	/* 1095: a non-silent frame byte for byte the previous one (the same DSP period handed out twice) */
	if (peak == 0) {
		cw.zero++;
	} else if (same) {
		cw.dup++;
	}
	cw.dl_peak = peak > cw.dl_peak ? peak : cw.dl_peak;
	dtmf_frame(x, n, at);
	if (px.mode == 2) {
		sweet_voicedev_dl(x, n, at);    /* 1091: to /dev/redwood-voice, not played here */
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		int32_t prev = i ? x[i - 1] : dl_prev;

		for (uint32_t k = 1; k <= f; k++) {
			int32_t v = prev + (x[i] - prev) * (int32_t)k / (int32_t)f;

			v = (int32_t)(((int64_t)v * gain) >> 15);
			if (g12 != 4096) {
				v = gain_apply(v, g12);         /* 1095: kern.sweet_voice_proxy_gain_dl */
			}
			out[2 * o] = (int16_t)v;
			out[2 * o + 1] = (int16_t)v;
			o++;
		}
	}
	dl_prev = x[n - 1];
	if (dl_slip_mode < 0) {
		o--;                    /* one 48 kHz frame less */
		cw.dl_slip--;
	} else if (dl_slip_mode > 0) {
		out[2 * o] = out[2 * o - 2];
		out[2 * o + 1] = out[2 * o - 1];
		o++;
		cw.dl_slip++;
	}
	if (paced) {
		/* 1095: the call ring held under 60 ms: a frame that would take it past is dropped */
		redwood_sound_call_state(&ring, &queued, &under, &periods);
		if (ring + o > DL_RING_HIGH) {
			cw.trim++;
			o = 0;
		}
	}
	if (o != 0 && redwood_sound_call_put(out, o) < o) {
		cw.dl_over++;
	}
	redwood_sound_call_state(&ring, &queued, &under, &periods);
	if (under != sound_underruns_seen) {
		cw.dl_under += under - sound_underruns_seen;
		sound_underruns_seen = under;
	}
	cw.dl_ring_min = ring < cw.dl_ring_min ? ring : cw.dl_ring_min;
	cw.dl_ring_max = ring > cw.dl_ring_max ? ring : cw.dl_ring_max;
	cw.dl_ring_sum += ring;
	cw.dl_rings++;
}

/* An uplink slot is free: the next 10 ms of the microphone, 48 kHz -> 16/8 kHz (the average of each 3/6 samples) */
static void
ul_frame(unsigned slot)
{
	volatile int16_t *dst = (volatile int16_t *)(px.va + ul_off(slot));
	int16_t in[960 + 2];
	uint32_t n = px.samples, f = OUT_RATE / px.rate, need = n * f, take = need, peak = 0, g12 = gain_now(1);
	uint64_t written, avail;

	if (px.mode == 2) {
		int16_t pcm[160];

		sweet_voicedev_ul(pcm, n);      /* 1091: from /dev/redwood-voice (silence when nothing is there) */
		for (uint32_t i = 0; i < n; i++) {
			int32_t a;

			if (g12 != 4096) {
				pcm[i] = gain_apply(pcm[i], g12);       /* 1095: kern.sweet_voice_proxy_gain_ul */
			}
			a = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
			dst[i] = pcm[i];
			peak = (uint32_t)a > peak ? (uint32_t)a : peak;
		}
		cw.ul_peak = peak > cw.ul_peak ? peak : cw.ul_peak;
		__asm__ volatile ("dsb sy" ::: "memory");
		return;
	}
	(void)redwood_mic_voice_peek(0, in, 0, &written);       /* (only *written wanted) */
	if (!ul_primed) {
		if (written < MIC_KEEP) {
			goto silence;
		}
		ul_cursor = written - MIC_KEEP;
		ul_primed = true;
	}
	avail = written - ul_cursor;
	if (avail > MIC_MOST) {
		ul_cursor = written - MIC_KEEP;
		avail = MIC_KEEP;
		cw.ul_over++;
	}
	cw.ul_ring_min = avail < cw.ul_ring_min ? (uint32_t)avail : cw.ul_ring_min;
	cw.ul_ring_max = avail > cw.ul_ring_max ? (uint32_t)avail : cw.ul_ring_max;
	cw.ul_ring_sum += avail;
	cw.rings++;
	if (ul_slip_mode < 0) {
		take = need + 1;        /* one sample more consumed */
	} else if (ul_slip_mode > 0) {
		take = need - 1;
	}
	if (!redwood_mic_voice_peek(ul_cursor, in, need + 1 > take ? need + 1 : take, &written) &&
	    !redwood_mic_voice_peek(ul_cursor, in, need, &written)) {
		cw.ul_under++;
		goto silence;
	}
	if (take > need && avail < take) {
		take = need;
	}
	ul_cursor += take;
	cw.ul_slip += (int32_t)need - (int32_t)take;
	for (uint32_t i = 0; i < n; i++) {
		int32_t sum = 0;

		for (uint32_t k = 0; k < f; k++) {
			sum += in[i * f + k];
		}
		sum /= (int32_t)f;
		if (g12 != 4096) {
			sum = gain_apply(sum, g12);     /* 1095: kern.sweet_voice_proxy_gain_ul */
		}
		dst[i] = (int16_t)sum;
		peak = (uint32_t)(sum < 0 ? -sum : sum) > peak ? (uint32_t)(sum < 0 ? -sum : sum) : peak;
	}
	cw.ul_peak = peak > cw.ul_peak ? peak : cw.ul_peak;
	__asm__ volatile ("dsb sy" ::: "memory");
	return;
silence:
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
px_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t dl_retry = 0, ul_retry = 0, seconds = 0;
	uint64_t window_us = now_us(), last_retry_us = 0;
	bool said_no_dl = false, said_no_ul = false;
	uint32_t ring, queued, under, periods;

	for (unsigned s = 0; s < IN_FLIGHT; s++) {
		if (!dl_issue(s)) {
			dl_retry |= 1u << s;
		}
		ul_frame(s);
		if (!ul_issue(s)) {
			ul_retry |= 1u << s;
		}
	}
	SAY("data: %u reads on %#x and %u writes on %#x outstanding (%u-byte frames, 10 ms)\n", IN_FLIGHT, PORT_RX, IN_FLIGHT, PORT_TX,
	    px.bytes);
	while (!stop_req) {
		uint32_t d = __atomic_exchange_n(&dl_done, 0, __ATOMIC_ACQ_REL), u = __atomic_exchange_n(&ul_done, 0, __ATOMIC_ACQ_REL);
		uint32_t db = __atomic_exchange_n(&dl_bad, 0, __ATOMIC_ACQ_REL), ub = __atomic_exchange_n(&ul_bad, 0, __ATOMIC_ACQ_REL);
		uint64_t now = now_us();

		for (unsigned s = 0; s < SLOTS; s++) {
			if (!(d & (1u << s))) {
				continue;
			}
			if (db & (1u << s)) {
				cw.dl_fail++;
				dl_retry |= 1u << s;    /* sent again at the next 10 ms tick, not at once */
				continue;
			}
			if (last_dl_mach != 0) {
				uint32_t gap = (uint32_t)(mach_us(dl_at[s]) - mach_us(last_dl_mach));

				cw.gap_min = gap < cw.gap_min ? gap : cw.gap_min;
				cw.gap_max = gap > cw.gap_max ? gap : cw.gap_max;
				cw.gap_sum += gap;
				cw.gaps++;
			}
			last_dl_mach = dl_at[s];
			cw.dl++;
			dl_frame(s);
			if (!dl_issue(s)) {
				dl_retry |= 1u << s;
			}
		}
		for (unsigned s = 0; s < SLOTS; s++) {
			if (!(u & (1u << s))) {
				continue;
			}
			if (ub & (1u << s)) {
				cw.ul_fail++;
				ul_retry |= 1u << s;
				continue;
			}
			cw.ul++;
			ul_frame(s);
			if (!ul_issue(s)) {
				ul_retry |= 1u << s;
			}
		}
		if ((dl_retry | ul_retry) != 0 && now - last_retry_us >= 10000) {
			last_retry_us = now;
			for (unsigned s = 0; s < SLOTS; s++) {
				if ((dl_retry & (1u << s)) && dl_issue(s)) {
					dl_retry &= ~(1u << s);
				}
				if (ul_retry & (1u << s)) {
					ul_frame(s);
					if (ul_issue(s)) {
						ul_retry &= ~(1u << s);
					}
				}
			}
		}
		if (now - window_us >= 1000000) {
			char line[400];

			seconds++;
			window_us = now;
			redwood_sound_call_state(&ring, &queued, &under, &periods);
			if (!said_no_dl && ct.dl + cw.dl == 0) {
				SAY("THE DSP GIVES NO DOWNLINK FRAMES on %#x (%u read errors; AFE events: start %u stop %u)\n", PORT_RX, cw.dl_fail,
				    ev_start, ev_stop);
				said_no_dl = true;
			}
			if (!said_no_ul && ct.ul + cw.ul == 0) {
				SAY("THE DSP TAKES NO UPLINK FRAMES on %#x (%u write errors)\n", PORT_TX, cw.ul_fail);
				said_no_ul = true;
			}
			/* the slips for the next second: the rings' least fill over this one */
			dl_slip_mode = cw.dl_under ? 1 : (cw.dl_rings && cw.dl_ring_min > 960) ? -1 : 0;
			ul_slip_mode = cw.ul_under ? 1 : (cw.rings && cw.ul_ring_min > 2880) ? -1 : 0;
			if (seconds <= 10 || seconds % 10 == 0) {
				stats_format(line, sizeof(line), "1s", &cw, queued);
				SAY("%s\n", line);
			}
			lck_mtx_lock(&stats_lock);
			win_add(&ct, &cw);
			stats_format(stats_text, sizeof(stats_text), "call so far", &ct, queued);
			lck_mtx_unlock(&stats_lock);
			win_reset(&cw);
		}
		if (__atomic_load_n(&dl_done, __ATOMIC_ACQUIRE) == 0 && __atomic_load_n(&ul_done, __ATOMIC_ACQUIRE) == 0 && !stop_req) {
			assert_wait_timeout((event_t)&px_wake, THREAD_UNINT, 10, NSEC_PER_MSEC);
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
	lck_mtx_lock(&stats_lock);
	win_add(&ct, &cw);
	win_reset(&cw);
	lck_mtx_unlock(&stats_lock);
	thread_runs = false;
	thread_wakeup(&thread_runs);
	thread_terminate(current_thread());
}

/*
 * 1095: the paced data path (kern.sweet_voice_proxy_pace 1), as msm-pcm-afe-v2.c's hrtimer callbacks: every 10 ms
 * (absolute mach deadline) one READ_V2 on 0x2000 at downlink period dl_cnt and one WRITE_V2 on 0x2001 at uplink period
 * ul_cnt, each counter walking 0..PACE_PERIODS-1. A period is used again only after the DSP answered it (else "busy",
 * the tick skipped for that direction); an answer is the period done: the downlink frame is taken then, in issue
 * order. No answer within PACE_LOST_TICKS periods: given up ("lost"). More than 3 periods late (the thread was not
 * run): the schedule restarts from now ("late"), no burst of catch-up reads.
 */
static void
px_thread_paced(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t per_abs, next, window_us = now_us(), dl_sent[PACE_PERIODS], ul_sent[PACE_PERIODS];
	unsigned dl_cnt = 0, ul_cnt = 0, dl_proc = 0;
	uint32_t dl_pend = 0, ul_pend = 0, dl_acked = 0, ul_acked = 0, dl_badm = 0, ul_badm = 0, seconds = 0, wm_seen[4];
	uint32_t ring, queued, under, periods;
	bool said_no_dl = false, said_no_ul = false;

	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &per_abs);
	for (int i = 0; i < 4; i++) {
		wm_seen[i] = ev_wm[i / 2][i % 2];
	}
	next = mach_absolute_time();
	SAY("data: PACED: one read on %#x and one write on %#x every 10 ms, %u periods of %u B each way (kern.sweet_voice_proxy_pace 1)\n",
	    PORT_RX, PORT_TX, PACE_PERIODS, px.bytes);
	while (!stop_req) {
		uint64_t m;

		dl_acked |= __atomic_exchange_n(&dl_done, 0, __ATOMIC_ACQ_REL);
		ul_acked |= __atomic_exchange_n(&ul_done, 0, __ATOMIC_ACQ_REL);
		dl_badm |= __atomic_exchange_n(&dl_bad, 0, __ATOMIC_ACQ_REL);
		ul_badm |= __atomic_exchange_n(&ul_bad, 0, __ATOMIC_ACQ_REL);
		m = mach_absolute_time();

		/* downlink answers, oldest first (the pending periods run from dl_proc up to dl_cnt) */
		while (dl_pend & (1u << dl_proc)) {
			uint32_t bit = 1u << dl_proc;

			if (dl_acked & bit) {
				dl_pend &= ~bit;
				dl_acked &= ~bit;
				if (dl_badm & bit) {
					dl_badm &= ~bit;
					cw.dl_fail++;
				} else {
					if (last_dl_mach != 0) {
						uint32_t gap = (uint32_t)(mach_us(dl_at[dl_proc]) - mach_us(last_dl_mach));

						cw.gap_min = gap < cw.gap_min ? gap : cw.gap_min;
						cw.gap_max = gap > cw.gap_max ? gap : cw.gap_max;
						cw.gap_sum += gap;
						cw.gaps++;
					}
					last_dl_mach = dl_at[dl_proc];
					cw.dl++;
					dl_frame(dl_proc);
				}
			} else if (m - dl_sent[dl_proc] > PACE_LOST_TICKS * per_abs) {
				dl_pend &= ~bit;
				cw.lost++;
			} else {
				break;
			}
			dl_proc = (dl_proc + 1) % PACE_PERIODS;
		}
		/* uplink answers */
		for (unsigned s = 0; s < PACE_PERIODS; s++) {
			uint32_t bit = 1u << s;

			if (!(ul_pend & bit)) {
				continue;
			}
			if (ul_acked & bit) {
				ul_pend &= ~bit;
				if (ul_badm & bit) {
					cw.ul_fail++;
				} else {
					cw.ul++;
				}
			} else if (m - ul_sent[s] > PACE_LOST_TICKS * per_abs) {
				ul_pend &= ~bit;
				cw.lost++;
			}
		}
		ul_acked = ul_badm = 0;         /* (answers for periods no longer pending: dropped) */
		dl_acked &= dl_pend;
		dl_badm &= dl_pend;

		/* the 10 ms tick */
		if (m >= next) {
			uint32_t bit;

			if (m - next > 3 * per_abs) {
				cw.late++;
				next = m;
			}
			next += per_abs;
			bit = 1u << dl_cnt;
			if (dl_pend & bit) {
				cw.dl_busy++;
			} else {
				dl_sent[dl_cnt] = m;
				if (dl_issue(dl_cnt)) {
					dl_pend |= bit;
					dl_cnt = (dl_cnt + 1) % PACE_PERIODS;
				} else {
					cw.dl_fail++;   /* not sent (APR): the same period at the next tick */
				}
			}
			bit = 1u << ul_cnt;
			if (ul_pend & bit) {
				cw.ul_busy++;
			} else {
				ul_frame(ul_cnt);
				ul_sent[ul_cnt] = m;
				if (ul_issue(ul_cnt)) {
					ul_pend |= bit;
					ul_cnt = (ul_cnt + 1) % PACE_PERIODS;
				} else {
					cw.ul_fail++;
				}
			}
		}

		if (now_us() - window_us >= 1000000) {
			char line[sizeof(stats_text)];

			seconds++;
			window_us = now_us();
			for (int i = 0; i < 4; i++) {
				uint32_t v = ev_wm[i / 2][i % 2];

				cw.wm[i] = v - wm_seen[i];
				wm_seen[i] = v;
			}
			redwood_sound_call_state(&ring, &queued, &under, &periods);
			if (!said_no_dl && ct.dl + cw.dl == 0) {
				SAY("THE DSP GIVES NO DOWNLINK FRAMES on %#x (%u read errors, %u lost; AFE events: start %u stop %u)\n", PORT_RX,
				    cw.dl_fail, cw.lost, ev_start, ev_stop);
				said_no_dl = true;
			}
			if (!said_no_ul && ct.ul + cw.ul == 0) {
				SAY("THE DSP TAKES NO UPLINK FRAMES on %#x (%u write errors)\n", PORT_TX, cw.ul_fail);
				said_no_ul = true;
			}
			/* the slips for the next second: the call ring held at 30-60 ms, the mic ring as in 1090 */
			dl_slip_mode = cw.dl_under ? 1 : (cw.dl_rings && cw.dl_ring_min > DL_RING_SLIP) ? -1 : 0;
			ul_slip_mode = cw.ul_under ? 1 : (cw.rings && cw.ul_ring_min > 2880) ? -1 : 0;
			if (seconds <= PACED_LINES_S || seconds % 10 == 0) {
				stats_format(line, sizeof(line), "1s", &cw, queued);
				SAY("%s\n", line);
			}
			lck_mtx_lock(&stats_lock);
			win_add(&ct, &cw);
			stats_format(stats_text, sizeof(stats_text), "call so far", &ct, queued);
			lck_mtx_unlock(&stats_lock);
			win_reset(&cw);
		}

		/* sleep to the next tick, or until an answer comes */
		assert_wait_deadline((event_t)&px_wake, THREAD_UNINT, next);
		if (stop_req || __atomic_load_n(&dl_done, __ATOMIC_ACQUIRE) != 0 || __atomic_load_n(&ul_done, __ATOMIC_ACQUIRE) != 0 ||
		    mach_absolute_time() >= next) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
	lck_mtx_lock(&stats_lock);
	win_add(&ct, &cw);
	win_reset(&cw);
	lck_mtx_unlock(&stats_lock);
	thread_runs = false;
	thread_wakeup(&thread_runs);
	thread_terminate(current_thread());
}

/* After the CVP is on the proxy ports and voice started: the call mode on, the data thread started. */
bool
sweet_vproxy_run(void)
{
	thread_t thread;
	bool heard;

	win_reset(&cw);
	win_reset(&ct);
	dl_done = ul_done = dl_bad = ul_bad = 0;
	last_dl_mach = 0;
	dl_slip_mode = ul_slip_mode = 0;
	ul_primed = false;
	dl_prev = 0;
	dtmf_event_mach = 0;
	tone_on = false;
	tone_onset_mach = 0;
	dtmf_last_ms = 0x7fffffff;
	paced = vp_pace != 0;           /* 1095: kern.sweet_voice_proxy_pace, read once per call */
	dl_last_ok = false;
	call_began_us = now_us();
	heard = redwood_sound_call_mode_on(true);
	{
		uint32_t a, b, c, d;

		redwood_sound_call_state(&a, &b, &c, &d);
		sound_underruns_seen = c;
	}
	if (!heard && px.mode == 1) {
		SAY("mode 1: NO PLAYBACK STREAM RUNS (boot-arg sound < 2): the downlink is read but not heard\n");
	}
	stop_req = false;
	thread_runs = true;
	running = true;
	if (px.mode == 2) {
		sweet_voicedev_call(true, px.rate, px.mode);    /* 1091: before the first frame */
	}
	if (kernel_thread_start(paced ? px_thread_paced : px_thread, NULL, &thread) != KERN_SUCCESS) {
		thread_runs = false;
		running = false;
		redwood_sound_call_mode(false);
		if (px.mode == 2) {
			sweet_voicedev_call(false, 0, 0);
		}
		SAY("FALLBACK: no data thread\n");
		return false;
	}
	thread_deallocate(thread);
	SAY("RUNNING: mode %d, %u Hz mono 16 bit, 10 ms frames; %s; %s (gain dl %+d ul %+d dB)\n", px.mode, px.rate, px.mode == 2 ?
	    "downlink -> /dev/redwood-voice read, its write -> uplink" : "downlink -> speakers' call ring, mic ring -> uplink",
	    paced ? "PACED 10 ms (1095)" : "UNPACED, re-sent on each answer (1090; kern.sweet_voice_proxy_pace 0)", vp_gain_db[0],
	    vp_gain_db[1]);
	return true;
}

/* The call ended (or the switch failed): the thread stopped, the call mode off, the AFE side undone. */
void
sweet_vproxy_stop(const char *why)
{
	if (running) {
		stop_req = true;
		thread_wakeup(&px_wake);
		for (unsigned ms = 0; ms < 500 && thread_runs; ms += 10) {
			delay_for_interval(10, NSEC_PER_MSEC);
		}
		if (thread_runs) {
			SAY("the data thread did not stop within 500 ms\n");
		}
		if (px.mode == 2) {
			sweet_voicedev_call(false, 0, 0);       /* 1091: a blocked reader returns 0 bytes */
		}
		redwood_sound_call_mode(false);
		{
			char line[sizeof(stats_text)];
			uint32_t ring, queued, under, periods;

			redwood_sound_call_state(&ring, &queued, &under, &periods);
			lck_mtx_lock(&stats_lock);
			stats_format(line, sizeof(line), "call end", &ct, queued);
			strlcpy(stats_text, line, sizeof(stats_text));
			lck_mtx_unlock(&stats_lock);
			SAY("%s (%llu s; DTMF latency last %d ms; AFE events start %u stop %u low %u high %u)\n", line,
			    (now_us() - call_began_us) / 1000000, dtmf_last_ms == 0x7fffffff ? 0 : dtmf_last_ms, ev_start, ev_stop, ev_lwm,
			    ev_hwm);
		}
		running = false;
	}
	SAY("STOP (%s)\n", why);
	undo();
}

static int
sysctl_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char b[sizeof(stats_text)];

	lck_mtx_lock(&stats_lock);
	strlcpy(b, stats_text[0] ? stats_text : "no call on the proxy yet", sizeof(b));
	lck_mtx_unlock(&stats_lock);
	return SYSCTL_OUT(req, b, strlen(b) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_proxy_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_stats, "A",
    "redfin 1090: the voice proxy's counters (this call so far, or the last call's totals)");

/* 1095 */
static int
sysctl_pace SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int v = vp_pace, error = sysctl_handle_int(oidp, &v, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (v != 0 && v != 1) {
		return EINVAL;
	}
	vp_pace = v;
	SAY("kern.sweet_voice_proxy_pace = %d (%s; applies at the next call start)\n", v,
	    v ? "paced: one read/write per 10 ms period" : "1090: re-sent on each answer");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_proxy_pace, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_pace, "I",
    "redfin 1095: 1 (default) the proxy's reads/writes paced at one per 10 ms period, 0 = 1090 (re-sent on each answer)");

static int
sysctl_gain SYSCTL_HANDLER_ARGS
{
#pragma unused(arg2)
	int dir = (int)(uintptr_t)arg1, v = vp_gain_db[dir], error = sysctl_handle_int(oidp, &v, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (v < -24 || v > 24) {
		return EINVAL;
	}
	vp_gain_db[dir] = v;
	SAY("kern.sweet_voice_proxy_gain_%s = %+d dB (live)\n", dir ? "ul" : "dl", v);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_proxy_gain_dl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, (void *)0, 0, sysctl_gain, "I",
    "redfin 1095: downlink gain in dB on the proxy path (-24..+24, default 0)");
SYSCTL_PROC(_kern, OID_AUTO, sweet_voice_proxy_gain_ul, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, (void *)1, 0, sysctl_gain, "I",
    "redfin 1095: uplink gain in dB on the proxy path (-24..+24, default 0)");
#endif /* ARM64_BOARD_CONFIG_SWEET */
