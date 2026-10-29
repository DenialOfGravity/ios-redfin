/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, period) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 */
#include "SweetSurface.h"
#include "SweetDisplayMask.h"
#include "SweetTrace.h"     /* 0994 */

extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */

/* bsd/dev/sweet_gpu.c: the Adreno 640's 2D engine scales each frame onto the panel (boot-arg sweetgpu=1). */
extern "C" {
bool sweet_gpu_ready(void);
int sweet_gpu_surface_slot(uint64_t key);
int sweet_gpu_map_surface(uint64_t key, const uint64_t *pages, uint32_t npages, uint32_t page_size);
int sweet_gpu_present(int slot, uintptr_t kva, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
	uint32_t fb_pitch, uint32_t corner_radius, uint32_t *out_dx, uint32_t *out_dy,
	uint32_t *out_dw, uint32_t *out_dh);
void sweet_gpu_panel_stride(uint32_t stride);
void sweet_gpu_flip_poll(void);         /* 0905: sweetfbflip=1 */
void sweet_gpu_scanout_front(void);
bool sweet_gpu_present_poll(void);      /* 0943: a present's blit still out (polls its fence) */
void sweet_gpu_present_landed(uint64_t *sum_abs, uint64_t *n);
uint32_t sweet_mdp_rd(uint32_t off);    /* 0943: MDP registers, offset from 0x0ae00000 */
void sweet_mdp_wr(uint32_t off, uint32_t v);
void sweet_gpu_mdp_own(int on);         /* 0948: p mdp test/scale owns the pipes */
void sweet_gpu_scan_word(uint32_t v);   /* 0989: the latched-frame word the CP waits on (gate 2) */
uint32_t sweet_gpu_fence_of(int pid);   /* 0989: a process's last GPU fence */
int sweet_gpu_cpu_target(uint64_t *pa);         /* 0946 (sweetfbx & 8) */
void sweet_gpu_cpu_flip(void);
void sweet_gpu_present_tag(uint64_t swap_abs);
void sweet_gpu_present_visible(uint64_t *sum_abs, uint64_t *n);
void sweet_cpufreq_boost(int why);              /* 0970: bsd/dev/sweet_cpufreq.c */
void sweet_cpufreq_frame(void);                 /* 1037: kern.sweet_gov_framehold */
extern int sweet_fb_early_retire;               /* 1035: bsd/dev/sweet_gpu.c, kern.sweet_fb_early_retire */
void flush_dcache64(addr64_t addr, unsigned count, int phys);
uint32_t sweet_gpu_fence_submitted(void);       /* 0949: CPU present after the GPU has drawn the frame */
int sweet_gpu_fence_wait_soft(uint32_t fence, unsigned ms);
struct processor;                               /* 0946: the present thread on a big core (osfmk/kern/sched_prim.h) */
struct processor *thread_bind(struct processor *processor);
struct processor *cpu_to_processor(int cpu);
extern int sweet_display_fit_x, sweet_display_fit_y, sweet_display_fit_w, sweet_display_fit_h;
}
#define SWEET_FB_GPU_ROWBYTES           4352    /* the 2D engine writes 64-byte multiples */

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
	kSweetFBGetCurrentAbsoluteTime  = 77,   /* 0943 (sweetfbx & 1) */
	kSweetFBGetBlock                = 79,   /* 0943 (sweetfbx & 1): block 0x40 = the current mode */
};

/*
 * 0943: boot-arg sweetfbx=MASK, display experiments (default 0 = as before 0943, but for the stats line):
 *   0x1  publish TimingElements/ColorElements (SyncRate = the panel Hz), GetBlock(0x40), sel 77 = mach time
 *   0x2  no cache clean of a frame's surface unless the CPU locked it for writing since its last present
 *   0x4  (0944) present at SwapEnd (2 frames queued, SwapEnd held when full), vsync ticks phased to the TE
 *        (0946: or, the TE counter reading 0 on redfin, to INTF1's frame counter)
 *   0x8  (0946) the CPU scales the frame onto the panel (no GPU ring), into the back buffer with sweetfbflip=1
 *   0x10 (0995) sweetvsync |= 3, 0x20 (0995) sweetfb_refresh=1 unless given (boot only; the r45 cmdline is 583 of
 *        591 characters, so no room for new boot-arg names)
 *   0x80 (0998) sweetfb_refresh=1 unless given, and IOMFBDisplayRefresh as a variable-refresh display (DCP's
 *        full key set: min/max interval and step, each with its MachTime twin): max = 3 periods, step = 1 period
 * sweetfb_hz=90: the panel at 90 Hz (DCS 0x53 0x38) and 11.1 ms ticks; anything else is 60 Hz.
 */
#define SWEET_FBX_MODES         0x1
#define SWEET_FBX_NOCLEAN       0x2
#define SWEET_FBX_SWAPQ         0x4
#define SWEET_FBX_CPUPRESENT    0x8
#define SWEET_FBX_VSYNC         0x10    /* 0995: = sweetvsync 3 (ticks without timer slop, calibration keeps its tick) */
#define SWEET_FBX_REFRESH       0x20    /* 0995: = sweetfb_refresh=1 (IOMFBDisplayRefresh from the measured period) */
#define SWEET_FBX_TRIBUF        0x40    /* 0996: = p scanout tribuf 1 at boot (SwapWait polls answer Busy, gate by depth) */
#define SWEET_FBX_VRR           0x80    /* 0998: IOMFBDisplayRefresh with max interval and step (p vsync vrr N [STEP_NS]) */
static uint32_t sweet_fbx;

/*
 * 0977: boot-arg sweetpace=MASK, poke p pace N (default 0 = as before):
 *   0x1  CA (CA_FORCE_TIMER=1) starts a frame only on a vsync notification. When drawing a frame takes longer than
 *        a tick (encode ~10-14 ms vs 11.1 ms at 90 Hz) the tick in between finds CA busy and is not used, so every
 *        frame took two ticks (22.2 ms) or three. With this bit, while frames flow (a SwapEnd within 3 ticks), a
 *        tick that comes before the SwapEnd of the frame its predecessor started is held, and sent at that SwapEnd
 *        (timestamp = then, period as usual): CA starts its next frame at once. A held tick no SwapEnd took goes at
 *        the next tick (that one, fresh), so CA never waits longer than today.
 */
static uint32_t sweet_pace;
/*
 * 0983 (ws-u-pacing2): p pace N [MIN_US] / boot-args sweetpace=N sweetpace_min=US. N & 7 is the mode:
 *   0  as before 0977: a notification on every tick
 *   1  0977: a tick that finds the frame its predecessor started still in flight is held for that SwapEnd
 *   2  render-ahead: every tick as before, plus one at each SwapEnd (CA wants ticks and none went out for MIN);
 *      a tick less than MIN after the last notification is skipped
 *   3  as 2, but a SwapEnd less than MIN after the last notification arms one at last + MIN:
 *      notifications at max(SwapEnd, last + MIN) and on the ticks, never closer than MIN
 *   4  quiet: no notifications, but for one when CA has them on and none went out for 100 ms (for a CA that keeps
 *      a clock of its own, CA_FORCE_TIMER unset; with it set this caps CA at 10 frames/s, p pace 0 restores)
 *   +16 (gate, modes 1-3) the early tick only while the 0944 swap queue is empty (the GPU has taken the frame
 *      before): r37 Control Centre / Notification Centre frames (GPU 23 ms) got slower with 0977, their present
 *      blits queued behind the next frame's render on the one ring (fence 13 -> 27-37 ms, dropped 1 -> 11)
 * MIN defaults to 8000 us. Line SWEET FB: pace 5s.
 */
static uint32_t sweet_pace_min = 8000;

/*
 * 0987: boot-arg sweetswapdone=MASK, poke p swapdone [MASK | trace]. backboardd only ever turns on notification
 * type 5 (klogs: "notifications type 5 on/off", no other type; ios-sweet NOTES: 0 hotplug, 1 power, 3 frame info,
 * 4 CRC, 5 vsync) and learns that a swap is done from SwapWait (sel 6 [id, 0x80000000, 0], twice per frame: the
 * swap before, then the one just ended). So a swap-complete event can only reach CA as a type-5 tick:
 *   0x1  swap-complete tick: a vsync notification once a frame's present is done (GPU blit fence landed, CPU copy
 *        done, direct scanout latched), at least sweetpace_min after the last notification, while CA has them on
 *   0x2  trace: the next 256 events (SwapBegin/End/Wait, the swapper's GPU submits, ticks, swap-done) with thread
 *        ids and times, printed as SWEET SWAPTRACE lines once full; p swapdone trace arms it again
 */
static uint32_t sweet_swapdone;

/*
 * 0995 (ws-u-pace3): boot-args sweetvsync=MASK sweetvsync_lead=US, poke p vsync [MASK [LEAD_US]] (0 = as 0994).
 * From the 2026-10-28 page-swipe traces (sweet-trace.py --pace): the ticks came 0.7-1.5 ms after the panel edge
 * (INTF1 frame counter) instead of 0.5, and 17-20 ticks per 8 s were lost, one at almost every 33rd tick.
 *   0x1  the tick timer without timer coalescing (THREAD_CALL_DELAY_SYS_CRITICAL through IOTimerEventSource's
 *        kIOTimeOptionsSweetCritical): a kernel thread's 11 ms timer gets min(delay >> 3, 1 ms) of slop
 *        (arm timer_coalesce_kt_shift 3, kt_ns_max 1 ms)
 *   0x2  0946's calibration cannot lose a tick: the calibration tick is put 0.3 ms before the predicted edge and polls
 *        until the counter moves; with the slop it ran after the edge most times and then polled a whole period for
 *        the next one (that edge got no tick). Now a calibration tick that finds its edge gone by goes out as a
 *        normal tick, the next calibration comes 8 ticks later and earlier (margin +250 us, at most 2 ms; -50 us
 *        after a good one, at least 300 us)
 *   0x4  notifications carry the measured panel period (INTF frame counter, 11.143 ms at "90 Hz") instead of 11.111
 *   LEAD (default 500 us): ticks this long after the predicted edge (the 0946 phase)
 * sweetfb_refresh=1|NS (0995), p vsync refresh [NS]: publish IOMFBDisplayRefresh { displayMinRefreshInterval =
 * the period in 2^-32 s }, which CA takes for its heartbeat (the app CADisplayLinks' clock) when CA_HEARTBEAT_RATE is
 * not set (redfin-90fps-sensors-plan, 2026-10-28 13:30): 1 = the measured period (republished when it moves by more
 * than 0.01 %), NS > 1000 = that period in ns (11142700 for this panel). With CA_HEARTBEAT_RATE=90 the app ticks run
 * at 11.111 ms and drift 2.85 ms/s against the panel (a full period every 3.9 s).
 */
static uint32_t sweet_vsync, sweet_vsync_lead = 500, sweet_fb_refresh;
/*
 * 0998 (ws-u-fps): IOMFBDisplayRefresh as the A15 DCP firmware publishes it (IOMobileFramebuffer::
 * setDisplayRefreshProperties(), iphone14d6xdcp, iOS 16.7.2): displayMinRefreshInterval, displayMaxRefreshInterval,
 * displayRefreshStep, each in 2^-32 s and as ...MachTime (mach absolute time units). 0995 published only the minimum,
 * i.e. a fixed-rate panel. sweet_fb_vrr = N (2..16, 0 = off): max = N periods, step = sweet_fb_vrr_step_ns or one
 * period. Aim: UIKit's settle animations (inProcessAnimationManager) run at 30 Hz on the 90 Hz heartbeat (every
 * 3rd tick), presumably a 60 fps request quantised onto 90/n; a display CA treats as variable-rate may quantise it
 * to 90 or 45 instead. Boot: sweetfbx 0x80 (= N 3); poke p vsync vrr N [STEP_NS], then restart backboardd (CA reads
 * the property when it opens the display).
 */
static uint32_t sweet_fb_vrr, sweet_fb_vrr_step_ns;
static uint32_t sweet_vsync_cal_ok, sweet_vsync_cal_late, sweet_vsync_cal_lost;
#define SWEET_VSYNC_CRIT        0x1
#define SWEET_VSYNC_CAL         0x2
#define SWEET_VSYNC_PERIOD      0x4
#define SWEET_SWAPDONE_TICK     0x1
#define SWEET_SWAPDONE_TRACE    0x2
#define SWEET_ST_N              256
extern "C" uint64_t thread_tid(thread_t thread);
extern "C" thread_t current_thread(void);
extern "C" void sweet_swaptrace(char ev, uint32_t arg);
static struct { uint64_t abs, tid; uint32_t arg; char ev; } sweet_st[SWEET_ST_N];
static volatile SInt32 sweet_st_n;
static uint32_t sweet_swapdone_ticks, sweet_swapdone_events, sweet_swapdone_close;

/* 0987: one trace event (any thread, no lock); the one that fills the buffer prints it */
void
sweet_swaptrace(char ev, uint32_t arg)
{
	if (!(sweet_swapdone & SWEET_SWAPDONE_TRACE) || sweet_st_n >= SWEET_ST_N) {
		return;
	}
	SInt32 i = OSIncrementAtomic(&sweet_st_n);
	if (i >= SWEET_ST_N) {
		return;
	}
	sweet_st[i].abs = mach_absolute_time();
	sweet_st[i].tid = thread_tid(current_thread());
	sweet_st[i].arg = arg;
	sweet_st[i].ev = ev;
	if (i != SWEET_ST_N - 1) {
		return;
	}
	IOSleep(2);     /* the last writers finish their entries */
	IOLog("SWEET SWAPTRACE: %u events: B SwapBegin id, E SwapEnd id, W/w SwapWait id in/out, S GPU submit fence, "
	    "V tick, D swap done (1 blit, 2 CPU, 3 scanout)\n", SWEET_ST_N);
	for (uint32_t k = 0; k < SWEET_ST_N; k++) {
		uint64_t ns;
		absolutetime_to_nanoseconds(sweet_st[k].abs - sweet_st[0].abs, &ns);
		IOLog("SWEET SWAPTRACE: %llu us tid %llu %c %u\n", ns / 1000, sweet_st[k].tid, sweet_st[k].ev, sweet_st[k].arg);
	}
}

/* INTF1 (DSI0) and its tear check, offsets from the MDP (as shim/sde.c): the TE frame counter is INT_COUNT >> 16 */
#define SWEET_INTF1             0x6b800u
#define INTF_FRAME_COUNT        0x0ac
#define INTF_TEAR_EN            0x284
#define INTF_TEAR_SYNC_VSYNC    0x288   /* bits 18:0 vsync_count (19.2 MHz clocks a line), 19 counter on, 20 TE in */
#define INTF_TEAR_SYNC_HEIGHT   0x28c
#define INTF_TEAR_INIT_VAL      0x294
#define INTF_TEAR_INT_COUNT     0x298   /* 31:16 frames (read pointer), 15:0 line */
#define INTF_TEAR_LINE_COUNT    0x2b0
#define INTF_TEAR_AUTOREFRESH   0x2b4
#define SWEET_FB_HIST_FINE      500     /* shown-frame intervals: 0.1 ms bins to 50 ms ... */
#define SWEET_FB_HIST           950     /* ... then 1 ms bins to 500 ms; longer = a pause, not counted */

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;
	uint64_t            fNextVsync;     /* absolute deadline of the next tick */
	uint64_t            fVsyncAbs;      /* tick period in absolute time units */
	uint64_t            fStatSwaps, fStatPresents, fStatPresentAbs;
	/* 0943: the 5 s frame statistics (fLock) */
	uint64_t            fStWin, fStLastSwap, fStLastShown, fStBlitqAbs;
	uint32_t            fStSwaps, fStShown, fStDropped, fStBlitqN, fStHistN;
	uint32_t            fStBucket[5];
	uint16_t            fStHist[SWEET_FB_HIST];

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	uint32_t      fCornerRadius;    /* panel pixels; boot-arg sweetfb_corner_radius */
	uint32_t      fCornerExt;       /* rows/columns a corner covers */
	uint8_t      *fCornerCov;       /* fCornerExt^2 coverage, top-left corner (CPU copy) */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
	bool          fPowerOff;      /* RequestPowerChange(0) */
	bool          fGPU;           /* panel switched to the GPU's stride; frames scaled to fit */
	uint32_t      fFadeStep;      /* fade-out steps done while off */
	bool          fPresentReq;    /* 0149: vsync asked the present thread for the latest frame */
	bool          fPresentThread; /* 0149: the present thread runs (else: present on the vsync tick) */
	uint8_t      *fPresentRec;    /* 0149: the present thread's copy of the swap record */
	uint64_t      fPendingAbs;    /* 0943: when fPending was swapped */
	uint64_t      fPresentRecAbs; /* 0943: ... and fPresentRec */
	uint32_t      fHz;            /* 0943: panel refresh, 60 or 90 */
	IOLock       *fDsiLock;       /* 0943: one DSI command at a time (brightness, refresh rate) */
	struct { const void *s; uint32_t id, seed; } fSeen[8];  /* 0943: surfaces' seeds at their last present */
	uint32_t      fSeenNext;
	void          publishModes(void);
	void          publishSourceLimits(void);     /* 0990 */
	bool          setHz(uint32_t hz, bool tc);
	bool          dsiSend(const uint8_t *pkt, uint32_t size);
	bool          cpuWrote(SweetSurface *s);
	void          statShown(uint64_t swapAbs);
	void          statLine(uint64_t now);
	void          fbCmd(const char *s, const char *end);
	void          mdpCmd(const char *s, const char *end);    /* 0948: p mdp ... */
	/* 0946 */
	uint64_t      fPresentingAbs;         /* SwapEnd of the frame being presented */
	struct { uint64_t abs; uint32_t fence; } fSwapFence[4];      /* 0949: GPU fence submitted at each SwapEnd */
	uint32_t      fSwapFenceNext;
	uint64_t      fStWaitAbs;             /* 0949: CPU present waits for the GPU (fLock) */
	uint32_t      fStWaitN, fStWaitLate;
	uint64_t      fStVisAbs, fStCpuAbs;   /* SwapEnd -> panel-visible, CPU copy time (fLock) */
	uint32_t      fStVisN, fStCpuN;
	uintptr_t     fCpuDst;                /* copyRow's target while the CPU presents (0: fPixels) */
	uint32_t     *fCpuRow;                /* one panel row, built before it goes out */
	uint32_t     *fCpuX;                  /* source column for each panel column */
	uint64_t      fCpuXKey;
	uint64_t      fCpuPa[2];
	IOMemoryMap  *fCpuMap[2];
	uint64_t      fFcEdge, fFcPeriod;     /* INTF1 frame counter: last edge seen, measured period */
	uint32_t      fFcFrame, fFcTicks, fFcMiss;
	bool          fFcCal, fFcDead;
	uint32_t      fFcEarlyUs;             /* 0995: the calibration tick's margin before the edge */
	uint64_t      fRefreshNs;             /* 0995: the period published as IOMFBDisplayRefresh */
	void          publishRefresh(uint64_t ns);
	void          vsyncCmd(const char *s, const char *end);
	bool          presentCPU(const uint8_t *rec);
	uintptr_t     cpuBuffer(uint64_t pa);
	uintptr_t     cpuSource(SweetSurface *s);       /* 0978 */
	struct { IOMemoryDescriptor *md; IOMemoryMap *map; } fCpuSrc[4];
	uint32_t      fCpuSrcNext;
	void          phaseFrameCount(uint64_t now);
	/* 0944 (sweetfbx & 4): frames queued at SwapEnd for the present thread, ticks phased to the TE (fLock) */
	bool          fSwapQ;
	uint8_t      *fQ;             /* two swap records */
	uint64_t      fQAbs[2];
	uint32_t      fQHead, fQn;
	uint64_t      fQRetry;        /* a put-off present is tried again from then on */
	uint32_t      fTeFrame, fTeStale, fTeLogs;
	void          phaseTick(uint64_t now);
	bool          queueSwap(const uint8_t *rec, uint64_t now);
	/* 0977 (sweetpace & 1): a tick that comes while CA is still drawing goes at that frame's SwapEnd (fLock) */
	bool          fPaceSwapped;           /* a SwapEnd since the last vsync notification */
	bool          fPaceOwed;              /* a tick held for the SwapEnd of the frame in flight */
	uint64_t      fPaceOwedCount;
	uint32_t      fPaceHeld, fPaceAtSwap, fPaceLapsed;
	uint64_t      fPaceAheadAbs;          /* at-swap ticks: sum of (next tick - delivery) */
	bool          vsyncOn(void);
	void          sendVsync(uint64_t count, uint64_t now);
	void          swapDone(uint64_t now, uint32_t where);     /* 0987 (fLock) */
	IOReturn      erPoll(uint32_t id);                        /* 1035: SwapWait poll with early retire */
	/* 0983 (fLock) */
	IOTimerEventSource *fPaceTimer;       /* mode 3: the notification at last + MIN */
	bool          fPaceArmed;
	uint64_t      fPaceLastAbs;           /* the last notification sent */
	uint32_t      fPaceEarly, fPaceSkipped, fPaceGated, fPaceDog;
	static void   paceFired(OSObject *owner, IOTimerEventSource *sender);
	bool          paceGate(void);
	uint64_t      paceMinAbs(void);

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
	bool presentGPU(const uint8_t *rec);
	bool fPresentBusy;	/* 0158: the last present was put off (GPU busy): show fPending on the next tick */
	/* 0906: panel brightness (DCS 0x51 over DSI0), on its own thread: a DSI command waits for a gap
	 * between the frames autorefresh sends, and CoreBrightness must not stall on it (as topaz 0107) */
	volatile uint8_t  *fDsi;
	volatile uint32_t *fBlPkt;
	IOLock        *fBlLock;
	uint32_t       fBlWant, fBlHave;        /* panel DBV; fBlHave 0 = not sent yet */
	uint32_t       fBlFails;
	bool           fBlReq;
	bool           blStart(void);
	bool           dsiDcs(const uint8_t *payload, uint32_t len);
	void           blSet(uint32_t level);
	static void    blLoop(void *arg);
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	void copyRow(uint32_t x, uint32_t y, const uint32_t *src, uint32_t n);
	static void presentLoop(void *arg);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

/* 0976: the direct scanout (below) */
static uint32_t get32(const uint8_t *p, uint32_t off);      /* 0994: the trace reads swap ids before its definition */
static bool sweet_scan_live(void);
static bool sweet_scan_swap(AppleCLCD *fb, const uint8_t *rec, uint64_t now, uint32_t fence);
static void sweet_scan_wait(AppleCLCD *fb, uint32_t id);
static IOReturn sweet_scan_poll(AppleCLCD *fb, uint32_t id);    /* 0996 */
static void sweet_scan_set(AppleCLCD *fb, bool on, const char *why);

static AppleCLCD *sweet_fb_instance;    /* 0943: for the poke commands (p fc, p hz) */

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	int                 fPid;           /* 0994: for the trace */
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

/*
 * ---- 0906: panel brightness (redfin: Samsung S6E3HC2, command mode) ----
 *
 * The panel's DCS 0x51 takes a 10-bit level, MSB first (dsi-panel-s6e3hc2-dvt-dsc-1080p-cmd.dtsi:
 * bl_ctrl_dcs, min 0x4, max 0x3ff, high byte offset 8). It goes out as one DCS long write through
 * DSI0's command DMA (DSI ctrl v2.4 at 0x0ae94000, as topaz 0104 on SM6225): the packet sits in the
 * last MB of the cont_splash_region (0xa2200000), memory the display engine already reads through
 * ABL's mapping. The DMA is triggered once the MDP stream is idle between two autorefresh frames.
 * Three failures in a row (no CMD_DMA_DONE) turn it off for the boot. Boot-arg sweetbl_dcs=0: off.
 *
 * CoreBrightness sets IOMFBBrightnessLevel (nits, 16.16) on the framebuffer, and with an
 * AppleARMBacklight service (backlight-control) also "brightness" in IODisplayParameters units
 * (0..1023 here). The service is opt-in (sweetbl=1): with it CoreBrightness also runs digital dimming
 * over every frame, which the CPU renderer could not afford on topaz (<1 fps); with the GPU it is fine.
 */
#define SWEET_BL_DSI0_PA        0x0ae94000ULL
#define SWEET_BL_PKT_PA         0xa2200000ULL
#define SWEET_BL_MIN            0x4u
#define SWEET_BL_MAX            0x3ffu
#define SWEET_BL_MAX_NITS       500u    /* IOMFBBrightnessLevel that maps to SWEET_BL_MAX */

#define DSI_CTRL                0x004
#define DSI_STATUS0             0x008   /* bit 2: CMD_MODE_MDP_BUSY */
#define DSI_CMD_DMA_CTRL        0x03c   /* bit 26: low power */
#define DSI_DMA_CMD_OFFSET      0x048
#define DSI_DMA_CMD_LENGTH      0x04c
#define DSI_TRIG_CTRL           0x084   /* bits 2:0: DMA trigger, 4 = software */
#define DSI_DMA_SW_TRIGGER      0x090
#define DSI_INT_CTRL            0x110   /* bit 0: CMD_DMA_DONE (write 1 clears) */

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static inline uint32_t
dsi_rd(volatile uint8_t *b, uint32_t o)
{
	return *(volatile uint32_t *)(b + o);
}

static inline void
dsi_wr(volatile uint8_t *b, uint32_t o, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(b + o) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

bool
AppleCLCD::dsiDcs(const uint8_t *payload, uint32_t len)
{
	if (fDsi == NULL || fBlPkt == NULL || len == 0 || len > 28) {
		return false;
	}
	/* DCS long write (0x39): word count, type, flags last | long; payload padded with 0xff */
	uint8_t pkt[32];
	memset(pkt, 0xff, sizeof(pkt));
	pkt[0] = (uint8_t)len;
	pkt[1] = 0;
	pkt[2] = 0x39;
	pkt[3] = 0xc0;
	memcpy(pkt + 4, payload, len);
	return dsiSend(pkt, (4 + len + 3) & ~3u);
}

/*
 * 0943: one packet (4-byte header [+ payload], a multiple of 4 bytes) through the command DMA, between two
 * autorefresh frames. One at a time: brightness (blLoop) and the refresh rate (setHz) share it.
 */
bool
AppleCLCD::dsiSend(const uint8_t *pkt, uint32_t size)
{
	if (fDsi == NULL || fBlPkt == NULL || size < 4 || size > 32 || (size & 3)) {
		return false;
	}
	if (fDsiLock != NULL) {
		IOLockLock(fDsiLock);
	}
	for (uint32_t i = 0; i < size / 4; i++) {
		uint32_t w;
		memcpy(&w, pkt + 4 * i, 4);
		fBlPkt[i] = w;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t ctrl = dsi_rd(fDsi, DSI_CTRL), trig = dsi_rd(fDsi, DSI_TRIG_CTRL), dma = dsi_rd(fDsi, DSI_CMD_DMA_CTRL);
	if ((trig & 7) != 4) {
		dsi_wr(fDsi, DSI_TRIG_CTRL, (trig & ~7u) | 4);
	}
	dsi_wr(fDsi, DSI_CTRL, ctrl | 4 | 1);                   /* CMD_MODE_EN | DSI_EN (both on already) */
	dsi_wr(fDsi, DSI_INT_CTRL, dsi_rd(fDsi, DSI_INT_CTRL) | 1);
	dsi_wr(fDsi, DSI_DMA_CMD_OFFSET, (uint32_t)SWEET_BL_PKT_PA);
	dsi_wr(fDsi, DSI_DMA_CMD_LENGTH, size);
	dsi_wr(fDsi, DSI_CMD_DMA_CTRL, dma & ~(1u << 26));      /* high speed */
	for (int i = 0; i < 200 && (dsi_rd(fDsi, DSI_STATUS0) & 4); i++) {
		IODelay(100);                                   /* between two frames */
	}
	dsi_wr(fDsi, DSI_DMA_SW_TRIGGER, 1);
	bool done = false;
	for (int i = 0; i < 50 && !done; i++) {
		done = (dsi_rd(fDsi, DSI_INT_CTRL) & 1) != 0;
		if (!done) {
			IODelay(1000);
		}
	}
	dsi_wr(fDsi, DSI_INT_CTRL, dsi_rd(fDsi, DSI_INT_CTRL) | 1);
	dsi_wr(fDsi, DSI_CMD_DMA_CTRL, dma);
	dsi_wr(fDsi, DSI_CTRL, ctrl);
	if ((trig & 7) != 4) {
		dsi_wr(fDsi, DSI_TRIG_CTRL, trig);
	}
	if (fDsiLock != NULL) {
		IOLockUnlock(fDsiLock);
	}
	return done;
}

void
AppleCLCD::blLoop(void *arg)
{
	AppleCLCD *fb = (AppleCLCD *)arg;
	for (;;) {
		IOLockLock(fb->fBlLock);
		while (!fb->fBlReq) {
			IOLockSleep(fb->fBlLock, &fb->fBlReq, THREAD_UNINT);
		}
		fb->fBlReq = false;
		uint32_t want = fb->fBlWant;
		IOLockUnlock(fb->fBlLock);
		if (want == fb->fBlHave || fb->fBlFails >= 3) {
			continue;
		}
		uint8_t cmd[3] = { 0x51, (uint8_t)(want >> 8), (uint8_t)want };
		bool ok = fb->dsiDcs(cmd, sizeof(cmd));
		static uint32_t logs;
		if (logs++ < 12 || !ok) {
			IOLog("SWEET BL: panel level %#x %s (dsi ctrl %#x status %#x)\n", want, ok ? "set" : "NO DMA DONE",
			    dsi_rd(fb->fDsi, DSI_CTRL), dsi_rd(fb->fDsi, DSI_STATUS0));
		}
		if (ok) {
			fb->fBlHave = want;
			fb->fBlFails = 0;
		} else if (++fb->fBlFails >= 3) {
			IOLog("SWEET BL: three DSI commands without CMD_DMA_DONE: brightness off for this boot\n");
		}
		IODelay(16000);         /* at most one command a frame; later requests coalesce */
	}
}

void
AppleCLCD::blSet(uint32_t level)
{
	if (fBlLock == NULL) {
		return;
	}
	level = level < SWEET_BL_MIN ? SWEET_BL_MIN : level > SWEET_BL_MAX ? SWEET_BL_MAX : level;
	IOLockLock(fBlLock);
	fBlWant = level;
	fBlReq = true;
	IOLockWakeup(fBlLock, &fBlReq, false);
	IOLockUnlock(fBlLock);
}

bool
AppleCLCD::blStart(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweetbl_dcs", &on, sizeof(on));
	if (!on) {
		return false;
	}
	fDsi = (volatile uint8_t *)ml_io_map(SWEET_BL_DSI0_PA, 0x1000);
	fBlPkt = (volatile uint32_t *)ml_io_map(SWEET_BL_PKT_PA, 0x1000);
	fBlLock = IOLockAlloc();
	if (fDsi == NULL || fBlPkt == NULL || fBlLock == NULL || IOCreateThread(&AppleCLCD::blLoop, this) == NULL) {
		IOLog("SWEET BL: setup failed\n");
		fBlLock = NULL;
		return false;
	}
	IOLog("SWEET BL: DSI0 ctrl %#x status %#x trig %#x: brightness via DCS 0x51\n", dsi_rd(fDsi, DSI_CTRL),
	    dsi_rd(fDsi, DSI_STATUS0), dsi_rd(fDsi, DSI_TRIG_CTRL));
	setProperty("IOMFBBrightnessLevel", (unsigned long long)(250u << 16), 32);
	return true;
}

/* CoreBrightness: IOMFBBrightnessLevel, nits as 16.16 */
IOReturn
AppleCLCD::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject("IOMFBBrightnessLevel")) : NULL;
	if (n == NULL || fBlLock == NULL) {
		return kIOReturnUnsupported;
	}
	uint64_t v = n->unsigned64BitValue();
	setProperty("IOMFBBrightnessLevel", v, 32);
	static uint32_t logs;
	if (logs++ < 8) {
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET BL: %s sets IOMFBBrightnessLevel %llu.%02llu nits\n", name, v >> 16, ((v & 0xffff) * 100) >> 16);
	}
	blSet(SWEET_BL_MIN + (uint32_t)((v * (SWEET_BL_MAX - SWEET_BL_MIN)) / ((uint64_t)SWEET_BL_MAX_NITS << 16)));
	return kIOReturnSuccess;
}

/* For CoreBrightness: the service with backlight-control = true (ios-sailfish 0168, topaz 0105). sweetbl=1. */
class AppleARMBacklight : public IOService {
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	AppleCLCD *fFB;
	bool fNitsDone;
	void publish(uint32_t level);
	void publishNits(uint32_t level);
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
AppleARMBacklight::publish(uint32_t level)
{
	OSDictionary *b = OSDictionary::withCapacity(3);
	OSDictionary *p = OSDictionary::withCapacity(1);
	if (b != NULL && p != NULL) {
		OSNumber *n;
		n = OSNumber::withNumber(0ULL, 32); b->setObject("min", n); OSSafeReleaseNULL(n);
		n = OSNumber::withNumber(SWEET_BL_MAX, 32); b->setObject("max", n); OSSafeReleaseNULL(n);
		n = OSNumber::withNumber(level, 32); b->setObject("value", n); OSSafeReleaseNULL(n);
		p->setObject("brightness", b);
		setProperty("IODisplayParameters", p);
	}
	OSSafeReleaseNULL(b);
	OSSafeReleaseNULL(p);
	publishNits(level);
}

/*
 * redfin 0973: the panel's luminance in nits, under the names CoreBrightness's strings have (without them it
 * clamps its target to 1 nit and never updates the backlight: "systemShallUpdateBacklight=false"). The S6E3HC2
 * at its 500 nits <-> level 1023 (SWEET_BL_MAX_NITS), 2 nits the floor. Integer nits; boot-arg sweetnits=0: none.
 */
#define SWEET_BL_MIN_NITS       2u
void
AppleARMBacklight::publishNits(uint32_t level)
{
	static int on = -1;
	if (on < 0) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetnits", &v, sizeof(v));
		on = v != 0;
	}
	if (!on) {
		return;
	}
	uint32_t nits = level * SWEET_BL_MAX_NITS / SWEET_BL_MAX;
	nits = nits < SWEET_BL_MIN_NITS ? SWEET_BL_MIN_NITS : nits;
	if (!fNitsDone) {
		static const struct {
			const char *key;
			uint32_t value;
		} fixed[] = {
			{ "MinNits", SWEET_BL_MIN_NITS }, { "MaxNits", SWEET_BL_MAX_NITS }, { "MaxNitsPanel", SWEET_BL_MAX_NITS },
			{ "HardwareAccessibleMinNits", SWEET_BL_MIN_NITS }, { "HardwareAccessibleMaxNits", SWEET_BL_MAX_NITS },
			{ "user-accessible-max-nits", SWEET_BL_MAX_NITS }, { "digital-dimming-min-nits", SWEET_BL_MIN_NITS },
			{ "brightness-nits-cap", SWEET_BL_MAX_NITS },
			{ "DisplayPanelLuminanceMin", SWEET_BL_MIN_NITS }, { "DisplayPanelLuminanceMid", SWEET_BL_MAX_NITS / 2 },
			{ "DisplayPanelLuminanceMax", SWEET_BL_MAX_NITS },
			{ "DisplayProductLuminanceMin", SWEET_BL_MIN_NITS }, { "DisplayProductLuminanceMid", SWEET_BL_MAX_NITS / 2 },
			{ "DisplayProductLuminanceMax", SWEET_BL_MAX_NITS },
		};
		for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
			setProperty(fixed[i].key, (unsigned long long)fixed[i].value, 32);
		}
		fNitsDone = true;
		IOLog("SWEET BL: 0973 nits published: %u..%u nits (sweetnits=0: off)\n", SWEET_BL_MIN_NITS, SWEET_BL_MAX_NITS);
	}
	setProperty("brightness-nits", (unsigned long long)nits, 32);
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	OSNumber *b = d ? OSDynamicCast(OSNumber, d->getObject("brightness")) : NULL;
	if (b == NULL) {
		return d ? kIOReturnSuccess : kIOReturnBadArgument;
	}
	int32_t v = (int32_t)b->unsigned32BitValue();
	uint32_t level = v < 0 ? 0 : (uint32_t)v > SWEET_BL_MAX ? SWEET_BL_MAX : (uint32_t)v;
	static uint32_t logs;
	if (logs++ < 8) {
		IOLog("SWEET BL: backlight brightness %u\n", level);
	}
	fFB->blSet(level);
	publish(level);
	return kIOReturnSuccess;
}

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	fOffX = (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = (int32_t)(fLogicalH - fHeight) / 2;
	fCornerRadius = SWEET_DISPLAY_CORNER_DEFAULT;
	PE_parse_boot_argn("sweetfb_corner_radius", &fCornerRadius, sizeof(fCornerRadius));
	if (fCornerRadius > SWEET_DISPLAY_CORNER_MAX) {
		fCornerRadius = SWEET_DISPLAY_CORNER_MAX;
	}
	fCornerExt = fCornerRadius ? sweet_display_corner_extent(fCornerRadius) : 0;
	if (2 * fCornerExt >= fWidth || 2 * fCornerExt >= fHeight) {
		fCornerExt = 0;
	}
	fCornerCov = fCornerExt ? (uint8_t *)IOMalloc((vm_size_t)fCornerExt * fCornerExt) : NULL;
	if (fCornerCov != NULL) {
		uint32_t seg = 0;
		for (uint32_t cy = 0; cy < fCornerExt; cy++) {
			sweet_display_corner_row(fCornerRadius, fCornerExt, cy, &seg, fCornerCov + cy * fCornerExt);
		}
	} else {
		fCornerExt = 0;
	}
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	/* With the GPU (sweetgpu=1) the panel later switches to 4352-byte rows: map that much. */
	int gpu = 0;
	PE_parse_boot_argn("sweetgpu", &gpu, sizeof(gpu));
	uint32_t mapRowBytes = gpu && fRowBytes < SWEET_FB_GPU_ROWBYTES ? SWEET_FB_GPU_ROWBYTES : fRowBytes;
	fSurface = sweet_surface_create_physical(fPhys, fWidth, fHeight, mapRowBytes, SWEET_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 10;
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
	/* 0943: sweetfb_hz=90 also runs the panel and the ticks at 90 Hz (the DCS command goes out below) */
	PE_parse_boot_argn("sweetfbx", &sweet_fbx, sizeof(sweet_fbx));
	PE_parse_boot_argn("sweetpace", &sweet_pace, sizeof(sweet_pace));      /* 0977 */
	PE_parse_boot_argn("sweetpace_min", &sweet_pace_min, sizeof(sweet_pace_min));      /* 0983 */
	PE_parse_boot_argn("sweetswapdone", &sweet_swapdone, sizeof(sweet_swapdone));      /* 0987 */
	PE_parse_boot_argn("sweetvsync", &sweet_vsync, sizeof(sweet_vsync));      /* 0995 */
	PE_parse_boot_argn("sweetvsync_lead", &sweet_vsync_lead, sizeof(sweet_vsync_lead));
	PE_parse_boot_argn("sweetfb_refresh", &sweet_fb_refresh, sizeof(sweet_fb_refresh));
	if (sweet_fbx & SWEET_FBX_VSYNC) {
		sweet_vsync |= SWEET_VSYNC_CRIT | SWEET_VSYNC_CAL;     /* 0995: sweetfbx 0x10 (the cmdline is nearly full) */
	}
	if (sweet_fbx & SWEET_FBX_VRR) {
		sweet_fb_vrr = 3;                                       /* 0998: sweetfbx 0x80 */
		if (sweet_fb_refresh == 0) {
			sweet_fb_refresh = 1;
		}
	}
	if ((sweet_fbx & SWEET_FBX_REFRESH) && sweet_fb_refresh == 0) {
		sweet_fb_refresh = 1;                                   /* 0995: sweetfbx 0x20 */
	}
	fFcEarlyUs = 300;
	if (sweet_vsync_lead > 5000) {
		sweet_vsync_lead = 5000;
	}
	if (sweet_vsync || sweet_vsync_lead != 500) {
		IOLog("SWEET FB: vsync %#x lead %u us (0995: 1 no timer slop, 2 calibration keeps its tick, 4 measured period)\n",
		    sweet_vsync, sweet_vsync_lead);
	}
	if (sweet_swapdone) {
		IOLog("SWEET FB: swapdone %#x (0987: 1 tick at swap done, 2 trace)\n", sweet_swapdone);
	}
	fPaceTimer = IOTimerEventSource::timerEventSource(this, &AppleCLCD::paceFired);
	if (fPaceTimer != NULL && fWorkLoop->addEventSource(fPaceTimer) != kIOReturnSuccess) {
		fPaceTimer->release();
		fPaceTimer = NULL;
	}
	if (sweet_pace) {
		IOLog("SWEET FB: pace %u min %u us (0983 modes: 1 hold, 2 at SwapEnd, 3 max(SwapEnd, last+min), 4 quiet; +16 gate)%s\n",
		    sweet_pace, sweet_pace_min, fPaceTimer ? "" : " (no pace timer)");
	}
	fHz = hz == 90 ? 90 : 60;
	fDsiLock = IOLockAlloc();
	fVsync->setTimeoutUS(fHz == 90 ? 11111 : SWEET_FB_VSYNC_US);
	/*
	 * 0149: panel copies on their own thread. On the vsync tick a GPU present waited ~30 ms behind the render
	 * server's GPU jobs, delaying the next vsync notification and capping the panel at ~18 updates/s.
	 */
	fPresentRec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
	int pt = 1;
	PE_parse_boot_argn("sweetfb_thread", &pt, sizeof(pt));
	/* 0944: sweetfbx & 4 (needs the present thread) */
	fQ = pt ? (uint8_t *)IOMallocData(2 * SWEET_FB_SWAP_REC_SIZE) : NULL;    /* 0946: always, for p fbx */
	if (pt && fPresentRec != NULL && IOCreateThread(&AppleCLCD::presentLoop, this) != NULL) {
		fPresentThread = true;
		fSwapQ = fQ != NULL && (sweet_fbx & SWEET_FBX_SWAPQ);
	}
	if ((sweet_fbx & SWEET_FBX_SWAPQ) && !fSwapQ) {
		IOLog("SWEET FB: sweetfbx 0x4 needs the present thread (sweetfb_thread): off\n");
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	/*
	 * 0925: a dictionary, as on an iPhone's AppleCLCD. VideoToolbox's FigGetAlignmentForIOSurfaceOutputToDisplayService
	 * takes CFDictionaryGetValue(property, "PlaneBaseAlignmentLinear"): on a number mediaserverd died
	 * ("-[__NSCFNumber objectForKey:]: unrecognized selector") whenever Camera.app's preview started.
	 */
	OSDictionary *align = OSDictionary::withCapacity(1);
	OSNumber *linear = OSNumber::withNumber(64ULL, 32);
	if (align != NULL && linear != NULL) {
		align->setObject("PlaneBaseAlignmentLinear", linear);
		setProperty("DisplayPipePlaneBaseAlignment", align);
	}
	OSSafeReleaseNULL(linear);
	OSSafeReleaseNULL(align);
	publishSourceLimits();          /* 0990: sweetiomfbprops=1 */
	if (blStart()) {
		uint32_t blOn = 0;
		PE_parse_boot_argn("sweetbl", &blOn, sizeof(blOn));
		AppleARMBacklight *bl = blOn ? OSTypeAlloc(AppleARMBacklight) : NULL;
		if (bl != NULL && bl->init() && bl->attach(this)) {
			bl->fFB = this;
			bl->setProperty("backlight-control", true);
			bl->publish(SWEET_BL_MAX / 2);
			bl->registerService();
			IOLog("SWEET BL: AppleARMBacklight published (sweetbl=1)\n");
		}
		OSSafeReleaseNULL(bl);
	}
	if (fHz == 90 && !setHz(90, false)) {
		IOLog("SWEET FB: sweetfb_hz=90: the panel did not take DCS 0x53 0x38: 60 Hz\n");
		fHz = 60;
		fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);
	}
	if (sweet_fbx & SWEET_FBX_MODES) {
		publishModes();
	}
	if (sweet_fb_refresh != 0) {
		/* 0995: a fixed period now, or (1) the nominal one until the frame counter has measured the panel */
		publishRefresh(sweet_fb_refresh > 1000 ? sweet_fb_refresh : (fHz == 90 ? 11111111ULL : SWEET_FB_VSYNC_US * 1000ULL));
	}
	sweet_fb_instance = this;
	sweet_surface_scan_reserve();   /* 0979: the scanout slots' memory before userland (sweetscanout) */
	if (sweet_surface_scan_mode() == 1) {
		sweet_scan_set(this, true, "boot-arg sweetscanout=1");  /* 0976 */
	}
	IOLog("SWEET FB: sweetfbx %#x, panel %u Hz\n", sweet_fbx, fHz);
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
	return true;
}

static uint64_t sweet_fb_swap_count;
/*
 * 1035 (kern.sweet_fb_early_retire): the swap the present thread is presenting (accepted onto the ring when present()
 * returns shown), and the 5 s counters: retired at acceptance, SwapWait polls answered busy / done / stale (pending
 * over 100 ms: done), present-thread re-kicks from the vsync tick. All with the fLock.
 */
static bool sweet_er_presenting;
static uint32_t sweet_er_presenting_id;
static uint32_t sweet_er_acc, sweet_er_busy, sweet_er_done, sweet_er_stale, sweet_er_kick;

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	/*
	 * Fixed-rate ticks: re-arm from the previous deadline before any work.
	 * Re-arming 16.7 ms after the panel copy made "60 Hz" vsync run at
	 * ~40 Hz with jitter, and backboardd paces its frames on it.
	 */
	if (fb->fVsyncAbs == 0) {
		nanoseconds_to_absolutetime(fb->fHz == 90 ? 11111111ULL : SWEET_FB_VSYNC_US * 1000ULL, &fb->fVsyncAbs);
		fb->fNextVsync = now;
	}
	fb->fNextVsync += fb->fVsyncAbs;
	if (fb->fNextVsync <= now) {
		fb->fNextVsync = now + fb->fVsyncAbs;
	}
	if (sweet_fbx & SWEET_FBX_SWAPQ) {
		fb->phaseTick(now);	/* 0944: the next tick just after a TE */
	}
	if (sweet_vsync & SWEET_VSYNC_CRIT) {
		sender->wakeAtTime(kIOTimeOptionsSweetCritical, fb->fNextVsync, 0);    /* 0995: no coalescing slop */
	} else {
		sender->wakeAtTime(fb->fNextVsync);
	}
	sweet_gpu_flip_poll();	/* 0905: a finished frame in the back buffer goes on screen at this vsync */
	sweet_gpu_present_poll();	/* 0943: blit->fence latency */
	fb->statLine(now);		/* 0943: every 5 s, the frame statistics */
	if ((sweet_fb_early_retire & 2) && fb->fSwapQ) {
		/* 1035: the flip latched at the TE this tick follows: a put-off queued frame is tried now, not 1 ms later */
		IOLockLock(fb->fLock);
		if (fb->fQn != 0 && fb->fQRetry != 0) {
			fb->fQRetry = 0;
			sweet_er_kick++;
			IOLockWakeup(fb->fLock, &fb->fPresentReq, true);
		}
		IOLockUnlock(fb->fLock);
	}
	/*
	 * CA::WindowServer::IOMFBServer::vsync_callback(fb, a1, a2, a3, refcon): a1 only goes to kdebug, a2 is
	 * the vsync's host time and a2 + a3 the next one (a3 = the refresh period, also CA's CVTimeStamp
	 * videoRefreshPeriod). a3 was 0: CA saw every frame's deadline as already past.
	 */
	IOLockLock(fb->fLock);
	bool send = true;
	uint32_t mode = sweet_pace & 7;
	if ((mode == 2 || mode == 3) && !fb->fPowerOff && fb->fPaceLastAbs != 0 && now - fb->fPaceLastAbs < fb->paceMinAbs()) {
		/* 0983: a notification went out at a SwapEnd less than MIN ago */
		fb->fPaceSkipped++;
		send = false;
	} else if (mode == 4 && !fb->fPowerOff) {
		/* 0983 quiet: one only when CA has them on and none went out for 100 ms */
		uint64_t dog;
		nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &dog);
		send = fb->vsyncOn() && (fb->fPaceLastAbs == 0 || now - fb->fPaceLastAbs >= dog);
		fb->fPaceDog += send;
		fb->fPaceSkipped += !send;
	}
	if (mode == 1 && !fb->fPowerOff) {
		/* 0977: CA still drawing the frame the last tick started (no SwapEnd since), frames flowing: hold this one */
		bool flowing = fb->fStLastSwap != 0 && now - fb->fStLastSwap < 3 * fb->fVsyncAbs;
		if (fb->fPaceOwed) {
			fb->fPaceOwed = false;  /* no SwapEnd took it: this tick goes, as it would have */
			fb->fPaceLapsed++;
		} else if (flowing && !fb->fPaceSwapped && fb->vsyncOn()) {
			fb->fPaceOwed = true;
			fb->fPaceOwedCount = count;
			fb->fPaceHeld++;
			send = false;
		}
	}
	SWEET_TRACE(SWT_VSYNC, count, send, mode | (sweet_vsync << 8),     /* 0994; 0995: sweetvsync in bits 8+ */
	    fb->fPowerOff ? 0 : sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT));
	if (send) {
		fb->sendVsync(count, now);
	}
	if (fb->fPowerOff) {
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
		if (step <= 6) {
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
		}
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	uint8_t *rec = NULL;
	if (flush && fb->fPresentThread) {
		fb->fPresentReq = true;
		IOLockWakeup(fb->fLock, &fb->fPresentReq, true);
		flush = false;
	}
	if (flush) {
		rec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	uint64_t recAbs = fb->fPendingAbs;
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		uint64_t t0 = mach_absolute_time();
		fb->fPresentBusy = false;
		fb->fPresentingAbs = recAbs;	/* 0946 */
		SWEET_TRACE_START(SWT_PRESENT, get32(rec, 0x50), 0, 0, 0);       /* 0994 */
		fb->present(rec);
		SWEET_TRACE_END(SWT_PRESENT, get32(rec, 0x50), fb->fPresentBusy, 0, 0);
		fb->fStatPresentAbs += mach_absolute_time() - t0;
		if (fb->fPresentBusy) {
			IOLockLock(fb->fLock);
			fb->fDirty = true;	/* not shown: again on the next tick (fPending, maybe newer by then) */
			fb->fStDropped += fb->fPendingAbs != recAbs;	/* 0943: a newer one came meanwhile */
			IOLockUnlock(fb->fLock);
		} else {
			fb->fStatPresents++;
			fb->statShown(recAbs);
		}
		IOFreeData(rec, SWEET_FB_SWAP_REC_SIZE);
	}
}

/* 0977: with the fLock. The vsync notification to every client that has it on (values as 0121). */
void
AppleCLCD::sendVsync(uint64_t count, uint64_t now)
{
	uint64_t period = (sweet_vsync & SWEET_VSYNC_PERIOD) && fFcPeriod != 0 ? fFcPeriod : fVsyncAbs;     /* 0995 */
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		(uint32_t)period, (uint32_t)(period >> 32),
	};

	fPaceSwapped = false;
	fPaceLastAbs = now;     /* 0983 */
	sweet_swaptrace('V', (uint32_t)count);  /* 0987 */
	if (SWEET_TRACE_ON()) {        /* 0994 */
		uint32_t on = 0;
		for (unsigned int i = 0; i < fClients->getCount(); i++) {
			IOMobileFramebufferUserClient *c = (IOMobileFramebufferUserClient *)fClients->getObject(i);
			on += c->fFunc[SWEET_FB_NOTIFY_VSYNC] != 0 && c->fPort != MACH_PORT_NULL;
		}
		SWEET_TRACE(SWT_VSYNC_SEND, count, on, now, period);
	}
	for (unsigned int i = 0; i < fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
}

/*
 * 0987: with the fLock. A frame's present is done (where: 1 GPU blit fence landed, 2 CPU copy done, 3 direct scanout
 * latched; 1035: 4 blit accepted onto the ring): with sweetswapdone & 1, the swap-complete tick (type 5, the only notification CA takes).
 */
void
AppleCLCD::swapDone(uint64_t now, uint32_t where)
{
	sweet_swaptrace('D', where);
	SWEET_TRACE(SWT_SWAPDONE, where, 0, 0, 0);     /* 0994 */
	if (!(sweet_swapdone & SWEET_SWAPDONE_TICK) && !(where == 4 && (sweet_fb_early_retire & 1))) {
		return;
	}
	sweet_swapdone_events++;
	if (fPowerOff || !vsyncOn()) {
		return;
	}
	if (fPaceLastAbs != 0 && now - fPaceLastAbs < paceMinAbs()) {
		sweet_swapdone_close++;         /* a tick went out less than MIN ago: CA has one already */
		return;
	}
	sweet_swapdone_ticks++;
	sendVsync(fVsyncCount, now);
}

/*
 * 1035 (kern.sweet_fb_early_retire & 4): SwapWait poll (scalar 1 bit 31: CA's can_update_ frees the swap ring entry
 * before swap id, and the surface it holds, only when this answers 0 for id) on the 0944 queue path. Without it the
 * poll answers 0 at once (as before 1035): CA may draw into the surface of K-1 while K-1's present is still queued,
 * i.e. before its blit is on the ring, and that blit then reads the next frame (out-of-order / torn content). Busy while swap id still waits in the queue or is being presented;
 * 0 once its blit is on the ring (or it was replaced, or anything else). Source hazard: the blit reads CA's surface
 * from the ring, after the swap is retired. CA's next rendering into that surface is queued on the same ring after
 * the blit (one ring, in order, no preemption: sweet_a6xx.c), and the blit IB starts with CACHE_INVALIDATE + WFI and
 * ends with CCU flushes + CACHE_FLUSH_TS, so the GPU finishes reading before it writes the surface again. A CPU
 * write by CA into the surface (software layers) after retirement could still tear the frame: CA draws its
 * framebuffer surfaces with the GPU here (metal2vulkan). Pending over 100 ms: 0 (never stuck). With no queue: 0.
 */
IOReturn
AppleCLCD::erPoll(uint32_t id)
{
	if (!fSwapQ || (sweet_fbx & SWEET_FBX_CPUPRESENT)) {
		return kIOReturnSuccess;
	}
	IOLockLock(fLock);
	bool pending = false;
	uint64_t since = 0;
	if (sweet_er_presenting && sweet_er_presenting_id == id) {
		pending = true;
		since = fPresentRecAbs;
	}
	for (uint32_t i = 0; !pending && i < fQn; i++) {
		uint32_t s = (fQHead + i) % 2;
		if (get32(fQ + s * SWEET_FB_SWAP_REC_SIZE, 0x50) == id) {
			pending = true;
			since = fQAbs[s];
		}
	}
	if (pending && since != 0) {
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
		if (ns > 100 * NSEC_PER_MSEC) {
			sweet_er_stale++;
			pending = false;
		}
	}
	if (pending) {
		sweet_er_busy++;
	} else {
		sweet_er_done++;
	}
	IOLockUnlock(fLock);
	return pending ? kIOReturnBusy : kIOReturnSuccess;
}

/* 0977: with the fLock. Does any client take vsync notifications now (CA turns them on while it animates)? */
bool
AppleCLCD::vsyncOn(void)
{
	for (unsigned int i = 0; i < fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fClients->getObject(i);
		if (client->fFunc[SWEET_FB_NOTIFY_VSYNC] != 0 && client->fPort != MACH_PORT_NULL) {
			return true;
		}
	}
	return false;
}

/* 0983: with the fLock. MIN in absolute time (at least 1 ms). */
uint64_t
AppleCLCD::paceMinAbs(void)
{
	uint64_t abs;
	nanoseconds_to_absolutetime((uint64_t)(sweet_pace_min < 1000 ? 1000 : sweet_pace_min) * 1000ULL, &abs);
	return abs;
}

/* 0983: with the fLock. May a tick go early (at a SwapEnd)? With +16 only while the swap queue is empty. */
bool
AppleCLCD::paceGate(void)
{
	if (!(sweet_pace & 16) || !fSwapQ || fQn == 0) {
		return true;
	}
	fPaceGated++;
	return false;
}

/* 0983 mode 3: the notification at last + MIN (unless a tick went out meanwhile). On the work loop. */
void
AppleCLCD::paceFired(OSObject *owner, __unused IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	IOLockLock(fb->fLock);
	if (fb->fPaceArmed) {
		fb->fPaceArmed = false;
		uint64_t min = fb->paceMinAbs();
		if ((sweet_pace & 7) == 3 && !fb->fPowerOff && fb->vsyncOn() &&
		    (fb->fPaceLastAbs == 0 || now - fb->fPaceLastAbs >= min - min / 8)) {
			fb->fPaceEarly++;
			fb->sendVsync(fb->fVsyncCount, now);
		}
	}
	IOLockUnlock(fb->fLock);
}

void
AppleCLCD::presentLoop(void *arg)
{
	AppleCLCD *fb = (AppleCLCD *)arg;
	bool bound = false;
	bool blitOut = false;   /* 0987: a present blit was out at the last look */
	for (;;) {
		/*
		 * 0946: while the CPU presents (sweetfbx & 8), this thread runs on a big core only: CPU6 (A76 gold; the
		 * render server takes CPU7 first, 0147/0962), boot-arg sweetfbcpu=N. Takes effect when it next blocks.
		 */
		bool want = (sweet_fbx & SWEET_FBX_CPUPRESENT) != 0;
		if (want != bound) {
			uint32_t cpu = 6;
			PE_parse_boot_argn("sweetfbcpu", &cpu, sizeof(cpu));
			struct processor *p = want ? cpu_to_processor((int)cpu) : NULL;
			if (!want || p != NULL) {
				thread_bind(p);
				IOLog("SWEET FB: present thread %s CPU%u\n", want ? "bound to" : "unbound from", cpu);
			}
			bound = want;
		}
		IOLockLock(fb->fLock);
		for (;;) {
			/* 0944: a queued frame goes at once (a put-off one again 1 ms later) */
			bool queued = fb->fSwapQ && fb->fQn != 0 && !fb->fPowerOff;
			if (fb->fPresentReq || (queued && mach_absolute_time() >= fb->fQRetry)) {
				break;
			}
			bool out = sweet_gpu_present_poll();
			if (blitOut && !out && !(sweet_fb_early_retire & 1)) {
				fb->swapDone(mach_absolute_time(), 1);  /* 0987: the present blit's fence landed (1035 & 1: at acceptance) */
			}
			if (blitOut && !out && (sweet_fb_early_retire & 2) && queued && fb->fQRetry != 0) {
				fb->fQRetry = 0;                        /* 1035: the IB slot / back buffer is free now: try at once */
				sweet_er_kick++;
				blitOut = out;
				continue;
			}
			blitOut = out;
			if (fb->fSwapQ && out) {
				sweet_gpu_flip_poll();	/* 0944: a landed frame flips at once, latched by the next TE */
			}
			if (out || queued) {
				/* 0943: a blit is out: look at its fence every 1 ms (the stats' blit->fence latency) */
				uint64_t dl;
				clock_interval_to_deadline(1, kMillisecondScale, &dl);
				IOLockSleepDeadline(fb->fLock, &fb->fPresentReq, dl, THREAD_UNINT);
			} else {
				IOLockSleep(fb->fLock, &fb->fPresentReq, THREAD_UNINT);
			}
		}
		if (!fb->fPresentReq) {
			/* 0944: the oldest queued frame to the panel */
			uint32_t h = fb->fQHead;
			memcpy(fb->fPresentRec, fb->fQ + h * SWEET_FB_SWAP_REC_SIZE, SWEET_FB_SWAP_REC_SIZE);
			fb->fPresentRecAbs = fb->fQAbs[h];
			sweet_er_presenting = true;     /* 1035: still pending for SwapWait until present() has queued it */
			sweet_er_presenting_id = get32(fb->fPresentRec, 0x50);
			IOLockUnlock(fb->fLock);
			uint64_t t0 = mach_absolute_time();
			fb->fPresentBusy = false;
			fb->fPresentingAbs = fb->fPresentRecAbs;	/* 0946 */
			SWEET_TRACE_START(SWT_PRESENT, get32(fb->fPresentRec, 0x50), 0, 0, 0);       /* 0994 */
			fb->present(fb->fPresentRec);
			SWEET_TRACE_END(SWT_PRESENT, get32(fb->fPresentRec, 0x50), fb->fPresentBusy, 0, 0);
			fb->fStatPresentAbs += mach_absolute_time() - t0;
			bool shown = !fb->fPresentBusy, pop = shown;
			IOLockLock(fb->fLock);
			if (!shown) {
				/* the GPU still has the present before last (or a flip is not latched): a newer frame replaces it */
				pop = fb->fQn >= 2;
				fb->fStDropped += pop;
				clock_interval_to_deadline(1, kMillisecondScale, &fb->fQRetry);
			} else {
				fb->fQRetry = 0;
			}
			if (pop && fb->fQn != 0 && fb->fQHead == h) {
				fb->fQHead = (h + 1) % 2;
				fb->fQn--;
				IOLockWakeup(fb->fLock, &fb->fQn, true);   /* a held SwapEnd may go on */
			}
			sweet_er_presenting = false;
			if (shown && (sweet_fb_early_retire & 1) && !(sweet_fbx & SWEET_FBX_CPUPRESENT)) {
				/* 1035: the blit is on the ring, behind the frame's rendering and ahead of anything CA queues next */
				sweet_er_acc++;
				fb->swapDone(mach_absolute_time(), 4);
			}
			IOLockUnlock(fb->fLock);
			if (shown) {
				fb->fStatPresents++;
				fb->statShown(fb->fPresentRecAbs);
				if (sweet_fbx & SWEET_FBX_CPUPRESENT) {
					IOLockLock(fb->fLock);
					fb->swapDone(mach_absolute_time(), 2);  /* 0987: the CPU copy is done */
					IOLockUnlock(fb->fLock);
				}
			}
			continue;
		}
		fb->fPresentReq = false;
		if (fb->fSwapQ && fb->fDirty) {
			fb->fQn = 0;    /* 0944: the display came back on: the latest frame, not the queued ones */
			IOLockWakeup(fb->fLock, &fb->fQn, true);
		}
		bool go = fb->fDirty && !fb->fPowerOff;
		if (go) {
			memcpy(fb->fPresentRec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fPresentRecAbs = fb->fPendingAbs;
			fb->fDirty = false;
		}
		IOLockUnlock(fb->fLock);
		if (go) {
			uint64_t t0 = mach_absolute_time();
			fb->fPresentBusy = false;
			fb->fPresentingAbs = fb->fPresentRecAbs;	/* 0946 */
			SWEET_TRACE_START(SWT_PRESENT, get32(fb->fPresentRec, 0x50), 0, 0, 0);       /* 0994 */
			fb->present(fb->fPresentRec);
			SWEET_TRACE_END(SWT_PRESENT, get32(fb->fPresentRec, 0x50), fb->fPresentBusy, 0, 0);
			fb->fStatPresentAbs += mach_absolute_time() - t0;
			if (fb->fPresentBusy) {
				IOLockLock(fb->fLock);
				fb->fDirty = true;
				fb->fStDropped += fb->fPendingAbs != fb->fPresentRecAbs;	/* 0943 */
				IOLockUnlock(fb->fLock);
			} else {
				fb->fStatPresents++;
				fb->statShown(fb->fPresentRecAbs);
				if (sweet_fbx & SWEET_FBX_CPUPRESENT) {
					IOLockLock(fb->fLock);
					fb->swapDone(mach_absolute_time(), 2);  /* 0987: the CPU copy is done */
					IOLockUnlock(fb->fLock);
				}
			}
		}
	}
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg sweetfb_hz,
 * default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	uint64_t now = mach_absolute_time();
	uint32_t fence = sweet_gpu_fence_submitted();   /* 0949: CA's rendering of this frame is at or before it */
	SWEET_TRACE_START(SWT_SWAP, get32(rec, 0x50), fence, get32(rec, 0x104), 0);      /* 0994 */
	IOLockLock(fLock);
	fSwapFence[fSwapFenceNext].abs = now;
	fSwapFence[fSwapFenceNext].fence = fence;
	fSwapFenceNext = (fSwapFenceNext + 1) % 4;
	/* 0943: swap->swap interval (ms buckets, pauses over 500 ms not counted), and frames never shown */
	if (fStLastSwap != 0) {
		uint64_t ns;
		absolutetime_to_nanoseconds(now - fStLastSwap, &ns);
		uint64_t t = ns / 100000;       /* 0.1 ms */
		if (t <= 5000) {
			fStBucket[t <= 111 ? 0 : t <= 180 ? 1 : t <= 260 ? 2 : t <= 350 ? 3 : 4]++;
		}
	}
	static uint64_t flowing;
	if (flowing == 0) {
		nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &flowing);
	}
	if (fStLastSwap != 0 && now - fStLastSwap < flowing) {
		sweet_cpufreq_boost(1);         /* 0970: frames flowing (< 50 ms apart): animation boost */
	}
	sweet_cpufreq_frame();          /* 1037: frames flow: hold every cluster >= kern.sweet_gov_framehold % (0 = off) */
	fStLastSwap = now;
	fStSwaps++;
	fStDropped += fDirty;
	if ((sweet_pace & 7) == 1) {
		/* 0977: the frame in flight is done: a tick held for it goes now (0983 +16: if the queue is empty) */
		fPaceSwapped = true;
		if (fPaceOwed && !fPowerOff && paceGate()) {
			fPaceOwed = false;
			fPaceAtSwap++;
			if (fNextVsync > now) {
				fPaceAheadAbs += fNextVsync - now;
			}
			sendVsync(fPaceOwedCount, now);
		}
	} else if (((sweet_pace & 7) == 2 || (sweet_pace & 7) == 3) && !fPowerOff && vsyncOn() && paceGate()) {
		/* 0983: render-ahead: CA may start its next frame now rather than at the next tick */
		uint64_t min = paceMinAbs();
		if (fPaceLastAbs == 0 || now - fPaceLastAbs >= min) {
			fPaceEarly++;
			fPaceAtSwap++;
			if (fNextVsync > now) {
				fPaceAheadAbs += fNextVsync - now;
			}
			sendVsync(fVsyncCount, now);
		} else if ((sweet_pace & 7) == 3 && !fPaceArmed && fPaceTimer != NULL && fPaceLastAbs + min < fNextVsync) {
			fPaceArmed = true;
			fPaceTimer->wakeAtTime(fPaceLastAbs + min);
		}
	}
	if (sweet_scan_live()) {
		/* 0976: a frame on a scanout-slot surface goes to the display engine itself */
		IOLockUnlock(fLock);
		/* 0989: the swapper's own last submit (its frame), not the latest of any client's */
		uint32_t own = sweet_gpu_fence_of(proc_selfpid());
		if (sweet_scan_swap(this, rec, now, own != 0 && (int32_t)(fence - own) >= 0 ? own : fence)) {
			SWEET_TRACE_END(SWT_SWAP, get32(rec, 0x50), 1, 0, 0);    /* 0994 */
			return;
		}
		IOLockLock(fLock);
	}
	if (fSwapQ && queueSwap(rec, now)) {
		IOLockUnlock(fLock);
		SWEET_TRACE_END(SWT_SWAP, get32(rec, 0x50), 2, 0, 0);    /* 0994 */
		return;
	}
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fPendingAbs = now;
	fDirty = true;
	IOLockUnlock(fLock);
	SWEET_TRACE_END(SWT_SWAP, get32(rec, 0x50), 0, 0, 0);    /* 0994 */
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

/*
 * ---- 0943: frame statistics, display modes, refresh rate ----
 *
 * One line every 5 s while frames come (tools parse it; keep the format):
 *   SWEET FB: 5s: swaps N shown N dropped N | le11 a le18 b le26 c le35 d gt35 e | p50 X.X p95 Y.Y ms |
 *   blitq Q us fence F us | hz H
 * swaps = SwapEnds, shown = frames that went to the panel (blit queued or CPU copy done), dropped = frames
 * replaced before they were shown; le11..gt35 = swap->swap intervals (<= 11.1, 18, 26, 35 ms, longer; pauses
 * over 500 ms not counted); p50/p95 = intervals between shown frames; blitq = mean SwapEnd -> blit queued,
 * fence = mean blit queued -> its fence seen signalled (polled every 1 ms); hz = the panel's rate.
 */
void
AppleCLCD::statShown(uint64_t swapAbs)
{
	uint64_t now = mach_absolute_time();
	SWEET_TRACE(SWT_SHOWN, swapAbs, 0, 0, 0);       /* 0994 */
	IOLockLock(fLock);
	fStShown++;
	if (swapAbs != 0 && now > swapAbs) {
		fStBlitqAbs += now - swapAbs;
		fStBlitqN++;
	}
	if (fStLastShown != 0) {
		uint64_t ns;
		absolutetime_to_nanoseconds(now - fStLastShown, &ns);
		uint64_t t = ns / 100000;       /* 0.1 ms */
		if (t < 5000) {
			fStHist[t < SWEET_FB_HIST_FINE ? t : SWEET_FB_HIST_FINE + (t - SWEET_FB_HIST_FINE) / 10]++;
			fStHistN++;
		}
	}
	fStLastShown = now;
	IOLockUnlock(fLock);
}

/* The interval (0.1 ms) under which pct % of the shown frames' intervals are. With fLock. */
static uint32_t
sweet_fb_pct(const uint16_t *hist, uint32_t n, uint32_t pct)
{
	uint32_t want = (n * pct + 99) / 100, acc = 0;
	for (uint32_t i = 0; n != 0 && i < SWEET_FB_HIST; i++) {
		acc += hist[i];
		if (acc >= want) {
			return i < SWEET_FB_HIST_FINE ? i : SWEET_FB_HIST_FINE + (i - SWEET_FB_HIST_FINE) * 10;
		}
	}
	return 0;
}

void
AppleCLCD::statLine(uint64_t now)
{
	uint64_t win;
	nanoseconds_to_absolutetime(5000000000ULL, &win);
	IOLockLock(fLock);
	if (fStWin == 0) {
		fStWin = now;
	}
	if (now - fStWin < win) {
		IOLockUnlock(fLock);
		return;
	}
	uint32_t swaps = fStSwaps, shown = fStShown, dropped = fStDropped, b[5];
	memcpy(b, fStBucket, sizeof(b));
	uint32_t p50 = sweet_fb_pct(fStHist, fStHistN, 50), p95 = sweet_fb_pct(fStHist, fStHistN, 95);
	uint64_t blitq = 0, fence = 0, fenceN = 0, vis = 0, visN = 0, cpu = 0, cpuN = fStCpuN, cvis = 0, cvisN = fStVisN;
	absolutetime_to_nanoseconds(fStCpuAbs, &cpu);
	absolutetime_to_nanoseconds(fStVisAbs, &cvis);
	fStCpuAbs = fStVisAbs = 0;
	fStCpuN = fStVisN = 0;
	uint64_t gw = 0, gwN = fStWaitN, gwLate = fStWaitLate;  /* 0949 */
	absolutetime_to_nanoseconds(fStWaitAbs, &gw);
	fStWaitAbs = 0;
	fStWaitN = fStWaitLate = 0;
	absolutetime_to_nanoseconds(fStBlitqAbs, &blitq);
	blitq = fStBlitqN ? blitq / 1000 / fStBlitqN : 0;
	uint32_t pHeld = fPaceHeld, pAt = fPaceAtSwap, pLapsed = fPaceLapsed;   /* 0977 */
	uint32_t pEarly = fPaceEarly, pSkip = fPaceSkipped, pGated = fPaceGated, pDog = fPaceDog;      /* 0983 */
	fPaceEarly = fPaceSkipped = fPaceGated = fPaceDog = 0;
	uint64_t pAhead = 0;
	absolutetime_to_nanoseconds(fPaceAheadAbs, &pAhead);
	fPaceHeld = fPaceAtSwap = fPaceLapsed = 0;
	fPaceAheadAbs = 0;
	uint32_t erAcc = sweet_er_acc, erBusy = sweet_er_busy, erDone = sweet_er_done, erStale = sweet_er_stale,
	    erKick = sweet_er_kick;       /* 1035 */
	sweet_er_acc = sweet_er_busy = sweet_er_done = sweet_er_stale = sweet_er_kick = 0;
	fStWin = now;
	fStSwaps = fStShown = fStDropped = fStBlitqN = fStHistN = 0;
	fStBlitqAbs = 0;
	bzero(fStBucket, sizeof(fStBucket));
	bzero(fStHist, sizeof(fStHist));
	IOLockUnlock(fLock);
	sweet_gpu_present_landed(&fence, &fenceN);
	absolutetime_to_nanoseconds(fence, &fence);
	fence = fenceN ? fence / 1000 / fenceN : 0;
	/* 0946: SwapEnd -> panel-visible (GPU blit landed, or CPU copy done) over both paths; CPU copy time */
	sweet_gpu_present_visible(&vis, &visN);
	absolutetime_to_nanoseconds(vis, &vis);
	vis = visN + cvisN ? (vis + cvis) / 1000 / (visN + cvisN) : 0;
	cpu = cpuN ? cpu / 1000 / cpuN : 0;
	gw = gwN ? gw / 1000 / gwN : 0;
	if (swaps == 0 && shown == 0) {
		return;
	}
	IOLog("SWEET FB: 5s: swaps %u shown %u dropped %u | le11 %u le18 %u le26 %u le35 %u gt35 %u | "
	    "p50 %u.%u p95 %u.%u ms | blitq %llu us fence %llu us | hz %u | vis %llu us cpu %llu us gpuwait %llu us late %llu\n", swaps, shown,
	    dropped, b[0], b[1], b[2], b[3], b[4], p50 / 10, p50 % 10, p95 / 10, p95 % 10, blitq, fence, fHz, vis, cpu, gw, gwLate);
	if (sweet_fb_early_retire || erAcc || erBusy || erKick) {
		/* 1035: swaps retired at ring acceptance; SwapWait polls busy/done/stale; present re-kicks at the vsync tick */
		IOLog("SWEET FB: early retire 5s: retired %u | polls busy %u done %u stale %u | rekick %u | mode %d\n",
		    erAcc, erBusy, erDone, erStale, erKick, sweet_fb_early_retire);
	}
	if (sweet_pace) {
		/* 0977: ticks held for a SwapEnd, sent at one (and how much before the next tick), lapsed to the next tick */
		IOLog("SWEET FB: pace 5s: held %u at-swap %u (%llu us before the next tick) lapsed %u | early %u skipped %u "
		    "gated %u dog %u | pace %u min %u\n", pHeld, pAt, pAt ? pAhead / 1000 / pAt : 0, pLapsed, pEarly, pSkip,
		    pGated, pDog, sweet_pace, sweet_pace_min);
	}
}

/* sweetfbx & 2: has the CPU locked the surface for writing (IOSurface seed bumped) since its last present? */
bool
AppleCLCD::cpuWrote(SweetSurface *s)
{
	/* the seed: u32 at +0xc of the shared state page (SweetSurface.cpp SWEET_SURFACE_STATE_SEED), bumped by
	 * every unlock that was not read-only */
	uint32_t seed = s->state != NULL ? *(volatile uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + 0xc) : 0;
	static uint32_t logs;
	for (uint32_t i = 0; i < sizeof(fSeen) / sizeof(fSeen[0]); i++) {
		if (fSeen[i].s == s && fSeen[i].id == s->id) {
			bool wrote = fSeen[i].seed != seed;
			fSeen[i].seed = seed;
			if (logs < 6) {
				logs++;
				IOLog("SWEET FB: surface %u seed %u: %s\n", s->id, seed, wrote ? "CPU-written, cleaned" : "clean skipped");
			}
			return wrote;
		}
	}
	fSeen[fSeenNext].s = s;
	fSeen[fSeenNext].id = s->id;
	fSeen[fSeenNext].seed = seed;
	fSeenNext = (fSeenNext + 1) % (sizeof(fSeen) / sizeof(fSeen[0]));
	return true;    /* first present seen: clean */
}

static void
sweet_fb_set_number(OSDictionary *d, const char *key, uint64_t v)
{
	OSNumber *n = OSNumber::withNumber(v, 32);
	if (n != NULL) {
		d->setObject(key, n);
		n->release();
	}
}

/*
 * 0990 (boot-arg sweetiomfbprops=1): the display pipe's source limits. QuartzCore os_logs "kIOMFB... missing!" when
 * the framebuffer service lacks them; the key strings (imgdump grep in QuartzCore and IOMobileFramebuffer, r39):
 * IOMFBMaxSrcPixels = { MaxSrcBufferWidth, MaxSrcBufferHeight, MaxSrcRectWidth, MaxSrcRectTotal } (read by CA's
 * maximumSourceRectPixels / maximumSourceRectWidth) and IOMFBMaxCompressedSizeInBytes. Values: buffers up to 16384,
 * a 4096-wide source rectangle, two 1125x2436 frames of source pixels in all, one uncompressed frame's bytes.
 */
void
AppleCLCD::publishSourceLimits(void)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sweetiomfbprops", &on, sizeof(on));
	if (!on) {
		return;
	}
	OSDictionary *px = OSDictionary::withCapacity(4);
	if (px != NULL) {
		sweet_fb_set_number(px, "MaxSrcBufferWidth", 16384);
		sweet_fb_set_number(px, "MaxSrcBufferHeight", 16384);
		sweet_fb_set_number(px, "MaxSrcRectWidth", 4096);
		sweet_fb_set_number(px, "MaxSrcRectTotal", 2ULL * SWEET_FB_LOGICAL_W * SWEET_FB_LOGICAL_H);
		setProperty("IOMFBMaxSrcPixels", px);
		px->release();
	}
	OSNumber *n = OSNumber::withNumber(4ULL * SWEET_FB_LOGICAL_W * SWEET_FB_LOGICAL_H, 32);
	if (n != NULL) {
		setProperty("IOMFBMaxCompressedSizeInBytes", n);
		n->release();
	}
	IOLog("SWEET FB: source limits published (IOMFBMaxSrcPixels, IOMFBMaxCompressedSizeInBytes)\n");
}

/*
 * sweetfbx & 1: the display's mode as CoreAnimation reads it from IOMFB (TimingElements, ColorElements; the
 * current one by GetBlock 0x40): one timing, the presented size at the panel's rate (16.16), 8-bit RGB.
 * As upstream hackios MiniIOMobileFramebuffer (publishModes).
 */
#define SWEET_FB_TIMING_ID      1
#define SWEET_FB_COLOR_ID       1
void
AppleCLCD::publishModes(void)
{
	OSDictionary *timing = OSDictionary::withCapacity(8), *horizontal = OSDictionary::withCapacity(4);
	OSDictionary *vertical = OSDictionary::withCapacity(4), *color = OSDictionary::withCapacity(8);
	OSArray *timings = OSArray::withCapacity(1), *colors = OSArray::withCapacity(1);
	uint64_t rate = (uint64_t)fHz << 16;
	if (timing != NULL && horizontal != NULL && vertical != NULL && color != NULL && timings != NULL && colors != NULL) {
		sweet_fb_set_number(color, "ID", SWEET_FB_COLOR_ID);
		sweet_fb_set_number(color, "PixelEncoding", 0);         /* RGB 4:4:4 */
		sweet_fb_set_number(color, "Depth", 8);
		sweet_fb_set_number(color, "Colorimetry", 1);
		sweet_fb_set_number(color, "EOTF", 1);
		sweet_fb_set_number(color, "DynamicRange", 0);
		sweet_fb_set_number(color, "Score", 1);
		color->setObject("IsVirtual", kOSBooleanFalse);
		colors->setObject(color);
		sweet_fb_set_number(horizontal, "Active", fLogicalW);
		sweet_fb_set_number(horizontal, "Total", fLogicalW);
		sweet_fb_set_number(horizontal, "SyncRate", rate);
		sweet_fb_set_number(horizontal, "PreciseSyncRate", rate);
		sweet_fb_set_number(vertical, "Active", fLogicalH);
		sweet_fb_set_number(vertical, "Total", fLogicalH);
		sweet_fb_set_number(vertical, "SyncRate", rate);
		sweet_fb_set_number(vertical, "PreciseSyncRate", rate);
		sweet_fb_set_number(timing, "ID", SWEET_FB_TIMING_ID);
		sweet_fb_set_number(timing, "Score", 1);
		timing->setObject("IsVirtual", kOSBooleanFalse);
		timing->setObject("IsOverscanned", kOSBooleanFalse);
		timing->setObject("HorizontalAttributes", horizontal);
		timing->setObject("VerticalAttributes", vertical);
		timing->setObject("ColorModes", colors);
		timings->setObject(timing);
		setProperty("TimingElements", timings);
		setProperty("ColorElements", colors);
		IOLog("SWEET FB: TimingElements %ux%u at %u Hz (SyncRate %#llx)\n", fLogicalW, fLogicalH, fHz, rate);
	}
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

/*
 * 0995: IOMFBDisplayRefresh { displayMinRefreshInterval } in 2^-32 s (2^32 / 90 = 47721859): CA's heartbeat when the
 * render server's environment has no CA_HEARTBEAT_RATE (the order found in QuartzCore: env, this property, 1/60).
 */
void
AppleCLCD::publishRefresh(uint64_t ns)
{
	if (ns < 1000000 || ns > 100000000) {
		return;
	}
	OSDictionary *d = OSDictionary::withCapacity(6);
	if (d == NULL) {
		return;
	}
	uint64_t v = ((ns << 32) + 500000000ULL) / 1000000000ULL;
	uint64_t abs = 0;
	sweet_fb_set_number(d, "displayMinRefreshInterval", v);
	uint64_t maxNs = 0, stepNs = 0;
	if (sweet_fb_vrr >= 2 && sweet_fb_vrr <= 16) {
		/* 0998: the DCP's full set, so that CA sees a variable-rate display (see sweet_fb_vrr) */
		maxNs = ns * sweet_fb_vrr;
		stepNs = sweet_fb_vrr_step_ns >= 1000000 && sweet_fb_vrr_step_ns <= ns ? sweet_fb_vrr_step_ns : ns;
		nanoseconds_to_absolutetime(ns, &abs);
		sweet_fb_set_number(d, "displayMinRefreshIntervalMachTime", abs);
		sweet_fb_set_number(d, "displayMaxRefreshInterval", ((maxNs << 32) + 500000000ULL) / 1000000000ULL);
		nanoseconds_to_absolutetime(maxNs, &abs);
		sweet_fb_set_number(d, "displayMaxRefreshIntervalMachTime", abs);
		sweet_fb_set_number(d, "displayRefreshStep", ((stepNs << 32) + 500000000ULL) / 1000000000ULL);
		nanoseconds_to_absolutetime(stepNs, &abs);
		sweet_fb_set_number(d, "displayRefreshStepMachTime", abs);
	}
	setProperty("IOMFBDisplayRefresh", d);
	d->release();
	fRefreshNs = ns;
	IOLog("SWEET FB: IOMFBDisplayRefresh displayMinRefreshInterval %llu (period %llu ns, %llu.%03llu Hz)"
	    " max %llu ns step %llu ns (0998 vrr %u)\n", v, ns, 1000000000000ULL / ns / 1000, 1000000000000ULL / ns % 1000,
	    maxNs, stepNs, sweet_fb_vrr);
}

/*
 * 0995: p vsync [MASK [LEAD_US]] | p vsync refresh [NS]: the tick options (see sweet_vsync); no argument: the state and
 * the calibration counts since the last p vsync (ok, late = would have lost a tick, lost = did lose one).
 * 0998: p vsync vrr N [STEP_NS]: IOMFBDisplayRefresh with max = N periods and the step (0 = one period); N 0 = only
 * the minimum again. Republished at once with the last published (else the measured) period.
 */
void
AppleCLCD::vsyncCmd(const char *q, const char *end)
{
	while (q < end && *q == ' ') {
		q++;
	}
	if (end - q >= 7 && !strncmp(q, "refresh", 7)) {
		q += 7;
		while (q < end && *q == ' ') {
			q++;
		}
		uint64_t ns = 0;
		while (q < end && *q >= '0' && *q <= '9') {
			ns = ns * 10 + (uint64_t)(*q++ - '0');
		}
		if (ns == 0 && fFcPeriod != 0) {
			absolutetime_to_nanoseconds(fFcPeriod, &ns);
		}
		publishRefresh(ns);
		return;
	}
	if (end - q >= 3 && !strncmp(q, "vrr", 3)) {
		q += 3;
		uint32_t n = 0, step = 0;
		while (q < end && *q == ' ') {
			q++;
		}
		while (q < end && *q >= '0' && *q <= '9') {
			n = n * 10 + (uint32_t)(*q++ - '0');
		}
		while (q < end && *q == ' ') {
			q++;
		}
		while (q < end && *q >= '0' && *q <= '9') {
			step = step * 10 + (uint32_t)(*q++ - '0');
		}
		sweet_fb_vrr = n >= 2 && n <= 16 ? n : 0;
		sweet_fb_vrr_step_ns = step;
		uint64_t ns = fRefreshNs;
		if (ns == 0 && fFcPeriod != 0) {
			absolutetime_to_nanoseconds(fFcPeriod, &ns);
		}
		if (ns == 0) {
			ns = fHz == 90 ? 11111111ULL : SWEET_FB_VSYNC_US * 1000ULL;
		}
		publishRefresh(ns);
		return;
	}
	bool set = false;
	uint32_t v = 0, lead = 0;
	bool haveLead = false;
	if (q < end && *q >= '0' && *q <= '9') {
		set = true;
		while (q < end && *q >= '0' && *q <= '9') {
			v = v * 10 + (uint32_t)(*q++ - '0');
		}
		while (q < end && *q == ' ') {
			q++;
		}
		while (q < end && *q >= '0' && *q <= '9') {
			haveLead = true;
			lead = lead * 10 + (uint32_t)(*q++ - '0');
		}
	}
	IOLockLock(fLock);
	if (set) {
		sweet_vsync = v;
		if (haveLead) {
			sweet_vsync_lead = lead > 5000 ? 5000 : lead;
		}
		fFcEarlyUs = 300;
	}
	uint32_t ok = sweet_vsync_cal_ok, late = sweet_vsync_cal_late, lost = sweet_vsync_cal_lost;
	sweet_vsync_cal_ok = sweet_vsync_cal_late = sweet_vsync_cal_lost = 0;
	uint64_t pns = 0;
	absolutetime_to_nanoseconds(fFcPeriod, &pns);
	IOLockUnlock(fLock);
	IOLog("SWEET FB: vsync now %#x lead %u us (1 no timer slop, 2 calibration keeps its tick, 4 measured period); "
	    "calibrations since last: ok %u late %u lost %u, margin %u us; panel period %llu ns; refresh published %llu ns\n",
	    sweet_vsync, sweet_vsync_lead, ok, late, lost, fFcEarlyUs, pns, fRefreshNs);
}

/*
 * The S6E3HC2's refresh rate: DCS short write 0x53 0x28 = 60 Hz, 0x53 0x38 = 90 Hz, same DSI clock and porches
 * (dsi-panel-s6e3hc2-dvt-dsc-1080p-cmd.dtsi, android-msm-redbull-4.19). Autorefresh starts one frame per TE, so
 * the MDP follows the panel. Then the vsync ticks and SyncRate. tc: also scale INTF1's tear-check vsync_count
 * (the line clock of its read pointer) by old/new rate, as the SDE driver does for a mode switch.
 */
bool
AppleCLCD::setHz(uint32_t hz, bool tc)
{
	if (hz != 60 && hz != 90) {
		return false;
	}
	if (fDsi == NULL) {
		fDsi = (volatile uint8_t *)ml_io_map(SWEET_BL_DSI0_PA, 0x1000);
	}
	if (fBlPkt == NULL) {
		fBlPkt = (volatile uint32_t *)ml_io_map(SWEET_BL_PKT_PA, 0x1000);
	}
	const uint8_t pkt[4] = { 0x53, (uint8_t)(hz == 90 ? 0x38 : 0x28), 0x15, 0x80 };   /* short write, last */
	bool ok = dsiSend(pkt, sizeof(pkt));
	IOLog("SWEET FB: hz %u: DCS 0x53 %#x %s\n", hz, pkt[1], ok ? "sent" : "NO DMA DONE");
	if (!ok) {
		return false;
	}
	uint32_t old = fHz;
	uint64_t period;
	nanoseconds_to_absolutetime(hz == 90 ? 11111111ULL : SWEET_FB_VSYNC_US * 1000ULL, &period);
	fHz = hz;
	if (fVsyncAbs != 0) {
		fVsyncAbs = period;     /* from the next tick on */
	}
	if (sweet_fbx & SWEET_FBX_MODES) {
		publishModes();
	}
	if (tc && old != hz) {
		uint32_t v = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_SYNC_VSYNC), cnt = v & 0x7ffff;
		uint32_t ncnt = cnt * old / hz;
		if (cnt != 0) {
			sweet_mdp_wr(SWEET_INTF1 + INTF_TEAR_SYNC_VSYNC, (v & ~0x7ffffu) | ncnt);
		}
		IOLog("SWEET FB: hz %u: tear-check vsync %#x -> %#x\n", hz, v, sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_SYNC_VSYNC));
	}
	return true;
}

/*
 * ---- 0948: direct-scanout experiments, steps (a)-(c) of docs/redfin/scanout-plan.md; inert unless poked ----
 *
 *   p mdp dump          log the display engine as ABL left it (and as we changed it): MDSS/MDP top, CTL0-3, VIG0/1
 *                       with their QSEED3LITE scalers, DMA0-2, LM0/1, PP0/1, DSC0/1, INTF1 (non-zero words only)
 *   p mdp smmu [walk]   the apps SMMU's match registers that take the MDP streams (lito DT smmu_sde_unsec
 *                       <&apps_smmu 0xb40 0x402>, sec 0xb41), their S2CR, CBAR and context bank; walk: also the bank's
 *                       stage-1 tables for IOVA 0xa0000000 (reads table pages: only once SCTLR.M = 1 was seen)
 *   p mdp test [SECS]   VIG0/VIG1 on a test pattern at 0xa1400000 (splash memory above 0905's two panel buffers),
 *                       the current geometry, no scaler; put back after SECS (default 10, 0 = at p mdp restore)
 *   p mdp scale [SECS]  a 1125x2436 pattern (4608-byte rows, like CA's surfaces) through both scalers to 2 x 540x2340:
 *                       overlapping source crops (0 and 562, 563 wide), the right pipe's initial phase 0.5
 *   p mdp ca [SLOT [SECS]] 0975: the CA surface in scanout slot SLOT (default 0; boot-arg sweetscanout) scaled the
 *                       same way, as a still frame
 *   p mdp restore       every pipe register written goes back, one CTL0 flush, presents resume
 *
 * Registers only inside the MDP's 2 MB window (sweet_mdp_rd/wr); the SMMU through its own 4 KB pages. While a test
 * owns the pipes, presents are skipped (present()) and 0905's flips leave the pipes alone (sweet_gpu_mdp_own).
 */
#define MDP_CTL(n)              (0x2000u + 0x200u * (n))
#define MDP_VIG(n)              (0x5000u + 0x2000u * (n))
#define MDP_DMA(n)              (0x25000u + 0x2000u * (n))
#define MDP_LM(n)               (0x45000u + 0x1000u * (n))
#define MDP_PP(n)               (0x71000u + 0x800u * (n))
#define MDP_DSC(n)              (0x81000u + 0x400u * (n))
#define MDP_CTL_FLUSH           0x018u
#define MDP_FLUSH_VIG01         0x3u
#define SSPP_SRC_SIZE           0x000u
#define SSPP_SRC_IMG            0x004u
#define SSPP_SRC_XY             0x008u
#define SSPP_OUT_SIZE           0x00cu
#define SSPP_OUT_XY             0x010u
#define SSPP_SRC0               0x014u
#define SSPP_YSTRIDE0           0x024u
#define SSPP_FORMAT             0x030u  /* bit 8: alpha (SRCC3_EN) */
#define SSPP_PE(c, r)           (0x100u + 0x10u * (c) + 4u * (r))       /* pixel extension C0/C1C2/C3: LR, TB, REQ */
#define QSEED                   0xa00u  /* lito DT qcom,sde-vig-qseed-off, "qseedv3lite" */
#define QS_VERSION              (QSEED + 0x00)
#define QS_OP_MODE              (QSEED + 0x04)
#define QS_STEP_Y_H             (QSEED + 0x10)
#define QS_STEP_Y_V             (QSEED + 0x14)
#define QS_STEP_UV_H            (QSEED + 0x18)
#define QS_STEP_UV_V            (QSEED + 0x1c)
#define QS_PRELOAD              (QSEED + 0x20)
#define QS_SRC_SIZE             (QSEED + 0x40)
#define QS_SRC_SIZE_UV          (QSEED + 0x44)
#define QS_DST_SIZE             (QSEED + 0x48)
#define QS_INIT_Y_H             (QSEED + 0x90)
#define QS_INIT_Y_V             (QSEED + 0x94)
#define QS_INIT_UV_H            (QSEED + 0x98)
#define QS_INIT_UV_V            (QSEED + 0x9c)
#define SWEET_SPLASH_PA         0xa0000000ULL   /* redfin cont_splash_region */
#define SWEET_SPLASH_LEN        0x2300000ULL
#define SWEET_MDP_TEST_PA       0xa1400000ULL   /* above A6_FB_SIZE (0905's buffers), below the backlight packet */
#define SWEET_MDP_TEST_LEN      0xab8000ULL     /* 4608 x 2436 in 16 KB pages: ends 0xa1eb8000 < 0xa2200000 */
#define SWEET_MDP_SURF_STRIDE   4608u
#define SWEET_SMMU_PA           0x15000000ULL
#define SWEET_MDP_SID           0xb40u
#define SWEET_MDP_SID_MASK      0x402u

static const uint16_t sweet_mdp_regs[] = {
	SSPP_SRC_SIZE, SSPP_SRC_IMG, SSPP_SRC_XY, SSPP_OUT_SIZE, SSPP_OUT_XY, SSPP_SRC0, SSPP_YSTRIDE0,
	SSPP_PE(0, 0), SSPP_PE(0, 1), SSPP_PE(0, 2), SSPP_PE(1, 0), SSPP_PE(1, 1), SSPP_PE(1, 2),
	SSPP_PE(2, 0), SSPP_PE(2, 1), SSPP_PE(2, 2),
	QS_OP_MODE, QS_STEP_Y_H, QS_STEP_Y_V, QS_STEP_UV_H, QS_STEP_UV_V, QS_PRELOAD, QS_SRC_SIZE, QS_SRC_SIZE_UV,
	QS_DST_SIZE, QS_INIT_Y_H, QS_INIT_Y_V, QS_INIT_UV_H, QS_INIT_UV_V,
};
#define SWEET_MDP_NREGS         (sizeof(sweet_mdp_regs) / sizeof(sweet_mdp_regs[0]))
enum { MR_SRC_SIZE, MR_SRC_IMG, MR_SRC_XY, MR_OUT_SIZE, MR_OUT_XY, MR_SRC0, MR_YSTRIDE0 };

static struct {
	IOLock             *lock;
	IOTimerEventSource *timer;
	bool                active;         /* the pipes are ours; old[] holds what goes back */
	bool                scan;           /* 0976: ... for the direct scanout (p scanout 0 gives them back) */
	uint32_t            old[2][SWEET_MDP_NREGS];
	vm_offset_t         smmu_va[6];
	uint64_t            smmu_pa[6];
} sweet_mdp;
static volatile bool sweet_mdp_busy;    /* present() skips frames */

/* Non-zero words of one block, eight to a line. */
static void
sweet_mdp_block(const char *name, uint32_t base, uint32_t len)
{
	char line[200];
	int n = snprintf(line, sizeof(line), "SWEET MDP: %s +%05x:", name, base), k = 0, lines = 0;
	for (uint32_t o = 0; o < len; o += 4) {
		uint32_t v = sweet_mdp_rd(base + o);
		if (v == 0) {
			continue;
		}
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %x=%x", o, v);
		if (++k == 8) {
			IOLog("%s\n", line);
			lines++;
			n = snprintf(line, sizeof(line), "SWEET MDP: %s  ", name);
			k = 0;
		}
	}
	if (k != 0 || lines == 0) {
		IOLog("%s%s\n", line, k == 0 ? " (all zero)" : "");
	}
}

static void
sweet_mdp_dump(void)
{
	IOLog("SWEET MDP: dump: mdss %08x intf1 frames %u ctl0 flush %x\n", sweet_mdp_rd(0),
	    sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT), sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH));
	sweet_mdp_block("mdss", 0, 0x60);
	sweet_mdp_block("top", 0x1000, 0x494);
	for (uint32_t c = 0; c < 4; c++) {
		uint32_t b = MDP_CTL(c);
		IOLog("SWEET MDP: ctl%u: layer %x %x %x %x top %x flush %x dsc %x intf %x\n", c, sweet_mdp_rd(b),
		    sweet_mdp_rd(b + 4), sweet_mdp_rd(b + 8), sweet_mdp_rd(b + 0xc), sweet_mdp_rd(b + 0x14),
		    sweet_mdp_rd(b + MDP_CTL_FLUSH), sweet_mdp_rd(b + 0xe8), sweet_mdp_rd(b + 0xf4));
	}
	sweet_mdp_block("ctl0", MDP_CTL(0), 0x1e4);
	sweet_mdp_block("vig0", MDP_VIG(0), 0x1f8);
	sweet_mdp_block("vig0.qseed", MDP_VIG(0) + QSEED, 0xa0);
	sweet_mdp_block("vig1", MDP_VIG(1), 0x1f8);
	sweet_mdp_block("vig1.qseed", MDP_VIG(1) + QSEED, 0xa0);
	for (uint32_t d = 0; d < 3; d++) {
		uint32_t b = MDP_DMA(d);
		IOLog("SWEET MDP: dma%u: size %x xy %x out %x addr %x stride %x fmt %x\n", d, sweet_mdp_rd(b + SSPP_SRC_SIZE),
		    sweet_mdp_rd(b + SSPP_SRC_XY), sweet_mdp_rd(b + SSPP_OUT_SIZE), sweet_mdp_rd(b + SSPP_SRC0),
		    sweet_mdp_rd(b + SSPP_YSTRIDE0), sweet_mdp_rd(b + SSPP_FORMAT));
	}
	sweet_mdp_block("lm0", MDP_LM(0), 0x320);
	sweet_mdp_block("lm1", MDP_LM(1), 0x320);
	sweet_mdp_block("pp0", MDP_PP(0), 0xd4);
	sweet_mdp_block("pp1", MDP_PP(1), 0xd4);
	sweet_mdp_block("dsc0", MDP_DSC(0), 0x140);
	sweet_mdp_block("dsc1", MDP_DSC(1), 0x140);
	sweet_mdp_block("intf1", SWEET_INTF1, 0x2b8);
}

/* ---- the apps SMMU (read only), one 4 KB device page per register page used ---- */

static volatile uint32_t *
sweet_smmu_reg(uint64_t pa)
{
	uint64_t page = pa & ~0xfffULL;
	for (uint32_t i = 0; i < 6; i++) {
		if (sweet_mdp.smmu_va[i] == 0) {
			sweet_mdp.smmu_va[i] = ml_io_map((vm_offset_t)page, 0x1000);
			sweet_mdp.smmu_pa[i] = page;
		}
		if (sweet_mdp.smmu_va[i] != 0 && sweet_mdp.smmu_pa[i] == page) {
			return (volatile uint32_t *)(sweet_mdp.smmu_va[i] + (pa & 0xfff));
		}
	}
	return NULL;
}

static uint32_t
sweet_smmu_rd(uint64_t pa)
{
	volatile uint32_t *r = sweet_smmu_reg(pa);
	uint32_t v = r != NULL ? *r : 0xdeadbeefu;
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

/* One 64-bit descriptor of a table page (mapped uncached for the read, then dropped). */
static bool
sweet_smmu_desc(uint64_t pa, uint64_t *out)
{
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(pa & ~0xfffULL, 0x1000, kIODirectionIn);
	IOMemoryMap *map = md != NULL ? md->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(md);
	if (map == NULL) {
		return false;
	}
	*out = *(volatile uint64_t *)(map->getVirtualAddress() + (pa & 0xff8));
	map->release();
	return true;
}

static void
sweet_smmu_walk(uint64_t cb, uint64_t iova)
{
	uint32_t tcr = sweet_smmu_rd(cb + 0x30), t0sz = tcr & 0x3f;
	uint64_t table = ((uint64_t)sweet_smmu_rd(cb + 0x24) << 32 | sweet_smmu_rd(cb + 0x20)) & 0x0000fffffffff000ULL;
	if (((tcr >> 14) & 3) != 0 || t0sz < 16 || t0sz > 39) {
		IOLog("SWEET MDP: smmu walk: TCR %#x not a 4 KB-granule stage 1 handled here\n", tcr);
		return;
	}
	for (uint32_t level = t0sz >= 34 ? 2 : t0sz >= 25 ? 1 : 0; level <= 3; level++) {
		uint32_t shift = 39 - 9 * level;
		uint64_t d = 0, at = table + 8 * ((iova >> shift) & 0x1ff);
		if (!sweet_smmu_desc(at, &d)) {
			IOLog("SWEET MDP: smmu walk: table page %#llx not mappable\n", at);
			return;
		}
		IOLog("SWEET MDP: smmu walk %#llx L%u [%#llx] = %#llx\n", iova, level, at, d);
		if (!(d & 1) || level == 3 || !(d & 2)) {
			return;         /* invalid, page or block */
		}
		table = d & 0x0000fffffffff000ULL;
	}
}

static void
sweet_mdp_smmu(bool walk)
{
	uint64_t g0 = SWEET_SMMU_PA;
	uint32_t id0 = sweet_smmu_rd(g0 + 0x20), id1 = sweet_smmu_rd(g0 + 0x24);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff, psize = (id1 >> 31) ? 0x10000 : 0x1000;
	uint32_t npage = 1u << (((id1 >> 28) & 7) + 1), valid = 0, hits = 0;
	IOLog("SWEET MDP: smmu: cr0 %x idr0 %x idr1 %x: %u SMRs, %u banks, page %#x, banks at +%#x\n",
	    sweet_smmu_rd(g0), id0, id1, nsmr, ncb, psize, npage * psize);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = sweet_smmu_rd(g0 + 0x800 + 4 * i);
		if (!(smr >> 31)) {
			continue;
		}
		valid++;
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (((id ^ SWEET_MDP_SID) & ~(mask | SWEET_MDP_SID_MASK) & 0x7ffe) != 0) {
			continue;       /* takes none of 0xb40/0xb41/0xb42/0xf40.. */
		}
		hits++;
		uint32_t s2cr = sweet_smmu_rd(g0 + 0xc00 + 4 * i), type = (s2cr >> 16) & 3, cb = s2cr & 0xff;
		IOLog("SWEET MDP: smmu: SMR%u %#x (id %#x mask %#x) S2CR %#x (%s, bank %u)\n", i, smr, id, mask, s2cr,
		    type == 0 ? "translate" : type == 1 ? "bypass" : "fault", cb);
		if (type != 0 || cb >= ncb) {
			continue;
		}
		uint64_t b = g0 + (uint64_t)(npage + cb) * psize;
		uint32_t sctlr = sweet_smmu_rd(b);
		IOLog("SWEET MDP: smmu: bank %u CBAR %#x CBA2R %#x SCTLR %#x (M %u) TCR %#x TCR2 %#x TTBR0 %08x%08x MAIR0 %#x "
		    "FSR %#x FAR %08x%08x\n", cb, sweet_smmu_rd(g0 + psize + 4 * cb), sweet_smmu_rd(g0 + psize + 0x800 + 4 * cb),
		    sctlr, sctlr & 1, sweet_smmu_rd(b + 0x30), sweet_smmu_rd(b + 0x10), sweet_smmu_rd(b + 0x24),
		    sweet_smmu_rd(b + 0x20), sweet_smmu_rd(b + 0x38), sweet_smmu_rd(b + 0x58), sweet_smmu_rd(b + 0x64),
		    sweet_smmu_rd(b + 0x60));
		if (walk && (sctlr & 1)) {
			sweet_smmu_walk(b, SWEET_SPLASH_PA);
		}
	}
	IOLog("SWEET MDP: smmu: %u valid SMRs, %u take MDP streams\n", valid, hits);
}

/* ---- steps (b) and (c): the pipes on a test pattern, everything written put back ---- */

/* A w x h pattern at SWEET_MDP_TEST_PA: white border, red block top left, colour bars above, a 64 px grid below,
 * a red 2 px line left and right of column seam (where the two pipes meet). Through a write-combined mapping. */
static bool
sweet_mdp_pattern(uint32_t w, uint32_t h, uint32_t stride, uint32_t seam, IOMemoryDescriptor *into = NULL)
{
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	uint64_t len = round_page((uint64_t)stride * h);
	if (w == 0 || h == 0 || stride < w * 4 || len > SWEET_MDP_TEST_LEN) {
		return false;
	}
	/* 0979: or into a scanout slot's memory (RAM: through the cached kernel mapping, cleaned after) */
	IOBufferMemoryDescriptor *ram = OSDynamicCast(IOBufferMemoryDescriptor, into);
	IOMemoryMap *map = NULL;
	if (ram == NULL) {
		IOMemoryDescriptor *md = into;
		if (md != NULL) {
			md->retain();
		} else {
			md = IOMemoryDescriptor::withPhysicalAddress(SWEET_MDP_TEST_PA, len, kIODirectionInOut);
		}
		map = md != NULL ? md->map(kIOMapWriteCombineCache) : NULL;
		OSSafeReleaseNULL(md);
		if (map == NULL) {
			return false;
		}
	} else if (ram->getLength() < len) {
		return false;
	}
	uint8_t *base = ram != NULL ? (uint8_t *)ram->getBytesNoCopy() : (uint8_t *)map->getVirtualAddress();
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(base + (uint64_t)y * stride);
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c;
			if (x < 4 || x + 4 >= w || y < 4 || y + 4 >= h) {
				c = 0xffffffff;
			} else if (x >= 40 && x < 240 && y >= 40 && y < 240) {
				c = 0xffff0000;
			} else if (x + 1 == seam || x == seam) {
				c = 0xffff0000;
			} else if (y < h / 2) {
				c = bars[x * 8 / w];
			} else {
				c = (x % 64 == 0 || y % 64 == 0) ? 0xffffffff : 0xff303030;
			}
			row[x] = c;
		}
	}
	if (ram != NULL) {
		flush_dcache64((addr64_t)(uintptr_t)base, (unsigned)len, FALSE);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	OSSafeReleaseNULL(map);
	return true;
}

static void
sweet_mdp_pipes(const char *what)
{
	for (uint32_t p = 0; p < 2; p++) {
		uint32_t b = MDP_VIG(p);
		IOLog("SWEET MDP: %s: vig%u size %x img %x xy %x out %x/%x addr %x stride %x fmt %x pe %x | qseed v%x op %x "
		    "step %x/%x init %x/%x src %x dst %x\n", what, p, sweet_mdp_rd(b + SSPP_SRC_SIZE),
		    sweet_mdp_rd(b + SSPP_SRC_IMG), sweet_mdp_rd(b + SSPP_SRC_XY), sweet_mdp_rd(b + SSPP_OUT_SIZE),
		    sweet_mdp_rd(b + SSPP_OUT_XY), sweet_mdp_rd(b + SSPP_SRC0), sweet_mdp_rd(b + SSPP_YSTRIDE0),
		    sweet_mdp_rd(b + SSPP_FORMAT), sweet_mdp_rd(b + SSPP_PE(0, 2)), sweet_mdp_rd(b + QS_VERSION),
		    sweet_mdp_rd(b + QS_OP_MODE), sweet_mdp_rd(b + QS_STEP_Y_H), sweet_mdp_rd(b + QS_STEP_Y_V),
		    sweet_mdp_rd(b + QS_INIT_Y_H), sweet_mdp_rd(b + QS_INIT_Y_V), sweet_mdp_rd(b + QS_SRC_SIZE),
		    sweet_mdp_rd(b + QS_DST_SIZE));
	}
}

/* CTL0 flush of both pipes; autorefresh latches them at the next frame start (the flush bits read back clear). */
static void
sweet_mdp_flush(const char *what)
{
	uint32_t f0 = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT), i;
	uint64_t t0 = mach_absolute_time(), ns;
	sweet_mdp_wr(MDP_CTL(0) + MDP_CTL_FLUSH, MDP_FLUSH_VIG01);
	for (i = 0; i < 100 && (sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH) & MDP_FLUSH_VIG01); i++) {
		IOSleep(1);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	IOLog("SWEET MDP: %s: flush %s after %llu us, intf1 frames +%u, ctl0 flush %x\n", what,
	    i < 100 ? "latched" : "NOT latched", ns / 1000, sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT) - f0,
	    sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH));
}

/* With sweet_mdp.lock: stop the presents, wait out a flip in flight, save the pipes. */
static bool
sweet_mdp_take(AppleCLCD *fb, const char *what)
{
	if (sweet_mdp.active) {
		IOLog("SWEET MDP: %s: a test still owns the pipes (p mdp restore)\n", what);
		return false;
	}
	if (fb->fPhys != SWEET_SPLASH_PA) {
		IOLog("SWEET MDP: %s: panel memory %#llx is not redfin's splash region\n", what, fb->fPhys);
		return false;
	}
	for (uint32_t p = 0; p < 2; p++) {
		uint32_t a = sweet_mdp_rd(MDP_VIG(p) + SSPP_SRC0), out = sweet_mdp_rd(MDP_VIG(p) + SSPP_OUT_SIZE);
		if (a < SWEET_SPLASH_PA || a >= SWEET_SPLASH_PA + SWEET_SPLASH_LEN || (out & 0xffff) == 0 || (out >> 16) == 0 ||
		    sweet_mdp_rd(MDP_VIG(p) + SSPP_SRC_SIZE) == 0) {
			IOLog("SWEET MDP: %s: vig%u (addr %x out %x) is not ABL's set-up: left alone\n", what, p, a, out);
			return false;
		}
	}
	sweet_gpu_mdp_own(1);
	sweet_mdp_busy = true;
	for (uint32_t i = 0; i < 50 && (sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH) & MDP_FLUSH_VIG01); i++) {
		IOSleep(1);     /* a flip flushed just before: let it latch */
	}
	for (uint32_t p = 0; p < 2; p++) {
		for (uint32_t i = 0; i < SWEET_MDP_NREGS; i++) {
			sweet_mdp.old[p][i] = sweet_mdp_rd(MDP_VIG(p) + sweet_mdp_regs[i]);
		}
	}
	sweet_mdp.active = true;
	sweet_mdp_pipes("saved");
	return true;
}

/* With sweet_mdp.lock. */
static void
sweet_mdp_restore_locked(AppleCLCD *fb, const char *why)
{
	if (!sweet_mdp.active) {
		IOLog("SWEET MDP: restore (%s): nothing to put back\n", why);
		return;
	}
	if (sweet_mdp.timer != NULL) {
		sweet_mdp.timer->cancelTimeout();
	}
	for (uint32_t p = 0; p < 2; p++) {
		for (uint32_t i = 0; i < SWEET_MDP_NREGS; i++) {
			sweet_mdp_wr(MDP_VIG(p) + sweet_mdp_regs[i], sweet_mdp.old[p][i]);
		}
	}
	sweet_mdp_flush("restore");
	sweet_mdp_pipes("restored");
	sweet_mdp.active = false;
	sweet_mdp.scan = false;
	sweet_mdp_busy = false;
	sweet_gpu_mdp_own(0);
	IOLockLock(fb->fLock);
	fb->fDirty = true;      /* the latest frame again at the next tick */
	IOLockUnlock(fb->fLock);
	IOLog("SWEET MDP: restored (%s), presents resume\n", why);
}

static void
sweet_mdp_timer_fired(OSObject *owner, IOTimerEventSource *sender __unused)
{
	AppleCLCD *fb = OSDynamicCast(AppleCLCD, owner);
	if (fb == NULL || sweet_mdp.lock == NULL) {
		return;
	}
	IOLockLock(sweet_mdp.lock);
	if (sweet_mdp.active) {
		sweet_mdp_restore_locked(fb, "timer");
	}
	IOLockUnlock(sweet_mdp.lock);
}

/* Step (b): the pattern in the current geometry (no scaler), both pipes moved by the same offset. */
static bool
sweet_mdp_test(void)
{
	uint32_t w = (sweet_mdp.old[0][MR_OUT_SIZE] & 0xffff) + (sweet_mdp.old[1][MR_OUT_SIZE] & 0xffff);
	uint32_t h = sweet_mdp.old[0][MR_OUT_SIZE] >> 16, stride = sweet_mdp.old[0][MR_YSTRIDE0] & 0xffff;
	if (!sweet_mdp_pattern(w, h, stride, w / 2)) {
		IOLog("SWEET MDP: test: no pattern (%ux%u, stride %u)\n", w, h, stride);
		return false;
	}
	uint32_t base = sweet_mdp.old[0][MR_SRC0];
	for (uint32_t p = 0; p < 2; p++) {
		sweet_mdp_wr(MDP_VIG(p) + SSPP_SRC0, (uint32_t)SWEET_MDP_TEST_PA + (sweet_mdp.old[p][MR_SRC0] - base));
	}
	sweet_mdp_flush("test");
	sweet_mdp_pipes("test");
	return true;
}

/*
 * Step (c): 1125x2436 -> the two 540-wide halves (OUT_SIZE as ABL set it). One phase step for the whole width
 * (2^21 x 1125 / 1080), so the halves meet: panel column X samples source X x step; the right pipe starts at source
 * column 562 with phase 0.5 and both fetch 563 columns (bilinear needs the next column). QSEED3LITE in bilinear
 * mode needs no coefficient tables (as upstream DPU: _dpu_plane_setup_scaler3, dpu_hw_setup_scaler3).
 */
static bool
sweet_mdp_scale_regs(uint32_t pa, uint32_t stride, bool quiet)
{
	const uint32_t sw = SWEET_FB_LOGICAL_W, sh = SWEET_FB_LOGICAL_H;
	uint32_t ver = sweet_mdp_rd(MDP_VIG(0) + QS_VERSION), ver1 = sweet_mdp_rd(MDP_VIG(1) + QS_VERSION);
	uint32_t dw0 = sweet_mdp.old[0][MR_OUT_SIZE] & 0xffff, dw1 = sweet_mdp.old[1][MR_OUT_SIZE] & 0xffff;
	uint32_t dh = sweet_mdp.old[0][MR_OUT_SIZE] >> 16, W = dw0 + dw1;
	if (ver < 0x2000 || ver != ver1 || dh == 0 || W == 0 || dh != sweet_mdp.old[1][MR_OUT_SIZE] >> 16 ||
	    stride < sw * 4 || stride > 0xffff) {
		IOLog("SWEET MDP: scale: scaler version %x/%x, outputs %x/%x or stride %u not handled\n", ver, ver1,
		    sweet_mdp.old[0][MR_OUT_SIZE], sweet_mdp.old[1][MR_OUT_SIZE], stride);
		return false;
	}
	uint64_t step_x = ((1ULL << 21) * sw + W / 2) / W, step_y = ((1ULL << 21) * sh + dh / 2) / dh;
	for (uint32_t p = 0; p < 2; p++) {
		uint32_t b = MDP_VIG(p), dw = p ? dw1 : dw0;
		uint64_t pos = (uint64_t)(p ? dw0 : 0) * step_x;
		uint32_t sx = (uint32_t)(pos >> 21), phase = (uint32_t)(pos & 0x1fffff);
		uint32_t cw = (uint32_t)((pos + (uint64_t)(dw - 1) * step_x) >> 21) - sx + 2;
		if (sx + cw > sw) {
			cw = sw - sx;
		}
		uint32_t src = sh << 16 | cw;
		uint32_t op = 1u | 3u << 16 | 1u << 14 | 1u << 31;      /* on, Y/RGB bilinear, 8-bit format, blend cfg */
		if (sweet_mdp_rd(b + SSPP_FORMAT) & (1u << 8)) {
			op |= 1u << 10 | 1u << 29;                      /* alpha through the scaler, bilinear */
		}
		sweet_mdp_wr(b + SSPP_SRC0, pa);
		sweet_mdp_wr(b + SSPP_YSTRIDE0, (sweet_mdp.old[p][MR_YSTRIDE0] & 0xffff0000u) | stride);
		sweet_mdp_wr(b + SSPP_SRC_IMG, sh << 16 | sw);
		sweet_mdp_wr(b + SSPP_SRC_XY, sx);
		sweet_mdp_wr(b + SSPP_SRC_SIZE, src);
		for (uint32_t c = 0; c < 3; c++) {
			sweet_mdp_wr(b + SSPP_PE(c, 0), 0);
			sweet_mdp_wr(b + SSPP_PE(c, 1), 0);
			sweet_mdp_wr(b + SSPP_PE(c, 2), src);   /* requested pixels = the crop */
		}
		sweet_mdp_wr(b + QS_STEP_Y_H, (uint32_t)step_x & 0xffffff);
		sweet_mdp_wr(b + QS_STEP_UV_H, (uint32_t)step_x & 0xffffff);
		sweet_mdp_wr(b + QS_STEP_Y_V, (uint32_t)step_y & 0xffffff);
		sweet_mdp_wr(b + QS_STEP_UV_V, (uint32_t)step_y & 0xffffff);
		sweet_mdp_wr(b + QS_INIT_Y_H, phase);
		sweet_mdp_wr(b + QS_INIT_UV_H, phase);
		sweet_mdp_wr(b + QS_INIT_Y_V, 0);
		sweet_mdp_wr(b + QS_INIT_UV_V, 0);
		sweet_mdp_wr(b + QS_PRELOAD, 0x03040304u);       /* DPU_QSEED3_DEFAULT_PRELOAD_H 4, _V 3 */
		sweet_mdp_wr(b + QS_SRC_SIZE, src);
		sweet_mdp_wr(b + QS_SRC_SIZE_UV, src);
		sweet_mdp_wr(b + QS_DST_SIZE, dh << 16 | dw);
		sweet_mdp_wr(b + QS_OP_MODE, op);
		if (!quiet) {
			IOLog("SWEET MDP: scale: vig%u source x %u w %u phase %#x -> %ux%u, step %#llx/%#llx op %#x\n", p, sx, cw,
			    phase, dw, dh, step_x, step_y, op);
		}
	}
	return true;
}

static bool
sweet_mdp_scale(void)
{
	if (!sweet_mdp_pattern(SWEET_FB_LOGICAL_W, SWEET_FB_LOGICAL_H, SWEET_MDP_SURF_STRIDE, SWEET_FB_LOGICAL_W / 2 + 1)) {
		IOLog("SWEET MDP: scale: no pattern\n");
		return false;
	}
	if (!sweet_mdp_scale_regs((uint32_t)SWEET_MDP_TEST_PA, SWEET_MDP_SURF_STRIDE, false)) {
		return false;
	}
	sweet_mdp_flush("scale");
	sweet_mdp_pipes("scale");
	return true;
}

/* 0975 (step d): one of CA's surfaces in a scanout slot, scaled, as it is now (a still frame). */
static bool
sweet_mdp_ca(uint32_t slot)
{
	uint64_t pa = 0;
	SweetSurface *s = sweet_surface_scan_copy(slot, &pa);
	if (s == NULL) {
		/* 0979: an empty slot shows the test pattern from its own memory (does the MDP read this RAM?) */
		IOMemoryDescriptor *md = sweet_surface_scan_md(slot, &pa);
		bool ok = md != NULL && sweet_mdp_pattern(SWEET_FB_LOGICAL_W, SWEET_FB_LOGICAL_H, SWEET_MDP_SURF_STRIDE,
		    SWEET_FB_LOGICAL_W / 2 + 1, md) && sweet_mdp_scale_regs((uint32_t)pa, SWEET_MDP_SURF_STRIDE, false);
		OSSafeReleaseNULL(md);
		if (!ok) {
			IOLog("SWEET MDP: ca: no surface and no memory in scanout slot %u (PA %#llx; boot-arg sweetscanout=1|2)\n",
			    slot, pa);
			return false;
		}
		sweet_mdp_flush("ca");
		sweet_mdp_pipes("ca");
		IOLog("SWEET MDP: ca: slot %u is empty: the test pattern from its memory (PA %#llx) on screen\n", slot, pa);
		return true;
	}
	uint32_t id = s->id, bpr = s->bytesPerRow, seed = s->state != NULL ?
	    *(volatile uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + 0xc) : 0;
	if (OSDynamicCast(IOBufferMemoryDescriptor, s->memory) != NULL) {
		uintptr_t va = s->kernelAddress();
		if (va != 0) {
			flush_dcache64((addr64_t)va, (unsigned)((uint64_t)bpr * s->height), FALSE);
		}
	}
	s->release();
	bool ok = sweet_mdp_scale_regs((uint32_t)pa, bpr, false);
	if (ok) {
		sweet_mdp_flush("ca");
		sweet_mdp_pipes("ca");
		IOLog("SWEET MDP: ca: slot %u = surface %u (PA %#llx, %u-byte rows, seed %u) on screen\n", slot, id, pa, bpr, seed);
	}
	return ok;
}

void
AppleCLCD::mdpCmd(const char *s, const char *end)
{
	const char *q = s;
	while (q < end && *q == ' ') {
		q++;
	}
	const char *w = q;
	while (q < end && *q >= 'a' && *q <= 'z') {
		q++;
	}
	size_t wl = (size_t)(q - w);
	while (q < end && *q == ' ') {
		q++;
	}
	uint32_t num[2] = { 0, 0 }, nums = 0;
	while (nums < 2 && q < end && *q >= '0' && *q <= '9') {
		while (q < end && *q >= '0' && *q <= '9') {
			num[nums] = num[nums] * 10 + (uint32_t)(*q++ - '0');
		}
		nums++;
		while (q < end && *q == ' ') {
			q++;
		}
	}
	bool ca = wl == 2 && !strncmp(w, "ca", 2);
	uint32_t slot = ca && nums >= 1 ? num[0] : 0;           /* 0975: p mdp ca [SLOT [SECS]] */
	uint32_t secs = ca ? (nums >= 2 ? num[1] : 10) : (nums >= 1 ? num[0] : 10);
	if (secs > 600) {
		secs = 600;
	}
	if (sweet_mdp.lock == NULL) {
		IOLock *l = IOLockAlloc();
		if (l != NULL && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&sweet_mdp.lock)) {
			IOLockFree(l);
		}
		if (sweet_mdp.lock == NULL) {
			return;
		}
	}
	IOLockLock(sweet_mdp.lock);
	if (wl == 4 && !strncmp(w, "dump", 4)) {
		sweet_mdp_dump();
	} else if (wl == 4 && !strncmp(w, "smmu", 4)) {
		sweet_mdp_smmu(end - q >= 4 && !strncmp(q, "walk", 4));
	} else if ((wl == 4 && !strncmp(w, "test", 4)) || (wl == 5 && !strncmp(w, "scale", 5)) || ca) {
		bool scale = wl == 5;
		const char *what = ca ? "ca" : scale ? "scale" : "test";
		uint64_t slot0 = 0;
		SweetSurface *held = ca ? NULL : sweet_surface_scan_copy(0, &slot0);
		if (held != NULL) {
			/* 0975: the test pattern would draw over CA's surface in scanout slot 0 (same memory) */
			IOLog("SWEET MDP: %s: scanout slot 0 (%#llx) holds surface %u: use p mdp ca\n", what, slot0, held->id);
			held->release();
			IOLockUnlock(sweet_mdp.lock);
			return;
		}
		if (sweet_mdp.timer == NULL) {
			sweet_mdp.timer = IOTimerEventSource::timerEventSource(this, sweet_mdp_timer_fired);
			if (sweet_mdp.timer != NULL && fWorkLoop->addEventSource(sweet_mdp.timer) != kIOReturnSuccess) {
				OSSafeReleaseNULL(sweet_mdp.timer);
			}
		}
		if (sweet_mdp_take(this, what)) {
			if (!(ca ? sweet_mdp_ca(slot) : scale ? sweet_mdp_scale() : sweet_mdp_test())) {
				sweet_mdp_restore_locked(this, "failed");
			} else if (secs != 0 && sweet_mdp.timer != NULL) {
				sweet_mdp.timer->setTimeoutMS(secs * 1000);
				IOLog("SWEET MDP: %s on screen for %u s (p mdp restore puts it back sooner)\n", what, secs);
			} else {
				IOLog("SWEET MDP: %s on screen until p mdp restore\n", what);
			}
		}
	} else if (wl == 7 && !strncmp(w, "restore", 7)) {
		if (sweet_mdp.scan) {
			IOLog("SWEET MDP: restore: the direct scanout owns the pipes (p scanout 0)\n");
		} else {
			sweet_mdp_restore_locked(this, "poke");
		}
	} else {
		IOLog("SWEET MDP: p mdp dump | smmu [walk] | test [SECS] | scale [SECS] | ca [SLOT [SECS]] | restore\n");
	}
	IOLockUnlock(sweet_mdp.lock);
}

/*
 * ---- 0976: direct scanout (step e of docs/redfin/scanout-plan.md): boot-arg sweetscanout=1 (on) or 2 (slots
 * only, off until p scanout 1); p scanout 0|1 switches live. Inert with sweetscanout=0 (the default). ----
 *
 * A SwapEnd whose single layer is a CA surface in a scanout slot (0975) is not copied: it waits for the GPU fence
 * submitted at its SwapEnd (0949: SwapEnd comes when CA has queued the rendering, not when the GPU has drawn it),
 * then VIG0/VIG1 are pointed at it (SRC0_ADDR, CTL0 flush) and the next frame start latches both. The first such
 * frame switches the pipes to the scaled geometry of p mdp scale (registers saved as by p mdp test); a frame on any
 * other surface switches them back to the panel buffer (saved registers, flush) and goes the normal way.
 *
 * Never shown before its fence has landed, never written while the GPU may draw: the scanout thread (1 ms polls)
 * flips a frame only after a6 fence >= its SwapEnd fence, and one flip at a time (the next waits for the latch:
 * CTL0's VIG flush bits clear, or INTF1 frame counter +2). Back pressure: SwapEnd K returns once frame K-1 (or a
 * later one) is on screen (at most 50 ms), so with CA's surfaces used in turn CA never draws into the one shown.
 * SwapWait(id) waits (50 ms at most) until swap id or a later one is on screen. Each held frame counts in the
 * surface's displayUse; IOSurfaceIncrementUseCount/Decrement go to clientUse (logged: who uses them).
 */
static struct {
	volatile bool on;               /* frames go direct (p scanout / boot-arg) */
	uint32_t      gate;             /* 0985: 1 = the swapper's GPU submits wait on the CPU; 0 = SwapEnd back pressure
	                                 * (0979/0983); 0989: 2 = the CP waits (CP_WAIT_REG_MEM on the latched-frame word) */
	uint32_t      word;             /* 0989: the latched-frame word as last written (monotonic) */
	uint32_t      armedRef;         /* 0989: the highest reference a gated submit was given */
	uint64_t      armedAbs;         /* ... when that was armed */
	uint32_t      stGpuWait, stGpuRelease;
	bool          corners;          /* 0986: p scanout corners 1: the corner mattes drawn into each frame */
	bool          tribuf;           /* 0996: p scanout tribuf 1 / sweetfbx 0x40: honest SwapWait polls, gate by depth */
	uint32_t      gateDepth;        /* 0996: the chain depth the pending gate was set for (2 = K on screen) */
	uint32_t      stTbBusy, stTbDone, stTbStale, stTbShort, stTbGate3;
	uint64_t      stCornerAbs;
	uint32_t      stCorners;
	int           gatePid;          /* 0985: who swapped (backboardd): its GPU submits wait for gateSeq on screen */
	volatile uint64_t gateSeq;
	uint64_t      stGateAbs;
	uint32_t      stGate, stGateWait, stGateTimeout;
	bool          geom;             /* the pipes are in the scaled direct geometry (fLock) */
	bool          entering;
	bool          thread;
	uint32_t      stride;           /* rows of the surfaces the geometry was set for */
	SweetSurface *next, *flipping, *shown;  /* retained, displayUse held (fLock) */
	struct { SweetSurface *s; uint32_t fence, swap; uint64_t seq, abs; } later[3];  /* 0991: gate 2 queue after next */
	uint32_t      laterN, stQueued, stOverflow;
	uint32_t      nextFence, nextSwap, flipSwap, flipFrame, shownSwap;
	uint64_t      nextSeq, flipSeq, shownSeq, seq;
	uint64_t      nextAbs, flipSwapAbs, flipAbs;
	uint32_t      seed[SWEET_SCAN_SLOTS];
	uint32_t      slotRun;          /* 0979: slot frames in a row (direct only from 3: no switching every frame) */
	uint32_t      chain[8], chainNext;      /* 0979: the last slot frames' surfaces: CA's chain depth */
	uint64_t      chainSeq[8];              /* 0983: ... and their frame numbers */
	uint32_t      depth;                    /* 0983: frames since this frame's surface was last shown */
	uint32_t      stDepthMax, stDepthMin;
	/* 5 s statistics (fLock) */
	uint64_t      stWin, stFenceAbs, stLatchAbs, stLatchMax;
	uint32_t      stFrames, stFlips, stLatched, stDropped, stNonslot, stLate, stBp, stBpTimeout, stWait, stWaitTimeout;
	uint32_t      stEnter, stLeave, stClean, stLatchN, stLatchFc, stHold;
	uint32_t      lastSwap, stWaitNewest;   /* 0983: the newest slot swap's id; SwapWaits on it (not waited) */
} sweet_scan;

/*
 * 0989: the latched-frame word (gate 2). Only goes up: the frame on screen, or, to release every wait armed so far
 * (gate change, scanout off, leaving the direct geometry, a wait past 50 ms), the newest reference given out.
 */
static void
sweet_scan_word_locked(uint32_t v)
{
	if ((int32_t)(v - sweet_scan.word) > 0) {
		sweet_scan.word = v;
		sweet_gpu_scan_word(v);
	}
}

static void
sweet_scan_release_locked(void)
{
	if ((int32_t)(sweet_scan.armedRef - sweet_scan.word) > 0) {
		sweet_scan.stGpuRelease++;
	}
	sweet_scan_word_locked(sweet_scan.armedRef);
	sweet_scan_word_locked((uint32_t)sweet_scan.shownSeq);
}

static bool
sweet_scan_live(void)
{
	return sweet_scan.on;
}

static bool
sweet_mdp_lock_ready(void)
{
	if (sweet_mdp.lock == NULL) {
		IOLock *l = IOLockAlloc();
		if (l != NULL && !OSCompareAndSwapPtr(NULL, l, (void * volatile *)&sweet_mdp.lock)) {
			IOLockFree(l);
		}
	}
	return sweet_mdp.lock != NULL;
}

static void
sweet_scan_hold(SweetSurface *s)
{
	s->retain();
	OSIncrementAtomic(&s->displayUse);
	sweet_surface_publish_use(s);   /* 0983: in use for CA's swap chain */
}

static void
sweet_scan_drop(SweetSurface **sp)
{
	if (*sp != NULL) {
		OSDecrementAtomic(&(*sp)->displayUse);
		sweet_surface_publish_use(*sp);
		(*sp)->release();
		*sp = NULL;
	}
}

/* With fLock: everything the display holds goes back. */
static void
sweet_scan_drop_all_locked(void)
{
	sweet_scan_drop(&sweet_scan.next);
	sweet_scan_drop(&sweet_scan.flipping);
	sweet_scan_drop(&sweet_scan.shown);
	while (sweet_scan.laterN > 0) {
		sweet_scan_drop(&sweet_scan.later[--sweet_scan.laterN].s);
	}
}

/*
 * 0991: with gate 2 no frame may be dropped: the GPU work of frame K+1 sits in the ring behind a CP wait for frame K
 * on screen, so a K replaced by K+1 before its flip was never shown and K+1 never drew (r40a: 221 of 1302 CP waits
 * ran to the 50 ms release). Frames that come while one is waiting go into a queue of 3 and are flipped in order.
 */
static void
sweet_scan_pop_locked(void)
{
	if (sweet_scan.next != NULL || sweet_scan.laterN == 0) {
		return;
	}
	sweet_scan.next = sweet_scan.later[0].s;
	sweet_scan.nextFence = sweet_scan.later[0].fence;
	sweet_scan.nextSwap = sweet_scan.later[0].swap;
	sweet_scan.nextSeq = sweet_scan.later[0].seq;
	sweet_scan.nextAbs = sweet_scan.later[0].abs;
	sweet_scan.laterN--;
	for (uint32_t i = 0; i < sweet_scan.laterN; i++) {
		sweet_scan.later[i] = sweet_scan.later[i + 1];
	}
	sweet_scan.later[sweet_scan.laterN].s = NULL;
}

/* The pipes into the direct geometry, showing surface s (its fence has landed). Without fLock. */
static bool
sweet_scan_enter(AppleCLCD *fb, SweetSurface *s)
{
	if (!sweet_mdp_lock_ready()) {
		return false;
	}
	IOLockLock(sweet_mdp.lock);
	bool ok = sweet_mdp_take(fb, "scanout");
	if (ok) {
		sweet_mdp.scan = true;
		if (!sweet_mdp_scale_regs((uint32_t)s->scanPA, s->bytesPerRow, true)) {
			sweet_mdp_restore_locked(fb, "scanout geometry refused");
			ok = false;
		} else {
			sweet_mdp_flush("scanout on");
			IOLockLock(fb->fLock);
			sweet_scan.geom = true;
			sweet_scan.stride = s->bytesPerRow;
			sweet_scan.stEnter++;
			IOLockUnlock(fb->fLock);
		}
	}
	IOLockUnlock(sweet_mdp.lock);
	return ok;
}

/* The pipes back to the panel buffer (presents resume). Without fLock. */
static void
sweet_scan_leave(AppleCLCD *fb, const char *why)
{
	if (!sweet_mdp_lock_ready()) {
		return;
	}
	IOLockLock(sweet_mdp.lock);
	IOLockLock(fb->fLock);
	bool was = sweet_scan.geom;
	sweet_scan.geom = false;
	sweet_scan_drop_all_locked();
	sweet_scan_release_locked();    /* 0989: no CP wait outlives the direct geometry */
	if (was) {
		sweet_scan.stLeave++;
	}
	IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
	IOLockUnlock(fb->fLock);
	if (was && sweet_mdp.active && sweet_mdp.scan) {
		sweet_mdp_restore_locked(fb, why);      /* fDirty: the latest frame the normal way */
	}
	IOLockUnlock(sweet_mdp.lock);
}

/* SwapEnd (without fLock): true if the frame went direct, false for the normal path. */
static bool
sweet_scan_swap(AppleCLCD *fb, const uint8_t *rec, uint64_t now, uint32_t fence)
{
	uint32_t mask = get32(rec, 0x104);
	if (mask == 0) {
		return sweet_scan.geom;         /* changes no layer: the frame on screen stays */
	}
	SweetSurface *s = NULL;
	if (!(mask & (mask - 1))) {
		uint32_t l = (uint32_t)__builtin_ctz(mask);
		uint32_t id = l < SWEET_FB_LAYERS ? get32(rec, 0x54 + 4 * l) : 0;
		s = id != 0 ? sweet_surface_copy(id) : NULL;
	}
	if (s == NULL || s->scanSlot == 0 || s->width != SWEET_FB_LOGICAL_W || s->height != SWEET_FB_LOGICAL_H ||
	    (sweet_scan.geom && s->bytesPerRow != sweet_scan.stride)) {
		static uint32_t logs;
		if (logs < 8) {
			logs++;
			IOLog("SWEET SCANOUT: swap %u (layers %#x, surface %u, slot %u): not a scanout surface: the normal path\n",
			    get32(rec, 0x50), mask, s != NULL ? s->id : 0, s != NULL ? s->scanSlot : 0);
		}
		OSSafeReleaseNULL(s);
		SWEET_TRACE(SWT_SCAN_SWAP, get32(rec, 0x50), 0, 4, 0);  /* 0994 */
		IOLockLock(fb->fLock);
		sweet_scan.stNonslot++;
		sweet_scan.slotRun = 0;
		bool geom = sweet_scan.geom;
		IOLockUnlock(fb->fLock);
		if (geom) {
			sweet_scan_leave(fb, "non-slot frame");
		}
		return false;
	}
	IOLockLock(fb->fLock);
	if (!sweet_scan.on) {
		IOLockUnlock(fb->fLock);
		s->release();
		return false;
	}
	/*
	 * 0979: r36a's chain had one surface in a slot and one not: the pipes switched geometry on every frame (enter 70,
	 * leave 69 in 5 s). Direct only after 3 slot frames in a row; until then the normal path.
	 */
	sweet_scan.slotRun++;
	/*
	 * 0983: CA's chain depth = how many frames ago this frame's surface was last swapped (round robin). 0979 counted
	 * distinct surfaces in the last 8 frames, which after a backboardd restart (new ids next to old ones) overcounts.
	 * Unknown (first use of a surface): 2, the safe value.
	 */
	uint32_t d = 0;
	for (uint32_t i = 1; i <= 8 && d == 0; i++) {
		uint32_t k = (sweet_scan.chainNext + 8 - i) % 8;
		if (sweet_scan.chain[k] == s->id && sweet_scan.chainSeq[k] != 0) {
			d = (uint32_t)(sweet_scan.seq + 1 - sweet_scan.chainSeq[k]);
		}
	}
	sweet_scan.depth = d >= 2 ? d : 2;
	if (sweet_scan.tribuf && sweet_scan.gateDepth > sweet_scan.depth) {
		sweet_scan.stTbShort++;         /* 0996: drawn into a surface the relaxed gate did not wait for */
	}
	sweet_scan.chain[sweet_scan.chainNext] = s->id;
	sweet_scan.chainSeq[sweet_scan.chainNext] = sweet_scan.seq + 1;
	sweet_scan.chainNext = (sweet_scan.chainNext + 1) % 8;
	if (sweet_scan.depth > sweet_scan.stDepthMax) {
		sweet_scan.stDepthMax = sweet_scan.depth;
	}
	if (sweet_scan.stDepthMin == 0 || sweet_scan.depth < sweet_scan.stDepthMin) {
		sweet_scan.stDepthMin = sweet_scan.depth;
	}
	if (!sweet_scan.geom && (sweet_scan.slotRun < 3 || sweet_scan.entering)) {
		sweet_scan.stHold++;
		SWEET_TRACE(SWT_SCAN_SWAP, get32(rec, 0x50), sweet_scan.seq, 3, sweet_scan.depth);       /* 0994 */
		IOLockUnlock(fb->fLock);
		s->release();
		return false;
	}
	sweet_scan_pop_locked();
	if (sweet_scan.next != NULL && sweet_scan.gate == 2 && sweet_scan.laterN < 3) {
		/* 0991: queued behind the waiting one, not dropped */
		sweet_scan_hold(s);
		s->release();
		uint32_t i = sweet_scan.laterN++;
		sweet_scan.later[i].s = s;
		sweet_scan.later[i].fence = fence;
		sweet_scan.later[i].swap = get32(rec, 0x50);
		sweet_scan.later[i].seq = ++sweet_scan.seq;
		sweet_scan.later[i].abs = now;
		sweet_scan.lastSwap = sweet_scan.later[i].swap;
		sweet_scan.gatePid = proc_selfpid();
		sweet_scan.gateSeq = sweet_scan.later[i].seq;
		sweet_scan.gateDepth = 2;
		if (sweet_scan.tribuf && sweet_scan.depth >= 3) {
			sweet_scan.gateSeq = sweet_scan.later[i].seq - 1;      /* 0996: as below */
			sweet_scan.gateDepth = 3;
			sweet_scan.stTbGate3++;
		}
		sweet_scan.stQueued++;
		sweet_scan.stFrames++;
		SWEET_TRACE(SWT_SCAN_SWAP, sweet_scan.later[i].swap, sweet_scan.later[i].seq, 1, sweet_scan.depth);    /* 0994 */
		IOLockWakeup(fb->fLock, &sweet_scan, false);
		IOLockUnlock(fb->fLock);
		return true;
	}
	if (sweet_scan.next != NULL) {
		sweet_scan.stDropped++;         /* a newer frame before the last one was flipped */
		SWEET_TRACE(SWT_SCAN_SWAP, sweet_scan.nextSwap, sweet_scan.nextSeq, 2, 0);    /* 0994: this one is replaced */
		if (sweet_scan.gate == 2) {
			sweet_scan.stOverflow++;        /* 0991: never shown: its CP waiters go on */
			sweet_scan_word_locked((uint32_t)sweet_scan.nextSeq);
		}
		sweet_scan_drop(&sweet_scan.next);
		sweet_scan_pop_locked();
		if (sweet_scan.next != NULL) {
			/* (the queue was full: the oldest went; this frame goes to the end) */
			sweet_scan_word_locked((uint32_t)sweet_scan.nextSeq);
			sweet_scan_drop(&sweet_scan.next);
			sweet_scan_pop_locked();
		}
	}
	sweet_scan_hold(s);
	s->release();
	uint64_t seq = ++sweet_scan.seq;
	sweet_scan.next = s;
	sweet_scan.nextFence = fence;
	sweet_scan.nextSwap = get32(rec, 0x50);
	sweet_scan.lastSwap = sweet_scan.nextSwap;
	sweet_scan.nextSeq = seq;
	sweet_scan.nextAbs = now;
	sweet_scan.stFrames++;
	SWEET_TRACE(SWT_SCAN_SWAP, sweet_scan.nextSwap, seq, 0, sweet_scan.depth);    /* 0994 */
	IOLockWakeup(fb->fLock, &sweet_scan, false);
	/*
	 * Back pressure (0979: by CA's chain depth N, the different surfaces of the last 8 slot frames). CA draws frame
	 * seq + 1 into the surface of frame seq + 1 - N, which must be off screen: return once frame seq + 2 - N (or a
	 * later one) is on screen. N = 2 (r36a): this frame itself; N = 3: the one before.
	 */
	uint32_t depth = sweet_scan.depth;
	uint64_t need = seq + 2 > depth ? seq + 2 - depth : 0;
	if (!(sweet_scan.gate == 0)) {
		/*
		 * 0985: double buffering without holding CA. SwapEnd K always came before K was on screen (its fence lands
		 * ~5-12 ms later, the latch at the next frame start), so 0983's "SwapEnd K returns once K is on screen" waited
		 * on every frame (bp = frames) and CA could not even start building K+1 meanwhile. CA's frame K+1 can only
		 * write the other surface (K-1's) from the GPU, and its GPU work enters the ring through sweet_gpu_submit_v2:
		 * there the swapper's submits wait (sweet_scan_submit_gate) until frame K is on screen, i.e. until K-1's
		 * surface is no longer scanned. CA's CPU work for K+1 overlaps K's fence and latch.
		 */
		sweet_scan.gatePid = proc_selfpid();
		sweet_scan.gateSeq = seq;
		sweet_scan.gateDepth = 2;
		if (sweet_scan.tribuf && depth >= 3) {
			/*
			 * 0996: CA rotates 3 surfaces: K+1 writes the one of K-2 (if it keeps the rotation), which is free once
			 * K-1 is on screen. CA itself only reuses a surface after SwapWait said the swap after it is on screen
			 * (honest polls, sweet_scan_poll), so this gate is a second line; a frame that comes back with a shorter
			 * depth than this is counted (short).
			 */
			sweet_scan.gateSeq = seq - 1;
			sweet_scan.gateDepth = 3;
			sweet_scan.stTbGate3++;
		}
		IOLockUnlock(fb->fLock);
		return true;
	}
	if (sweet_scan.shownSeq < need && sweet_scan.geom) {
		uint64_t dl;
		clock_interval_to_deadline(50, kMillisecondScale, &dl);
		sweet_scan.stBp++;
		SWEET_TRACE_START(SWT_SCAN_BP, need, sweet_scan.shownSeq, 0, 0);       /* 0994 */
		while (sweet_scan.on && sweet_scan.geom && sweet_scan.shownSeq < need) {
			if (IOLockSleepDeadline(fb->fLock, &sweet_scan.shownSeq, dl, THREAD_UNINT) == THREAD_TIMED_OUT) {
				sweet_scan.stBpTimeout++;
				break;
			}
		}
		SWEET_TRACE_END(SWT_SCAN_BP, need, sweet_scan.shownSeq, 0, 0);
	}
	IOLockUnlock(fb->fLock);
	return true;
}

/*
 * 0985: from sweet_gpu_submit_v2, before the IBs go into the ring (without the GPU lock): the swapper's GPU work after
 * SwapEnd K waits until frame K is on screen (30 ms at most). K's own work is in the ring before its SwapEnd (0949),
 * so this cannot hold up K's fence.
 */
extern "C" uint32_t sweet_scan_submit_gate(void);
uint32_t
sweet_scan_submit_gate(void)
{
	AppleCLCD *fb = sweet_fb_instance;
	if (!sweet_scan.on || !sweet_scan.geom || sweet_scan.gate == 0 || fb == NULL || proc_selfpid() != sweet_scan.gatePid) {
		return 0;
	}
	IOLockLock(fb->fLock);
	sweet_scan.stGate++;
	SWEET_TRACE(SWT_GPU_GATE, sweet_scan.gate, sweet_scan.gateSeq, sweet_scan.shownSeq, sweet_scan.word);      /* 0994 */
	if (sweet_scan.gate == 2 && sweet_scan.geom) {
		/* 0989: the CP waits instead of this thread: until the word reaches gateSeq (released after 50 ms) */
		uint32_t ref = (uint32_t)sweet_scan.gateSeq;
		if ((int32_t)(ref - sweet_scan.word) > 0) {
			if ((int32_t)(ref - sweet_scan.armedRef) > 0 || sweet_scan.armedAbs == 0) {
				sweet_scan.armedAbs = mach_absolute_time();
			}
			if ((int32_t)(ref - sweet_scan.armedRef) > 0) {
				sweet_scan.armedRef = ref;
			}
			sweet_scan.stGpuWait++;
			IOLockWakeup(fb->fLock, &sweet_scan, false);    /* the scanout thread watches the armed wait */
			IOLockUnlock(fb->fLock);
			return ref != 0 ? ref : 1;
		}
		IOLockUnlock(fb->fLock);
		return 0;
	}
	if (sweet_scan.shownSeq < sweet_scan.gateSeq && sweet_scan.geom) {
		uint64_t dl, t0 = mach_absolute_time();
		clock_interval_to_deadline(50, kMillisecondScale, &dl);    /* 0989: 30 -> 50 ms */
		sweet_scan.stGateWait++;
		SWEET_TRACE_START(SWT_GPU_GATE, proc_selfpid(), sweet_scan.gateSeq, sweet_scan.shownSeq, 0);   /* 0994 */
		while (sweet_scan.on && sweet_scan.geom && sweet_scan.gate == 1 && sweet_scan.shownSeq < sweet_scan.gateSeq) {
			if (IOLockSleepDeadline(fb->fLock, &sweet_scan.shownSeq, dl, THREAD_UNINT) == THREAD_TIMED_OUT) {
				sweet_scan.stGateTimeout++;
				break;
			}
		}
		sweet_scan.stGateAbs += mach_absolute_time() - t0;
		SWEET_TRACE_END(SWT_GPU_GATE, proc_selfpid(), sweet_scan.gateSeq, sweet_scan.shownSeq, 0);
	}
	IOLockUnlock(fb->fLock);
	return 0;
}

/* SwapWait(id): until swap id (or a later one) is on screen, 50 ms at most. */
static void
sweet_scan_wait(AppleCLCD *fb, uint32_t id)
{
	if (!sweet_scan.on) {
		return;
	}
	/*
	 * 0983: r37a: CA calls SwapWait twice per frame, for the previous swap and for the one it just ended (scalars
	 * id, 0x80000000, ...). 0979 waited until a swap was *retired* (a later one on screen): for the newest swap that
	 * needs the next frame, which CA only makes after this returns: 50 ms timeout on every frame (10 fps). Now:
	 * until the swap is on screen (latched), and the newest one never waits (as upstream HakiOS: "a swap is on the
	 * screen as soon as it is made"). Reuse of a surface on screen is kept away by SwapEnd's back pressure and the
	 * use counts, not here.
	 */
	uint64_t dl;
	clock_interval_to_deadline(20, kMillisecondScale, &dl);
	IOLockLock(fb->fLock);
	sweet_scan.stWait++;
	if (id == sweet_scan.nextSwap || id == sweet_scan.flipSwap || (int32_t)(id - sweet_scan.lastSwap) >= 0) {
		sweet_scan.stWaitNewest++;
		IOLockUnlock(fb->fLock);
		return;
	}
	while (sweet_scan.on && sweet_scan.geom && (int32_t)(sweet_scan.shownSwap - id) < 0) {
		if (IOLockSleepDeadline(fb->fLock, &sweet_scan.shownSeq, dl, THREAD_UNINT) == THREAD_TIMED_OUT) {
			sweet_scan.stWaitTimeout++;
			break;
		}
	}
	IOLockUnlock(fb->fLock);
}

/*
 * 0996: SwapWait as a poll (p scanout tribuf 1). QuartzCore's IOMFBDisplay::can_update_ (iOS 18.2 decompile, the same
 * call r37a logged on 16.7: scalars id, 0x80000000 | (a later swap exists)) frees a swap ring entry, and the
 * framebuffer surface it holds, only when SwapWait answers 0 for the swap after it; kIOReturnBusy (0xe00002d5) means
 * "still pending" and it tries again on its next update. 0983 answered 0 for the newest swap at once, so the entry of
 * K-1 and its surface were free as soon as K was ended: CA drew K+1 into K-1's surface (a 2-surface chain) and the
 * GPU had to wait for K on screen (the 0985/0989 gate). Here a poll answers 0 only once that swap (or a later one) is
 * latched, or it is no longer pending (dropped, late, a normal-path frame); pending past 100 ms: 0 (never stuck).
 * With CA's ring of 3 that keeps K-1's surface in use until K is on screen and CA takes a third one for K+1.
 */
static IOReturn
sweet_scan_poll(AppleCLCD *fb, uint32_t id)
{
	if (!sweet_scan.on) {
		return kIOReturnSuccess;
	}
	IOLockLock(fb->fLock);
	sweet_scan.stWait++;
	uint64_t since = 0;
	bool pending = false;
	if (sweet_scan.geom && (int32_t)(sweet_scan.shownSwap - id) < 0) {
		if (sweet_scan.flipping != NULL && sweet_scan.flipSwap == id) {
			pending = true;
			since = sweet_scan.flipSwapAbs;
		} else if (sweet_scan.next != NULL && sweet_scan.nextSwap == id) {
			pending = true;
			since = sweet_scan.nextAbs;
		} else {
			for (uint32_t i = 0; i < sweet_scan.laterN; i++) {
				if (sweet_scan.later[i].s != NULL && sweet_scan.later[i].swap == id) {
					pending = true;
					since = sweet_scan.later[i].abs;
				}
			}
		}
	}
	if (pending) {
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - since, &ns);
		if (ns > 100 * NSEC_PER_MSEC) {
			sweet_scan.stTbStale++;
			pending = false;
		}
	}
	if (pending) {
		sweet_scan.stTbBusy++;
	} else {
		sweet_scan.stTbDone++;
	}
	IOLockUnlock(fb->fLock);
	return pending ? kIOReturnBusy : kIOReturnSuccess;
}

/*
 * 0986: corner mattes in direct mode (p scanout corners 1). The CPU present darkens the panel's rounded corners with
 * fCornerCov (copyRow); the display engine has no matte, so the drawn frame gets one itself: after its fence (the GPU
 * is done with it) and before the flip, in the surface's own corners (fCornerExt rows and columns, coverage in panel
 * pixels: the radius shows 4 % smaller after the 1125 -> 1080 scale). Pixels with no coverage are written black
 * without a read (the surface is write-combined), partly covered ones are read and dimmed, the rest is not touched:
 * about 4 x 0.21 x ext^2 writes and 4 x 2 x ext reads. CA does not draw into a frame it has swapped.
 */
static void
sweet_scan_corners(AppleCLCD *fb, SweetSurface *s)
{
	uint32_t e = fb->fCornerExt, W = s->width, H = s->height, bpr = s->bytesPerRow;
	uintptr_t va = s->kernelAddress();
	if (e == 0 || fb->fCornerCov == NULL || va == 0 || 2 * e >= W || 2 * e >= H) {
		return;
	}
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < 2 * e; i++) {
		uint32_t y = i < e ? i : H - 1 - (i - e);
		const uint8_t *cov = fb->fCornerCov + (size_t)(i < e ? i : i - e) * e;
		volatile uint32_t *row = (volatile uint32_t *)(va + (uint64_t)y * bpr);
		for (uint32_t x = 0; x < e; x++) {
			uint32_t c = cov[x];
			if (c == 255) {
				break;  /* the rest of this corner row is inside */
			}
			row[x] = c == 0 ? 0xff000000u : sweet_display_dim(row[x], c);
			row[W - 1 - x] = c == 0 ? 0xff000000u : sweet_display_dim(row[W - 1 - x], c);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	sweet_scan.stCornerAbs += mach_absolute_time() - t0;
	sweet_scan.stCorners++;
}

/* With fLock, from the scanout thread: latch, then the next flip. False: nothing in flight. */
static bool
sweet_scan_step_locked(AppleCLCD *fb, uint64_t now)
{
	if (sweet_scan.flipping != NULL) {
		/*
		 * 0979: on screen once INTF1's frame counter (0x6b8ac, the one that works; the tear-check TE counter reads 0)
		 * has moved past the flush and the VIG flush bits are clear; or after 2 frames whatever the bits say.
		 */
		uint32_t frames = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT) - sweet_scan.flipFrame;
		bool bits = (sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH) & MDP_FLUSH_VIG01) == 0;
		if (!(frames >= 2 || (frames >= 1 && bits))) {
			return true;
		}
		sweet_scan.stLatchFc += !bits;
		SWEET_TRACE(SWT_SCAN_LATCH, sweet_scan.flipSwap, sweet_scan.flipSeq, now - sweet_scan.flipSwapAbs, frames);  /* 0994 */
		fb->swapDone(now, 3);   /* 0987: the flipped frame is on screen */
		sweet_scan_drop(&sweet_scan.shown);
		sweet_scan.shown = sweet_scan.flipping;
		sweet_scan.flipping = NULL;
		sweet_scan.shownSeq = sweet_scan.flipSeq;
		sweet_scan_word_locked((uint32_t)sweet_scan.shownSeq);     /* 0989: the CP's wait goes on */
		sweet_scan.shownSwap = sweet_scan.flipSwap;
		uint64_t l = now - sweet_scan.flipSwapAbs;
		sweet_scan.stLatched++;
		sweet_scan.stLatchN++;
		sweet_scan.stLatchAbs += l;
		if (l > sweet_scan.stLatchMax) {
			sweet_scan.stLatchMax = l;
		}
		IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
	}
	SweetSurface *s = sweet_scan.next;
	if (s == NULL) {
		return false;
	}
	if (sweet_gpu_fence_wait_soft(sweet_scan.nextFence, 0) != 0) {
		uint64_t ns;
		absolutetime_to_nanoseconds(now - sweet_scan.nextAbs, &ns);
		if (ns > 200 * NSEC_PER_MSEC) {
			sweet_scan.stLate++;            /* the GPU never got there: not shown */
			sweet_scan_word_locked((uint32_t)sweet_scan.nextSeq);     /* 0991: its CP waiters go on */
			sweet_scan_drop(&sweet_scan.next);
			sweet_scan_pop_locked();
			IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
			return false;
		}
		return true;
	}
	sweet_scan.stFenceAbs += now - sweet_scan.nextAbs;
	/* drawn: a frame the CPU wrote (seed bumped) is cleaned first, for a RAM slot (cached) */
	uint32_t n = s->scanSlot - 1;
	uint32_t seed = s->state != NULL ? *(volatile uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + 0xc) : 0;
	if (n < SWEET_SCAN_SLOTS && sweet_scan.seed[n] != seed) {
		sweet_scan.seed[n] = seed;
		if (OSDynamicCast(IOBufferMemoryDescriptor, s->memory) != NULL) {
			uintptr_t va = s->kernelAddress();
			if (va != 0) {
				flush_dcache64((addr64_t)va, (unsigned)((uint64_t)s->bytesPerRow * s->height), FALSE);
				sweet_scan.stClean++;
			}
		}
	}
	if (sweet_scan.corners) {
		sweet_scan_corners(fb, s);      /* 0986 */
	}
	sweet_scan.next = NULL;
	uint64_t sSeq = sweet_scan.nextSeq, sAbs = sweet_scan.nextAbs;    /* 0979: a newer frame may come while entering */
	uint32_t sSwap = sweet_scan.nextSwap;
	sweet_scan_pop_locked();        /* 0991: the next queued frame (gate 2) waits for this one's latch */
	if (!sweet_scan.geom) {
		/* the first direct frame: switch the pipes (they show s once it returns) */
		sweet_scan.entering = true;
		IOLockUnlock(fb->fLock);
		bool ok = sweet_scan_enter(fb, s);
		IOLockLock(fb->fLock);
		sweet_scan.entering = false;
		if (!ok || !sweet_scan.on) {
			sweet_scan_drop(&s);
			if (ok) {
				IOLockUnlock(fb->fLock);
				sweet_scan_leave(fb, "scanout off");
				IOLockLock(fb->fLock);
			} else {
				sweet_scan.on = false;
				IOLog("SWEET SCANOUT: the pipes could not be switched: off\n");
			}
			IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
			return false;
		}
		if (!sweet_scan.geom) {
			sweet_scan_drop(&s);            /* switched back meanwhile (a non-slot frame) */
			return false;
		}
		sweet_scan_drop(&sweet_scan.shown);
		sweet_scan.shown = s;
		sweet_scan.shownSeq = sSeq;
		sweet_scan_word_locked((uint32_t)sweet_scan.shownSeq);
		sweet_scan.shownSwap = sSwap;
		sweet_scan.stFlips++;
		sweet_scan.stLatched++;
		sweet_scan.stLatchN++;
		sweet_scan.stLatchAbs += mach_absolute_time() - sAbs;
		SWEET_TRACE(SWT_SCAN_LATCH, sSwap, sSeq, mach_absolute_time() - sAbs, 0);    /* 0994: pipes switched to it */
		IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
		return sweet_scan.next != NULL;
	}
	sweet_mdp_wr(MDP_VIG(0) + SSPP_SRC0, (uint32_t)s->scanPA);
	sweet_mdp_wr(MDP_VIG(1) + SSPP_SRC0, (uint32_t)s->scanPA);
	sweet_scan.flipFrame = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT);
	sweet_mdp_wr(MDP_CTL(0) + MDP_CTL_FLUSH, MDP_FLUSH_VIG01);
	SWEET_TRACE(SWT_SCAN_FLIP, sSwap, sSeq, now - sAbs, sweet_scan.flipFrame);    /* 0994 */
	sweet_scan.flipping = s;
	sweet_scan.flipSeq = sSeq;
	sweet_scan.flipSwap = sSwap;
	sweet_scan.flipSwapAbs = sAbs;
	sweet_scan.flipAbs = now;
	sweet_scan.stFlips++;
	return true;
}

/* With fLock: one line every 5 s while frames come. */
static void
sweet_scan_stats_locked(uint64_t now)
{
	uint64_t ns;
	if (sweet_scan.stWin == 0) {
		sweet_scan.stWin = now;
		return;
	}
	absolutetime_to_nanoseconds(now - sweet_scan.stWin, &ns);
	if (ns < 5 * NSEC_PER_SEC) {
		return;
	}
	sweet_scan.stWin = now;
	if (sweet_scan.stFrames == 0 && sweet_scan.stNonslot == 0) {
		return;
	}
	uint64_t fw = 0, lat = 0, lmax = 0;
	absolutetime_to_nanoseconds(sweet_scan.stFenceAbs, &fw);
	absolutetime_to_nanoseconds(sweet_scan.stLatchAbs, &lat);
	absolutetime_to_nanoseconds(sweet_scan.stLatchMax, &lmax);
	IOLog("SWEET SCANOUT: 5s: frames %u flips %u latched %u dropped %u nonslot %u late %u | fence %llu us swap->latch %llu "
	    "us max %llu us | bp %u timeout %u | swapwait %u timeout %u newest %u | chain %u-%u | clean %u enter %u leave %u held %u "
	    "latch-fc %u | %s\n",
	    sweet_scan.stFrames, sweet_scan.stFlips, sweet_scan.stLatched, sweet_scan.stDropped, sweet_scan.stNonslot,
	    sweet_scan.stLate, sweet_scan.stFlips ? fw / 1000 / sweet_scan.stFlips : 0,
	    sweet_scan.stLatchN ? lat / 1000 / sweet_scan.stLatchN : 0, lmax / 1000, sweet_scan.stBp, sweet_scan.stBpTimeout,
	    sweet_scan.stWait, sweet_scan.stWaitTimeout, sweet_scan.stWaitNewest, sweet_scan.stDepthMin, sweet_scan.stDepthMax,
	    sweet_scan.stClean, sweet_scan.stEnter, sweet_scan.stLeave,
	    sweet_scan.stHold, sweet_scan.stLatchFc, sweet_scan.geom ? "direct" : "panel buffer");
	{
		uint64_t gw = 0;    /* 0985 */
		absolutetime_to_nanoseconds(sweet_scan.stGateAbs, &gw);
		uint64_t cw = 0;    /* 0986 */
		absolutetime_to_nanoseconds(sweet_scan.stCornerAbs, &cw);
		IOLog("SWEET SCANOUT: 5s gate %u: submits %u waited %u (avg %llu us) timeout %u | cp waits %u released %u queued %u "
		    "overflow %u | corners %u (avg %llu us)\n", sweet_scan.gate, sweet_scan.stGate, sweet_scan.stGateWait,
		    sweet_scan.stGateWait ? gw / 1000 / sweet_scan.stGateWait : 0, sweet_scan.stGateTimeout, sweet_scan.stGpuWait,
		    sweet_scan.stGpuRelease, sweet_scan.stQueued, sweet_scan.stOverflow, sweet_scan.stCorners,
		    sweet_scan.stCorners ? cw / 1000 / sweet_scan.stCorners : 0);
		sweet_scan.stGpuWait = sweet_scan.stGpuRelease = sweet_scan.stQueued = sweet_scan.stOverflow = 0;
		sweet_scan.stCornerAbs = 0;
		sweet_scan.stCorners = 0;
		sweet_scan.stGateAbs = 0;
		sweet_scan.stGate = sweet_scan.stGateWait = sweet_scan.stGateTimeout = 0;
	}
	if (sweet_scan.tribuf) {
		/* 0996 */
		IOLog("SWEET SCANOUT: 5s tribuf: polls busy %u done %u stale %u | gate at K-1 %u | short %u | chain %u-%u\n",
		    sweet_scan.stTbBusy, sweet_scan.stTbDone, sweet_scan.stTbStale, sweet_scan.stTbGate3, sweet_scan.stTbShort,
		    sweet_scan.stDepthMin, sweet_scan.stDepthMax);
		sweet_scan.stTbBusy = sweet_scan.stTbDone = sweet_scan.stTbStale = sweet_scan.stTbGate3 = sweet_scan.stTbShort = 0;
	}	sweet_scan.stFenceAbs = sweet_scan.stLatchAbs = sweet_scan.stLatchMax = 0;
	sweet_scan.stFrames = sweet_scan.stFlips = sweet_scan.stLatched = sweet_scan.stDropped = sweet_scan.stNonslot = 0;
	sweet_scan.stLate = sweet_scan.stBp = sweet_scan.stBpTimeout = sweet_scan.stWait = sweet_scan.stWaitTimeout = 0;
	sweet_scan.stClean = sweet_scan.stEnter = sweet_scan.stLeave = sweet_scan.stLatchN = 0;
	sweet_scan.stHold = sweet_scan.stLatchFc = 0;
	sweet_scan.stWaitNewest = sweet_scan.stDepthMin = sweet_scan.stDepthMax = 0;
}

static void
sweet_scan_thread(void *arg)
{
	AppleCLCD *fb = (AppleCLCD *)arg;
	IOLockLock(fb->fLock);
	for (;;) {
		uint64_t now = mach_absolute_time();
		bool busy = sweet_scan.on && sweet_scan_step_locked(fb, now);
		if (sweet_scan.on) {
			sweet_scan_stats_locked(now);
		}
		bool armed = (int32_t)(sweet_scan.armedRef - sweet_scan.word) > 0;
		if (armed) {
			/* 0989: a CP wait not released by a latch within 50 ms is released here (the ring must not stall) */
			uint64_t ns;
			absolutetime_to_nanoseconds(now - sweet_scan.armedAbs, &ns);
			if (ns > 50 * NSEC_PER_MSEC || !sweet_scan.on || !sweet_scan.geom || sweet_scan.gate != 2) {
				sweet_scan.stGateTimeout++;
				sweet_scan_release_locked();
				armed = false;
			}
		}
		if (busy || sweet_scan.flipping != NULL || armed) {
			uint64_t dl;
			clock_interval_to_deadline(1, kMillisecondScale, &dl);
			IOLockSleepDeadline(fb->fLock, &sweet_scan, dl, THREAD_UNINT);
		} else {
			uint64_t dl;
			clock_interval_to_deadline(100, kMillisecondScale, &dl);
			IOLockSleepDeadline(fb->fLock, &sweet_scan, dl, THREAD_UNINT);
		}
	}
}

/*
 * 0999: from the GPU interrupt (sweet_gpu.c, sweetgpuirq=1 / p gpuirq 1): a fence landed; the scanout thread, asleep
 * for 1 ms between looks at its next frame's fence, looks now (GPU done -> flip ~0.6 ms on average without it).
 * Interrupt context: only the wakeup (IOLockWakeup does not take the lock).
 */
extern "C" void sweet_scan_gpu_kick(void);
void
sweet_scan_gpu_kick(void)
{
	AppleCLCD *fb = sweet_fb_instance;
	if (fb != NULL && sweet_scan.thread && sweet_scan.on && sweet_scan.next != NULL) {
		IOLockWakeup(fb->fLock, &sweet_scan, false);
	}
}

/* p scanout 0|1 and the boot-arg: frames direct or the normal way. */
static void
sweet_scan_set(AppleCLCD *fb, bool on, const char *why)
{
	if (on && sweet_surface_scan_mode() == 0) {
		IOLog("SWEET SCANOUT: no scanout slots (boot-arg sweetscanout=1 or 2): stays off\n");
		return;
	}
	if (on && !sweet_scan.thread) {
		uint32_t g = 1;         /* 0989: boot-arg sweetscangate=0|1|2 (default 1, the CPU gate) */
		PE_parse_boot_argn("sweetscangate", &g, sizeof(g));
		sweet_scan.gate = g <= 2 ? g : 1;
		sweet_scan.tribuf = (sweet_fbx & SWEET_FBX_TRIBUF) != 0;     /* 0996 */
		sweet_scan.thread = IOCreateThread(sweet_scan_thread, fb) != NULL;
		if (!sweet_scan.thread) {
			IOLog("SWEET SCANOUT: no thread: stays off\n");
			return;
		}
	}
	IOLockLock(fb->fLock);
	sweet_scan.on = on;
	IOLockWakeup(fb->fLock, &sweet_scan, false);
	IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
	IOLockUnlock(fb->fLock);
	if (!on) {
		sweet_scan_leave(fb, "scanout off");
	}
	uint64_t pa = 0;
	uint32_t held = 0;
	for (uint32_t n = 0; n < SWEET_SCAN_SLOTS; n++) {
		SweetSurface *s = sweet_surface_scan_copy(n, &pa);
		if (s != NULL) {
			IOLog("SWEET SCANOUT: slot %u PA %#llx: surface %u, display use %d, client use %d\n", n, pa, s->id,
			    (int)s->displayUse, (int)s->clientUse);
			s->release();
			held++;
		}
	}
	IOLog("SWEET SCANOUT: %s (%s), %u slot surfaces\n", on ? "on" : "off", why, held);
}

/*
 * Poke commands (bsd/dev/sweet_poke.c "p WORD"):
 *   p fc            INTF1's TE frame counter (and INTF frame counter) twice, 1 s apart: the panel's real rate,
 *                   with the tear-check configuration
 *   p hz 60|90 [tc] the panel's rate now (DCS 0x53), ticks and SyncRate with it; tc: tear-check line clock too
 *   p mdp ...       0948: display-engine dump and scanout tests (above)
 *   p scanout 0|1   0976: the direct scanout off / on (needs boot-arg sweetscanout=1|2); no argument: the state
 *   p scanout gate 0|1|2   0985: SwapEnd back pressure (0), the swapper's GPU submits wait (1, default), 0989: the
 *                          CP waits for the latch (2; boot-arg sweetscangate)
 *   p scanout corners 0|1  0986: corner mattes drawn into each direct frame (default off)
 *   p scanout tribuf 0|1   0996: SwapWait polls (0x80000000) answer Busy until latched; gate by chain depth (off)
 *   p vsync [MASK [LEAD_US]] | p vsync refresh [NS]   0995: tick timer options, IOMFBDisplayRefresh (above)
 *   p vsync vrr N [STEP_NS]                         0998: IOMFBDisplayRefresh max = N periods, step (above)
 */
extern "C" void sweet_gpu_stats_flush(void);     /* 0991 */
extern "C" void sweet_prof_cmd(const char *s, const char *end);   /* 0991 */

void
AppleCLCD::fbCmd(const char *s, const char *end)
{
	if (end - s >= 4 && !strncmp(s, "disp", 4)) {
		/* 0991: p disp: the display power state, for scripts (tools-claude/redfin-fpsbench.sh) */
		IOLog("SWEET FB: display %s (p disp; scanout %s)\n", fPowerOff ? "off" : "on", sweet_scan_live() ? "direct" : "panel buffer");
		return;
	}
	if (end - s >= 5 && !strncmp(s, "phase", 5)) {
		/*
		 * 0991: p phase NAME: the FB, SCANOUT and GPU stats lines of the phase that ends now (when it had frames),
		 * their windows restart, then "SWEET PHASE: NAME" (and a profiler mark). Every stats line between two
		 * PHASE lines covers only that phase (tools-claude/fps-parse.py).
		 */
		const char *q = s + 5;
		while (q < end && *q == ' ') {
			q++;
		}
		uint64_t now = mach_absolute_time();
		IOLockLock(fLock);
		fStWin = 1;
		IOLockUnlock(fLock);
		statLine(now);
		IOLockLock(fLock);
		if (sweet_scan.on) {
			sweet_scan.stWin = 1;
			sweet_scan_stats_locked(now);
		}
		sweet_scan.stWin = now;
		IOLockUnlock(fLock);
		sweet_gpu_stats_flush();
		IOLog("SWEET PHASE: %.*s\n", (int)(end - q > 60 ? 60 : end - q), q);
		char mk[72];
		snprintf(mk, sizeof(mk), "prof mark %.*s", (int)(end - q > 56 ? 56 : end - q), q);
		sweet_prof_cmd(mk, mk + strlen(mk));
		return;
	}
	if (end - s >= 8 && !strncmp(s, "swapdone", 8)) {
		/* 0987: p swapdone [MASK | trace]: 1 swap-complete tick, 2 trace (trace = re-arm with 2 set); no argument: state */
		const char *q = s + 8;
		while (q < end && *q == ' ') {
			q++;
		}
		IOLockLock(fLock);
		if (end - q >= 5 && !strncmp(q, "trace", 5)) {
			sweet_swapdone |= SWEET_SWAPDONE_TRACE;
			sweet_st_n = 0;
		} else if (q < end && *q >= '0' && *q <= '9') {
			uint32_t v = 0;
			while (q < end && *q >= '0' && *q <= '9') {
				v = v * 10 + (uint32_t)(*q++ - '0');
			}
			if ((v & SWEET_SWAPDONE_TRACE) && !(sweet_swapdone & SWEET_SWAPDONE_TRACE)) {
				sweet_st_n = 0;
			}
			sweet_swapdone = v;
		}
		uint32_t ev = sweet_swapdone_events, tk = sweet_swapdone_ticks, cl = sweet_swapdone_close;
		sweet_swapdone_events = sweet_swapdone_ticks = sweet_swapdone_close = 0;
		IOLockUnlock(fLock);
		IOLog("SWEET FB: swapdone now %#x (1 tick at swap done, 2 trace %d/%u); since last: swap done %u, ticks %u, "
		    "within MIN %u\n", sweet_swapdone, (int)sweet_st_n, SWEET_ST_N, ev, tk, cl);
		return;
	}
	if (end - s >= 2 && s[0] == 'f' && s[1] == 'c') {
		uint32_t a0 = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_INT_COUNT), f0 = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT);
		uint64_t t0 = mach_absolute_time();
		IOSleep(1000);
		uint32_t a1 = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_INT_COUNT), f1 = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT);
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		uint64_t te = (uint16_t)((a1 >> 16) - (a0 >> 16)), fr = (uint32_t)(f1 - f0);
		uint64_t te10 = ns ? te * 10000000000ULL / ns : 0, fr10 = ns ? fr * 10000000000ULL / ns : 0;
		uint32_t vs = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_SYNC_VSYNC);
		IOLog("SWEET FB: fc: te %llu frames in %llu ms = %llu.%llu Hz (int_count %08x -> %08x); intf frame_count "
		    "%u -> %u = %llu.%llu Hz; panel set to %u Hz\n", te, ns / 1000000, te10 / 10, te10 % 10, a0, a1, f0, f1,
		    fr10 / 10, fr10 % 10, fHz);
		IOLog("SWEET FB: fc: tear en %x vsync %08x (count %u = line %llu ns) height %x init %x line %x autorefresh %08x\n",
		    sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_EN), vs, vs & 0x7ffff, (uint64_t)(vs & 0x7ffff) * 1000000000ULL / 19200000,
		    sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_SYNC_HEIGHT), sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_INIT_VAL),
		    sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_LINE_COUNT), sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_AUTOREFRESH));
		return;
	}
	if (end - s >= 2 && s[0] == 'h' && s[1] == 'z') {
		const char *q = s + 2;
		uint32_t hz = 0;
		while (q < end && *q == ' ') {
			q++;
		}
		while (q < end && *q >= '0' && *q <= '9') {
			hz = hz * 10 + (uint32_t)(*q++ - '0');
		}
		while (q < end && *q == ' ') {
			q++;
		}
		bool tc = end - q >= 2 && q[0] == 't' && q[1] == 'c';
		bool ok = setHz(hz, tc);
		IOLog("SWEET FB: hz %u%s -> %s (panel %u Hz)\n", hz, tc ? " tc" : "", ok ? "ok" : "FAILED", fHz);
		return;
	}
	if (end - s >= 3 && s[0] == 'f' && s[1] == 'b' && s[2] == 'x') {
		/* 0946: p fbx HEX: bits 0x2, 0x4 and 0x8 live (0x1 is published at boot) */
		const char *q = s + 3;
		uint32_t v = 0;
		while (q < end && *q == ' ') {
			q++;
		}
		if (end - q >= 2 && q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
			q += 2;
		}
		for (; q < end; q++) {
			char c = *q;
			uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) :
			    c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16;
			if (d == 16) {
				break;
			}
			v = v * 16 + d;
		}
		const uint32_t live = SWEET_FBX_NOCLEAN | SWEET_FBX_SWAPQ | SWEET_FBX_CPUPRESENT;
		IOLockLock(fLock);
		sweet_fbx = (sweet_fbx & ~live) | (v & live);
		bool q4 = fQ != NULL && fPresentThread && (sweet_fbx & SWEET_FBX_SWAPQ);
		if (fSwapQ && !q4 && fQn != 0) {
			/* queue off: its newest frame is fPending already; show that the old way */
			fDirty = true;
		}
		if (!q4) {
			fQn = 0;
			IOLockWakeup(fLock, &fQn, true);
		}
		fSwapQ = q4;
		fTeStale = 0;
		fFcEdge = 0;            /* the tick phase is found again */
		fPresentReq = true;     /* the present thread (re)binds itself */
		IOLockWakeup(fLock, &fPresentReq, true);
		IOLockUnlock(fLock);
		IOLog("SWEET FB: fbx now %#x (swap queue %s)\n", sweet_fbx, q4 ? "on" : "off");
		return;
	}
	if (end - s >= 3 && s[0] == 'm' && s[1] == 'd' && s[2] == 'p') {
		mdpCmd(s + 3, end);     /* 0948 */
		return;
	}
	if (end - s >= 5 && !strncmp(s, "vsync", 5)) {
		vsyncCmd(s + 5, end);   /* 0995 */
		return;
	}
	if (end - s >= 4 && !strncmp(s, "pace", 4)) {
		/* 0977: p pace N (0 = as before, 1 = ticks held for the SwapEnd of a frame in flight); 0983: p pace N [MIN_US] */
		const char *q = s + 4;
		uint32_t v = 0, min = 0;
		while (q < end && *q == ' ') {
			q++;
		}
		while (q < end && *q >= '0' && *q <= '9') {
			v = v * 10 + (uint32_t)(*q++ - '0');
		}
		while (q < end && *q == ' ') {
			q++;
		}
		while (q < end && *q >= '0' && *q <= '9') {
			min = min * 10 + (uint32_t)(*q++ - '0');
		}
		IOLockLock(fLock);
		sweet_pace = v;
		if (min != 0) {
			sweet_pace_min = min;
		}
		fPaceArmed = false;     /* a pending mode-3 timer finds nothing to do */
		bool owed = fPaceOwed;
		fPaceOwed = false;
		fPaceSwapped = false;
		if (owed) {
			sendVsync(fPaceOwedCount, mach_absolute_time());       /* nothing stays held */
		}
		IOLockUnlock(fLock);
		IOLog("SWEET FB: pace now %u min %u us\n", sweet_pace, sweet_pace_min);
		return;
	}
	if (end - s >= 7 && !strncmp(s, "scanout", 7)) {
		/* 0976: p scanout 0|1 */
		const char *q = s + 7;
		while (q < end && *q == ' ') {
			q++;
		}
		if (end - q >= 7 && !strncmp(q, "corners", 7)) {
			/* 0986: p scanout corners 0|1 */
			const char *v = q + 7;
			while (v < end && *v == ' ') {
				v++;
			}
			IOLockLock(fLock);
			sweet_scan.corners = v < end && *v == '1';
			IOLockUnlock(fLock);
			IOLog("SWEET SCANOUT: corners %s (radius %u, %u rows)\n", sweet_scan.corners ? "on" : "off", fCornerRadius,
			    fCornerExt);
		} else if (end - q >= 4 && !strncmp(q, "gate", 4)) {
			/* 0985: p scanout gate 0|1; 0989: 2 */
			const char *v = q + 4;
			while (v < end && *v == ' ') {
				v++;
			}
			IOLockLock(fLock);
			sweet_scan.gate = v < end && *v >= '0' && *v <= '2' ? (uint32_t)(*v - '0') : 1;
			sweet_scan_release_locked();    /* 0989: nothing armed survives a change */
			IOLockWakeup(fLock, &sweet_scan.shownSeq, false);
			IOLockWakeup(fLock, &sweet_scan, false);
			IOLockUnlock(fLock);
			IOLog("SWEET SCANOUT: gate %u (%s)\n", sweet_scan.gate, sweet_scan.gate == 0 ? "SwapEnd back pressure" :
			    sweet_scan.gate == 1 ? "GPU submits wait on the CPU" : "the CP waits for the latch");
		} else if (end - q >= 6 && !strncmp(q, "tribuf", 6)) {
			/* 0996: p scanout tribuf 0|1 */
			const char *v = q + 6;
			while (v < end && *v == ' ') {
				v++;
			}
			IOLockLock(fLock);
			sweet_scan.tribuf = v < end && *v == '1';
			sweet_scan.gateDepth = 2;
			IOLockWakeup(fLock, &sweet_scan.shownSeq, false);
			IOLockUnlock(fLock);
			IOLog("SWEET SCANOUT: tribuf %s (SwapWait polls %s, gate %s)\n", sweet_scan.tribuf ? "on" : "off",
			    sweet_scan.tribuf ? "busy until latched" : "done at once for the newest",
			    sweet_scan.tribuf ? "K-1 on screen at chain 3" : "K on screen");
		} else if (q < end && (*q == '0' || *q == '1')) {
			sweet_scan_set(this, *q == '1', "p scanout");
		} else {
			sweet_scan_set(this, sweet_scan.on, "p scanout (state)");
		}
		return;
	}
	IOLog("SWEET FB: poke p: fc | hz 60|90 [tc] | fbx HEX | mdp dump|smmu|test|scale|ca|restore | scanout 0|1 | pace N [MIN_US]\n");
}

extern "C" void sweet_fb_cmd(const char *s, const char *end);
void
sweet_fb_cmd(const char *s, const char *end)
{
	if (sweet_fb_instance == NULL) {
		IOLog("SWEET FB: no framebuffer for p %.8s\n", s);
		return;
	}
	sweet_fb_instance->fbCmd(s, end);
}

/*
 * ---- 0944 (sweetfbx & 4): present at SwapEnd, ticks on the TE ----
 *
 * SwapEnd queues the frame (two at most) and wakes the present thread, which blits it at once instead of on the
 * next tick (where it waited up to a period and then queued behind backboardd's next GPU job). A third SwapEnd is
 * held until the oldest frame has gone to the GPU (at most 50 ms, then it replaces the newest waiting frame):
 * back-pressure, as upstream hackios MiniIOMobileFramebuffer. A present put off because the GPU still has the
 * one before last is tried again every 1 ms; a newer frame waiting replaces it. With the fLock.
 */
bool
AppleCLCD::queueSwap(const uint8_t *rec, uint64_t now)
{
	if (fPowerOff) {
		fQn = 0;                /* off: the latest frame for when it comes back on (fPending) */
		IOLockWakeup(fLock, &fQn, true);
		return false;
	}
	if (fQn >= 2) {
		uint64_t dl;
		clock_interval_to_deadline(50, kMillisecondScale, &dl);
		while (fQn >= 2 && !fPowerOff &&
		    IOLockSleepDeadline(fLock, &fQn, dl, THREAD_UNINT) != THREAD_TIMED_OUT) {
		}
	}
	uint32_t slot;
	if (fQn >= 2) {
		slot = (fQHead + 1) % 2;        /* still full: the newest waiting frame is never shown */
		fStDropped++;
	} else {
		slot = (fQHead + fQn) % 2;
		fQn++;
	}
	memcpy(fQ + slot * SWEET_FB_SWAP_REC_SIZE, rec, SWEET_FB_SWAP_REC_SIZE);
	fQAbs[slot] = now;
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);  /* the latest, for a redraw */
	fPendingAbs = now;
	IOLockWakeup(fLock, &fPresentReq, true);
	return true;
}

/*
 * The vsync tick on the panel's TE: INTF1's tear check counts TE frames (INT_COUNT 31:16) and, since the last TE,
 * lines from VSYNC_INIT_VAL at one per vsync_count 19.2 MHz clocks. The next tick is put 0.5 ms after the TE
 * nearest a period from now. A frame counter that stops (4 ticks) leaves the ticks free-running; the 0905 flip
 * follows the same ticks. Register reads only inside the MDP window (sweet_mdp_rd).
 */
void
AppleCLCD::phaseTick(uint64_t now)
{
	uint32_t ic = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_INT_COUNT);
	uint32_t cnt = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_SYNC_VSYNC) & 0x7ffff;
	uint32_t init = sweet_mdp_rd(SWEET_INTF1 + INTF_TEAR_INIT_VAL) & 0xffff;
	uint32_t frame = ic >> 16, line = ic & 0xffff;
	if (frame == fTeFrame) {
		if (++fTeStale == 4) {
			IOLog("SWEET FB: TE frame counter stopped at %u (int_count %08x): ticks on the INTF frame counter\n",
			    frame, ic);
		}
		if (fTeStale >= 4) {
			phaseFrameCount(now);   /* 0946 */
		}
		return;
	}
	if (fTeStale >= 4) {
		IOLog("SWEET FB: TE frame counter moving again (%u)\n", frame);
	}
	fTeFrame = frame;
	fTeStale = 0;
	if (cnt == 0 || line < init || fVsyncAbs == 0) {
		return;
	}
	uint64_t ns = (uint64_t)(line - init) * cnt * 625 / 12, since, lead;   /* 19.2 MHz clocks -> ns */
	nanoseconds_to_absolutetime(ns, &since);
	nanoseconds_to_absolutetime(500000, &lead);
	if (since > 2 * fVsyncAbs || since > now) {
		return;
	}
	uint64_t next = now - since + lead;
	while (next < now + fVsyncAbs / 2) {
		next += fVsyncAbs;
	}
	if (fTeLogs < 3) {
		uint64_t in;
		absolutetime_to_nanoseconds(next - now, &in);
		fTeLogs++;
		IOLog("SWEET FB: tick phased: TE %llu us ago (frame %u line %u init %u count %u), next in %llu us\n",
		    ns / 1000, frame, line, init, cnt, in / 1000);
	}
	fNextVsync = next;
}

/*
 * ---- 0946 (sweetfbx & 8): the CPU puts each frame on the panel ----
 *
 * The GPU's present blit was queued within ~50 us of SwapEnd but landed 23-28 ms later, behind backboardd's next
 * frame on the one ring (no second ring or preemption in sweet_a6xx.c). Here the present thread scales the frame
 * itself, with the GPU path's geometry (fit, centred; 1125x2436 -> 1080x2339), nearest sample, through the corner
 * mattes (copyRow): the source's cache lines are cleaned+invalidated first (the GPU wrote it), the panel mapping
 * is write-combined. With sweetfbflip=1 it draws into the buffer not on screen and flips (latched at the next
 * frame start, no tearing); else into the scanned one. Not exactly one full-frame BGRA layer: the GPU / old CPU
 * path as before.
 */
/*
 * 0978: the source frame through a cacheable kernel mapping. SweetSurface::kernelAddress() maps a surface that is
 * not a kernel buffer (CA's, every other one on r36a) write-combined, i.e. uncached: the scaled copy read it at
 * 41 ms a frame against 4.3 ms for the cacheable one. Kept for the last 4 surfaces' memory (the map holds it);
 * the copy cleans+invalidates the lines first, as before.
 */
uintptr_t
AppleCLCD::cpuSource(SweetSurface *s)
{
	if (OSDynamicCast(IOBufferMemoryDescriptor, s->memory) != NULL) {
		return s->kernelAddress();      /* already a cacheable kernel buffer */
	}
	for (uint32_t i = 0; i < 4; i++) {
		if (fCpuSrc[i].md == s->memory && fCpuSrc[i].map != NULL) {
			return (uintptr_t)fCpuSrc[i].map->getVirtualAddress();
		}
	}
	IOMemoryMap *map = s->memory->map(kIOMapCopybackCache);
	if (map == NULL) {
		return s->kernelAddress();
	}
	uint32_t k = fCpuSrcNext;
	fCpuSrcNext = (k + 1) % 4;
	OSSafeReleaseNULL(fCpuSrc[k].map);
	fCpuSrc[k].md = s->memory;
	fCpuSrc[k].map = map;
	static uint32_t logs;
	if (logs < 4) {
		logs++;
		IOLog("SWEET FB: CPU present: surface %u mapped cacheable for the copy\n", s->id);
	}
	return (uintptr_t)map->getVirtualAddress();
}

uintptr_t
AppleCLCD::cpuBuffer(uint64_t pa)
{
	if (pa == fPhys) {
		return fPixels;
	}
	for (uint32_t i = 0; i < 2; i++) {
		if (fCpuMap[i] != NULL && fCpuPa[i] == pa) {
			return (uintptr_t)fCpuMap[i]->getVirtualAddress();
		}
	}
	for (uint32_t i = 0; i < 2; i++) {
		if (fCpuMap[i] == NULL) {
			uint64_t len = round_page((uint64_t)SWEET_FB_GPU_ROWBYTES * fHeight);
			IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(pa, len, kIODirectionInOut);
			fCpuMap[i] = md != NULL ? md->map(kIOMapWriteCombineCache) : NULL;
			OSSafeReleaseNULL(md);
			if (fCpuMap[i] == NULL) {
				return 0;
			}
			fCpuPa[i] = pa;
			bzero((void *)fCpuMap[i]->getVirtualAddress(), (size_t)len);    /* not on screen yet */
			IOLog("SWEET FB: CPU present: panel buffer %#llx mapped\n", pa);
			return (uintptr_t)fCpuMap[i]->getVirtualAddress();
		}
	}
	return 0;
}

bool
AppleCLCD::presentCPU(const uint8_t *rec)
{
	static uint32_t why_logged, logs;
	uint32_t mask = get32(rec, 0x104), layer = 0, why = 0;
	SweetSurface *s = NULL;
	uintptr_t src = 0;
	if (mask == 0 || (mask & (mask - 1))) {
		why = 1;
	} else {
		while (!(mask & (1u << layer))) {
			layer++;
		}
		uint32_t id = layer < SWEET_FB_LAYERS ? get32(rec, 0x54 + 4 * layer) : 0;
		s = id ? sweet_surface_copy(id) : NULL;
		if (s == NULL || s == fSurface || s->memory == NULL) {
			why = 2;
		} else if (s->pixelFormat != SWEET_FB_FORMAT_BGRA || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->width == 0 || s->height == 0 || s->bytesPerRow < s->width * 4 ||
		    (uint64_t)s->offset + (uint64_t)s->bytesPerRow * s->height > s->memory->getLength()) {
			why = 3;
		} else if ((src = cpuSource(s)) == 0) {
			why = 4;
		}
	}
	if (why == 0 && fCpuRow == NULL) {
		fCpuRow = (uint32_t *)IOMallocZeroData((vm_size_t)fWidth * 4);
		fCpuX = (uint32_t *)IOMallocZeroData((vm_size_t)fWidth * 4);
		why = fCpuRow == NULL || fCpuX == NULL ? 5 : 0;
	}
	if (why != 0) {
		if (!(why_logged & (1u << why))) {
			why_logged |= 1u << why;
			IOLog("SWEET FB: CPU present: not this frame (reason %u), the other paths\n", why);
		}
		OSSafeReleaseNULL(s);
		return false;
	}
	uint32_t w = s->width, h = s->height, pitch = s->bytesPerRow, pw = fWidth, ph = fHeight, dw, dh;
	if ((uint64_t)w * ph <= (uint64_t)pw * h) {
		dh = ph;
		dw = (uint32_t)(((uint64_t)w * ph + h / 2) / h);
	} else {
		dw = pw;
		dh = (uint32_t)(((uint64_t)h * pw + w / 2) / w);
	}
	dw = dw > pw ? pw : dw ? dw : 1;
	dh = dh > ph ? ph : dh ? dh : 1;
	dh = ph - dh <= 2 ? ph : dh;    /* 0947: as the GPU fit, the whole panel within 2 px */
	dw = pw - dw <= 2 ? pw : dw;
	uint32_t dx = (pw - dw) / 2, dy = (ph - dh) / 2;
	uint64_t key = (uint64_t)w << 32 | (uint64_t)dw << 16 | dx;
	if (key != fCpuXKey) {
		bzero(fCpuRow, (size_t)pw * 4);
		for (uint32_t i = 0; i < dw; i++) {
			fCpuX[i] = (uint32_t)(((2ULL * i + 1) * w) / (2ULL * dw));
		}
		fCpuXKey = key;
	}
	sweet_cpufreq_boost(2);         /* 0970: the copy on a boosted core (the gold idles at 650-900 MHz) */
	uint64_t pa = 0;
	if (sweet_gpu_cpu_target(&pa) == 1) {
		fPresentBusy = true;    /* the last flip has not latched: again soon */
		s->release();
		return true;
	}
	uintptr_t dst = cpuBuffer(pa);
	if (dst == 0) {
		s->release();
		return false;
	}
	/*
	 * 0949: the frame is read only once the GPU has drawn it. Copied at SwapEnd it was still being rendered
	 * (cleared, then filled bin by bin): black regions with a stepped edge where the GPU had got to. The fence
	 * submitted at this frame's SwapEnd (unknown: the latest), at most 50 ms.
	 */
	uint32_t fence = 0;
	IOLockLock(fLock);
	for (uint32_t i = 0; i < 4; i++) {
		if (fSwapFence[i].abs == fPresentingAbs && fPresentingAbs != 0) {
			fence = fSwapFence[i].fence;
		}
	}
	IOLockUnlock(fLock);
	if (fence == 0) {
		fence = sweet_gpu_fence_submitted();
	}
	uint64_t w0 = mach_absolute_time();
	bool late = sweet_gpu_fence_wait_soft(fence, 50) != 0;
	uint64_t t0 = mach_absolute_time();
	IOLockLock(fLock);
	fStWaitAbs += t0 - w0;
	fStWaitN++;
	fStWaitLate += late;
	IOLockUnlock(fLock);
	const uint8_t *base = (const uint8_t *)src + s->offset;
	flush_dcache64((addr64_t)(uintptr_t)base, (unsigned)((uint64_t)pitch * h), FALSE);
	fCpuDst = dst;
	for (uint32_t y = 0; y < ph; y++) {
		if (y < dy || y >= dy + dh) {
			bzero((void *)(dst + (uint64_t)y * fRowBytes), (size_t)pw * 4);
			continue;
		}
		const uint32_t *srow = (const uint32_t *)(base + (uint64_t)(((2ULL * (y - dy) + 1) * h) / (2ULL * dh)) * pitch);
		uint32_t *row = fCpuRow + dx;
		for (uint32_t i = 0; i < dw; i++) {
			row[i] = srow[fCpuX[i]];
		}
		copyRow(0, y, fCpuRow, pw);
	}
	fCpuDst = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	sweet_gpu_cpu_flip();
	uint64_t t1 = mach_absolute_time();
	IOLockLock(fLock);
	fStCpuAbs += t1 - t0;
	fStCpuN++;
	if (fPresentingAbs != 0 && t1 > fPresentingAbs) {
		fStVisAbs += t1 - fPresentingAbs;
		fStVisN++;
	}
	IOLockUnlock(fLock);
	sweet_display_fit_x = (int)dx;
	sweet_display_fit_y = (int)dy;
	sweet_display_fit_w = (int)dw;
	sweet_display_fit_h = (int)dh;
	if (logs < 3) {
		uint64_t ns;
		absolutetime_to_nanoseconds(t1 - t0, &ns);
		logs++;
		IOLog("SWEET FB: CPU present: surface %u %ux%u pitch %u -> %ux%u at %u,%u into %#llx in %llu us\n", s->id, w, h,
		    pitch, dw, dh, dx, dy, pa, ns / 1000);
	}
	s->release();
	return true;
}

/*
 * 0946: ticks on INTF1's frame counter (0x0ac; it counts at the panel's rate, 89.9 Hz at 90, where the tear
 * check's counter reads 0). It gives no position inside a frame, so its edge is found by polling it every 20 us:
 * at the first tick (up to 1.5 periods), then every 32 ticks from a tick put 0.3 ms before the predicted edge.
 * Between, ticks go 0.5 ms after the predicted edges, with the period measured between two edges. Three edges
 * not found: free-running ticks.
 */
void
AppleCLCD::phaseFrameCount(uint64_t now)
{
	if (fFcDead || fVsyncAbs == 0) {
		return;
	}
	uint64_t lead, early, period = fFcPeriod ? fFcPeriod : fVsyncAbs;
	bool cal = (sweet_vsync & SWEET_VSYNC_CAL) != 0;
	nanoseconds_to_absolutetime((uint64_t)sweet_vsync_lead * 1000, &lead);     /* 0995: default 500 us */
	nanoseconds_to_absolutetime((uint64_t)(cal ? fFcEarlyUs : 300) * 1000, &early);
	if (fFcCal && fFcEdge != 0 && cal) {
		/* 0995: the edge this calibration tick was put before (the nearest predicted one) already went by? */
		uint32_t want = fFcFrame + (uint32_t)((now - fFcEdge + period / 2) / period);
		if ((int32_t)(sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT) - want) >= 0) {
			fFcCal = false;         /* no poll for the next edge: this tick goes out now, as a normal one */
			fFcTicks = 24;          /* calibrate again in 8 ticks, with a larger margin */
			fFcEarlyUs = fFcEarlyUs + 250 > 2000 ? 2000 : fFcEarlyUs + 250;
			sweet_vsync_cal_late++;
		}
	}
	if (fFcEdge == 0 || fFcCal) {
		uint32_t f0 = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT), f = f0;
		uint64_t t = now, limit = now + period * 3 / 2;
		while (f == f0 && t < limit) {
			IODelay(20);
			f = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT);
			t = mach_absolute_time();
		}
		fFcCal = false;
		fFcTicks = 0;
		if (f == f0) {
			if (++fFcMiss >= 3) {
				fFcDead = true;
				IOLog("SWEET FB: INTF frame counter stopped at %u too: free-running ticks\n", f);
			}
			return;
		}
		fFcMiss = 0;
		if (fFcEdge != 0 && t - now > period / 2) {
			sweet_vsync_cal_lost++;         /* 0995: polled past an edge that had gone by: that edge got no tick */
		} else if (cal) {
			sweet_vsync_cal_ok++;
			fFcEarlyUs = fFcEarlyUs >= 350 ? fFcEarlyUs - 50 : 300;
		}
		uint32_t frames = f - fFcFrame;
		if (fFcEdge != 0 && frames >= 1 && frames < 4096) {
			uint64_t p = (t - fFcEdge) / frames;
			if (p > fVsyncAbs * 3 / 4 && p < fVsyncAbs * 5 / 4) {
				fFcPeriod = p;
				period = p;
				if (sweet_fb_refresh == 1) {
					/* 0995: the measured period as CA's heartbeat source, when it moved by more than 0.01 % */
					uint64_t ns;
					absolutetime_to_nanoseconds(p, &ns);
					if (fRefreshNs == 0 || (ns > fRefreshNs ? ns - fRefreshNs : fRefreshNs - ns) * 10000 > fRefreshNs) {
						publishRefresh(ns);
					}
				}
			}
		}
		if (fTeLogs < 3) {
			uint64_t ns;
			absolutetime_to_nanoseconds(period, &ns);
			fTeLogs++;
			IOLog("SWEET FB: tick phased on the INTF frame counter: frame %u, period %llu us\n", f, ns / 1000);
		}
		fFcEdge = t;
		fFcFrame = f;
		now = t;
	}
	uint64_t next = fFcEdge + ((now - fFcEdge) / period + 1) * period + lead;
	while (next < now + period / 2) {
		next += period;
	}
	if (++fFcTicks >= 32) {
		next -= lead + early;   /* just before the edge: the next tick looks for it */
		fFcCal = true;
	}
	fNextVsync = next;
}

static uint32_t sweet_fb_gpu_why_logged;

/* Why presentGPU fell back to the CPU copy, logged once per reason. */
static bool
gpu_fallback(uint32_t why, const char *what)
{
	if (!(sweet_fb_gpu_why_logged & (1u << why))) {
		sweet_fb_gpu_why_logged |= 1u << why;
		IOLog("SWEET FB: GPU present not used: %s\n", what);
	}
	return false;
}

/*
 * One row of the CPU copy, n pixels to panel (x, y): rows in the corner bands
 * are darkened on the way (the GPU path has its own mattes), so the panel
 * never shows the corners unmasked.
 */
void
AppleCLCD::copyRow(uint32_t x, uint32_t y, const uint32_t *src, uint32_t n)
{
	uint32_t *dst = (uint32_t *)((fCpuDst ? fCpuDst : fPixels) + (uint64_t)y * fRowBytes) + x;	/* 0946: fCpuDst */
	uint32_t e = fCornerExt;
	const uint8_t *cov = NULL;

	if (y < e) {
		cov = fCornerCov + (size_t)y * e;
	} else if (y >= fHeight - e) {
		cov = fCornerCov + (size_t)(fHeight - 1 - y) * e;
	}
	if (cov == NULL) {
		memcpy(dst, src, (size_t)n * 4);
		return;
	}
	/* [x, l): left corner, [l, r): plain, [r, end): right corner. */
	uint32_t end = x + n, l = e < end ? e : end, r = fWidth - e;
	l = l < x ? x : l;
	r = r < l ? l : (r > end ? end : r);
	for (uint32_t px = x; px < l; px++) {
		dst[px - x] = cov[px] == 255 ? src[px - x] : sweet_display_dim(src[px - x], cov[px]);
	}
	memcpy(&dst[l - x], &src[l - x], (size_t)(r - l) * 4);
	for (uint32_t px = r; px < end; px++) {
		uint32_t c = cov[fWidth - 1 - px];
		dst[px - x] = c == 255 ? src[px - x] : sweet_display_dim(src[px - x], c);
	}
}

/*
 * One full-frame BGRA layer (what backboardd swaps): the GPU scales the whole
 * iOS frame (1125x2436) to fit the panel (1053x2280 at x 13), instead of the
 * CPU copying its centre (cropping the status bar and home indicator). The
 * first time, the panel switches to the GPU's row stride and is cleared, so
 * the bars stay black. False: use the CPU copy.
 */
bool
AppleCLCD::presentGPU(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104), layer = 0;
	if (mask == 0 || (mask & (mask - 1))) {
		return gpu_fallback(0, "not exactly one layer");
	}
	while (!(mask & (1u << layer))) {
		layer++;
	}
	if (layer >= SWEET_FB_LAYERS) {
		return gpu_fallback(1, "layer out of range");
	}
	uint32_t id = get32(rec, 0x54 + 4 * layer);
	SweetSurface *s = id ? sweet_surface_copy(id) : NULL;
	if (s == NULL || s == fSurface || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return gpu_fallback(2, "no surface (or the panel's own)");
	}
	if (s->pixelFormat != SWEET_FB_FORMAT_BGRA || s->elementWidth != 1 || s->elementHeight != 1 ||
	    (s->bytesPerRow & 63) || s->width == 0 || s->height == 0 ||
	    (uint64_t)s->offset + (uint64_t)s->bytesPerRow * s->height > s->memory->getLength()) {
		s->release();
		return gpu_fallback(3, "surface format/size/pitch");
	}
	uint64_t key = ((uint64_t)id << 48) ^ (uint64_t)(uintptr_t)s->memory;
	int slot = sweet_gpu_surface_slot(key);
	if (slot < 0) {
		/* Wire the pages for the GPU (left wired: surfaces live as long as backboardd). */
		if (s->memory->prepare() != kIOReturnSuccess) {
			s->release();
			return gpu_fallback(4, "prepare() failed");
		}
		static uint64_t pages[1024];
		uint64_t len = s->memory->getLength();
		uint32_t n = 0;
		bool ok = len <= sizeof(pages) / sizeof(pages[0]) * PAGE_SIZE;
		for (uint64_t off = 0; ok && off < len; off += PAGE_SIZE) {
			IOByteCount seg = 0;
			addr64_t pa = s->memory->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
			ok = pa != 0 && (pa & PAGE_MASK) == 0;
			pages[n++] = pa;
		}
		if (!ok) {
			s->release();
			return gpu_fallback(5, "surface pages not page-aligned physical memory");
		}
		slot = sweet_gpu_map_surface(key, pages, n, PAGE_SIZE);
	}
	if (slot < 0) {
		s->release();
		return gpu_fallback(6, "GPU mapping failed");
	}
	if (!fGPU) {
		IOLockLock(fLock);
		sweet_gpu_panel_stride(SWEET_FB_GPU_ROWBYTES);
		fRowBytes = SWEET_FB_GPU_ROWBYTES;
		bzero((void *)fPixels, (size_t)fRowBytes * fHeight);
		fGPU = true;
		IOLockUnlock(fLock);
		IOLog("SWEET FB: frames now scaled to fit by the GPU (panel stride %u)\n", fRowBytes);
	}
	uint32_t dx = 0, dy = 0, dw = 0, dh = 0;
	/* 0943 (sweetfbx & 2): no cache clean of a surface the CPU has not written (locked) since its last present */
	uintptr_t clean = (sweet_fbx & SWEET_FBX_NOCLEAN) && !cpuWrote(s) ? 0 : s->kernelAddress();
	sweet_gpu_present_tag(fPresentingAbs);	/* 0946: SwapEnd -> landed */
	int rc = sweet_gpu_present(slot, clean, s->offset, s->width, s->height, s->bytesPerRow,
	    fRowBytes, fCornerRadius, &dx, &dy, &dw, &dh);
	s->release();
	if (rc == 1) {
		fPresentBusy = true;	/* 0158: the GPU still has the present before last: next tick */
		return true;
	}
	if (rc) {
		return gpu_fallback(7, "GPU blit failed");
	}
	sweet_display_fit_x = (int)dx;
	sweet_display_fit_y = (int)dy;
	sweet_display_fit_w = (int)dw;
	sweet_display_fit_h = (int)dh;
	return true;
}

void
AppleCLCD::present(const uint8_t *rec)
{
	if (sweet_mdp_busy) {
		return;         /* 0948: a p mdp test owns the pipes; the latest frame goes out after p mdp restore */
	}
	uint32_t mask = get32(rec, 0x104);


	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			sweet_console_muted = 1;
		}
	}

	/*
	 * 0947: once frames are fitted (GPU blit or CPU present), a swap that sets no layer (the others keep theirs)
	 * or only the panel's own surface changes nothing: it used to fall through to the crop copy below, which flips
	 * back to the first buffer and draws the cropped frame over the fitted ones: the tearing/flicker seen with
	 * the CPU present, and with the GPU path whenever it fell back.
	 */
	if (fGPU || fCpuXKey) {
		bool keep = mask == 0;
		if (!keep && !(mask & (mask - 1))) {
			uint32_t l = (uint32_t)__builtin_ctz(mask);
			uint32_t id = l < SWEET_FB_LAYERS ? get32(rec, 0x54 + 4 * l) : 0;
			keep = id == 0 || (fSurface != NULL && id == fSurface->id);
		}
		if (keep) {
			static uint32_t logs;
			if (logs < 4) {
				logs++;
				IOLog("SWEET FB: swap %u (layers %#x) kept the fitted frame on screen\n", get32(rec, 0x50), mask);
			}
			return;
		}
	}
	if ((sweet_fbx & SWEET_FBX_CPUPRESENT) && presentCPU(rec)) {
		return;         /* 0946 */
	}
	if (sweet_gpu_ready() && presentGPU(rec)) {
		return;
	}
	if (fGPU || fCpuXKey) {
		/* 0947: frames are fitted: a frame neither fit path takes is not cropped over them (see above) */
		static uint32_t logs;
		if (logs < 4) {
			logs++;
			IOLog("SWEET FB: swap %u (layers %#x) not fitted: the last frame stays\n", get32(rec, 0x50), mask);
		}
		return;
	}
	sweet_gpu_scanout_front();	/* 0905: the CPU copy below draws into the first panel buffer */
	if (sweet_display_fit_w) {
		sweet_display_fit_w = 0;        /* touch back to the crop mapping */
		sweet_display_fit_x = sweet_display_fit_y = sweet_display_fit_h = 0;
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			for (int32_t y = 0; y < h; y++) {
				copyRow((uint32_t)dx, (uint32_t)(dy + y),
				    (const uint32_t *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
				    (uint32_t)w);
			}
		}
		s->release();
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fPid = proc_selfpid();  /* 0994 */
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	IOReturn r = sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
	SWEET_TRACE(SWT_NOTIFY, type, fPid, r, count > 0 ? values[0] : 0);      /* 0994: r != 0: dropped */
}

static uint64_t sweet_fb_logged[2];     /* selectors already logged (< 128; 0943: each once) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;
	bool structDone = false;        /* 0943: structureOutput filled in, not to be zeroed */
	uint32_t traceSwap = 0;         /* 0994 */
	SWEET_TRACE_START(SWT_IOMFB_SEL, selector, args->scalarInputCount > 0 ? args->scalarInput[0] : 0,
	    args->scalarInputCount > 1 ? args->scalarInput[1] : 0, args->structureInputSize);

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		sweet_swaptrace('B', (uint32_t)out[0]);  /* 0987 */
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		sweet_swaptrace('E', get32(rec, 0x50));  /* 0987 */
		traceSwap = get32(rec, 0x50);   /* 0994 */
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			if (off && sweet_scan_live()) {
				sweet_scan_leave(fFB, "display off");  /* 0989: the fade goes through the panel buffer */
			}
			IOLog("SWEET FB: display %s\n", off ? "off" : "on");
		}
		break;
	case kSweetFBGetCurrentAbsoluteTime:
		/* 0943 (sweetfbx & 1): was 0 through the default case */
		if (sweet_fbx & SWEET_FBX_MODES) {
			out[0] = mach_absolute_time();
			outCount = 1;
		}
		break;
	case kSweetFBGetBlock:
		/* 0943 (sweetfbx & 1): block 0x40 = the current mode: colour and timing ids at +4 */
		if ((sweet_fbx & SWEET_FBX_MODES) && args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
			if (args->scalarInputCount >= 1 && args->scalarInput[0] == 0x40 && args->structureOutputSize >= 12) {
				uint32_t ids[2] = { SWEET_FB_COLOR_ID, SWEET_FB_TIMING_ID };
				memcpy((uint8_t *)args->structureOutput + 4, ids, sizeof(ids));
			}
			structDone = true;
		}
		break;
	case kSweetFBSwapWait:
		sweet_swaptrace('W', args->scalarInputCount >= 1 ? (uint32_t)args->scalarInput[0] : 0);  /* 0987 */
		/* 0976: with the direct scanout, until that swap is on screen (50 ms at most) */
		if (sweet_scan_live() && args->scalarInputCount >= 1) {
			static uint32_t logs;
			uint32_t id = (uint32_t)args->scalarInput[0], last = fFB->fSwapID;
			if (logs < 4) {
				logs++;
				IOLog("SWEET SCANOUT: SwapWait scalars %u: %#llx %#llx (last swap %u)\n", args->scalarInputCount,
				    args->scalarInput[0], args->scalarInputCount > 1 ? args->scalarInput[1] : 0, last);
			}
			if ((uint32_t)(last - id) < 16) {
				if (sweet_scan.tribuf && args->scalarInputCount >= 2 && (args->scalarInput[1] & 0x80000000ULL)) {
					ret = sweet_scan_poll(fFB, id);         /* 0996 */
				} else {
					sweet_scan_wait(fFB, id);
				}
			}
		} else if ((sweet_fb_early_retire & 4) && args->scalarInputCount >= 2 &&
		    (args->scalarInput[1] & 0x80000000ULL)) {
			ret = fFB->erPoll((uint32_t)args->scalarInput[0]);      /* 1035 */
		}
		sweet_swaptrace('w', args->scalarInputCount >= 1 ? (uint32_t)args->scalarInput[0] : 0);  /* 0987 */
		break;
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (!structDone && args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	SWEET_TRACE_END(SWT_IOMFB_SEL, selector, ret, out[0], traceSwap);      /* 0994 */
	if (selector >= 128 || !(sweet_fb_logged[selector / 64] & (1ULL << (selector % 64)))) {
		if (selector < 128) {
			sweet_fb_logged[selector / 64] |= 1ULL << (selector % 64);
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u [%#llx] out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize, args->scalarInputCount ? args->scalarInput[0] : 0ULL,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */
