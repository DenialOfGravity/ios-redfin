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
bool sweet_gpu_fence_done(uint32_t fence);      /* 1113: look once */
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
	kSweetFBSetGammaTable           = 17,   /* 1116: stored, traced (kern.sweet_cm_trace) */
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetGammaTable           = 27,   /* 1116: the stored table with kern.sweet_cm & 1 */
	kSweetFBGetDotPitch             = 28,
	kSweetFBSetBrightnessCorrection = 50,   /* 1116: stored */
	kSweetFBGetMatrix               = 54,   /* 1116: the stored matrix with kern.sweet_cm & 1 */
	kSweetFBSetMatrix               = 55,   /* 1116: DSPP PCC with kern.sweet_cm & 1 */
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
/*
 * 1042 (kern.sweet_vsync_lock, bsd/dev/sweet_gpu.c; default 0 = as before): a runtime switch over 0995 for a full cmdline.
 * 1 = sweetvsync 7 (ticks without timer slop, calibration keeps its tick, notifications carry the measured period) and
 * sweetfb_refresh 1 (IOMFBDisplayRefresh = the measured INTF1 frame-counter period, republished when it moves > 0.01 %;
 * published at once). The ticks themselves already follow the panel's edges with sweetfbx 0x4 (0944/0946 phase on the
 * INTF1 frame counter); 0 restores the values from before (and removes IOMFBDisplayRefresh if 1042 published it).
 * CA reads IOMFBDisplayRefresh when it opens the display and only when backboardd has no CA_HEARTBEAT_RATE: restart
 * backboardd without CA_HEARTBEAT_RATE=90 for the heartbeat to take the panel's 89.7 Hz.
 * Line every 5 s while on: SWEET FB: vsync lock 5s: period .. | ticks .. | cal ok/late/lost | drift corrected ...
 */
static uint32_t sweet_vlock, sweet_vlock_old_vsync, sweet_vlock_old_refresh;
static bool sweet_vlock_published;
static uint32_t sweet_vlock_ticks, sweet_vlock_ok, sweet_vlock_late, sweet_vlock_lost, sweet_vlock_driftN;
static uint64_t sweet_vlock_drift_abs, sweet_vlock_drift_max;
/*
 * redfin 1049 (Yofuness 0274/0275): kern.sweet_vsyncphase US / boot-arg sweetvsyncphase=US (default 0 = off, as before).
 * Apps' CADisplayLinks fire on a fixed grid of CA's heartbeat period from mach time 0 (no frame-info presentation time);
 * with ticks free on the panel's edges that grid lands anywhere in the frame, and where it falls just before backboardd's
 * render gate edge commits miss it (on sunfish ~30 % of frames skipped at 12.5-14.8 ms after the tick). On: each tick
 * goes US us before a grid point (k * grid - US), so the display link fires US us after the tick; 0944/0946's panel-edge
 * phasing (sweetfbx 0x4) is left out while on (it comes back at the next tick after 0), and the notifications carry
 * the grid period. Sunfish runs 5000. Grid: kern.sweet_vsyncphase_grid / boot-arg sweetvsyncphasegrid, 0 (default) =
 * IOMFBDisplayRefresh's period when published (0995/1042, CA takes it without CA_HEARTBEAT_RATE), else 1 s / panel Hz
 * (CA_HEARTBEAT_RATE=90); 1..1000 = that rate in Hz, > 1000 = that period in ns. Both live (bsd/dev/sweet_gpu.c).
 * Line every 5 s while on: SWEET FB: vsync phase 5s: ... ticks .. | wake late avg/max.
 */
extern "C" uint32_t sweet_vsyncphase, sweet_vsyncphase_grid;
uint32_t sweet_vsyncphase = 5000, sweet_vsyncphase_grid;	/* default 5000 since 2026-11-05 (sweetvsyncphase=0 = off) */
/* 1060: RD kern.sweet_fb_frames (frames that reached the panel: statShown + 1044 scanout latches), kern.sweet_fb_hz */
extern "C" uint64_t sweet_fb_frames;
extern "C" uint32_t sweet_fb_hz;
uint64_t sweet_fb_frames;
uint32_t sweet_fb_hz = 60;
static uint32_t sweet_vphase_ticks;
static uint64_t sweet_vphase_late_abs, sweet_vphase_late_max;
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
	int           vsyncLock(int on);             /* 1042 */
	void          vsyncLockLine(void);
	uint64_t      fPhasePeriodAbs;              /* 1049: the grid period of the last phased tick */
	uint64_t      vsyncPhaseNext(uint64_t now);
	void          vsyncPhaseLine(void);
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
	bool           fBlSvc;          /* 1099: AppleARMBacklight published (sweetbl=1) */
	bool           blStart(void);
	bool           dsiDcs(const uint8_t *payload, uint32_t len);
	void           blSet(uint32_t level);
	static void    blLoop(void *arg);
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	using IOMobileFramebuffer::copyProperty;
	OSPtr<OSObject> copyProperty(const char *aKey) const APPLE_KEXT_OVERRIDE;      /* 1099: kern.sweet_bl_trace */
	bool serializeProperties(OSSerialize *s) const APPLE_KEXT_OVERRIDE;            /* 1099 */
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
static void sweet_tt_tick(uint64_t now);        /* 1107 */
static void sweet_tt_swap(uint32_t id, uint64_t now);
static void sweet_tt_latch(uint32_t swapId, uint64_t now);
static void sweet_pt_begin(uint32_t id, uint64_t now);  /* 1113 */
static void sweet_pt_swap(uint32_t id, uint64_t now, uint32_t fence, uint32_t ahead);
static void sweet_pt_poll(uint64_t now);
static void sweet_pt_flip(uint32_t id, uint64_t now);
static void sweet_pt_latch(uint32_t id, uint64_t now, uint64_t per);
static void sweet_pt_line(void);
extern "C" uint32_t sweet_pace_trace;
static void sweet_pt_tick(uint64_t now, bool on);       /* 1114 */
static void sweet_pt_wait(IOReturn ret);

static AppleCLCD *sweet_fb_instance;    /* 0943: for the poke commands (p fc, p hz) */
static volatile bool sweet_scan_auto_armed;     /* 1046: sweetscanout=2 (default): direct on at the first slot swap */
static void sweet_scan_auto(AppleCLCD *fb, const uint8_t *rec);

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

/*
 * redfin 1099: the cold-boot brightness bug (after every cold boot CoreBrightness sets the backlight to 1 of 1023 and
 * ignores the slider; after a backboardd restart it works). Two sysctls, boot-args sweetbltrace= / sweetblfix=.
 *
 * kern.sweet_bl_trace (default 3): "SWEET BLT:" lines, rate-limited (20 a second, bursts of 60; the number dropped
 * is printed with the next line), with the calling process:
 *   bit 0  user-space property reads (IORegistryEntryCreateCFProperty / SearchCFProperty / all properties) and
 *          writes (IORegistryEntrySetCFProperties) of AppleARMBacklight and AppleCLCD, key and value;
 *   bit 1  every IOMobileFramebuffer user-client call except the swap path (selectors 4, 5, 6, 77).
 * kern.sweet_bl_fix (default 7):
 *   bit 0  with AppleARMBacklight published, IOMFBBrightnessLevel is CoreBrightness's display brightness *factor*
 *          (16.16, 0..1.0: -[CBCAManager displayBrightnessPropertyHandler:] divides it by 65536; klogs show exactly
 *          0.00 at display off and 1.00 at display on, together with the ALS's IOHIDBacklightLevel 0x10000), not
 *          nits. 0906 drove the panel from it, so every display on/off put DBV 0x4/0x6 over the backlight's level
 *          (after a backboardd restart: 629 -> 0x275, then 0x4, then 0x6 within 0.4 s). Now it is only stored.
 *   bit 1  AppleCLCD starts with IOMFBBrightnessLevel 1.0 (a lit display's factor) instead of 250 nits when the
 *          backlight service is published, the value it has after a backboardd restart.
 *   bit 2  the ALS sends a neutral 100 lux sample at its client's ReportInterval until sensd's first sample
 *          (SweetHID.cpp), so CoreBrightness starts with ALS samples flowing, as after a backboardd restart.
 */
extern "C" {
uint32_t sweet_bl_trace = 3;
uint32_t sweet_bl_fix = 11;     /* 1102: 1 + 2 + 8 (calibration proven on rk1f); 4 (ALS keep-alive) off: pointless once the panel range is right */
}
static IOSimpleLock *sweet_blt_lock;
static uint64_t sweet_blt_refill;
static uint32_t sweet_blt_tokens, sweet_blt_dropped;

static bool
sweet_blt_take(uint32_t *dropped)
{
	if (sweet_blt_lock == NULL) {
		return false;
	}
	uint64_t now = mach_absolute_time(), ns;
	IOSimpleLockLock(sweet_blt_lock);
	absolutetime_to_nanoseconds(now - sweet_blt_refill, &ns);
	uint64_t add = ns / 50000000ULL;        /* one line per 50 ms */
	if (add != 0) {
		sweet_blt_tokens = (uint32_t)(sweet_blt_tokens + add > 60 ? 60 : sweet_blt_tokens + add);
		sweet_blt_refill = now;
	}
	bool ok = sweet_blt_tokens != 0;
	if (ok) {
		sweet_blt_tokens--;
		*dropped = sweet_blt_dropped;
		sweet_blt_dropped = 0;
	} else {
		sweet_blt_dropped++;
	}
	IOSimpleLockUnlock(sweet_blt_lock);
	return ok;
}

static void
sweet_blt_fmt(const OSObject *co, char *buf, size_t len)
{
	OSObject *o = const_cast<OSObject *>(co);
	OSNumber *n;
	OSBoolean *b;
	OSString *str;
	OSData *d;
	OSDictionary *dict;
	if (o == NULL) {
		strlcpy(buf, "ABSENT", len);
	} else if ((n = OSDynamicCast(OSNumber, o)) != NULL) {
		snprintf(buf, len, "%llu (%#llx)", n->unsigned64BitValue(), n->unsigned64BitValue());
	} else if ((b = OSDynamicCast(OSBoolean, o)) != NULL) {
		strlcpy(buf, b->isTrue() ? "true" : "false", len);
	} else if ((str = OSDynamicCast(OSString, o)) != NULL) {
		snprintf(buf, len, "\"%.60s\"", str->getCStringNoCopy());
	} else if ((d = OSDynamicCast(OSData, o)) != NULL) {
		snprintf(buf, len, "<data %u bytes>", d->getLength());
	} else if ((dict = OSDynamicCast(OSDictionary, o)) != NULL) {
		OSDictionary *br = OSDynamicCast(OSDictionary, dict->getObject("brightness"));
		OSNumber *v = br ? OSDynamicCast(OSNumber, br->getObject("value")) : NULL;
		if (v != NULL) {
			snprintf(buf, len, "{%u keys, brightness.value %u}", dict->getCount(), v->unsigned32BitValue());
		} else {
			snprintf(buf, len, "{%u keys}", dict->getCount());
		}
	} else {
		snprintf(buf, len, "<%s>", o->getMetaClass()->getClassName());
	}
}

/*
 * 1100: reads are logged once per (process, service, key, present/absent) instead of rate-limited (1099's
 * limiter dropped the 72 lines that mattered at a cold boot); writes are always logged.
 */
static uint32_t sweet_blt_seen[1024];
static uint32_t sweet_blt_nseen;

static bool
sweet_blt_first(int pid, const char *svc, const char *key, bool found)
{
	uint32_t h = 2166136261u;
	for (const char *q = svc; *q; q++) {
		h = (h ^ (uint8_t)*q) * 16777619u;
	}
	for (const char *q = key; *q; q++) {
		h = (h ^ (uint8_t)*q) * 16777619u;
	}
	h = (h ^ (uint32_t)pid) * 16777619u;
	h = (h ^ (found ? 1u : 2u)) * 16777619u;
	h |= 1;
	if (sweet_blt_lock == NULL) {
		return false;
	}
	bool first = true;
	IOSimpleLockLock(sweet_blt_lock);
	for (uint32_t i = 0; i < sweet_blt_nseen; i++) {
		if (sweet_blt_seen[i] == h) {
			first = false;
			break;
		}
	}
	if (first && sweet_blt_nseen < sizeof(sweet_blt_seen) / sizeof(sweet_blt_seen[0])) {
		sweet_blt_seen[sweet_blt_nseen++] = h;
	} else if (first) {
		first = false;  /* table full: no more read lines */
	}
	IOSimpleLockUnlock(sweet_blt_lock);
	return first;
}

/* what: "read", "write", "read-all"; user-space callers only */
static void
sweet_blt_prop(const char *svc, const char *what, const char *key, const OSObject *value)
{
	if (!(sweet_bl_trace & 1)) {
		return;
	}
	int pid = proc_selfpid();
	uint32_t dropped = 0;
	if (pid <= 0) {
		return;
	}
	if (what[0] == 'r' && !sweet_blt_first(pid, svc, key, value != NULL || what[4] == '-')) {
		return;         /* 1100: reads once each */
	}
	char name[32], v[96];
	proc_selfname(name, sizeof(name));
	sweet_blt_fmt(value, v, sizeof(v));
	if (dropped != 0) {
		IOLog("SWEET BLT: (%u lines dropped by the rate limit)\n", dropped);
	}
	bool all = strcmp(what, "read-all") == 0;
	IOLog("SWEET BLT: %s[%d] %s %s %s%s%s\n", name, pid, what, svc, key, all ? "" : " = ", all ? "" : v);
}

static void
sweet_blt_props(const char *svc, OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	if (!(sweet_bl_trace & 1) || d == NULL) {
		return;
	}
	OSCollectionIterator *it = OSCollectionIterator::withCollection(d);
	if (it == NULL) {
		return;
	}
	while (OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject())) {
		sweet_blt_prop(svc, "write", k->getCStringNoCopy(), d->getObject(k));
	}
	it->release();
}

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
	sweet_blt_props("AppleCLCD", properties);       /* 1099 */
	if (n == NULL || fBlLock == NULL) {
		return kIOReturnUnsupported;
	}
	uint64_t v = n->unsigned64BitValue();
	setProperty("IOMFBBrightnessLevel", v, 32);
	if ((sweet_bl_fix & 1) && fBlSvc) {
		/* 1099: CoreBrightness's brightness factor (0..1.0), not nits: the panel follows AppleARMBacklight */
		static uint64_t last = ~0ULL;
		if (v != last) {
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET BL: %s sets IOMFBBrightnessLevel %llu.%02llu (1099: a factor, panel unchanged)\n", name,
			    v >> 16, ((v & 0xffff) * 100) >> 16);
			last = v;
		}
		return kIOReturnSuccess;
	}
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
	void publishCalibration(void);  /* 1100 */
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	using IOService::copyProperty;
	OSPtr<OSObject> copyProperty(const char *aKey) const APPLE_KEXT_OVERRIDE;      /* 1099: kern.sweet_bl_trace */
	bool serializeProperties(OSSerialize *s) const APPLE_KEXT_OVERRIDE;            /* 1099 */
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

/* 1099: user-space reads of the brightness services (kern.sweet_bl_trace & 1) */
OSPtr<OSObject>
AppleARMBacklight::copyProperty(const char *aKey) const
{
	OSPtr<OSObject> obj = IOService::copyProperty(aKey);
	if (aKey != NULL) {
		sweet_blt_prop("AppleARMBacklight", "read", aKey, obj ? &*obj : NULL);
	}
	return obj;
}

bool
AppleARMBacklight::serializeProperties(OSSerialize *s) const
{
	sweet_blt_prop("AppleARMBacklight", "read-all", "(all properties)", NULL);
	return IOService::serializeProperties(s);
}

OSPtr<OSObject>
AppleCLCD::copyProperty(const char *aKey) const
{
	OSPtr<OSObject> obj = IOMobileFramebuffer::copyProperty(aKey);
	if (aKey != NULL) {
		sweet_blt_prop("AppleCLCD", "read", aKey, obj ? &*obj : NULL);
	}
	return obj;
}

bool
AppleCLCD::serializeProperties(OSSerialize *s) const
{
	sweet_blt_prop("AppleCLCD", "read-all", "(all properties)", NULL);
	return IOMobileFramebuffer::serializeProperties(s);
}

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
		publishCalibration();   /* 1100 */
		fNitsDone = true;
		IOLog("SWEET BL: 0973 nits published: %u..%u nits (sweetnits=0: off)\n", SWEET_BL_MIN_NITS, SWEET_BL_MAX_NITS);
	}
	setProperty("brightness-nits", (unsigned long long)nits, 32);
}

/*
 * redfin 1100 (kern.sweet_bl_fix & 8 at boot, default on): the panel luminance CoreBrightness really reads.
 * CBDisplayModuleHIDiOS's Display init (CoreBrightness 0x1af0d8a2c..0x1af0d8c88) takes "backlight-calibration-
 * parameters" from the backlight service (IORegistryEntryCreateCFProperty, no search), a dictionary of numbers read
 * as float: lMaxProduct/lMidProduct/lMinProduct, lMaxPanel/lMidPanel/lMinPanel (nits), current-for-max/mid-backlight,
 * hardware-max-current-limit, milliAmps2NitsScaleFactor. Absent (as with 0973's keys, which it never reads), its panel
 * range stays at the defaults 0..1 nit: backboardd logs "End ramp: ... Lmin = 0.000000, Lmax = 1.000000", "Factor=1
 * ... maxPanel=1.000000 minPanel=0.000000", "Changing nitsCap to 1.000000". At a cold boot auto-brightness turns on
 * at the first ALS sample and ramps to its target clamped to that 1 nit ("Begin ramp: L_target=1.0000 Lmax=1.000000",
 * then backboardd "brightness change:0.000977517 reason:BrightnessSystemDidChange"): DBV 1 of 1023, and every slider
 * position maps to the same 1 nit. After a backboardd restart auto-brightness stays inactive and the slider's
 * DisplayBrightness goes straight to the backlight. D22AP's ADT /backlight has LmaxProduct 674, LmidProduct 155,
 * LminProduct 2 (16.16); here the S6E3HC2 at the 500 nits 0906/0973 assume, mid 155, min 2. The current keys are
 * left out (CoreBrightness keeps its defaults for them, as now).
 */
/*
 * 1101: 1100's dictionary crashed backboardd (NULL deref in CFNumberGetValue, lr = CoreBrightness 0x1af0d8aa4): the
 * Display init does CFNumberGetValue(CFDictionaryGetValue(dict, key), kCFNumberFloatType, &f) WITHOUT a NULL check
 * for nine keys, then divides each by 65536 (16.16 fixed point):
 *   current-for-max-backlight, current-for-mid-backlight, hardware-max-current-limit (mA),
 *   lMaxProduct, lMidProduct, lMinProduct, lMaxPanel, lMidPanel, lMinPanel (nits).
 * Only milliAmps2NitsScaleFactor is checked (left out). All nine are published, as 16.16 integers. Currents: 20 mA
 * at 500 nits, 6.2 mA at 155 (proportional), 25 mA hardware limit; non-zero so no division by zero.
 * kern.sweet_bl_fix & 8 is now live: writing the sysctl publishes or removes the dictionary (then restart
 * backboardd to make CoreBrightness read it again).
 */
static AppleARMBacklight *sweet_bl_service;    /* 1101: for the kern.sweet_bl_fix handler; never released */

void
AppleARMBacklight::publishCalibration(void)
{
	if (!(sweet_bl_fix & 8)) {
		if (getProperty("backlight-calibration-parameters") != NULL) {
			removeProperty("backlight-calibration-parameters");
			IOLog("SWEET BL: 1101 backlight-calibration-parameters removed (kern.sweet_bl_fix & 8 off)\n");
		} else {
			IOLog("SWEET BL: 1101 backlight-calibration-parameters not published (kern.sweet_bl_fix & 8 off)\n");
		}
		return;
	}
#define SWEET_Q16(milli) ((unsigned long long)(((uint64_t)(milli) << 16) / 1000))
	static const struct {
		const char *key;
		uint32_t milli;         /* value x 1000 */
	} cal[] = {
		{ "current-for-max-backlight", 20000 }, { "current-for-mid-backlight", 6200 },
		{ "hardware-max-current-limit", 25000 },
		{ "lMaxProduct", SWEET_BL_MAX_NITS * 1000 }, { "lMidProduct", 155000 }, { "lMinProduct", SWEET_BL_MIN_NITS * 1000 },
		{ "lMaxPanel", SWEET_BL_MAX_NITS * 1000 }, { "lMidPanel", 155000 }, { "lMinPanel", SWEET_BL_MIN_NITS * 1000 },
	};
	OSDictionary *d = OSDictionary::withCapacity(sizeof(cal) / sizeof(cal[0]));
	if (d == NULL) {
		return;
	}
	for (unsigned i = 0; i < sizeof(cal) / sizeof(cal[0]); i++) {
		OSNumber *n = OSNumber::withNumber(SWEET_Q16(cal[i].milli), 32);
		if (n == NULL) {
			d->release();
			return;
		}
		d->setObject(cal[i].key, n);
		n->release();
	}
#undef SWEET_Q16
	setProperty("backlight-calibration-parameters", d);
	d->release();
	IOLog("SWEET BL: 1101 backlight-calibration-parameters published (16.16): lMax/lMid/lMin Panel+Product %u/155/%u nits, "
	    "current max/mid/limit 20/6.2/25 mA\n", SWEET_BL_MAX_NITS, SWEET_BL_MIN_NITS);
}

/* 1101: kern.sweet_bl_fix written (sweet_gpu.c): bit 8 takes effect now */
extern "C" void
sweet_bl_fix_changed(void)
{
	if (sweet_bl_service != NULL) {
		sweet_bl_service->publishCalibration();
	}
}

IOReturn
AppleARMBacklight::setProperties(OSObject *properties)
{
	OSDictionary *d = OSDynamicCast(OSDictionary, properties);
	OSNumber *b = d ? OSDynamicCast(OSNumber, d->getObject("brightness")) : NULL;
	sweet_blt_props("AppleARMBacklight", properties);       /* 1099 */
	if (b == NULL) {
		return d ? kIOReturnSuccess : kIOReturnBadArgument;
	}
	int32_t v = (int32_t)b->unsigned32BitValue();
	uint32_t level = v < 0 ? 0 : (uint32_t)v > SWEET_BL_MAX ? SWEET_BL_MAX : (uint32_t)v;
	static uint32_t logs;
	if (logs++ < 8 || (sweet_bl_trace & 1)) {      /* 1099/1100: every one while tracing */
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
	PE_parse_boot_argn("sweetvsyncphase", &sweet_vsyncphase, sizeof(sweet_vsyncphase));             /* 1049 */
	PE_parse_boot_argn("sweetvsyncphasegrid", &sweet_vsyncphase_grid, sizeof(sweet_vsyncphase_grid));
	if (sweet_vsyncphase || sweet_vsyncphase_grid) {
		IOLog("SWEET FB: vsync phase %u us, grid %u (1049: ticks US before CA's display-link grid; grid 0 auto, "
		    "<= 1000 Hz, else ns)\n", sweet_vsyncphase, sweet_vsyncphase_grid);
	}
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
	sweet_fb_hz = fHz;     /* 1060 */
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
	if (sweet_blt_lock == NULL) {
		sweet_blt_lock = IOSimpleLockAlloc();   /* 1099 */
	}
	PE_parse_boot_argn("sweetbltrace", &sweet_bl_trace, sizeof(sweet_bl_trace));
	PE_parse_boot_argn("sweetblfix", &sweet_bl_fix, sizeof(sweet_bl_fix));
	if (blStart()) {
		uint32_t blOn = 0;
		PE_parse_boot_argn("sweetbl", &blOn, sizeof(blOn));
		AppleARMBacklight *bl = blOn ? OSTypeAlloc(AppleARMBacklight) : NULL;
		if (bl != NULL && bl->init() && bl->attach(this)) {
			bl->fFB = this;
			sweet_bl_service = bl;  /* 1101 */
			bl->retain();
			bl->setProperty("backlight-control", true);
			bl->publish(SWEET_BL_MAX / 2);
			bl->registerService();
			fBlSvc = true;
			if (sweet_bl_fix & 2) {
				setProperty("IOMFBBrightnessLevel", 1ULL << 16, 32);   /* 1099: a lit display's factor */
			}
			IOLog("SWEET BL: AppleARMBacklight published (sweetbl=1); 1099 kern.sweet_bl_fix %#x kern.sweet_bl_trace %#x\n",
			    sweet_bl_fix, sweet_bl_trace);
		}
		OSSafeReleaseNULL(bl);
	}
	if (fHz == 90 && !setHz(90, false)) {
		IOLog("SWEET FB: sweetfb_hz=90: the panel did not take DCS 0x53 0x38: 60 Hz\n");
		fHz = 60;
		sweet_fb_hz = 60;      /* 1060 */
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
	} else if (sweet_surface_scan_mode() == 2) {
		sweet_scan_auto_armed = true;   /* 1046: on at the first slot surface's swap (AppleCLCD::swap) */
	}
	IOLog("SWEET SCANOUT: sweetscanout=%u (1046: 0 off, 1 on at start, 2 = default: on at the first slot swap, "
	    "3 slots only)%s\n", sweet_surface_scan_mode(), sweet_scan_auto_armed ? ": auto armed" : "");
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
	if (sweet_vsyncphase != 0 && fb->fPhasePeriodAbs != 0) {
		/* 1049: how late this tick woke after its grid deadline */
		uint64_t late = now > fb->fNextVsync ? now - fb->fNextVsync : 0;
		sweet_vphase_ticks++;
		sweet_vphase_late_abs += late;
		if (late > sweet_vphase_late_max) {
			sweet_vphase_late_max = late;
		}
	}
	fb->fNextVsync += fb->fVsyncAbs;
	if (fb->fNextVsync <= now) {
		fb->fNextVsync = now + fb->fVsyncAbs;
	}
	if (sweet_vsyncphase != 0) {
		fb->fNextVsync = fb->vsyncPhaseNext(now);	/* 1049: US before CA's grid, not on the panel's edges */
	} else if (sweet_fbx & SWEET_FBX_SWAPQ) {
		fb->fPhasePeriodAbs = 0;
		fb->phaseTick(now);	/* 0944: the next tick just after a TE */
	} else {
		fb->fPhasePeriodAbs = 0;
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
		sweet_vlock_ticks++;    /* 1042 */
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
	if (sweet_vsyncphase != 0 && fPhasePeriodAbs != 0) {
		period = fPhasePeriodAbs;       /* 1049: the grid the ticks are on */
	}
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		(uint32_t)period, (uint32_t)(period >> 32),
	};

	fPaceSwapped = false;
	fPaceLastAbs = now;     /* 0983 */
	sweet_tt_tick(now);     /* 1107: vsync -> submit / SwapEnd */
	if (__improbable(sweet_pace_trace & 4)) {
		sweet_pt_tick(now, vsyncOn());  /* 1114: what CA did with the previous tick; is the newest swap still pending */
	}
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
	if (__improbable(sweet_scan_auto_armed)) {
		sweet_scan_auto(this, rec);     /* 1046: before this frame's sweet_scan_live() check, so it already counts */
	}
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
	OSAddAtomic64(1, &sweet_fb_frames);    /* 1060 */
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
	if (sweet_vlock) {
		IOLockUnlock(fLock);
		vsyncLockLine();        /* 1042 */
		IOLockLock(fLock);
	}
	if (sweet_vsyncphase) {
		IOLockUnlock(fLock);
		vsyncPhaseLine();       /* 1049 */
		IOLockLock(fLock);
	}
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

/* 1042: kern.sweet_vsync_lock 0|1 (see sweet_vlock). From the sysctl handler (no locks held). */
int
AppleCLCD::vsyncLock(int on)
{
	uint64_t ns = 0;
	IOLockLock(fLock);
	if (on && !sweet_vlock) {
		sweet_vlock_old_vsync = sweet_vsync;
		sweet_vlock_old_refresh = sweet_fb_refresh;
		sweet_vlock_published = fRefreshNs == 0;
		sweet_vsync |= SWEET_VSYNC_CRIT | SWEET_VSYNC_CAL | SWEET_VSYNC_PERIOD;
		sweet_fb_refresh = 1;
		fFcEarlyUs = 300;
		sweet_vlock_ticks = sweet_vlock_ok = sweet_vlock_late = sweet_vlock_lost = sweet_vlock_driftN = 0;
		sweet_vlock_drift_abs = sweet_vlock_drift_max = 0;
		absolutetime_to_nanoseconds(fFcPeriod, &ns);
	} else if (!on && sweet_vlock) {
		sweet_vsync = sweet_vlock_old_vsync;
		sweet_fb_refresh = sweet_vlock_old_refresh;
		fFcEarlyUs = 300;
	}
	bool was = sweet_vlock != 0;
	sweet_vlock = on ? 1 : 0;
	IOLockUnlock(fLock);
	if (on && !was) {
		publishRefresh(ns != 0 ? ns : (fHz == 90 ? 11111111ULL : SWEET_FB_VSYNC_US * 1000ULL));
	} else if (!on && was && sweet_vlock_published && sweet_fb_refresh == 0) {
		removeProperty("IOMFBDisplayRefresh");
		fRefreshNs = 0;
	}
	IOLog("SWEET FB: vsync lock %d (1042: sweetvsync %#x, refresh %u, panel period %llu ns; CA takes IOMFBDisplayRefresh "
	    "at backboardd start, without CA_HEARTBEAT_RATE)\n", sweet_vlock, sweet_vsync, sweet_fb_refresh, ns);
	return 0;
}

/* 1042: from statLine, every 5 s while the lock is on */
void
AppleCLCD::vsyncLockLine(void)
{
	if (!sweet_vlock) {
		return;
	}
	IOLockLock(fLock);
	uint64_t pns = 0, dsum = 0, dmax = 0;
	absolutetime_to_nanoseconds(fFcPeriod, &pns);
	absolutetime_to_nanoseconds(sweet_vlock_drift_abs, &dsum);
	absolutetime_to_nanoseconds(sweet_vlock_drift_max, &dmax);
	uint32_t ticks = sweet_vlock_ticks, ok = sweet_vlock_ok, late = sweet_vlock_late, lost = sweet_vlock_lost;
	uint32_t dn = sweet_vlock_driftN;
	sweet_vlock_ticks = sweet_vlock_ok = sweet_vlock_late = sweet_vlock_lost = sweet_vlock_driftN = 0;
	sweet_vlock_drift_abs = sweet_vlock_drift_max = 0;
	uint64_t rns = fRefreshNs;
	IOLockUnlock(fLock);
	IOLog("SWEET FB: vsync lock 5s: period %llu ns (%llu.%03llu Hz) published %llu ns | ticks %u | cal ok %u late %u lost %u | "
	    "drift corrected %u x avg %llu us max %llu us | dead %d\n", pns, pns ? 1000000000000ULL / pns / 1000 : 0,
	    pns ? 1000000000000ULL / pns % 1000 : 0, rns, ticks, ok, late, lost, dn, dn ? dsum / 1000 / dn : 0, dmax / 1000,
	    fFcDead ? 1 : 0);
}

/*
 * 1049: the first point of CA's display-link grid minus sweet_vsyncphase us that is at least half a grid period after
 * NOW (a late wake skips no grid point and two ticks never come closer than half a period). Exact in ns: the grid
 * period is num / den ns (1 s / 90 is not a whole number of ns, nor of 19.2 MHz ticks).
 */
uint64_t
AppleCLCD::vsyncPhaseNext(uint64_t now)
{
	uint32_t g = sweet_vsyncphase_grid;
	uint64_t num, den, t, pns, at;
	if (g > 1000) {
		num = g;
		den = 1;
	} else if (g > 0) {
		num = NSEC_PER_SEC;
		den = g;
	} else if (fRefreshNs != 0) {
		num = fRefreshNs;
		den = 1;
	} else {
		num = NSEC_PER_SEC;
		den = fHz == 90 ? 90 : 60;
	}
	uint64_t per = num / den;
	if (per < 1000000 || per > 100000000) {         /* outside 10..1000 Hz: the nominal panel rate */
		num = NSEC_PER_SEC;
		den = fHz == 90 ? 90 : 60;
		per = num / den;
	}
	pns = ((uint64_t)sweet_vsyncphase * 1000) % per;
	absolutetime_to_nanoseconds(now, &t);
	t += per / 2;
	uint64_t k = ((t + pns) * den + num - 1) / num;
	nanoseconds_to_absolutetime(k * num / den - pns, &at);
	nanoseconds_to_absolutetime(per, &fPhasePeriodAbs);
	if (at <= now) {
		at = now + fPhasePeriodAbs;
	}
	return at;
}

/* 1049: from statLine, every 5 s while sweet_vsyncphase is on */
void
AppleCLCD::vsyncPhaseLine(void)
{
	uint64_t pns = 0, lsum = 0, lmax = 0;
	absolutetime_to_nanoseconds(fPhasePeriodAbs, &pns);
	absolutetime_to_nanoseconds(sweet_vphase_late_abs, &lsum);
	absolutetime_to_nanoseconds(sweet_vphase_late_max, &lmax);
	uint32_t n = sweet_vphase_ticks;
	sweet_vphase_ticks = 0;
	sweet_vphase_late_abs = sweet_vphase_late_max = 0;
	IOLog("SWEET FB: vsync phase 5s: %u us before a %llu ns grid (grid %u, refresh %llu ns, panel %u Hz) | ticks %u | "
	    "wake late avg %llu us max %llu us\n", sweet_vsyncphase, pns, sweet_vsyncphase_grid, fRefreshNs, fHz, n,
	    n ? lsum / 1000 / n : 0, lmax / 1000);
}

extern "C" int sweet_fb_vsync_lock(int on);
int
sweet_fb_vsync_lock(int on)
{
	if (sweet_fb_instance == NULL) {
		return 19;      /* ENODEV (no errno.h in this file) */
	}
	return sweet_fb_instance->vsyncLock(on);
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
	sweet_fb_hz = hz;      /* 1060 */
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
/* 1111: UBWC fetch (upstream dpu_hw_sspp.c; MDSS offsets from 0x0ae00000 as every other here, msm_mdss.c) */
#define SSPP_SRC2               0x01cu  /* UBWC: the meta plane */
#define SSPP_YSTRIDE1           0x028u  /* UBWC: bits 15:0 the meta pitch */
#define SSPP_UNPACK             0x034u
#define SSPP_OP_MODE            0x038u  /* bit 0: BWC_EN */
#define SSPP_UBWC_STATIC        0x044u
#define SSPP_FETCH_CONFIG       0x048u
#define SSPP_UBWC_ERROR         0x138u
#define MDSS_UBWC_DEC_VERSION   0x058u
#define MDSS_UBWC_STATIC        0x144u
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
	bool                ufmt;           /* 1111: the pipes are set for a UBWC surface (kern.sweet_ubwc_disp) */
	bool                usaved;         /* 1111: uold holds the format registers as they were before the first */
	uint32_t            uold[2][7];     /* 1111: SRC2, YSTRIDE1, FORMAT, UNPACK, OP_MODE, UBWC_STATIC, FETCH_CONFIG */
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

/*
 * ---- 1111: UBWC display surfaces (kern.sweet_ubwc_disp, boot-arg sweetubwc; SweetSurface.cpp, SweetAccelerator.cpp) ----
 * A scanout surface whose GPU client draws it compressed (ubwcLive) is fetched by VIG0/VIG1 through their UBWC decoder
 * (VIG pipes have one, DMA pipes do not; the pipes and the QSEED3LITE 1125x2436 -> 1080x2340 scale stay as they are:
 * the decoder sits before the scaler). Per pipe, as upstream dpu_hw_sspp_setup_format/_sourceaddress/_rects do for
 * DRM_FORMAT_ABGR8888 + QCOM_COMPRESSED: SRC0 the colour plane, SRC2 the meta plane, YSTRIDE0 the colour pitch,
 * YSTRIDE1 the meta pitch, FORMAT fetch mode 2 (bits 31:30), UNPACK R,G,B,A (0x03010002: the GPU writes compressed
 * targets with swap WZYX, so byte 0 is red whatever the IOSurface's 'BGRA' says; the only RGB order the MDP's UBWC
 * decoder has), OP_MODE BWC_EN, FETCH_CONFIG 0x87 | hbb-13 << 18, UBWC_STATIC by the decoder version (MDSS
 * +0x58). The first switch saves these registers (uold); restore and a linear frame put them back. Nothing here runs
 * unless a surface went ubwcLive, which needs boot-arg sweetubwc and kern.sweet_ubwc_disp 1.
 *
 * kern.sweet_ubwc_sspp (0 = auto as above; else the raw SSPP UBWC_STATIC value) and kern.sweet_ubwc_mdss (0 = auto:
 * MDSS UBWC_STATIC written msm_mdss-style only if it reads 0; 1 = never written; else the raw value written): read
 * at each switch to UBWC, so a variant is tried by a backboardd restart, no rebuild.
 */
extern "C" int sweet_ubwc_sspp, sweet_ubwc_mdss;
static struct {
	uint32_t frames, linear, switches, skipped, corners, errs[2];
	uint64_t win;
	bool     logged;
} sweet_ubwc_st;

static uint32_t
sweet_ubwc_sspp_static(uint32_t fmt, uint32_t *ver)
{
	const uint32_t swz = 6, hbb = SWEET_UBWC_HBB - 13;      /* as the GPU: level 2+3 swizzle, hbb 14 */
	uint32_t v = sweet_mdp_rd(MDSS_UBWC_DEC_VERSION) >> 28;
	*ver = v;
	if (sweet_ubwc_sspp != 0) {
		return (uint32_t)sweet_ubwc_sspp;
	}
	uint32_t fast = (fmt & (1u << 8)) ? 1u << 31 : 0;
	switch (v) {
	case 1:
		return fast | (swz & 1) | 1u << 8 | hbb << 4;
	case 2:
		return fast | swz | hbb << 4;
	case 4:
		return 1u << 30;
	default:        /* 3 (SM7250), or an unreadable version: as 3 */
		return 1u << 30 | swz | hbb << 4;
	}
}

static void
sweet_ubwc_mdss_setup(uint32_t ver)
{
	uint32_t cur = sweet_mdp_rd(MDSS_UBWC_STATIC), want = 0;
	if (sweet_ubwc_mdss == 1) {
		want = cur;
	} else if (sweet_ubwc_mdss != 0) {
		want = (uint32_t)sweet_ubwc_mdss;
	} else if (cur == 0) {
		/* msm_mdss_setup_ubwc_dec_{20,30,40}: swizzle 6, ubwc_static 1, hbb 14 (1), UBWC 3.0 encoder (bit 10) */
		want = ver == 4 ? (6u & 1) | 1u << 4 | 1u << 10 : ver == 2 ? (6u & 1) | 1u << 4 : 6u | 1u << 3 | 1u << 4 | 1u << 10;
	} else {
		want = cur;
	}
	if (want != cur) {
		sweet_mdp_wr(MDSS_UBWC_STATIC, want);
	}
	if (!sweet_ubwc_st.logged || want != cur) {
		sweet_ubwc_st.logged = true;
		IOLog("SWEET MDP: ubwc: decoder version %u (MDSS +0x58 %#x), MDSS UBWC_STATIC %#x -> %#x\n", ver,
		    sweet_mdp_rd(MDSS_UBWC_DEC_VERSION), cur, want);
	}
}

/* The pipes' format registers for surface s compressed (on) or back to what they were (off). */
static void
sweet_mdp_ubwc_fmt(bool on, SweetSurface *s)
{
	static const uint16_t r[7] = { SSPP_SRC2, SSPP_YSTRIDE1, SSPP_FORMAT, SSPP_UNPACK, SSPP_OP_MODE, SSPP_UBWC_STATIC,
		                       SSPP_FETCH_CONFIG };
	if (on == sweet_mdp.ufmt) {
		return;
	}
	if (on && !sweet_mdp.usaved) {
		for (uint32_t p = 0; p < 2; p++) {
			for (uint32_t i = 0; i < 7; i++) {
				sweet_mdp.uold[p][i] = sweet_mdp_rd(MDP_VIG(p) + r[i]);
			}
		}
		sweet_mdp.usaved = true;
	}
	if (!sweet_mdp.usaved) {
		return;
	}
	uint32_t ver = 0, st = on ? sweet_ubwc_sspp_static(sweet_mdp.uold[0][2], &ver) : 0;
	if (on) {
		sweet_ubwc_mdss_setup(ver);
	}
	for (uint32_t p = 0; p < 2; p++) {
		uint32_t b = MDP_VIG(p);
		if (!on) {
			for (uint32_t i = 0; i < 7; i++) {
				sweet_mdp_wr(b + r[i], sweet_mdp.uold[p][i]);
			}
			continue;
		}
		sweet_mdp_wr(b + SSPP_YSTRIDE1, (sweet_mdp.uold[p][1] & 0xffff0000u) | s->ubwcMetaPitch);
		sweet_mdp_wr(b + SSPP_FORMAT, (sweet_mdp.uold[p][2] & ~(3u << 30)) | 2u << 30);
		sweet_mdp_wr(b + SSPP_UNPACK, 0x03010002u);
		sweet_mdp_wr(b + SSPP_OP_MODE, sweet_mdp.uold[p][4] | 1u);
		sweet_mdp_wr(b + SSPP_UBWC_STATIC, st);
		sweet_mdp_wr(b + SSPP_FETCH_CONFIG, 0x87u | (uint32_t)(SWEET_UBWC_HBB - 13) << 18);
		(void)sweet_mdp_rd(b + SSPP_UBWC_ERROR);
		sweet_mdp_wr(b + SSPP_UBWC_ERROR, 1u << 31);    /* clear */
	}
	sweet_mdp.ufmt = on;
	sweet_ubwc_st.switches++;
	static uint32_t logs;
	if (logs < 16) {
		logs++;
		IOLog("SWEET MDP: ubwc: pipes %s (surface %u, format %#x -> %#x, unpack %#x, op %#x, static %#x, fetch %#x)\n",
		    on ? "compressed" : "linear again", s != NULL ? s->id : 0, sweet_mdp.uold[0][2], sweet_mdp_rd(MDP_VIG(0) + SSPP_FORMAT),
		    sweet_mdp_rd(MDP_VIG(0) + SSPP_UNPACK), sweet_mdp_rd(MDP_VIG(0) + SSPP_OP_MODE),
		    sweet_mdp_rd(MDP_VIG(0) + SSPP_UBWC_STATIC), sweet_mdp_rd(MDP_VIG(0) + SSPP_FETCH_CONFIG));
	}
}

/*
 * Surface s's planes into both pipes (latched at the next CTL0 flush): before 1111 only SRC0 = scanPA was written per
 * flip, and a linear surface still gets exactly that (the format registers go back first if the last one was UBWC).
 */
static void
sweet_mdp_scan_addr(SweetSurface *s)
{
	bool u = s->ubwcLive != 0;
	if (u || sweet_mdp.ufmt) {
		sweet_mdp_ubwc_fmt(u, s);
	}
	for (uint32_t p = 0; p < 2; p++) {
		uint32_t b = MDP_VIG(p);
		if (u) {
			sweet_mdp_wr(b + SSPP_YSTRIDE0, (sweet_mdp.old[p][MR_YSTRIDE0] & 0xffff0000u) | s->ubwcDataPitch);
			sweet_mdp_wr(b + SSPP_SRC2, (uint32_t)(s->scanPA + s->ubwcMeta));
			sweet_mdp_wr(b + SSPP_SRC0, (uint32_t)(s->scanPA + s->ubwcData));
		} else {
			sweet_mdp_wr(b + SSPP_SRC0, (uint32_t)s->scanPA);
		}
	}
	if (u) {
		sweet_ubwc_st.frames++;
	} else if (sweet_ubwc_st.switches != 0) {
		sweet_ubwc_st.linear++;
	}
}

/* restore: the format registers as they were (with sweet_mdp.lock; before the flush) */
static void
sweet_mdp_ubwc_restore(void)
{
	if (sweet_mdp.ufmt) {
		sweet_mdp_ubwc_fmt(false, NULL);
	}
	sweet_mdp.usaved = false;
}

/* 1111: one line per 5 s while UBWC frames are scanned (the scanout statistics' window; fLock) */
static void
sweet_ubwc_stats_locked(void)
{
	if (sweet_ubwc_st.frames == 0 && sweet_ubwc_st.skipped == 0) {
		return;
	}
	uint32_t e0 = sweet_mdp_rd(MDP_VIG(0) + SSPP_UBWC_ERROR), e1 = sweet_mdp_rd(MDP_VIG(1) + SSPP_UBWC_ERROR);
	IOLog("SWEET SCANOUT: 5s ubwc: frames %u linear %u switches %u not-presented %u corners-skipped %u | error vig0 %#x vig1 %#x\n",
	    sweet_ubwc_st.frames, sweet_ubwc_st.linear, sweet_ubwc_st.switches, sweet_ubwc_st.skipped, sweet_ubwc_st.corners, e0, e1);
	sweet_ubwc_st.frames = sweet_ubwc_st.linear = sweet_ubwc_st.switches = sweet_ubwc_st.skipped = sweet_ubwc_st.corners = 0;
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
	sweet_mdp_ubwc_restore();       /* 1111: nothing unless a UBWC surface was scanned */
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
	if (s->ubwcLive) {
		IOLog("SWEET MDP: ca: slot %u = surface %u is UBWC-compressed: not shown as linear rows (1111)\n", slot, s->id);
		s->release();
		return false;
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
 * only, off until p scanout 1); p scanout 0|1 switches live. Inert with sweetscanout=0. 1046: 2 is the default and
 * turns direct on at the first slot swap (sweet_scan_auto); 3 = slots only (the old 2). ----
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
	/* 1109: tribuf 2 = SwapWait polls answer "complete" only once the swap is off screen (a later one latched) */
	bool          pollRel;
	bool          queue;            /* 1110: kern.sweet_scangate_queue: a newer swap queues behind a pending one (any gate) */
	uint32_t      stRelShown, stHeldSwap, stRepeatSwap, lastSurf;
	uint64_t      stCornerAbs;
	uint32_t      stCorners;
	int           gatePid;          /* 0985: who swapped (backboardd): its GPU submits wait for gateSeq on screen */
	volatile uint64_t gateSeq;
	uint64_t      stGateAbs;
	uint32_t      stGate, stGateWait, stGateTimeout;
	uint32_t      stFlagDisp, stFlagBare, stUnflagged;      /* 1104: the swapper's submits flagged display / bare (not gated) / from a process that never flagged */
	/* 1105: gate 3 (per surface): display submits with a surface id, without one (gated as 1), held for the surface on
	 * screen / pending a latch, wait time, 50 ms timeouts, frames whose successor's display submit passed while they
	 * were still pending (overlap), surfaces not held at all */
	uint32_t      st3Id, st3NoId, st3Shown, st3Pending, st3Timeout, st3Overlap, st3Free;
	uint64_t      st3Abs, st3OvSeq;
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
 * ---- 1116: colour management (Night Shift / True Tone) on the DSPP PCC (logs/research/nightshift-truetone-plan-2026-11-15.md
 * P0 + P1) ----
 *
 * CoreBrightness picks a white point, QuartzCore (backboardd) turns it into a 3x3 matrix and calls
 * IOMobileFramebufferSetMatrix(fb, 9, int64 S31.32[3][3], row-major) = IOMFB selector 55, then
 * SetBrightnessCorrection (50, 16.16). Before 1116 the user client dropped 17/27/50/54/55 (default: case), and its
 * GetMatrix (54) / GetGammaTable (27) answered zeros. The SM7250 DPU has a DSPP behind each layer mixer we scan out
 * through (LM0 -> DSPP0, LM1 -> DSPP1; lito-sde.dtsi qcom,sde-dspp-off <0x55000 0x57000>) with a 3x3 PCC at +0x1700.
 *
 * kern.sweet_cm_trace (default 0; no register write):
 *   bit 0  "SWEET CM:" lines for selectors 17/27 (gamma: size, checksum, a few entries), 19, 50, 51, 54, 55 (location and
 *          the nine S31.32 values as decimals; each new matrix only), 57, 68, 69, 84, and every PCC commit; at most 20
 *          lines a second (the number dropped goes with the next line)
 *   bit 1  writing a value with it dumps DSPP0/1 (PCC enable/constants/coefficients, GC, IGC/PA/gamut op modes, DSPP
 *          top) and CTL0 once, from the sysctl caller (also: p cm dump)
 * kern.sweet_cm (default 0):
 *   bit 0  the last accepted sel 55 matrix (location 9) goes to DSPP0 + DSPP1 PCC (1.0 = 0x8000, enable bit 0) at the
 *          next direct-scanout flip, its DSPP flush bits (13, 14) ORed into that flip's CTL0 flush write. With no flip
 *          (static screen) the scanout thread writes them itself once nothing is in flight (or after 50 ms), under
 *          fLock, the lock every flip is written under (no scanout thread: the caller, under fLock). GetMatrix (54)
 *          then answers the stored matrix (identity before the first one, or for another location) and GetGammaTable
 *          (27) the table last set by 17 (zeros before one). With 0 both answer zeros as before 1116. Writing 0
 *          disables PCC (bypass = identity) at the next frame.
 * The matrix is stored (and guarded) with kern.sweet_cm 0 too, so turning it on applies CoreBrightness's current state.
 * Guard (always): a matrix with a coefficient outside [-2, 2], every row sum < 0.15 (nearly black), or a row sum > 1.6 /
 * a row's positive terms > 2 (clipping) is refused and logged, the previous one stays; identity is always taken.
 * Gamma (GC/IGC) is never written: sel 17 is stored and traced only. The brightness correction (50) is stored only.
 *
 * Unverified on the phone (plan 2.1), hence the pokes: the register orientation (downstream sde_hw_color_proc_v4:
 * plane = output at +4 * plane, coefficient of input R/G/B at +0x10/+0x1c/+0x28; upstream dpu_hw_dspp.c names them the
 * other way round) and the negative encoding (until known, negatives are written as 0).
 *   p cm                       state line
 *   p cm dump                  the DSPP/CTL0 dump (as kern.sweet_cm_trace bit 1)
 *   p cm test r|g|b|warm|id|rg|neg   a fixed matrix on PCC whatever kern.sweet_cm says (until p cm test off):
 *                              r/g/b one channel only (whole screen red / green / blue: the channel order); warm =
 *                              diag(1, 0.78, 0.55); id = identity with PCC enabled (no visible change: the write path);
 *                              rg = R_out <- G_in (right orientation: red turns black, green turns yellow; transposed:
 *                              red turns green, green stays green); neg = R_out = R - 0.5 G (negatives taken: white
 *                              turns cyan-ish; written as 0: no change; read as a large positive: green turns yellow)
 *   p cm test off              back to kern.sweet_cm
 *   p cm swap [0|1]            1: write the transpose (input-major registers); no argument: toggle
 *   p cm neg 0|1|2             negatives: 0 written as 0 (default), 1 two's complement (32 bits), 2 sign-magnitude
 *                              (bit 17 sign, S2.15)
 */
#define MDP_DSPP(n)             (0x55000u + 0x2000u * (n))
#define DSPP_PCC                0x1700u         /* lito-sde.dtsi qcom,sde-dspp-pcc <0x1700 0x00040000> */
#define DSPP_GC                 0x17c0u         /* qcom,sde-dspp-gc <0x17c0 0x00010008> */
#define MDP_DSPP_TOP            0x1300u         /* qcom,sde-dspp-top-off */
#define MDP_FLUSH_DSPP01        0x6000u         /* CTL_FLUSH bits 13/14, DPU < 7 (dpu_hw_ctl.c) */
#define SWEET_CM_ONE            0x8000          /* 1.0 in PCC units (S.15) */
#define SWEET_CM_GAMMA_BYTES    3084u           /* 3 x 257 x u32 */

extern "C" {
uint32_t sweet_cm_trace = 0;    /* kern.sweet_cm_trace (bsd/dev/sweet_gpu.c) */
uint32_t sweet_cm = 0;          /* kern.sweet_cm */
void sweet_cm_ctl(uint32_t what);
}

static struct {
	int64_t     m[9];           /* the last accepted sel 55 matrix, location 9 (S31.32, row = output) */
	bool        have;
	int64_t     seen[9];        /* the last one received (trace: new matrices only) */
	uint32_t    seenLoc;
	bool        seenAny;
	uint32_t    gamma[SWEET_CM_GAMMA_BYTES / 4];
	uint32_t    gammaSize;      /* bytes stored from the last sel 17, 0 = none */
	uint32_t    bc;             /* sel 50 (16.16), stored only */
	int32_t     test[9];        /* p cm test (PCC units, row = output) */
	const char *testName;       /* NULL = no test */
	bool        swap;
	uint32_t    neg;
	bool        dirty;          /* PCC differs from what it should be: commit at the next flip / idle look */
	uint64_t    dirtyAbs;
	bool        on;             /* PCC enabled by the last commit */
	bool        touched;        /* PCC written at least once this boot (before that, bypass needs no write) */
	int32_t     hw[9];          /* the last commit's coefficients */
	uint32_t    nAccepted, nRefused, nCommits, nMerged, nLone, nNegClamped;
	uint64_t    checkAbs;       /* a lone commit's flush, read back at the scanout thread's next look */
	/* trace rate limit (racy by design: log lines only) */
	uint64_t    trWin;
	uint32_t    trN, trDropped;
} sweet_cm_st;

static const int32_t sweet_cm_ident[9] = { SWEET_CM_ONE, 0, 0, 0, SWEET_CM_ONE, 0, 0, 0, SWEET_CM_ONE };

/* fixed point with SHIFT fraction bits as [-]d.dddd */
static void
sweet_cm_dec(char *b, size_t n, int64_t v, uint32_t shift)
{
	uint64_t a = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
	uint64_t fr = ((a & ((1ULL << shift) - 1)) * 10000ULL) >> shift;
	snprintf(b, n, "%s%llu.%04llu", v < 0 ? "-" : "", a >> shift, fr);
}

static bool
sweet_cm_tr_take(void)
{
	uint64_t now = mach_absolute_time(), ns;
	absolutetime_to_nanoseconds(now - sweet_cm_st.trWin, &ns);
	if (sweet_cm_st.trWin == 0 || ns >= NSEC_PER_SEC) {
		sweet_cm_st.trWin = now;
		sweet_cm_st.trN = 0;
		if (sweet_cm_st.trDropped != 0) {
			IOLog("SWEET CM: (%u lines dropped by the rate limit)\n", sweet_cm_st.trDropped);
			sweet_cm_st.trDropped = 0;
		}
	}
	if (sweet_cm_st.trN >= 20) {
		sweet_cm_st.trDropped++;
		return false;
	}
	sweet_cm_st.trN++;
	return true;
}

/* nine values as "a b c | d e f | g h i" */
static void
sweet_cm_fmt9(char *b, size_t n, const int64_t *v, uint32_t shift)
{
	size_t o = 0;
	for (uint32_t i = 0; i < 9 && o + 1 < n; i++) {
		char d[24];
		sweet_cm_dec(d, sizeof(d), v[i], shift);
		int k = snprintf(b + o, n - o, "%s%s", i == 0 ? "" : (i % 3 == 0 ? " | " : " "), d);
		if (k < 0) {
			break;
		}
		o += (size_t)k;
	}
}

static void
sweet_cm_fmt9p(char *b, size_t n, const int32_t *c)
{
	int64_t v[9];
	for (uint32_t i = 0; i < 9; i++) {
		v[i] = c[i];
	}
	sweet_cm_fmt9(b, n, v, 15);
}

/* S31.32 -> PCC units (1.0 = 0x8000), rounded, clamped to [-4, 4) */
static int32_t
sweet_cm_to_pcc(int64_t v)
{
	if (v > (4LL << 32)) {
		return 0x1ffff;
	}
	if (v < -(4LL << 32)) {
		return -0x20000;
	}
	int64_t c = (v + (1LL << 16)) >> 17;
	return (int32_t)(c > 0x1ffff ? 0x1ffff : (c < -0x20000 ? -0x20000 : c));
}

static bool
sweet_cm_is_ident(const int32_t c[9])
{
	return memcmp(c, sweet_cm_ident, sizeof(sweet_cm_ident)) == 0;
}

/* Always on. NULL: taken; else why not. PCC units, row = output. */
static const char *
sweet_cm_guard(const int32_t c[9])
{
	if (sweet_cm_is_ident(c)) {
		return NULL;
	}
	bool lit = false;
	for (uint32_t i = 0; i < 9; i++) {
		if (c[i] > 2 * SWEET_CM_ONE || c[i] < -2 * SWEET_CM_ONE) {
			return "a coefficient outside [-2, 2]";
		}
	}
	for (uint32_t r = 0; r < 3; r++) {
		int32_t sum = 0, pos = 0;
		for (uint32_t k = 0; k < 3; k++) {
			sum += c[3 * r + k];
			pos += c[3 * r + k] > 0 ? c[3 * r + k] : 0;
		}
		if (sum >= SWEET_CM_ONE * 15 / 100) {
			lit = true;
		}
		if (sum > SWEET_CM_ONE * 16 / 10 || pos > 2 * SWEET_CM_ONE) {
			return "a row would clip (sum > 1.6 or positive terms > 2)";
		}
	}
	return lit ? NULL : "every row sum < 0.15 (nearly black)";
}

static uint32_t
sweet_cm_enc(int32_t c, uint32_t *clamped)
{
	if (c >= 0) {
		return (uint32_t)c;
	}
	switch (sweet_cm_st.neg) {
	case 1:
		return (uint32_t)c;                                     /* two's complement, sign-extended */
	case 2:
		return (1u << 17) | ((uint32_t)(-c) & 0x1ffffu);        /* sign-magnitude S2.15 */
	default:
		(*clamped)++;
		return 0;
	}
}

/* What PCC should hold now (fLock). False: bypass. */
static bool
sweet_cm_want_locked(int32_t c[9], const char **src)
{
	if (sweet_cm_st.testName != NULL) {
		memcpy(c, sweet_cm_st.test, sizeof(sweet_cm_st.test));
		*src = sweet_cm_st.testName;
		return true;
	}
	if ((sweet_cm & 1) && sweet_cm_st.have) {
		for (uint32_t i = 0; i < 9; i++) {
			c[i] = sweet_cm_to_pcc(sweet_cm_st.m[i]);
		}
		if (!sweet_cm_is_ident(c)) {
			*src = "SetMatrix";
			return true;
		}
	}
	memcpy(c, sweet_cm_ident, sizeof(sweet_cm_ident));
	*src = "bypass";
	return false;
}

/* Write DSPP0/1 PCC as it should be (fLock). False: nothing written (bypass and PCC never touched this boot). */
static bool
sweet_cm_write_locked(const char *how)
{
	int32_t c[9];
	const char *src;
	bool en = sweet_cm_want_locked(c, &src);
	sweet_cm_st.dirty = false;
	if (!en && !sweet_cm_st.touched) {
		return false;   /* never written: leave ABL's state alone */
	}
	uint32_t enc[9], clamped = 0;
	for (uint32_t i = 0; i < 9; i++) {
		enc[i] = sweet_cm_enc(c[i], &clamped);
	}
	for (uint32_t n = 0; n < 2; n++) {
		uint32_t b = MDP_DSPP(n) + DSPP_PCC;
		if (!en) {
			sweet_mdp_wr(b, 0);     /* PCC_EN off: bypass */
			continue;
		}
		for (uint32_t p = 0; p < 3; p++) {
			sweet_mdp_wr(b + 0x4 + 4 * p, 0);                      /* constant */
			for (uint32_t k = 0; k < 7; k++) {
				sweet_mdp_wr(b + 0x34 + 0xc * k + 4 * p, 0);   /* RG RB GB RGB RR GG BB */
			}
		}
		for (uint32_t o = 0; o < 3; o++) {
			for (uint32_t i = 0; i < 3; i++) {
				uint32_t off = sweet_cm_st.swap ? 0x10 + 4 * i + 0xc * o : 0x10 + 4 * o + 0xc * i;
				sweet_mdp_wr(b + off, enc[3 * o + i]);
			}
		}
		sweet_mdp_wr(b, 1);     /* PCC_EN */
	}
	sweet_cm_st.touched = true;
	sweet_cm_st.on = en;
	memcpy(sweet_cm_st.hw, c, sizeof(c));
	sweet_cm_st.nCommits++;
	sweet_cm_st.nNegClamped += clamped;
	if (((sweet_cm_trace & 1) || sweet_cm_st.testName != NULL || sweet_cm_st.nCommits <= 3) && sweet_cm_tr_take()) {
		char t[200];
		sweet_cm_fmt9p(t, sizeof(t), c);
		IOLog("SWEET CM: PCC %s (%s, %s): [%s] swap %u neg %u%s\n", en ? "applied" : "off", src, how, t,
		    sweet_cm_st.swap, sweet_cm_st.neg, clamped ? " (negatives written as 0)" : "");
	}
	return true;
}

/* From the direct-scanout flip (fLock), just before its CTL0 flush: the DSPP bits to OR into that write. */
static uint32_t
sweet_cm_flip_locked(void)
{
	if (__probable(!sweet_cm_st.dirty)) {
		return 0;
	}
	if (!sweet_cm_write_locked("flip")) {
		return 0;
	}
	sweet_cm_st.nMerged++;
	return MDP_FLUSH_DSPP01;
}

/* A commit with its own flush (fLock): only where no flip is in flight. Pending bits are written back with it. */
static void
sweet_cm_lone_locked(uint64_t now)
{
	if (!sweet_cm_write_locked("idle")) {
		return;
	}
	sweet_mdp_wr(MDP_CTL(0) + MDP_CTL_FLUSH, sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH) | MDP_FLUSH_DSPP01);
	sweet_cm_st.nLone++;
	sweet_cm_st.checkAbs = now;
}

/* The scanout thread's look (fLock): a change with no flip to carry it, and the read-back of a lone flush. */
static void
sweet_cm_idle_locked(uint64_t now)
{
	uint64_t ns;
	if (sweet_cm_st.dirty && sweet_scan.flipping == NULL) {
		absolutetime_to_nanoseconds(now - sweet_cm_st.dirtyAbs, &ns);
		if (sweet_scan.next == NULL || !sweet_scan.on || ns > 50 * NSEC_PER_MSEC) {
			sweet_cm_lone_locked(now);
		}
	}
	if (sweet_cm_st.checkAbs != 0) {
		absolutetime_to_nanoseconds(now - sweet_cm_st.checkAbs, &ns);
		if (ns > 40 * NSEC_PER_MSEC) {
			sweet_cm_st.checkAbs = 0;
			if (((sweet_cm_trace & 1) || sweet_cm_st.testName != NULL || sweet_cm_st.nLone <= 3) && sweet_cm_tr_take()) {
				uint32_t f = sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH);
				IOLog("SWEET CM: lone flush %u: ctl0 flush %x (DSPP bits %s after %llu ms), PCC en %x/%x\n",
				    sweet_cm_st.nLone, f, (f & MDP_FLUSH_DSPP01) ? "STILL PENDING" : "latched", ns / NSEC_PER_MSEC,
				    sweet_mdp_rd(MDP_DSPP(0) + DSPP_PCC), sweet_mdp_rd(MDP_DSPP(1) + DSPP_PCC));
			}
		}
	}
}

/* Something changed what PCC should hold (fLock). */
static void
sweet_cm_kick_locked(AppleCLCD *fb)
{
	sweet_cm_st.dirty = true;
	sweet_cm_st.dirtyAbs = mach_absolute_time();
	if (sweet_scan.thread) {
		IOLockWakeup(fb->fLock, &sweet_scan, false);
	} else {
		sweet_cm_lone_locked(sweet_cm_st.dirtyAbs);
	}
}

/* Read only: DSPP0/1 and CTL0 (no fLock needed; data ports are not read, they may auto-increment). */
static void
sweet_cm_dump(const char *why)
{
	for (uint32_t n = 0; n < 2; n++) {
		uint32_t b = MDP_DSPP(n), p = b + DSPP_PCC, g = b + DSPP_GC, nz = 0;
		for (uint32_t o = 0x34; o < 0x88; o += 4) {
			nz += sweet_mdp_rd(p + o) != 0;
		}
		IOLog("SWEET CM: dump (%s) DSPP%u @%#x: PCC en %x const %x %x %x | coef +10.. %x %x %x %x %x %x %x %x %x | "
		    "2nd-order non-zero %u | GC en %x idx %x %x %x swap %x | IGC op %x dither %x | PA op %x | gamut op %x\n",
		    why, n, b, sweet_mdp_rd(p), sweet_mdp_rd(p + 0x4), sweet_mdp_rd(p + 0x8), sweet_mdp_rd(p + 0xc),
		    sweet_mdp_rd(p + 0x10), sweet_mdp_rd(p + 0x14), sweet_mdp_rd(p + 0x18), sweet_mdp_rd(p + 0x1c),
		    sweet_mdp_rd(p + 0x20), sweet_mdp_rd(p + 0x24), sweet_mdp_rd(p + 0x28), sweet_mdp_rd(p + 0x2c),
		    sweet_mdp_rd(p + 0x30), nz, sweet_mdp_rd(g), sweet_mdp_rd(g + 0x8), sweet_mdp_rd(g + 0x10),
		    sweet_mdp_rd(g + 0x18), sweet_mdp_rd(g + 0x1c), sweet_mdp_rd(b), sweet_mdp_rd(b + 0x82c),
		    sweet_mdp_rd(b + 0x800), sweet_mdp_rd(b + 0x1000));
	}
	char t[160];
	size_t o = 0;
	for (uint32_t r = MDP_DSPP_TOP; r < MDP_DSPP_TOP + 0x80 && o + 16 < sizeof(t); r += 4) {
		uint32_t v = sweet_mdp_rd(r);
		if (v != 0) {
			o += (size_t)snprintf(t + o, sizeof(t) - o, " +%x=%x", r - MDP_DSPP_TOP, v);
		}
	}
	IOLog("SWEET CM: dump (%s) CTL0 layer0 %x layer1 %x top %x flush %x | DSPP top:%s\n", why,
	    sweet_mdp_rd(MDP_CTL(0) + 0x0), sweet_mdp_rd(MDP_CTL(0) + 0x4), sweet_mdp_rd(MDP_CTL(0) + 0x14),
	    sweet_mdp_rd(MDP_CTL(0) + MDP_CTL_FLUSH), o ? t : " (all zero)");
}

static void
sweet_cm_state(const char *why)
{
	char t[200];
	sweet_cm_fmt9p(t, sizeof(t), sweet_cm_st.hw);
	IOLog("SWEET CM: %s: kern.sweet_cm %u trace %u | PCC %s [%s] | swap %u neg %u | test %s | SetMatrix taken %u refused "
	    "%u%s | commits %u (at a flip %u, idle %u), negatives written as 0 %u | brightness corr %#x | gamma %u bytes\n",
	    why, sweet_cm, sweet_cm_trace, sweet_cm_st.on ? "on" : (sweet_cm_st.touched ? "off" : "untouched"), t,
	    sweet_cm_st.swap, sweet_cm_st.neg, sweet_cm_st.testName ? sweet_cm_st.testName : "off", sweet_cm_st.nAccepted,
	    sweet_cm_st.nRefused, sweet_cm_st.have ? "" : " (none yet)", sweet_cm_st.nCommits, sweet_cm_st.nMerged,
	    sweet_cm_st.nLone, sweet_cm_st.nNegClamped, sweet_cm_st.bc, sweet_cm_st.gammaSize);
}

/* sel 55 (externalMethod, no lock held): guard, store, apply. The status for the trace line. */
static const char *
sweet_cm_set_matrix(AppleCLCD *fb, uint32_t loc, const int64_t m[9])
{
	if (loc != 9) {
		return "ignored (location is not 9)";
	}
	int32_t c[9];
	for (uint32_t i = 0; i < 9; i++) {
		c[i] = sweet_cm_to_pcc(m[i]);
	}
	const char *why = sweet_cm_guard(c);
	IOLockLock(fb->fLock);
	if (why != NULL) {
		sweet_cm_st.nRefused++;
		IOLockUnlock(fb->fLock);
		if ((sweet_cm || sweet_cm_trace) && sweet_cm_tr_take()) {
			char t[240];
			sweet_cm_fmt9(t, sizeof(t), m, 32);
			IOLog("SWEET CM: SetMatrix refused (%s), the previous one stays: [%s]\n", why, t);
		}
		return why;
	}
	bool change = !sweet_cm_st.have || memcmp(sweet_cm_st.m, m, sizeof(sweet_cm_st.m)) != 0;
	memcpy(sweet_cm_st.m, m, sizeof(sweet_cm_st.m));
	sweet_cm_st.have = true;
	sweet_cm_st.nAccepted++;
	if (change && (sweet_cm & 1) && sweet_cm_st.testName == NULL) {
		sweet_cm_kick_locked(fb);
	}
	IOLockUnlock(fb->fLock);
	return (sweet_cm & 1) ? "taken" : "stored (kern.sweet_cm 0)";
}

/* sel 54 with kern.sweet_cm & 1: the stored matrix, identity before one / for another location */
static void
sweet_cm_get_matrix(AppleCLCD *fb, uint32_t loc, int64_t m[9], const char **what)
{
	IOLockLock(fb->fLock);
	if (loc == 9 && sweet_cm_st.have) {
		memcpy(m, sweet_cm_st.m, sizeof(sweet_cm_st.m));
		*what = "the stored matrix";
	} else {
		for (uint32_t i = 0; i < 9; i++) {
			m[i] = (i % 4 == 0) ? (1LL << 32) : 0;
		}
		*what = "identity";
	}
	IOLockUnlock(fb->fLock);
}

/* kern.sweet_cm_trace & 1, after the selector ran (outputs filled in) */
static void
sweet_cm_trace_sel(uint32_t selector, IOExternalMethodArguments *args, IOReturn ret, const char *note)
{
	char name[32];
	switch (selector) {
	case 17: case 19: case 27: case 50: case 51: case 54: case 55: case 57: case 68: case 69: case 84:
		break;
	default:
		return;
	}
	const uint8_t *in = args->structureInputDescriptor == NULL ? (const uint8_t *)args->structureInput : NULL;
	uint32_t inSize = args->structureInputSize;
	if (selector == 55 && in != NULL && inSize >= 72) {
		int64_t m[9];
		uint32_t loc = args->scalarInputCount ? (uint32_t)args->scalarInput[0] : 0xffffffffu;
		memcpy(m, in, sizeof(m));
		if (sweet_cm_st.seenAny && loc == sweet_cm_st.seenLoc && memcmp(m, sweet_cm_st.seen, sizeof(m)) == 0) {
			return;         /* each new matrix only */
		}
		memcpy(sweet_cm_st.seen, m, sizeof(m));
		sweet_cm_st.seenLoc = loc;
		sweet_cm_st.seenAny = true;
		if (!sweet_cm_tr_take()) {
			return;
		}
		char t[240];
		sweet_cm_fmt9(t, sizeof(t), m, 32);
		proc_selfname(name, sizeof(name));
		IOLog("SWEET CM: %s SetMatrix loc %u [%s] -> 0x%x %s\n", name, loc, t, ret, note ? note : "");
		return;
	}
	if (!sweet_cm_tr_take()) {
		return;
	}
	proc_selfname(name, sizeof(name));
	if ((selector == 17 && in != NULL) || selector == 27) {
		const uint8_t *tb = selector == 17 ? in : (args->structureOutputDescriptor == NULL ?
		    (const uint8_t *)args->structureOutput : NULL);
		uint32_t sz = selector == 17 ? inSize : args->structureOutputSize, sum = 0, e[9] = { 0 };
		if (tb != NULL) {
			for (uint32_t i = 0; i + 4 <= sz; i += 4) {
				uint32_t w;
				memcpy(&w, tb + i, 4);
				sum = (sum << 1 | sum >> 31) ^ w;
			}
			for (uint32_t ch = 0; ch < 3; ch++) {
				static const uint32_t at[3] = { 0, 128, 256 };
				for (uint32_t k = 0; k < 3; k++) {
					uint32_t off = (ch * 257 + at[k]) * 4;
					if (off + 4 <= sz) {
						memcpy(&e[ch * 3 + k], tb + off, 4);
					}
				}
			}
		}
		IOLog("SWEET CM: %s %s %u bytes, checksum %08x, r[0,128,256] %x %x %x g %x %x %x b %x %x %x -> 0x%x %s\n", name,
		    selector == 17 ? "SetGammaTable" : "GetGammaTable out", sz, sum, e[0], e[1], e[2], e[3], e[4], e[5], e[6],
		    e[7], e[8], ret, note ? note : "");
		return;
	}
	if (selector == 50 && args->scalarInputCount >= 1) {
		char d[24];
		sweet_cm_dec(d, sizeof(d), (int64_t)(uint32_t)args->scalarInput[0], 16);
		IOLog("SWEET CM: %s SetBrightnessCorrection %#llx (%s) -> 0x%x (stored only)\n", name, args->scalarInput[0], d, ret);
		return;
	}
	if (selector == 54) {
		char t[240] = "";
		if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL && args->structureOutputSize >= 72) {
			int64_t m[9];
			memcpy(m, args->structureOutput, sizeof(m));
			sweet_cm_fmt9(t, sizeof(t), m, 32);
		}
		IOLog("SWEET CM: %s GetMatrix loc %llu out %u: [%s] -> 0x%x %s\n", name,
		    args->scalarInputCount ? args->scalarInput[0] : 0ULL, args->structureOutputSize, t, ret, note ? note : "");
		return;
	}
	uint64_t w = 0;
	if (in != NULL && inSize > 0) {
		memcpy(&w, in, inSize < 8 ? inSize : 8);
	}
	IOLog("SWEET CM: %s sel %u in %u [%#llx %#llx] struct %u [%#llx] out %u/%u [%#llx] -> 0x%x\n", name, selector,
	    args->scalarInputCount, args->scalarInputCount ? args->scalarInput[0] : 0ULL,
	    args->scalarInputCount > 1 ? args->scalarInput[1] : 0ULL, inSize, w, args->scalarOutputCount,
	    args->structureOutputSize, args->scalarOutputCount ? args->scalarOutput[0] : 0ULL, ret);
}

/* From the sysctl handlers (bsd/dev/sweet_gpu.c, no lock held): 1 kern.sweet_cm changed, 2 dump. */
void
sweet_cm_ctl(uint32_t what)
{
	AppleCLCD *fb = sweet_fb_instance;
	if (what == 2) {
		sweet_cm_dump("kern.sweet_cm_trace");
		return;
	}
	if (fb == NULL) {
		return;
	}
	IOLockLock(fb->fLock);
	sweet_cm_kick_locked(fb);
	IOLockUnlock(fb->fLock);
	sweet_cm_state("kern.sweet_cm");
}

/* p cm ... (fbCmd, no lock held) */
static void
sweet_cm_cmd(AppleCLCD *fb, const char *q, const char *end)
{
	static const struct { const char *name; int32_t c[9]; } tests[] = {
		{ "r",    { SWEET_CM_ONE, 0, 0, 0, 0, 0, 0, 0, 0 } },
		{ "g",    { 0, 0, 0, 0, SWEET_CM_ONE, 0, 0, 0, 0 } },
		{ "b",    { 0, 0, 0, 0, 0, 0, 0, 0, SWEET_CM_ONE } },
		{ "warm", { SWEET_CM_ONE, 0, 0, 0, 0x63d7, 0, 0, 0, 0x4666 } },        /* 1, 0.78, 0.55 */
		{ "id",   { SWEET_CM_ONE, 0, 0, 0, SWEET_CM_ONE, 0, 0, 0, SWEET_CM_ONE } },
		{ "rg",   { 0, SWEET_CM_ONE, 0, 0, SWEET_CM_ONE, 0, 0, 0, SWEET_CM_ONE } },
		{ "neg",  { SWEET_CM_ONE, -0x4000, 0, 0, SWEET_CM_ONE, 0, 0, 0, SWEET_CM_ONE } },
	};
	while (q < end && *q == ' ') {
		q++;
	}
	const char *w = q;
	while (q < end && *q != ' ') {
		q++;
	}
	size_t wl = (size_t)(q - w);
	while (q < end && *q == ' ') {
		q++;
	}
	const char *a = q;
	while (q < end && *q != ' ') {
		q++;
	}
	size_t al = (size_t)(q - a);
	if (wl == 4 && !strncmp(w, "dump", 4)) {
		sweet_cm_dump("p cm dump");
		return;
	}
	if (wl == 0) {
		sweet_cm_state("p cm");
		return;
	}
	IOLockLock(fb->fLock);
	if (wl == 4 && !strncmp(w, "test", 4)) {
		if (al == 3 && !strncmp(a, "off", 3)) {
			sweet_cm_st.testName = NULL;
		} else {
			uint32_t i;
			for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
				if (strlen(tests[i].name) == al && !strncmp(tests[i].name, a, al)) {
					break;
				}
			}
			if (i == sizeof(tests) / sizeof(tests[0])) {
				IOLockUnlock(fb->fLock);
				IOLog("SWEET CM: p cm test r|g|b|warm|id|rg|neg|off\n");
				return;
			}
			const char *why = sweet_cm_guard(tests[i].c);
			if (why != NULL) {
				IOLockUnlock(fb->fLock);
				IOLog("SWEET CM: test %s refused by the guard (%s)\n", tests[i].name, why);
				return;
			}
			memcpy(sweet_cm_st.test, tests[i].c, sizeof(sweet_cm_st.test));
			sweet_cm_st.testName = tests[i].name;
		}
	} else if (wl == 4 && !strncmp(w, "swap", 4)) {
		sweet_cm_st.swap = al ? *a == '1' : !sweet_cm_st.swap;
	} else if (wl == 3 && !strncmp(w, "neg", 3)) {
		sweet_cm_st.neg = (al && *a >= '0' && *a <= '2') ? (uint32_t)(*a - '0') : 0;
	} else {
		IOLockUnlock(fb->fLock);
		IOLog("SWEET CM: p cm [dump | test r|g|b|warm|id|rg|neg|off | swap [0|1] | neg 0|1|2]\n");
		return;
	}
	sweet_cm_kick_locked(fb);
	IOLockUnlock(fb->fLock);
	sweet_cm_state("p cm");
}

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
			if (s->ubwcLive) {
				sweet_mdp_scan_addr(s);         /* 1111: the compressed planes and format */
			}
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

static uint32_t sweet_scan_surf_busy_locked(uint32_t id);      /* 1105 */

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
	/*
	 * 1109: the surface of this swap still held by the display (on screen, flipping or queued) means CA drew into a held
	 * surface (a tear, unless the gate held its GPU work); the same surface as the last swap (a re-swap of an unchanged
	 * buffer) is counted apart. Line SCANOUT 5s tribuf: held / repeat.
	 */
	if (s->id == sweet_scan.lastSurf) {
		sweet_scan.stRepeatSwap++;
	} else if (sweet_scan_surf_busy_locked(s->id) != 0) {
		sweet_scan.stHeldSwap++;
	}
	sweet_scan.lastSurf = s->id;
	if (sweet_scan.tribuf && !sweet_scan.pollRel && sweet_scan.gateDepth > sweet_scan.depth) {
		/* 0996: drawn into a surface the relaxed gate did not wait for. 1110: not with tribuf 2, where CA reuses a
		 * surface only after its swap is off screen (a prediction miss, no hazard; "swapped held" counts real ones) */
		sweet_scan.stTbShort++;
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
	if (sweet_scan.next != NULL && (sweet_scan.gate == 2 || sweet_scan.queue) && sweet_scan.laterN < 3) {
		/* 0991: queued behind the waiting one, not dropped; 1110: also with kern.sweet_scangate_queue 1 (any gate) */
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
		sweet_tt_swap(sweet_scan.later[i].swap, now);   /* 1107 */
		if (__improbable(sweet_pace_trace)) {
			sweet_pt_swap(sweet_scan.later[i].swap, now, fence, (sweet_scan.flipping != NULL) + 1 + i);   /* 1113 */
		}
		IOLockWakeup(fb->fLock, &sweet_scan, false);
		IOLockUnlock(fb->fLock);
		return true;
	}
	if (sweet_scan.next != NULL) {
		sweet_scan.stDropped++;         /* a newer frame before the last one was flipped */
		SWEET_TRACE(SWT_SCAN_SWAP, sweet_scan.nextSwap, sweet_scan.nextSeq, 2, 0);    /* 0994: this one is replaced */
		if (sweet_scan.gate == 2 || sweet_scan.queue) {
			sweet_scan.stOverflow++;        /* 0991: never shown: its CP waiters go on; 1110: queue full */
			sweet_scan_word_locked((uint32_t)sweet_scan.nextSeq);
		}
		sweet_scan_drop(&sweet_scan.next);
		sweet_scan_pop_locked();
		IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);  /* 1105: a gate 3 wait on the dropped surface goes on */
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
	sweet_tt_swap(sweet_scan.nextSwap, now);        /* 1107 */
	if (__improbable(sweet_pace_trace)) {
		sweet_pt_swap(sweet_scan.nextSwap, now, fence, sweet_scan.flipping != NULL);  /* 1113 */
	}
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
/*
 * 1105: with fLock. Is surface id held by the display: 1 on screen (shown), 2 flipping or queued for a latch (next,
 * later[]), 0 not held (CA may draw into it: the scanout reads it no more and will not before its next SwapEnd).
 */
static uint32_t
sweet_scan_surf_busy_locked(uint32_t id)
{
	if (sweet_scan.shown != NULL && sweet_scan.shown->id == id) {
		return 1;
	}
	if ((sweet_scan.flipping != NULL && sweet_scan.flipping->id == id) || (sweet_scan.next != NULL && sweet_scan.next->id == id)) {
		return 2;
	}
	for (uint32_t i = 0; i < sweet_scan.laterN; i++) {
		if (sweet_scan.later[i].s != NULL && sweet_scan.later[i].s->id == id) {
			return 2;
		}
	}
	return 0;
}

/*
 * 1104: gf (sweet_gpu.c, kern.sweet_scangate_flag): bit 0 the submission writes the display surface, bit 1 its process
 * flags its display submissions. A bare submission (bit 1 without bit 0) of the swapper is not gated: offscreen passes,
 * compute, pre-buffers. Counted in the 5 s gate line (flag display / bare / unflagged).
 */
extern "C" uint32_t sweet_scan_submit_gate(uint32_t gf);
uint32_t
sweet_scan_submit_gate(uint32_t gf)
{
	AppleCLCD *fb = sweet_fb_instance;
	if (!sweet_scan.on || !sweet_scan.geom || sweet_scan.gate == 0 || sweet_scan.gate == 4 || fb == NULL ||
	    proc_selfpid() != sweet_scan.gatePid) {        /* 1109: gate 4 = none (CA's own ring keeps it off held surfaces) */
		return 0;
	}
	if ((gf & 3u) == 2u) {
		OSIncrementAtomic((volatile SInt32 *)&sweet_scan.stFlagBare);     /* 1104: not the display: never gated */
		return 0;
	}
	OSIncrementAtomic((volatile SInt32 *)((gf & 1u) ? &sweet_scan.stFlagDisp : &sweet_scan.stUnflagged));
	IOLockLock(fb->fLock);
	sweet_scan.stGate++;
	SWEET_TRACE(SWT_GPU_GATE, sweet_scan.gate, sweet_scan.gateSeq, sweet_scan.shownSeq, sweet_scan.word);      /* 0994 */
	uint32_t sid = (gf & 1u) ? gf >> 8 : 0;
	if (sweet_scan.gate == 3 && sid != 0) {
		/*
		 * 1105: gate 3, a display submit that names its surface: held only while that surface is on screen or queued
		 * for a latch (50 ms at most), not until the last swap is on screen. With CA's 2-surface chain the surface of
		 * frame K+1 is K-1's, on screen until K latches: the same wait as gate 1. With 3 surfaces (tribuf 1) it is
		 * K-2's, free once K-1 latched: K+1 renders while K waits for its latch.
		 */
		sweet_scan.st3Id++;
		uint32_t b = sweet_scan_surf_busy_locked(sid);
		if (b != 0) {
			uint64_t dl, t0 = mach_absolute_time();
			clock_interval_to_deadline(50, kMillisecondScale, &dl);
			if (b == 1) {
				sweet_scan.st3Shown++;
			} else {
				sweet_scan.st3Pending++;
			}
			SWEET_TRACE_START(SWT_GPU_GATE, proc_selfpid(), sid, sweet_scan.shownSeq, 3);
			while (sweet_scan.on && sweet_scan.geom && sweet_scan.gate == 3 && sweet_scan_surf_busy_locked(sid) != 0) {
				if (IOLockSleepDeadline(fb->fLock, &sweet_scan.shownSeq, dl, THREAD_UNINT) == THREAD_TIMED_OUT) {
					sweet_scan.st3Timeout++;
					break;
				}
			}
			sweet_scan.st3Abs += mach_absolute_time() - t0;
			SWEET_TRACE_END(SWT_GPU_GATE, proc_selfpid(), sid, sweet_scan.shownSeq, 3);
		} else {
			sweet_scan.st3Free++;
		}
		if ((sweet_scan.next != NULL || sweet_scan.flipping != NULL) && sweet_scan.seq != sweet_scan.st3OvSeq) {
			sweet_scan.st3OvSeq = sweet_scan.seq;   /* the newest swap is not latched yet and its successor draws */
			sweet_scan.st3Overlap++;
		}
		IOLockUnlock(fb->fLock);
		return 0;
	}
	if (sweet_scan.gate == 3) {
		sweet_scan.st3NoId++;   /* 1105: no surface id: gated as 1 below */
	}
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
		while (sweet_scan.on && sweet_scan.geom && (sweet_scan.gate == 1 || sweet_scan.gate == 3) &&
		    sweet_scan.shownSeq < sweet_scan.gateSeq) {     /* 1105: 3 without a surface id waits as 1 */
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
	if (sweet_scan.pollRel && sweet_scan.geom && sweet_scan.shown != NULL && sweet_scan.shownSwap == id) {
		/*
		 * 1109 (tribuf 2): swap id is the one on screen: its surface is still scanned, so it is not complete. CA's
		 * can_update_ frees the ring page (and its buffer) of a swap only when this answers 0, and allocate_buffer takes a
		 * new buffer when every buffer belongs to an unfreed page: with K on its way and K-1 on screen CA draws K+1 into
		 * a third surface. Answered 0 once a later swap has latched (no time cap: the next latch or a leave ends it).
		 */
		sweet_scan.stRelShown++;
		sweet_scan.stTbBusy++;
		IOLockUnlock(fb->fLock);
		return kIOReturnBusy;
	}
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

/*
 * 1044: direct-scanout smoothness (always on, a few counters per latch; fLock): latch-to-latch intervals in panel
 * periods (1/2/3/4+), p50/p95/max ms, jitter = mean |interval - previous interval|, repeats = intervals of >= 3
 * periods (one frame held >= 3 vsyncs); gaps over 100 ms are pauses (counted, not in the statistics). Touch -> latch:
 * the newest touch HID event (SweetHID, IRQ time) before the swap's SwapEnd -> that swap's latch, when < 200 ms.
 * Second line of the 5 s report: SWEET SCANOUT: 5s frames: ...
 */
extern "C" volatile uint64_t sweet_hid_touch_abs[2];   /* iokit/Kernel/arm/hid/SweetHID.cpp: newest, previous */

/*
 * 1107: kern.sweet_touch_trace (default 1; bsd/dev/sweet_gpu.c): touch -> latch in stages. One touch event at a time is
 * followed (the next is taken once it has latched, or after 200 ms: abandoned): its time (the sample's IRQ time, SweetHID)
 * -> its HID event dispatched (SweetHID::touch, dispatchEvent returned: queued for backboardd) -> the swapper's first GPU
 * submit after that (sweet_gpu_submit_v2 entry, before the scanout gate) -> the first direct SwapEnd after that submit ->
 * that swap's (or a later one's) latch. Also per vsync notification: -> the swapper's first submit, -> its first SwapEnd
 * (CA's tick-to-work timing). 0.2 ms bins to 200 ms (50 ms for the vsync pair); p50/p95 in the 5 s line SWEET TOUCH.
 * No locks taken: a try-lock word; a busy one drops the sample. Cost: a few loads and stores per event/submit/swap/latch.
 */
extern "C" uint32_t sweet_touch_trace;
uint32_t sweet_touch_trace = 1;
#define SWEET_TT_BINS   1000            /* 0.2 ms */
static struct {
	volatile SInt32 busy;
	uint32_t state;                 /* 0 free, 1 dispatched, 2 submitted, 3 swapped */
	uint64_t irq, hid, sub, swap;
	uint32_t swapId, n, abandoned;
	uint16_t h[5][SWEET_TT_BINS];   /* irq->hid, hid->submit, submit->swapend, swapend->latch, total */
	volatile uint64_t tick;
	volatile bool tickSub, tickSwap;
	uint16_t tv[2][250];            /* vsync -> submit, vsync -> SwapEnd */
	uint32_t tvn[2];
} sweet_tt;

static bool
sweet_tt_take(void)
{
	return sweet_touch_trace != 0 && OSCompareAndSwap(0, 1, (volatile UInt32 *)&sweet_tt.busy);
}

static void
sweet_tt_give(void)
{
	OSCompareAndSwap(1, 0, (volatile UInt32 *)&sweet_tt.busy);
}

static void
sweet_tt_bin(uint16_t *h, uint32_t bins, uint64_t abs)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abs, &ns);
	uint64_t b = ns / 200000;
	if (b < bins && h[b] != 0xffff) {
		h[b]++;
	}
}

static void
sweet_tt_tick(uint64_t now)
{
	sweet_tt.tick = now;
	sweet_tt.tickSub = false;
	sweet_tt.tickSwap = false;
}

extern "C" void sweet_touch_trace_event(uint64_t irq, uint64_t hid);
void
sweet_touch_trace_event(uint64_t irq, uint64_t hid)
{
	if (!sweet_tt_take()) {
		return;
	}
	if (sweet_tt.state != 0) {
		uint64_t lim;
		nanoseconds_to_absolutetime(200 * NSEC_PER_MSEC, &lim);
		if (hid > sweet_tt.hid && hid - sweet_tt.hid > lim) {
			sweet_tt.abandoned++;
			sweet_tt.state = 0;
		}
	}
	if (sweet_tt.state == 0 && irq != 0 && hid >= irq) {
		sweet_tt.irq = irq;
		sweet_tt.hid = hid;
		sweet_tt.state = 1;
	}
	sweet_tt_give();
}

extern "C" void sweet_touch_trace_submit(int pid, uint64_t now);
void
sweet_touch_trace_submit(int pid, uint64_t now)
{
	if (pid != sweet_scan.gatePid || pid == 0 || sweet_touch_trace == 0) {
		return;
	}
	if (!sweet_tt.tickSub && sweet_tt.tick != 0 && now > sweet_tt.tick) {
		sweet_tt.tickSub = true;
		if (sweet_tt_take()) {
			sweet_tt_bin(sweet_tt.tv[0], 250, now - sweet_tt.tick);
			sweet_tt.tvn[0]++;
			sweet_tt_give();
		}
	}
	if (sweet_tt.state == 1 && sweet_tt_take()) {
		if (sweet_tt.state == 1 && now >= sweet_tt.hid) {
			sweet_tt.sub = now;
			sweet_tt.state = 2;
		}
		sweet_tt_give();
	}
}

static void
sweet_tt_swap(uint32_t id, uint64_t now)
{
	if (sweet_touch_trace == 0) {
		return;
	}
	if (!sweet_tt.tickSwap && sweet_tt.tick != 0 && now > sweet_tt.tick) {
		sweet_tt.tickSwap = true;
		if (sweet_tt_take()) {
			sweet_tt_bin(sweet_tt.tv[1], 250, now - sweet_tt.tick);
			sweet_tt.tvn[1]++;
			sweet_tt_give();
		}
	}
	if (sweet_tt.state == 2 && sweet_tt_take()) {
		if (sweet_tt.state == 2 && now >= sweet_tt.sub) {
			sweet_tt.swap = now;
			sweet_tt.swapId = id;
			sweet_tt.state = 3;
		}
		sweet_tt_give();
	}
}

static void
sweet_tt_latch(uint32_t swapId, uint64_t now)
{
	if (sweet_tt.state != 3 || (int32_t)(swapId - sweet_tt.swapId) < 0 || !sweet_tt_take()) {
		return;
	}
	if (sweet_tt.state == 3 && (int32_t)(swapId - sweet_tt.swapId) >= 0 && now >= sweet_tt.swap) {
		sweet_tt_bin(sweet_tt.h[0], SWEET_TT_BINS, sweet_tt.hid - sweet_tt.irq);
		sweet_tt_bin(sweet_tt.h[1], SWEET_TT_BINS, sweet_tt.sub - sweet_tt.hid);
		sweet_tt_bin(sweet_tt.h[2], SWEET_TT_BINS, sweet_tt.swap - sweet_tt.sub);
		sweet_tt_bin(sweet_tt.h[3], SWEET_TT_BINS, now - sweet_tt.swap);
		sweet_tt_bin(sweet_tt.h[4], SWEET_TT_BINS, now - sweet_tt.irq);
		sweet_tt.n++;
		sweet_tt.state = 0;
	}
	sweet_tt_give();
}

static uint32_t
sweet_tt_pct(const uint16_t *h, uint32_t bins, uint32_t pct)
{
	uint32_t n = 0, acc = 0;
	for (uint32_t i = 0; i < bins; i++) {
		n += h[i];
	}
	uint32_t want = (n * pct + 99) / 100;
	for (uint32_t i = 0; n != 0 && i < bins; i++) {
		acc += h[i];
		if (acc >= want) {
			return 2 * i;   /* in 0.1 ms */
		}
	}
	return 0;
}

static void
sweet_tt_line(void)
{
	if (sweet_touch_trace == 0 || !sweet_tt_take()) {
		return;
	}
	uint32_t p[5][2], v[2][2];
	for (int k = 0; k < 5; k++) {
		p[k][0] = sweet_tt_pct(sweet_tt.h[k], SWEET_TT_BINS, 50);
		p[k][1] = sweet_tt_pct(sweet_tt.h[k], SWEET_TT_BINS, 95);
	}
	for (int k = 0; k < 2; k++) {
		v[k][0] = sweet_tt_pct(sweet_tt.tv[k], 250, 50);
		v[k][1] = sweet_tt_pct(sweet_tt.tv[k], 250, 95);
	}
	uint32_t n = sweet_tt.n, ab = sweet_tt.abandoned, tn0 = sweet_tt.tvn[0], tn1 = sweet_tt.tvn[1];
	bzero(sweet_tt.h, sizeof(sweet_tt.h));
	bzero(sweet_tt.tv, sizeof(sweet_tt.tv));
	sweet_tt.n = sweet_tt.abandoned = sweet_tt.tvn[0] = sweet_tt.tvn[1] = 0;
	sweet_tt_give();
	if (n == 0 && ab == 0 && tn0 == 0) {
		return;
	}
#define TT2(a) (a)[0] / 10, (a)[0] % 10, (a)[1] / 10, (a)[1] % 10
	IOLog("SWEET TOUCH: 5s stages n %u abandoned %u | p50/p95 ms: irq->hid %u.%u/%u.%u | hid->submit %u.%u/%u.%u | "
	    "submit->swapend %u.%u/%u.%u | swapend->latch %u.%u/%u.%u | total %u.%u/%u.%u | vsync->submit n %u %u.%u/%u.%u | "
	    "vsync->swapend n %u %u.%u/%u.%u\n", n, ab, TT2(p[0]), TT2(p[1]), TT2(p[2]), TT2(p[3]), TT2(p[4]), tn0, TT2(v[0]),
	    tn1, TT2(v[1]));
#undef TT2
}
/*
 * 1113: kern.sweet_pace_trace (default 0; bsd/dev/sweet_gpu.c): where a presented frame loses its time. Per CA swap id
 * (ring of 8): SwapBegin (IOMFB user client entry), the vsync tick before it, the swapper's first GPU submit and its last
 * display submit since the previous SwapEnd (entry, and return from the scanout gate = release), SwapEnd (+ its fence and
 * the frames still ahead of it: flipping / next / queued), GPU done (first time the kernel saw the fence landed: the
 * scanout thread's 1 ms looks / GPU interrupt, or SwapEnd if it had landed by then), flip (programmed into the pipes:
 * after the queue/previous latch released it), latch. Vsyncs per frame from latch to latch (as SCANOUT 5s frames).
 * Line every 5 s: SWEET PACE: 5s ... with mean/p95 of each stage for 3+ vsync frames and the mean for all frames, plus
 * counts of what the slow frames coincide with. 2 = also one SWEET PACE: slow line per 3+ vsync frame (16 per 5 s,
 * kept in a buffer and printed with the 5 s line, never from the latch path). No locks: a try-lock word as 1107; the
 * vsync tick is 1107's sweet_tt.tick (stored on every tick already). 0 = one load per hook.
 */
extern "C" uint32_t sweet_pace_trace;
uint32_t sweet_pace_trace = 0;
#define SWEET_PT_BINS   256             /* 0.5 ms bins to 128 ms */
#define SWEET_PT_ST     8               /* idle, swap->sub, gate, sub->gpudone, gpudone->latch, wait (done->flip), flip->latch, swap->swapend */
#define SWEET_PT_SLOW   16
/*
 * 1114: kern.sweet_pace_trace & 4 (with 1 or 2; default 0): k4-1115 found a third to a half of the "slow" (3+ vsync)
 * frames of the home-screen phases to be single frames of ~40 ms CA bursts every ~89 ms outside any touch (GPU < 1 ms,
 * 8 vsyncs: the idle cadence, all of the idle phase), and the in-motion slow frames to start 1-3 ticks after the
 * previous latch with nothing ahead. Per frame, kept in a ring (SWEET_PT_RING, 3+ vsync frames; & 8 every frame) and
 * printed only on demand (kern.sweet_pace_dump=1, from the sysctl's thread, never from the latch or tick path), each
 * line with its latch time:
 *   ticks between this frame's SwapBegin and the previous one, each classed at the next tick: swap (a SwapBegin came),
 *   busy (CA polled SwapWait / can_update_ and every answer was Busy), none (no poll, no swap: nothing to draw or the
 *   thread did not run), ok (polled, got done, no swap); pend: ticks that found the newest ended swap not latched yet
 *   (its completion could not have reached CA); off: ticks while CA had the notifications off
 *   tick -> SwapBegin; tick -> the swapping thread's run start (on-core; "pre" = running since before the tick), that
 *   run's runnable wait, its CPU and class (1 CA display thread, 2 CA render-server thread)
 *   cumulative per class (CA display, CA render-server, SpringBoard's main thread) since the previous SwapBegin: on-CPU
 *   and runnable-waiting ms on big (CPU6/7) / little cores (osfmk/kern/sched_prim.c, counted when a run ends)
 *   touch: down at SwapBegin, ms since the last lift and the last event, events since the previous SwapBegin
 * 5 s line "SWEET PACE: 5s ticks ...": the tick classes, display-thread wake (tick -> on core) p50/p95, touch events
 * and the move interval p5/p50/p95/max (synthetic finger 9 and real), ring fill / lost.
 * No new locks: the 1113 try-lock word; a hook that finds it taken drops its update (counted: "missed").
 */
#define SWEET_PT_RING   2048
#define SWEET_PT_MIN(a, b)      ((a) < (b) ? (a) : (b))
struct sweet_pt_x {
	uint32_t tickBegin, onCore, curWait;            /* us; onCore 0xffffffff = running since before the tick */
	uint32_t gap[3][4];                             /* us: [display, rs, sb][run big, run little, wait big, wait little] */
	uint16_t ticks, tBusy, tNone, tOk, tPend, tOff, polls, pollsBusy, events, upAgo, evAgo;
	uint8_t cpu, cls, down, pendAtTick;
};
struct sweet_pt_rec {
	uint64_t latch;
	uint32_t id, st[SWEET_PT_ST];
	uint8_t k, ahead, nsub, pad;
	struct sweet_pt_x x;
};
extern "C" {
extern volatile uint64_t sweet_pace_tick_abs;  /* osfmk/kern/sched_prim.c (1114) */
void sweet_pace_cur_thread(uint64_t *epoch, uint64_t *made, uint32_t *cpu, uint32_t *cls);
void sweet_pace_acc_read(uint64_t out[15]);
void sweet_pace_wake_read(uint64_t *tick, uint64_t *on, uint32_t *cpu);
}
static struct {
	volatile SInt32 busy;
	/* 1114: the tick being classed, gap counters since the last SwapBegin, 5 s counters, touch state, ring */
	uint64_t tAbs;
	uint32_t tPolls, tDone;
	bool tBegan, tPend, tOn;
	uint32_t gTicks, gBusy, gNone, gOk, gPend, gOff, gPolls, gPollsBusy, gEvents;
	uint64_t accPrev[15];
	bool accInit;
	uint32_t xTicks, xSwap, xBusy, xNone, xOk, xPend, xOff, xWakeN, xWakeLittle, xMissed;
	uint16_t xWake[250];                            /* 0.1 ms bins */
	uint32_t tDownMask, tEvents, tSynth, tMoves, tGaps;
	uint64_t tUpAbs, tEvAbs, tPrevAbs;
	uint16_t tDt[256];                              /* 0.25 ms bins */
	uint64_t ringW, ringR, ringLost;
	struct sweet_pt_rec ring[SWEET_PT_RING];
	struct {
		uint32_t id, fence, ahead;
		uint64_t begin, tick, sub0, subD, rel, end, done, flip;
		struct sweet_pt_x x;    /* 1114 (kern.sweet_pace_trace & 4) */
	} f[8];
	uint64_t pSub0, pSubD, pRel;    /* the swapper's submits since its last SwapEnd */
	uint32_t pSubN;
	uint64_t lastLatch, lastLatchTick, prevGpu;
	/* window */
	uint32_t n, per[4], pauses, unmatched, doneAtEnd, subBeforeBegin, noBegin;
	uint64_t sumAll[SWEET_PT_ST], sumSlow[SWEET_PT_ST];
	uint32_t nAll, nSlow;
	uint16_t h[SWEET_PT_ST][SWEET_PT_BINS];
	uint32_t cIdle, cGpu, cPrevGpu, cAhead, cWait, cGate;   /* slow frames: idle > 1 vsync, GPU > 1 vsync, ... */
	uint32_t subsAll;
	struct { uint32_t id, k, ahead, phase, prevGpu, nsub; uint32_t st[SWEET_PT_ST]; } slow[SWEET_PT_SLOW];
	uint32_t slowN, slowLost;
} sweet_pt;

static bool
sweet_pt_take(void)
{
	return OSCompareAndSwap(0, 1, (volatile UInt32 *)&sweet_pt.busy);
}

static void
sweet_pt_give(void)
{
	OSCompareAndSwap(1, 0, (volatile UInt32 *)&sweet_pt.busy);
}

static uint32_t
sweet_pt_us(uint64_t a, uint64_t b)    /* b - a in us, 0 if either is unset or b < a */
{
	if (a == 0 || b == 0 || b <= a) {
		return 0;
	}
	uint64_t ns;
	absolutetime_to_nanoseconds(b - a, &ns);
	return ns / 1000 > 0xffffffffULL ? 0xffffffffu : (uint32_t)(ns / 1000);
}

/* SwapBegin (any process: only the swapper swaps) */
static void
sweet_pt_begin(uint32_t id, uint64_t now)
{
	if (!sweet_pt_take()) {
		return;
	}
	bzero(&sweet_pt.f[id & 7], sizeof(sweet_pt.f[0]));
	sweet_pt.f[id & 7].id = id;
	sweet_pt.f[id & 7].begin = now;
	sweet_pt.f[id & 7].tick = sweet_tt.tick;
	if (sweet_pace_trace & 4) {
		/* 1114 */
		auto *x = &sweet_pt.f[id & 7].x;
		uint64_t t = sweet_pt.tAbs, ep = 0, mr = 0, a[15];
		uint32_t cpu = 0, cls = 0;
		sweet_pace_cur_thread(&ep, &mr, &cpu, &cls);
		x->tickBegin = sweet_pt_us(t, now);
		x->onCore = t != 0 && ep >= t ? sweet_pt_us(t, ep) : 0xffffffffu;
		x->curWait = mr != ~0ULL ? sweet_pt_us(mr, ep) : 0;
		x->cpu = (uint8_t)cpu;
		x->cls = (uint8_t)cls;
		x->pendAtTick = sweet_pt.tPend;
		x->ticks = (uint16_t)SWEET_PT_MIN(sweet_pt.gTicks, 0xffff);
		x->tBusy = (uint16_t)SWEET_PT_MIN(sweet_pt.gBusy, 0xffff);
		x->tNone = (uint16_t)SWEET_PT_MIN(sweet_pt.gNone, 0xffff);
		x->tOk = (uint16_t)SWEET_PT_MIN(sweet_pt.gOk, 0xffff);
		x->tPend = (uint16_t)SWEET_PT_MIN(sweet_pt.gPend, 0xffff);
		x->tOff = (uint16_t)SWEET_PT_MIN(sweet_pt.gOff, 0xffff);
		x->polls = (uint16_t)SWEET_PT_MIN(sweet_pt.gPolls, 0xffff);
		x->pollsBusy = (uint16_t)SWEET_PT_MIN(sweet_pt.gPollsBusy, 0xffff);
		x->events = (uint16_t)SWEET_PT_MIN(sweet_pt.gEvents, 0xffff);
		sweet_pt.gTicks = sweet_pt.gBusy = sweet_pt.gNone = sweet_pt.gOk = sweet_pt.gPend = sweet_pt.gOff = 0;
		sweet_pt.gPolls = sweet_pt.gPollsBusy = sweet_pt.gEvents = 0;
		sweet_pt.tBegan = true;
		sweet_pace_acc_read(a);
		if (sweet_pt.accInit) {
			for (int c = 0; c < 3; c++) {
				for (int j = 0; j < 4; j++) {
					uint64_t d = a[c * 5 + j] - sweet_pt.accPrev[c * 5 + j];
					x->gap[c][j] = sweet_pt_us(1, 1 + d);
				}
			}
		}
		memcpy(sweet_pt.accPrev, a, sizeof(a));
		sweet_pt.accInit = true;
		uint32_t ms;
		x->down = sweet_pt.tDownMask != 0;
		ms = sweet_pt_us(sweet_pt.tUpAbs, now) / 1000;
		x->upAgo = sweet_pt.tUpAbs == 0 ? 0xffff : (uint16_t)SWEET_PT_MIN(ms, 0xfffe);
		ms = sweet_pt_us(sweet_pt.tEvAbs, now) / 1000;
		x->evAgo = sweet_pt.tEvAbs == 0 ? 0xffff : (uint16_t)SWEET_PT_MIN(ms, 0xfffe);
	}
	sweet_pt_give();
}

/*
 * 1114: a vsync notification round (sendVsync, fLock held; on = CA has them on): class the previous tick by what CA did
 * after it, and note whether the newest ended swap is still pending (not latched: a SwapWait poll on its predecessor
 * would answer Busy with tribuf 2, i.e. its completion cannot reach CA at this tick).
 */
static void
sweet_pt_tick(uint64_t now, bool on)
{
	sweet_pace_tick_abs = now;      /* osfmk: the first CA display-thread run after it */
	if (!sweet_pt_take()) {
		OSIncrementAtomic((volatile SInt32 *)&sweet_pt.xMissed);
		return;
	}
	if (sweet_pt.tAbs != 0) {
		if (!sweet_pt.tOn) {
			sweet_pt.xOff++;
		} else if (sweet_pt.tBegan) {
			sweet_pt.xSwap++;
		} else if (sweet_pt.tPolls == 0) {
			sweet_pt.gNone++;
			sweet_pt.xNone++;
		} else if (sweet_pt.tDone == 0) {
			sweet_pt.gBusy++;
			sweet_pt.xBusy++;
		} else {
			sweet_pt.gOk++;
			sweet_pt.xOk++;
		}
		uint64_t wt = 0, won = 0;
		uint32_t wc = 0;
		sweet_pace_wake_read(&wt, &won, &wc);
		if (sweet_pt.tOn && wt == sweet_pt.tAbs && won >= wt) {
			uint32_t b = sweet_pt_us(wt, won + 1) / 100;
			b = b < 250 ? b : 249;
			if (sweet_pt.xWake[b] != 0xffff) {
				sweet_pt.xWake[b]++;
			}
			sweet_pt.xWakeN++;
			sweet_pt.xWakeLittle += wc < 6;
		}
	}
	sweet_pt.tAbs = now;
	sweet_pt.tPolls = sweet_pt.tDone = 0;
	sweet_pt.tBegan = false;
	sweet_pt.tOn = on;
	sweet_pt.tPend = sweet_scan.on && sweet_scan.lastSwap != 0 && sweet_scan.shownSwap != sweet_scan.lastSwap;
	if (on) {
		sweet_pt.gTicks++;
		sweet_pt.gPend += sweet_pt.tPend;
		sweet_pt.xTicks++;
		sweet_pt.xPend += sweet_pt.tPend;
	} else {
		sweet_pt.gOff++;
	}
	sweet_pt_give();
}

/* 1114: SwapWait (CA's can_update_ poll, or a wait): its answer */
static void
sweet_pt_wait(IOReturn ret)
{
	if (!sweet_pt_take()) {
		OSIncrementAtomic((volatile SInt32 *)&sweet_pt.xMissed);
		return;
	}
	sweet_pt.tPolls++;
	sweet_pt.gPolls++;
	if (ret == kIOReturnSuccess) {
		sweet_pt.tDone++;
	} else {
		sweet_pt.gPollsBusy++;
	}
	sweet_pt_give();
}

/* 1114: iokit/Kernel/arm/hid/SweetHID.cpp, every touch event that goes out (finger 9 = the command server's swipes) */
extern "C" void sweet_pace_trace_touch(int finger, bool down, bool was, uint64_t now);
void
sweet_pace_trace_touch(int finger, bool down, bool was, uint64_t now)
{
	if (!(sweet_pace_trace & 4) || !sweet_pt_take()) {
		return;
	}
	uint32_t bit = 1u << (finger & 31);
	sweet_pt.tEvents++;
	sweet_pt.gEvents++;
	sweet_pt.tSynth += finger == 9;
	sweet_pt.tEvAbs = now;
	if (down && was && sweet_pt.tPrevAbs != 0 && now > sweet_pt.tPrevAbs) {
		uint32_t us = sweet_pt_us(sweet_pt.tPrevAbs, now), b = us / 250;
		b = b < 256 ? b : 255;
		if (sweet_pt.tDt[b] != 0xffff) {
			sweet_pt.tDt[b]++;
		}
		sweet_pt.tMoves++;
		sweet_pt.tGaps += us > 20000;
	}
	if (down) {
		sweet_pt.tDownMask |= bit;
		sweet_pt.tPrevAbs = now;
	} else {
		sweet_pt.tDownMask &= ~bit;
		sweet_pt.tUpAbs = now;
		sweet_pt.tPrevAbs = 0;
	}
	sweet_pt_give();
}

/* sweet_gpu_submit_v2: entry t0, after the scanout gate t1 */
extern "C" void sweet_pace_trace_submit(int pid, uint32_t gf, uint64_t t0, uint64_t t1);
void
sweet_pace_trace_submit(int pid, uint32_t gf, uint64_t t0, uint64_t t1)
{
	if (pid != sweet_scan.gatePid || pid == 0 || !sweet_pt_take()) {
		return;
	}
	if (sweet_pt.pSub0 == 0) {
		sweet_pt.pSub0 = t0;
	}
	sweet_pt.pSubN++;
	if ((gf & 3u) != 2u) {          /* a display submit (or unflagged): the frame's own, its fence is the SwapEnd's */
		sweet_pt.pSubD = t0;
		sweet_pt.pRel = t1;
	}
	sweet_pt_give();
}

/* SwapEnd taken by the direct scanout (with fLock): ahead = frames not latched yet in front of it */
static void
sweet_pt_swap(uint32_t id, uint64_t now, uint32_t fence, uint32_t ahead)
{
	if (sweet_pace_trace == 0 || !sweet_pt_take()) {
		return;
	}
	auto *f = &sweet_pt.f[id & 7];
	if (f->id != id || f->end != 0) {
		bzero(f, sizeof(*f));   /* no SwapBegin seen for it (trace just turned on) */
		f->id = id;
	}
	f->sub0 = sweet_pt.pSub0;
	f->subD = sweet_pt.pSubD;
	f->rel = sweet_pt.pRel;
	f->end = now;
	f->fence = fence;
	f->ahead = ahead | (sweet_pt.pSubN << 8);
	if (fence == 0 || sweet_gpu_fence_done(fence)) {
		f->done = now;
		sweet_pt.doneAtEnd++;
	}
	sweet_pt.pSub0 = sweet_pt.pSubD = sweet_pt.pRel = 0;
	sweet_pt.pSubN = 0;
	sweet_pt_give();
}

/* scanout thread, with fLock: fences of frames swapped and not yet seen done */
static void
sweet_pt_poll(uint64_t now)
{
	if (!sweet_pt_take()) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		auto *f = &sweet_pt.f[i];
		if (f->end != 0 && f->done == 0 && sweet_gpu_fence_done(f->fence)) {
			f->done = now;
		}
	}
	sweet_pt_give();
}

static void
sweet_pt_flip(uint32_t id, uint64_t now)
{
	if (sweet_pace_trace == 0 || !sweet_pt_take()) {
		return;
	}
	auto *f = &sweet_pt.f[id & 7];
	if (f->id == id && f->end != 0) {
		if (f->done == 0) {
			f->done = now;  /* the flip saw it landed this same look */
		}
		f->flip = now;
	}
	sweet_pt_give();
}

static void
sweet_pt_latch(uint32_t id, uint64_t now, uint64_t per)
{
	if (sweet_pace_trace == 0 || !sweet_pt_take()) {
		return;
	}
	auto *f = &sweet_pt.f[id & 7];
	uint64_t prev = sweet_pt.lastLatch;
	sweet_pt.lastLatch = now;
	uint32_t k = 0;
	if (prev != 0 && now > prev) {
		uint32_t us = sweet_pt_us(prev, now);
		if (us >= 100000) {
			sweet_pt.pauses++;
		} else {
			k = (uint32_t)((now - prev + per / 2) / per);
			k = k < 1 ? 1 : k;
			sweet_pt.per[k >= 4 ? 3 : k - 1]++;
			sweet_pt.n++;
		}
	}
	if (k == 0) {
		bzero(f, sizeof(*f));
		sweet_pt_give();
		return;
	}
	if (f->id != id || f->end == 0 || f->flip == 0) {
		sweet_pt.unmatched++;
		bzero(f, sizeof(*f));
		sweet_pt_give();
		return;
	}
	uint64_t start = f->begin != 0 ? f->begin : f->sub0;
	if (f->begin == 0) {
		sweet_pt.noBegin++;
	}
	if (f->begin != 0 && f->subD != 0 && f->subD < f->begin) {
		sweet_pt.subBeforeBegin++;
	}
	uint64_t done = f->done != 0 ? f->done : f->flip;
	uint32_t st[SWEET_PT_ST];
	st[0] = sweet_pt_us(prev, start);                       /* idle: previous latch -> this frame's SwapBegin */
	st[1] = sweet_pt_us(f->begin, f->subD);                 /* SwapBegin -> display submit (CA's CPU work) */
	st[2] = sweet_pt_us(f->subD, f->rel);                   /* scanout gate */
	st[3] = sweet_pt_us(f->rel != 0 ? f->rel : f->subD, done);     /* submit (released) -> GPU done seen */
	st[4] = sweet_pt_us(done, now);                         /* GPU done -> latch */
	st[5] = sweet_pt_us(done > f->end ? done : f->end, f->flip);  /* done and swapped -> flipped (queue / prev latch) */
	st[6] = sweet_pt_us(f->flip, now);                      /* flip -> latch (the panel edge) */
	st[7] = sweet_pt_us(f->begin, f->end);                  /* SwapBegin -> SwapEnd */
	sweet_pt.nAll++;
	for (int i = 0; i < SWEET_PT_ST; i++) {
		sweet_pt.sumAll[i] += st[i];
	}
	uint32_t perUs = sweet_pt_us(1, 1 + per);
	if (k >= 3) {
		sweet_pt.nSlow++;
		for (int i = 0; i < SWEET_PT_ST; i++) {
			sweet_pt.sumSlow[i] += st[i];
			uint32_t b = st[i] / 500;
			b = b < SWEET_PT_BINS ? b : SWEET_PT_BINS - 1;
			if (sweet_pt.h[i][b] != 0xffff) {
				sweet_pt.h[i][b]++;
			}
		}
		sweet_pt.cIdle += st[0] > perUs;
		sweet_pt.cGpu += st[3] > perUs;
		sweet_pt.cPrevGpu += sweet_pt.prevGpu > perUs;
		sweet_pt.cAhead += (f->ahead & 0xff) >= 2;
		sweet_pt.cWait += st[5] > perUs;
		sweet_pt.cGate += st[2] > 1000;
		if ((sweet_pace_trace & 2) && !(sweet_pace_trace & 4)) {       /* 1114: with 4 the ring has them, uncapped */
			if (sweet_pt.slowN < SWEET_PT_SLOW) {
				auto *r = &sweet_pt.slow[sweet_pt.slowN++];
				r->id = id;
				r->k = k;
				r->ahead = f->ahead & 0xff;
				r->nsub = f->ahead >> 8;
				r->phase = f->begin != 0 && f->tick != 0 ? sweet_pt_us(f->tick, f->begin) : 0;
				r->prevGpu = (uint32_t)sweet_pt.prevGpu;
				memcpy(r->st, st, sizeof(st));
			} else {
				sweet_pt.slowLost++;
			}
		}
	}
	if ((sweet_pace_trace & 4) && (k >= 3 || (sweet_pace_trace & 8))) {
		/* 1114: into the ring (overwrites the oldest; the dump counts what it never printed) */
		auto *r = &sweet_pt.ring[sweet_pt.ringW % SWEET_PT_RING];
		r->latch = now;
		r->id = id;
		memcpy(r->st, st, sizeof(st));
		r->k = (uint8_t)SWEET_PT_MIN(k, 255u);
		r->ahead = (uint8_t)(f->ahead & 0xff);
		r->nsub = (uint8_t)SWEET_PT_MIN(f->ahead >> 8, 255u);
		r->x = f->x;
		sweet_pt.ringW++;
	}
	sweet_pt.prevGpu = st[3];
	bzero(f, sizeof(*f));
	sweet_pt_give();
}

/* 1114: p-th percentile of a histogram (bin index; n = total) */
static uint32_t
sweet_pt_pct(const uint16_t *h, uint32_t bins, uint32_t n, uint32_t pct)
{
	uint32_t want = (n * pct + 99) / 100, acc = 0;
	for (uint32_t i = 0; n != 0 && i < bins; i++) {
		acc += h[i];
		if (acc >= want) {
			return i;
		}
	}
	return 0;
}

/*
 * 1114: kern.sweet_pace_dump=1 (bsd/dev/sweet_gpu.c, the sysctl caller's thread): print the ring since the last dump,
 * one "SWEET PACE: f" line per frame with its latch time. The records are copied out under the try-lock (hooks that
 * find it taken meanwhile drop their update), printed after. Returns the number printed.
 * 1118: the kernel log cuts a message at 255 bytes (the 1114 line lost everything after "| disp ru" and its
 * newline), so each record is printed as four lines f1..f4, each starting "SWEET PACE: fN <latch s.ms> id <id>",
 * in that order (realistic lengths 100-170 bytes; even with every field at its type's maximum each stays under
 * 255): f1 v + stages, f2 tick classes + swapper, f3 disp/rs/sb run/wait, f4 touch + ahead/subs.
 * tools-claude/pace-k5.py joins them by (latch, id). The header line is split the same way (dump / dump-key).
 */
extern "C" uint32_t sweet_pace_dump(void);
uint32_t
sweet_pace_dump(void)
{
	struct sweet_pt_rec *b = (struct sweet_pt_rec *)IOMallocData(sizeof(struct sweet_pt_rec) * SWEET_PT_RING);
	if (b == NULL) {
		return 0;
	}
	int tries = 0;
	while (!sweet_pt_take()) {
		if (++tries > 200) {
			IOFreeData(b, sizeof(struct sweet_pt_rec) * SWEET_PT_RING);
			IOLog("SWEET PACE: dump: trace word busy\n");
			return 0;
		}
		IOSleep(1);
	}
	uint64_t w = sweet_pt.ringW, r = sweet_pt.ringR, lost = 0;
	if (w - r > SWEET_PT_RING) {
		lost = w - r - SWEET_PT_RING;
		r = w - SWEET_PT_RING;
	}
	uint32_t n = (uint32_t)(w - r);
	for (uint32_t i = 0; i < n; i++) {
		b[i] = sweet_pt.ring[(r + i) % SWEET_PT_RING];
	}
	sweet_pt.ringR = w;
	sweet_pt.ringLost += lost;
	uint32_t missed = sweet_pt.xMissed;
	sweet_pt_give();
	IOLog("SWEET PACE: dump n %u lost %llu (trace %#x; ms; 1118: 4 lines f1..f4 per frame; missed updates %u)\n", n,
	    lost, sweet_pace_trace, missed);
	IOLog("SWEET PACE: dump-key tick classes: swap/busy = CA polled SwapWait, all Busy/none = no poll, no swap/ok = "
	    "polled done, no swap; pend = newest swap not latched at the tick; run/wait = big/little ms\n");
#define PX(a) (a) / 1000, (a) % 1000 / 100
	for (uint32_t i = 0; i < n; i++) {
		auto *e = &b[i];
		auto *x = &e->x;
		uint64_t ns;
		absolutetime_to_nanoseconds(e->latch, &ns);
		char oc[16];
		if (x->onCore == 0xffffffffu) {
			strlcpy(oc, "pre", sizeof(oc));
		} else {
			snprintf(oc, sizeof(oc), "%u.%u", PX(x->onCore));
		}
		/* 1118: four messages (each under the kernel log's 255-byte cut), same latch + id, in this order */
		unsigned long long sec = ns / 1000000000ULL, msec = ns / 1000000ULL % 1000;
		IOLog("SWEET PACE: f1 %llu.%03llu id %u v %u | idle %u.%u sub %u.%u gate %u.%u gpu %u.%u done->latch %u.%u "
		    "(wait %u.%u flip->latch %u.%u)\n",
		    sec, msec, e->id, e->k, PX(e->st[0]), PX(e->st[1]), PX(e->st[2]),
		    PX(e->st[3]), PX(e->st[4]), PX(e->st[5]), PX(e->st[6]));
		IOLog("SWEET PACE: f2 %llu.%03llu id %u | ticks %u swap-gap busy %u none %u ok %u pend %u off %u polls %u/%u "
		    "busy | tick->begin %u.%u on-core %s wait %u.%u cpu %u cls %u pend-at-tick %u\n",
		    sec, msec, e->id, x->ticks, x->tBusy, x->tNone, x->tOk, x->tPend,
		    x->tOff, x->polls, x->pollsBusy, PX(x->tickBegin), oc, PX(x->curWait), x->cpu, x->cls, x->pendAtTick);
		IOLog("SWEET PACE: f3 %llu.%03llu id %u | disp run %u.%u/%u.%u wait %u.%u/%u.%u | rs run %u.%u/%u.%u wait "
		    "%u.%u/%u.%u | sb run %u.%u/%u.%u wait %u.%u/%u.%u\n",
		    sec, msec, e->id,
		    PX(x->gap[0][0]), PX(x->gap[0][1]), PX(x->gap[0][2]), PX(x->gap[0][3]),
		    PX(x->gap[1][0]), PX(x->gap[1][1]), PX(x->gap[1][2]), PX(x->gap[1][3]),
		    PX(x->gap[2][0]), PX(x->gap[2][1]), PX(x->gap[2][2]), PX(x->gap[2][3]));
		IOLog("SWEET PACE: f4 %llu.%03llu id %u | touch down %u up %u ev %u n %u | ahead %u subs %u\n",
		    sec, msec, e->id, x->down, x->upAgo, x->evAgo, x->events, e->ahead, e->nsub);
		if ((i & 7) == 7) {
			IOSleep(2);     /* (1118: every 8 records = 32 lines, as 1114's every 32 lines) */
		}
	}
#undef PX
	IOFreeData(b, sizeof(struct sweet_pt_rec) * SWEET_PT_RING);
	return n;
}

static uint32_t
sweet_pt_p95(const uint16_t *h, uint32_t n)
{
	uint32_t want = (n * 95 + 99) / 100, acc = 0;
	for (uint32_t i = 0; n != 0 && i < SWEET_PT_BINS; i++) {
		acc += h[i];
		if (acc >= want) {
			return i * 5 + 5;       /* upper edge, 0.1 ms */
		}
	}
	return 0;
}

/* with fLock, from the 5 s stats */
static void
sweet_pt_line(void)
{
	if (sweet_pace_trace == 0 || !sweet_pt_take()) {
		return;
	}
	if (sweet_pt.n == 0 && sweet_pt.unmatched == 0 && (!(sweet_pace_trace & 4) || sweet_pt.xTicks == 0)) {
		sweet_pt_give();
		return;
	}
	uint32_t ma[SWEET_PT_ST], ms[SWEET_PT_ST], p[SWEET_PT_ST];
	for (int i = 0; i < SWEET_PT_ST; i++) {
		ma[i] = sweet_pt.nAll ? (uint32_t)(sweet_pt.sumAll[i] / sweet_pt.nAll / 100) : 0;     /* 0.1 ms */
		ms[i] = sweet_pt.nSlow ? (uint32_t)(sweet_pt.sumSlow[i] / sweet_pt.nSlow / 100) : 0;
		p[i] = sweet_pt_p95(sweet_pt.h[i], sweet_pt.nSlow);
	}
#define PT1(a) (a) / 10, (a) % 10
#define PT2(i) PT1(ms[i]), PT1(p[i])
	IOLog("SWEET PACE: 5s frames %u | vsyncs 1:%u 2:%u 3:%u 4+:%u | pauses %u unmatched %u | slow(3+) n %u mean/p95 ms: "
	    "idle %u.%u/%u.%u | swap->sub %u.%u/%u.%u | gate %u.%u/%u.%u | sub->gpudone %u.%u/%u.%u | gpudone->latch %u.%u/%u.%u "
	    "(wait %u.%u/%u.%u flip->latch %u.%u/%u.%u) | swap->swapend %u.%u/%u.%u | slow with: idle>1v %u gpu>1v %u "
	    "prevgpu>1v %u ahead>=2 %u wait>1v %u gate>1ms %u\n",
	    sweet_pt.n, sweet_pt.per[0], sweet_pt.per[1], sweet_pt.per[2], sweet_pt.per[3], sweet_pt.pauses, sweet_pt.unmatched,
	    sweet_pt.nSlow, PT2(0), PT2(1), PT2(2), PT2(3), PT2(4), PT2(5), PT2(6), PT2(7), sweet_pt.cIdle, sweet_pt.cGpu,
	    sweet_pt.cPrevGpu, sweet_pt.cAhead, sweet_pt.cWait, sweet_pt.cGate);
	IOLog("SWEET PACE: 5s all n %u mean ms: idle %u.%u | swap->sub %u.%u | gate %u.%u | sub->gpudone %u.%u | "
	    "gpudone->latch %u.%u (wait %u.%u flip->latch %u.%u) | swap->swapend %u.%u | done at SwapEnd %u | sub before "
	    "SwapBegin %u | no SwapBegin %u | slow lines %u lost %u\n", sweet_pt.nAll, PT1(ma[0]), PT1(ma[1]), PT1(ma[2]),
	    PT1(ma[3]), PT1(ma[4]), PT1(ma[5]), PT1(ma[6]), PT1(ma[7]), sweet_pt.doneAtEnd, sweet_pt.subBeforeBegin,
	    sweet_pt.noBegin, sweet_pt.slowN, sweet_pt.slowLost);
	for (uint32_t j = 0; j < sweet_pt.slowN; j++) {
		auto *r = &sweet_pt.slow[j];
		uint32_t s[SWEET_PT_ST];
		for (int i = 0; i < SWEET_PT_ST; i++) {
			s[i] = r->st[i] / 100;
		}
		IOLog("SWEET PACE: slow swap %u vsyncs %u | idle %u.%u | swap->sub %u.%u | gate %u.%u | sub->gpudone %u.%u | "
		    "gpudone->latch %u.%u (wait %u.%u flip->latch %u.%u) | swap->swapend %u.%u | ahead %u | submits %u | "
		    "prev gpu %u.%u | SwapBegin %u.%u ms after tick\n", r->id, r->k, PT1(s[0]), PT1(s[1]), PT1(s[2]), PT1(s[3]),
		    PT1(s[4]), PT1(s[5]), PT1(s[6]), PT1(s[7]), r->ahead, r->nsub, PT1(r->prevGpu / 100), PT1(r->phase / 100));
	}
#undef PT2
#undef PT1
	if (sweet_pace_trace & 4) {
		/* 1114 */
		uint32_t wn = sweet_pt.xWakeN, mv = sweet_pt.tMoves;
		uint32_t w50 = sweet_pt_pct(sweet_pt.xWake, 250, wn, 50), w95 = sweet_pt_pct(sweet_pt.xWake, 250, wn, 95);
		uint32_t d[4] = { sweet_pt_pct(sweet_pt.tDt, 256, mv, 5), sweet_pt_pct(sweet_pt.tDt, 256, mv, 50),
			          sweet_pt_pct(sweet_pt.tDt, 256, mv, 95), 0 };
		for (int i = 255; mv != 0 && i >= 0; i--) {
			if (sweet_pt.tDt[i] != 0) {
				d[3] = (uint32_t)i;
				break;
			}
		}
		IOLog("SWEET PACE: 5s ticks %u (CA on) swap %u busy %u none %u ok %u off %u | newest swap pending at tick %u | "
		    "tick->CA display thread on core n %u p50/p95 %u.%u/%u.%u ms (on little %u) | touch events %u (synthetic %u) "
		    "moves %u dt p5/p50/p95/max %u.%02u/%u.%02u/%u.%02u/%u.%02u ms gaps>20ms %u | ring %llu unread, lost %llu | "
		    "missed %u\n", sweet_pt.xTicks, sweet_pt.xSwap, sweet_pt.xBusy, sweet_pt.xNone, sweet_pt.xOk, sweet_pt.xOff,
		    sweet_pt.xPend, wn, w50 / 10, w50 % 10, w95 / 10, w95 % 10, sweet_pt.xWakeLittle, sweet_pt.tEvents,
		    sweet_pt.tSynth, mv, d[0] / 4, d[0] % 4 * 25, d[1] / 4, d[1] % 4 * 25, d[2] / 4, d[2] % 4 * 25,
		    d[3] / 4, d[3] % 4 * 25, sweet_pt.tGaps, sweet_pt.ringW - sweet_pt.ringR, sweet_pt.ringLost, sweet_pt.xMissed);
		sweet_pt.xTicks = sweet_pt.xSwap = sweet_pt.xBusy = sweet_pt.xNone = sweet_pt.xOk = sweet_pt.xPend = 0;
		sweet_pt.xOff = sweet_pt.xWakeN = sweet_pt.xWakeLittle = 0;
		bzero(sweet_pt.xWake, sizeof(sweet_pt.xWake));
		sweet_pt.tEvents = sweet_pt.tSynth = sweet_pt.tMoves = sweet_pt.tGaps = 0;
		bzero(sweet_pt.tDt, sizeof(sweet_pt.tDt));
	}
	uint64_t ll = sweet_pt.lastLatch, pg = sweet_pt.prevGpu;
	sweet_pt.n = sweet_pt.pauses = sweet_pt.unmatched = sweet_pt.doneAtEnd = sweet_pt.subBeforeBegin = sweet_pt.noBegin = 0;
	bzero(sweet_pt.per, sizeof(sweet_pt.per));
	bzero(sweet_pt.sumAll, sizeof(sweet_pt.sumAll));
	bzero(sweet_pt.sumSlow, sizeof(sweet_pt.sumSlow));
	bzero(sweet_pt.h, sizeof(sweet_pt.h));
	sweet_pt.nAll = sweet_pt.nSlow = 0;
	sweet_pt.cIdle = sweet_pt.cGpu = sweet_pt.cPrevGpu = sweet_pt.cAhead = sweet_pt.cWait = sweet_pt.cGate = 0;
	sweet_pt.slowN = sweet_pt.slowLost = 0;
	sweet_pt.lastLatch = ll;
	sweet_pt.prevGpu = pg;
	sweet_pt_give();
}

#define SWEET_SF_BINS   1000                            /* 0.1 ms bins to 100 ms */
static struct {
	uint64_t lastLatch, prevIntv;
	uint32_t per[4], pauses, repeats, n, tn;
	uint64_t jitAbs, maxAbs;
	uint16_t hist[SWEET_SF_BINS];
	uint16_t thist[200];                            /* touch -> latch, 1 ms bins */
} sweet_sf;

static void
sweet_scan_frame_latched(AppleCLCD *fb, uint64_t now, uint64_t swapAbs)
{
	uint64_t per = fb->fFcPeriod, ns;
	if (per == 0) {
		nanoseconds_to_absolutetime(fb->fHz == 90 ? 11111111ULL : 16666667ULL, &per);
	}
	uint64_t t0 = sweet_hid_touch_abs[0], t1 = sweet_hid_touch_abs[1];
	uint64_t t = t0 != 0 && t0 <= swapAbs ? t0 : (t1 != 0 && t1 <= swapAbs ? t1 : 0);
	if (t != 0 && now > t) {
		absolutetime_to_nanoseconds(now - t, &ns);
		if (ns < 200 * NSEC_PER_MSEC) {
			sweet_sf.thist[ns / NSEC_PER_MSEC]++;
			sweet_sf.tn++;
		}
	}
	if (sweet_sf.lastLatch != 0 && now > sweet_sf.lastLatch) {
		uint64_t iv = now - sweet_sf.lastLatch;
		absolutetime_to_nanoseconds(iv, &ns);
		if (ns >= 100 * NSEC_PER_MSEC) {
			sweet_sf.pauses++;
			sweet_sf.prevIntv = 0;
		} else {
			uint64_t k = (iv + per / 2) / per;
			sweet_sf.per[k <= 1 ? 0 : (k >= 4 ? 3 : k - 1)]++;
			sweet_sf.repeats += k >= 3;
			sweet_sf.hist[ns / 100000]++;
			sweet_sf.n++;
			if (iv > sweet_sf.maxAbs) {
				sweet_sf.maxAbs = iv;
			}
			if (sweet_sf.prevIntv != 0) {
				sweet_sf.jitAbs += iv > sweet_sf.prevIntv ? iv - sweet_sf.prevIntv : sweet_sf.prevIntv - iv;
			}
			sweet_sf.prevIntv = iv;
		}
	}
	sweet_sf.lastLatch = now;
}

static uint32_t
sweet_sf_pct(const uint16_t *h, uint32_t bins, uint32_t n, uint32_t pct)
{
	uint32_t want = (n * pct + 99) / 100, acc = 0;
	for (uint32_t i = 0; n != 0 && i < bins; i++) {
		acc += h[i];
		if (acc >= want) {
			return i;
		}
	}
	return 0;
}

static void
sweet_sf_line_locked(void)
{
	uint32_t n = sweet_sf.n;
	uint32_t p50 = sweet_sf_pct(sweet_sf.hist, SWEET_SF_BINS, n, 50), p95 = sweet_sf_pct(sweet_sf.hist, SWEET_SF_BINS, n, 95);
	uint32_t t50 = sweet_sf_pct(sweet_sf.thist, 200, sweet_sf.tn, 50), t95 = sweet_sf_pct(sweet_sf.thist, 200, sweet_sf.tn, 95);
	uint64_t mx = 0, jit = 0;
	absolutetime_to_nanoseconds(sweet_sf.maxAbs, &mx);
	absolutetime_to_nanoseconds(sweet_sf.jitAbs, &jit);
	jit = n > 1 ? jit / 1000 / (n - 1) : 0;
	IOLog("SWEET SCANOUT: 5s frames: latches %u | vsyncs 1:%u 2:%u 3:%u 4+:%u | p50 %u.%u p95 %u.%u max %llu.%llu ms | "
	    "jitter %llu us | repeats>=3 %u | pauses %u | touch->latch n %u p50 %u p95 %u ms\n", n, sweet_sf.per[0],
	    sweet_sf.per[1], sweet_sf.per[2], sweet_sf.per[3], p50 / 10, p50 % 10, p95 / 10, p95 % 10, mx / 1000000,
	    mx / 100000 % 10, jit, sweet_sf.repeats, sweet_sf.pauses, sweet_sf.tn, t50, t95);
	uint64_t last = sweet_sf.lastLatch, prev = sweet_sf.prevIntv;
	bzero(&sweet_sf, sizeof(sweet_sf));
	sweet_sf.lastLatch = last;
	sweet_sf.prevIntv = prev;
}

/* With fLock, from the scanout thread: latch, then the next flip. False: nothing in flight. */
static bool
sweet_scan_step_locked(AppleCLCD *fb, uint64_t now)
{
	if (__improbable(sweet_pace_trace)) {
		sweet_pt_poll(now);     /* 1113: GPU done of the frames swapped and not flipped yet */
	}
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
		sweet_scan_frame_latched(fb, now, sweet_scan.flipSwapAbs);    /* 1044 */
		sweet_tt_latch(sweet_scan.flipSwap, now);       /* 1107 */
		if (__improbable(sweet_pace_trace)) {
			uint64_t per = fb->fFcPeriod;   /* 1113: as sweet_scan_frame_latched */
			if (per == 0) {
				nanoseconds_to_absolutetime(fb->fHz == 90 ? 11111111ULL : 16666667ULL, &per);
			}
			sweet_pt_latch(sweet_scan.flipSwap, now, per);
		}
		OSAddAtomic64(1, &sweet_fb_frames);    /* 1060 */
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
		if (s->ubwcLive) {
			sweet_ubwc_st.corners++;        /* 1111: CPU writes into compressed planes would show as garbage */
		} else {
			sweet_scan_corners(fb, s);      /* 0986 */
		}
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
		sweet_sf.lastLatch = 0;         /* 1044: the pipes switched: a new run of latches */
		SWEET_TRACE(SWT_SCAN_LATCH, sSwap, sSeq, mach_absolute_time() - sAbs, 0);    /* 0994: pipes switched to it */
		IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);
		return sweet_scan.next != NULL;
	}
	if (s->ubwcLive || sweet_mdp.ufmt) {
		sweet_mdp_scan_addr(s);         /* 1111: a UBWC surface, or the first linear one after it */
	} else {
		sweet_mdp_wr(MDP_VIG(0) + SSPP_SRC0, (uint32_t)s->scanPA);
		sweet_mdp_wr(MDP_VIG(1) + SSPP_SRC0, (uint32_t)s->scanPA);
	}
	sweet_scan.flipFrame = sweet_mdp_rd(SWEET_INTF1 + INTF_FRAME_COUNT);
	sweet_mdp_wr(MDP_CTL(0) + MDP_CTL_FLUSH, MDP_FLUSH_VIG01 | sweet_cm_flip_locked());    /* 1116: + DSPP0/1 PCC */
	SWEET_TRACE(SWT_SCAN_FLIP, sSwap, sSeq, now - sAbs, sweet_scan.flipFrame);    /* 0994 */
	sweet_scan.flipping = s;
	sweet_scan.flipSeq = sSeq;
	sweet_scan.flipSwap = sSwap;
	if (__improbable(sweet_pace_trace)) {
		sweet_pt_flip(sSwap, now);      /* 1113 */
	}
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
	if (sweet_surface_ubwc_boot() != 0) {
		sweet_ubwc_stats_locked();      /* 1111 */
	}
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
	sweet_sf_line_locked();         /* 1044 */
	sweet_tt_line();                /* 1107 */
	sweet_pt_line();                /* 1113 */
	{
		uint64_t gw = 0;    /* 0985 */
		absolutetime_to_nanoseconds(sweet_scan.stGateAbs, &gw);
		uint64_t cw = 0;    /* 0986 */
		absolutetime_to_nanoseconds(sweet_scan.stCornerAbs, &cw);
		IOLog("SWEET SCANOUT: 5s gate %u: submits %u waited %u (avg %llu us) timeout %u | cp waits %u released %u queued %u "
		    "overflow %u | corners %u (avg %llu us) | flag display %u bare %u unflagged %u\n", sweet_scan.gate, sweet_scan.stGate,
		    sweet_scan.stGateWait, sweet_scan.stGateWait ? gw / 1000 / sweet_scan.stGateWait : 0, sweet_scan.stGateTimeout,
		    sweet_scan.stGpuWait, sweet_scan.stGpuRelease, sweet_scan.stQueued, sweet_scan.stOverflow, sweet_scan.stCorners,
		    sweet_scan.stCorners ? cw / 1000 / sweet_scan.stCorners : 0, sweet_scan.stFlagDisp, sweet_scan.stFlagBare,
		    sweet_scan.stUnflagged);
		sweet_scan.stFlagDisp = sweet_scan.stFlagBare = sweet_scan.stUnflagged = 0;     /* 1104 */
		sweet_scan.stGpuWait = sweet_scan.stGpuRelease = 0;    /* 1110: queued / overflow reset below (tribuf line) */
		sweet_scan.stCornerAbs = 0;
		sweet_scan.stCorners = 0;
		sweet_scan.stGateAbs = 0;
		sweet_scan.stGate = sweet_scan.stGateWait = sweet_scan.stGateTimeout = 0;
	}
	if (sweet_scan.gate == 3 || sweet_scan.st3Id != 0 || sweet_scan.st3NoId != 0) {
		/* 1105: gate 3; surfaces = distinct surfaces among the last 8 slot frames (CA's chain) */
		uint32_t nd = 0;
		for (uint32_t i = 0; i < 8; i++) {
			bool dup = sweet_scan.chain[i] == 0;
			for (uint32_t j = 0; j < i && !dup; j++) {
				dup = sweet_scan.chain[j] == sweet_scan.chain[i];
			}
			nd += !dup;
		}
		uint64_t w3 = 0;
		absolutetime_to_nanoseconds(sweet_scan.st3Abs, &w3);
		uint32_t nw = sweet_scan.st3Shown + sweet_scan.st3Pending;
		IOLog("SWEET SCANOUT: 5s gate3: display submits %u (no id %u, gated as 1) | free %u waited %u (on screen %u, pending "
		    "%u) avg %llu us timeout %u | overlapped frames %u of %u | surfaces %u chain %u-%u tribuf %u\n", sweet_scan.st3Id,
		    sweet_scan.st3NoId, sweet_scan.st3Free, nw, sweet_scan.st3Shown, sweet_scan.st3Pending, nw ? w3 / 1000 / nw : 0,
		    sweet_scan.st3Timeout, sweet_scan.st3Overlap, sweet_scan.stFrames, nd, sweet_scan.stDepthMin, sweet_scan.stDepthMax,
		    sweet_scan.tribuf ? 1 : 0);
		sweet_scan.st3Id = sweet_scan.st3NoId = sweet_scan.st3Shown = sweet_scan.st3Pending = 0;
		sweet_scan.st3Timeout = sweet_scan.st3Overlap = sweet_scan.st3Free = 0;
		sweet_scan.st3Abs = 0;
	}
	if (sweet_scan.tribuf) {
		/* 0996 */
		IOLog("SWEET SCANOUT: 5s tribuf %u: polls busy %u done %u stale %u | gate at K-1 %u | short %u | chain %u-%u | "
		    "on-screen busy %u | swapped held %u repeat %u | queue %u: queued %u replaced %u overflow %u late %u\n",
		    sweet_scan.pollRel ? 2 : 1,
		    sweet_scan.stTbBusy, sweet_scan.stTbDone, sweet_scan.stTbStale, sweet_scan.stTbGate3, sweet_scan.stTbShort,
		    sweet_scan.stDepthMin, sweet_scan.stDepthMax, sweet_scan.stRelShown, sweet_scan.stHeldSwap, sweet_scan.stRepeatSwap,
		    sweet_scan.queue ? 1 : 0, sweet_scan.stQueued, sweet_scan.stDropped, sweet_scan.stOverflow, sweet_scan.stLate);
		sweet_scan.stTbBusy = sweet_scan.stTbDone = sweet_scan.stTbStale = sweet_scan.stTbGate3 = sweet_scan.stTbShort = 0;
		sweet_scan.stRelShown = 0;
	}
	sweet_scan.stHeldSwap = sweet_scan.stRepeatSwap = 0;    /* 1109 */
	sweet_scan.stQueued = sweet_scan.stOverflow = 0;        /* 1110 */
	sweet_scan.stFenceAbs = sweet_scan.stLatchAbs = sweet_scan.stLatchMax = 0;
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
		if (__improbable(sweet_cm_st.dirty || sweet_cm_st.checkAbs != 0)) {
			sweet_cm_idle_locked(now);      /* 1116: a PCC change with no flip to carry it */
		}
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

/*
 * 1046: sweetscanout=2 (the default): direct scanout turns itself on, once, at the first SwapEnd whose single layer is
 * a scanout-slot surface of the panel's logical size, with the display on: the same sweet_scan_set as p scanout 1,
 * from backboardd's SwapEnd thread (AppleCLCD::start has made the slots, the MDP is ABL's set-up, CA's chain is in the
 * slots). Afterwards nothing differs from the live poke: display off leaves the direct geometry (0989) with
 * sweet_scan.on kept, and the next slot frames (3 in a row, 0979) enter it again. p scanout 0|1 before that disarms it.
 */
static void
sweet_scan_auto(AppleCLCD *fb, const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);
	if (mask == 0 || (mask & (mask - 1)) || fb->fPowerOff) {
		return;
	}
	uint32_t l = (uint32_t)__builtin_ctz(mask);
	uint32_t id = l < SWEET_FB_LAYERS ? get32(rec, 0x54 + 4 * l) : 0;
	SweetSurface *s = id != 0 ? sweet_surface_copy(id) : NULL;
	if (s == NULL) {
		return;
	}
	bool slot = s->scanSlot != 0 && s->width == SWEET_FB_LOGICAL_W && s->height == SWEET_FB_LOGICAL_H;
	uint32_t sid = s->id, n = s->scanSlot;
	s->release();
	if (!slot || !OSCompareAndSwap8(1, 0, (volatile UInt8 *)&sweet_scan_auto_armed)) {
		return;
	}
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET SCANOUT: auto on (1046 default sweetscanout=2): swap %u from %s, surface %u in slot %u\n",
	    get32(rec, 0x50), name, sid, n - 1);
	sweet_scan_set(fb, true, "auto (1046: sweetscanout=2, first slot swap)");
}

/*
 * 1105: kern.sweet_scangate_mode / kern.sweet_scangate_tribuf (bsd/dev/sweet_gpu.c) and p scanout gate|tribuf. Before the
 * scanout has started (no thread yet) the value is kept and taken at the start instead of the boot-arg.
 */
static int sweet_scan_gate_req = -1, sweet_scan_tribuf_req = -1;
static int sweet_scan_queue_req = -1;   /* 1112: kern.sweet_scangate_queue written before the scanout's start */

/*
 * 1112: the frame queue is the default: tribuf 2 (SwapWait busy until a later swap latched, 1109) + queue 1 (a newer swap
 * queues behind a pending one, 1110), with the gate's default 1 (boot-arg sweetscangate). Boot-arg sweetframeq=0 restores
 * the old defaults (tribuf 0, or 1 with sweetfbx 0x40; queue 0). kern.sweet_scangate_tribuf / _queue / _mode stay
 * writable and win over these defaults (before or after the scanout's start).
 */
static int
sweet_frameq_boot(void)
{
	static int fq = -1;
	if (fq < 0) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetframeq", &v, sizeof(v));
		fq = v != 0;
	}
	return fq;
}

static int
sweet_scan_tribuf_default(void)
{
	return sweet_frameq_boot() ? 2 : ((sweet_fbx & SWEET_FBX_TRIBUF) != 0);
}

extern "C" int sweet_scan_gate_get(int what);
int
sweet_scan_gate_get(int what)
{
	if (what == 2) {
		if (!sweet_scan.thread) {
			return sweet_scan_queue_req >= 0 ? sweet_scan_queue_req : sweet_frameq_boot();   /* 1112 */
		}
		return sweet_scan.queue ? 1 : 0;        /* 1110 */
	}
	if (!sweet_scan.thread) {
		int r = what == 0 ? sweet_scan_gate_req : sweet_scan_tribuf_req;
		if (r >= 0) {
			return r;
		}
		if (what == 0) {
			uint32_t g = 1;
			PE_parse_boot_argn("sweetscangate", &g, sizeof(g));
			return g <= 4 ? (int)g : 1;
		}
		return sweet_scan_tribuf_default();     /* 1112 */
	}
	return what == 0 ? (int)sweet_scan.gate : (sweet_scan.pollRel ? 2 : (sweet_scan.tribuf ? 1 : 0));
}

extern "C" int sweet_scan_gate_put(int what, int v);
int
sweet_scan_gate_put(int what, int v)
{
	AppleCLCD *fb = sweet_fb_instance;
	if (v < 0 || v > (what == 0 ? 4 : (what == 1 ? 2 : 1))) {     /* 1109: gate 4, tribuf 2; 1110: queue 0|1 */
		return 22;     /* EINVAL (no errno.h in this file) */
	}
	if (what == 2) {
		/* 1110: the field alone (fLock not needed: read under fLock at each SwapEnd, a change takes the next one) */
		sweet_scan.queue = v != 0;
		sweet_scan_queue_req = v;       /* 1112: also the value at the scanout's start */
		IOLog("SWEET SCANOUT: queue %d (%s)\n", v, v ? "a newer swap queues behind a pending one (no replacement until 3 wait)" :
		    "a newer swap replaces a pending one (gate 2 queues)");
		return 0;
	}
	if (what == 0) {
		sweet_scan_gate_req = v;
	} else {
		sweet_scan_tribuf_req = v;
	}
	if (fb == NULL || !sweet_scan.thread) {
		IOLog("SWEET SCANOUT: %s %d at the scanout's start (1105)\n", what == 0 ? "gate" : "tribuf", v);
		return 0;
	}
	IOLockLock(fb->fLock);
	if (what == 0) {
		sweet_scan.gate = (uint32_t)v;
		sweet_scan_release_locked();    /* 0989: nothing armed survives a change */
		IOLockWakeup(fb->fLock, &sweet_scan, false);
	} else {
		sweet_scan.tribuf = v != 0;
		sweet_scan.pollRel = v == 2;    /* 1109 */
		sweet_scan.gateDepth = 2;
	}
	IOLockWakeup(fb->fLock, &sweet_scan.shownSeq, false);   /* waiters re-check under the new rule */
	IOLockUnlock(fb->fLock);
	if (what == 0) {
		IOLog("SWEET SCANOUT: gate %d (%s)\n", v, v == 0 ? "SwapEnd back pressure" : v == 1 ? "GPU submits wait on the CPU" :
		    v == 2 ? "the CP waits for the latch" : v == 3 ? "per surface: a display submit waits while its own surface is held" :
		    "none: CoreAnimation's ring (tribuf 2) keeps it off held surfaces");
	} else {
		IOLog("SWEET SCANOUT: tribuf %d (SwapWait polls %s, gate %s)\n", v,
		    v == 2 ? "busy until a later swap latched" : v ? "busy until latched" : "done at once for the newest",
		    v ? "K-1 on screen at chain 3" : "K on screen");
	}
	return 0;
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
		uint32_t g = 1;         /* 0989: boot-arg sweetscangate=0|1|2 (default 1, the CPU gate); 1105: 3 */
		PE_parse_boot_argn("sweetscangate", &g, sizeof(g));
		sweet_scan.gate = g <= 4 ? g : 1;
		/* 0996; 1112: tribuf 2 + queue 1 unless boot-arg sweetframeq=0 */
		int tb = sweet_scan_tribuf_default();
		sweet_scan.tribuf = tb != 0;
		sweet_scan.pollRel = tb == 2;
		sweet_scan.queue = (sweet_scan_queue_req >= 0 ? sweet_scan_queue_req : sweet_frameq_boot()) != 0;
		if (sweet_scan_gate_req >= 0) {
			sweet_scan.gate = (uint32_t)sweet_scan_gate_req;        /* 1105: kern.sweet_scangate_mode before the start */
		}
		if (sweet_scan_tribuf_req >= 0) {
			sweet_scan.tribuf = sweet_scan_tribuf_req != 0;         /* 1105: kern.sweet_scangate_tribuf */
			sweet_scan.pollRel = sweet_scan_tribuf_req == 2;        /* 1109 */
		}
		IOLog("SWEET SCANOUT: start: gate %u tribuf %u queue %u (1112: frame queue %s, boot-arg sweetframeq)\n",
		    sweet_scan.gate, sweet_scan.pollRel ? 2 : (sweet_scan.tribuf ? 1 : 0), sweet_scan.queue ? 1 : 0,
		    sweet_frameq_boot() ? "default" : "off");
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
 *   p scanout 0|1   0976: the direct scanout off / on (needs sweetscanout 1|2|3; 1046: 2 by default); no argument: the state
 *   p scanout gate 0|1|2|3 0985: SwapEnd back pressure (0), the swapper's GPU submits wait (1, default), 0989: the
 *                          CP waits for the latch (2; boot-arg sweetscangate); 1105: 3 per surface (kern.sweet_scangate_mode)
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
	if (end - s >= 2 && !strncmp(s, "cm", 2) && (end - s == 2 || s[2] == ' ')) {
		sweet_cm_cmd(this, s + 2, end);         /* 1116 */
		return;
	}
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
			(void)sweet_scan_gate_put(0, v < end && *v >= '0' && *v <= '4' ? *v - '0' : 1);   /* 1105: 3; 1109: 4 */
		} else if (end - q >= 6 && !strncmp(q, "tribuf", 6)) {
			/* 0996: p scanout tribuf 0|1 */
			const char *v = q + 6;
			while (v < end && *v == ' ') {
				v++;
			}
			(void)sweet_scan_gate_put(1, v < end && (*v == '1' || *v == '2') ? *v - '0' : 0);  /* 1105; 1109: 2 */
		} else if (q < end && (*q == '0' || *q == '1')) {
			if (sweet_scan_auto_armed) {
				sweet_scan_auto_armed = false;  /* 1046: the poke decides from now on */
				IOLog("SWEET SCANOUT: auto disarmed (p scanout %c)\n", *q);
			}
			sweet_scan_set(this, *q == '1', "p scanout");
		} else {
			sweet_scan_set(this, sweet_scan.on, "p scanout (state)");
		}
		return;
	}
	IOLog("SWEET FB: poke p: fc | hz 60|90 [tc] | fbx HEX | mdp dump|smmu|test|scale|ca|restore | scanout 0|1 | pace N [MIN_US] | cm ...\n");
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
			sweet_vlock_late++;     /* 1042 */
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
			sweet_vlock_lost++;
		} else if (cal) {
			sweet_vsync_cal_ok++;
			sweet_vlock_ok++;
			fFcEarlyUs = fFcEarlyUs >= 350 ? fFcEarlyUs - 50 : 300;
		}
		uint32_t frames = f - fFcFrame;
		if (fFcEdge != 0 && frames >= 1 && frames < 4096) {
			/* 1042: how far the edge found is from the one predicted with the old period (the phase corrected) */
			uint64_t pred = fFcEdge + (uint64_t)frames * period, d = t > pred ? t - pred : pred - t;
			sweet_vlock_drift_abs += d;
			sweet_vlock_driftN++;
			if (d > sweet_vlock_drift_max) {
				sweet_vlock_drift_max = d;
			}
		}
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
	/*
	 * 1111: a compressed (UBWC) surface is only ever scanned out directly: every present below reads it as linear rows
	 * (CPU copy, GPU blit, crop copy) and would put its compressed bytes on the panel. Such a frame is not presented
	 * (the last one stays); the direct path shows the next.
	 */
	for (uint32_t l = 0; sweet_surface_ubwc_boot() != 0 && mask != 0 && l < SWEET_FB_LAYERS; l++) {
		uint32_t id = (mask & (1u << l)) ? get32(rec, 0x54 + 4 * l) : 0;
		SweetSurface *u = id != 0 ? sweet_surface_copy(id) : NULL;
		bool live = u != NULL && u->ubwcLive;
		OSSafeReleaseNULL(u);
		if (live) {
			static uint32_t logs;
			sweet_ubwc_st.skipped++;
			if (logs < 4) {
				logs++;
				IOLog("SWEET FB: swap %u: surface %u is UBWC-compressed: not presented (only the direct scanout shows it)\n",
				    get32(rec, 0x50), id);
			}
			return;
		}
	}


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
	const char *cmNote = NULL;      /* 1116 */
	SWEET_TRACE_START(SWT_IOMFB_SEL, selector, args->scalarInputCount > 0 ? args->scalarInput[0] : 0,
	    args->scalarInputCount > 1 ? args->scalarInput[1] : 0, args->structureInputSize);

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin: {
		uint64_t ptAbs = __improbable(sweet_pace_trace) ? mach_absolute_time() : 0;    /* 1113 */
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		if (ptAbs != 0) {
			sweet_pt_begin((uint32_t)out[0], ptAbs);
		}
		outCount = 1;
		sweet_swaptrace('B', (uint32_t)out[0]);  /* 0987 */
		break;
	}
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
				if (!off && sweet_cm_st.touched) {
					sweet_cm_kick_locked(fFB);      /* 1116: PCC again, in case the display power lost it */
				}
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
		if (__improbable(sweet_pace_trace & 4)) {
			sweet_pt_wait(ret);     /* 1114: CA's can_update_ poll and its answer */
		}
		break;
	/*
	 * 1116: colour management (see sweet_cm_st). With kern.sweet_cm 0 the answers are as before 1116 (success, outputs
	 * zero-filled below); 55/17/50 are only stored.
	 */
	case kSweetFBSetMatrix:
		if (args->scalarInputCount >= 1 && args->structureInputDescriptor == NULL && args->structureInput != NULL &&
		    args->structureInputSize >= 72) {
			int64_t m[9];
			memcpy(m, args->structureInput, sizeof(m));
			cmNote = sweet_cm_set_matrix(fFB, (uint32_t)args->scalarInput[0], m);
		}
		break;
	case kSweetFBGetMatrix:
		if ((sweet_cm & 1) && args->structureOutputDescriptor == NULL && args->structureOutput != NULL &&
		    args->structureOutputSize >= 72) {
			int64_t m[9];
			sweet_cm_get_matrix(fFB, args->scalarInputCount ? (uint32_t)args->scalarInput[0] : 9, m, &cmNote);
			bzero(args->structureOutput, args->structureOutputSize);
			memcpy(args->structureOutput, m, sizeof(m));
			structDone = true;
		}
		break;
	case kSweetFBSetGammaTable:
		if (args->structureInputDescriptor == NULL && args->structureInput != NULL && args->structureInputSize > 0) {
			uint32_t n = args->structureInputSize < SWEET_CM_GAMMA_BYTES ? args->structureInputSize : SWEET_CM_GAMMA_BYTES;
			IOLockLock(fFB->fLock);
			bzero(sweet_cm_st.gamma, sizeof(sweet_cm_st.gamma));
			memcpy(sweet_cm_st.gamma, args->structureInput, n);
			sweet_cm_st.gammaSize = n;
			IOLockUnlock(fFB->fLock);
			cmNote = "(stored; GC not written)";
		}
		break;
	case kSweetFBGetGammaTable:
		if ((sweet_cm & 1) && args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
			IOLockLock(fFB->fLock);
			bzero(args->structureOutput, args->structureOutputSize);
			uint32_t n = sweet_cm_st.gammaSize < args->structureOutputSize ? sweet_cm_st.gammaSize : args->structureOutputSize;
			memcpy(args->structureOutput, sweet_cm_st.gamma, n);
			IOLockUnlock(fFB->fLock);
			structDone = true;
			cmNote = n ? "(the stored table)" : "(zeros: no table set yet)";
		}
		break;
	case kSweetFBSetBrightnessCorrection:
		if (args->scalarInputCount >= 1) {
			sweet_cm_st.bc = (uint32_t)args->scalarInput[0];
		}
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
	if (__improbable(sweet_cm_trace & 1)) {
		sweet_cm_trace_sel(selector, args, ret, cmNote);        /* 1116 */
	}
	if ((sweet_bl_trace & 2) && selector != kSweetFBSwapBegin && selector != kSweetFBSwapEnd &&
	    selector != kSweetFBSwapWait && selector != kSweetFBGetCurrentAbsoluteTime &&
	    selector != kSweetFBNotifications) {         /* 1100: 72 toggles the vsync callback ~20 times a second */
		uint32_t dropped = 0;
		if (sweet_blt_take(&dropped)) {         /* 1099 */
			char name[32];
			proc_selfname(name, sizeof(name));
			if (dropped != 0) {
				IOLog("SWEET BLT: (%u lines dropped by the rate limit)\n", dropped);
			}
			IOLog("SWEET BLT: %s[%d] IOMFB sel %u in %u/%u [%#llx %#llx] out %u/%u [%#llx] -> 0x%x\n", name,
			    proc_selfpid(), selector, args->scalarInputCount, args->structureInputSize,
			    args->scalarInputCount ? args->scalarInput[0] : 0ULL,
			    args->scalarInputCount > 1 ? args->scalarInput[1] : 0ULL, args->scalarOutputCount,
			    args->structureOutputSize, args->scalarOutputCount ? args->scalarOutput[0] : 0ULL, ret);
		}
	}
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
