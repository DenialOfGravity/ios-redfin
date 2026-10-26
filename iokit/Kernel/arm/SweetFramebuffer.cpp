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
int sweet_gpu_cpu_target(uint64_t *pa);         /* 0946 (sweetfbx & 8) */
void sweet_gpu_cpu_flip(void);
void sweet_gpu_present_tag(uint64_t swap_abs);
void sweet_gpu_present_visible(uint64_t *sum_abs, uint64_t *n);
void sweet_cpufreq_boost(int why);              /* 0970: bsd/dev/sweet_cpufreq.c */
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
 * sweetfb_hz=90: the panel at 90 Hz (DCS 0x53 0x38) and 11.1 ms ticks; anything else is 60 Hz.
 */
#define SWEET_FBX_MODES         0x1
#define SWEET_FBX_NOCLEAN       0x2
#define SWEET_FBX_SWAPQ         0x4
#define SWEET_FBX_CPUPRESENT    0x8
static uint32_t sweet_fbx;

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
	bool          presentCPU(const uint8_t *rec);
	uintptr_t     cpuBuffer(uint64_t pa);
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

static AppleCLCD *sweet_fb_instance;    /* 0943: for the poke commands (p fc, p hz) */

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
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
	void publish(uint32_t level);
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
	sweet_fb_instance = this;
	IOLog("SWEET FB: sweetfbx %#x, panel %u Hz\n", sweet_fbx, fHz);
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
	return true;
}

static uint64_t sweet_fb_swap_count;

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
	sender->wakeAtTime(fb->fNextVsync);
	sweet_gpu_flip_poll();	/* 0905: a finished frame in the back buffer goes on screen at this vsync */
	sweet_gpu_present_poll();	/* 0943: blit->fence latency */
	fb->statLine(now);		/* 0943: every 5 s, the frame statistics */
	/*
	 * CA::WindowServer::IOMFBServer::vsync_callback(fb, a1, a2, a3, refcon): a1 only goes to kdebug, a2 is
	 * the vsync's host time and a2 + a3 the next one (a3 = the refresh period, also CA's CVTimeStamp
	 * videoRefreshPeriod). a3 was 0: CA saw every frame's deadline as already past.
	 */
	uint64_t period = fb->fVsyncAbs;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		(uint32_t)period, (uint32_t)(period >> 32),
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
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
		fb->present(rec);
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

void
AppleCLCD::presentLoop(void *arg)
{
	AppleCLCD *fb = (AppleCLCD *)arg;
	bool bound = false;
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
			IOLockUnlock(fb->fLock);
			uint64_t t0 = mach_absolute_time();
			fb->fPresentBusy = false;
			fb->fPresentingAbs = fb->fPresentRecAbs;	/* 0946 */
			fb->present(fb->fPresentRec);
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
			IOLockUnlock(fb->fLock);
			if (shown) {
				fb->fStatPresents++;
				fb->statShown(fb->fPresentRecAbs);
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
			fb->present(fb->fPresentRec);
			fb->fStatPresentAbs += mach_absolute_time() - t0;
			if (fb->fPresentBusy) {
				IOLockLock(fb->fLock);
				fb->fDirty = true;
				fb->fStDropped += fb->fPendingAbs != fb->fPresentRecAbs;	/* 0943 */
				IOLockUnlock(fb->fLock);
			} else {
				fb->fStatPresents++;
				fb->statShown(fb->fPresentRecAbs);
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
	fStLastSwap = now;
	fStSwaps++;
	fStDropped += fDirty;
	if (fSwapQ && queueSwap(rec, now)) {
		IOLockUnlock(fLock);
		return;
	}
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fPendingAbs = now;
	fDirty = true;
	IOLockUnlock(fLock);
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
sweet_mdp_pattern(uint32_t w, uint32_t h, uint32_t stride, uint32_t seam)
{
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	uint64_t len = round_page((uint64_t)stride * h);
	if (w == 0 || h == 0 || stride < w * 4 || len > SWEET_MDP_TEST_LEN) {
		return false;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(SWEET_MDP_TEST_PA, len, kIODirectionInOut);
	IOMemoryMap *map = md != NULL ? md->map(kIOMapWriteCombineCache) : NULL;
	OSSafeReleaseNULL(md);
	if (map == NULL) {
		return false;
	}
	uint8_t *base = (uint8_t *)map->getVirtualAddress();
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
	__asm__ volatile ("dsb sy" ::: "memory");
	map->release();
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
sweet_mdp_scale(void)
{
	const uint32_t sw = SWEET_FB_LOGICAL_W, sh = SWEET_FB_LOGICAL_H;
	uint32_t ver = sweet_mdp_rd(MDP_VIG(0) + QS_VERSION), ver1 = sweet_mdp_rd(MDP_VIG(1) + QS_VERSION);
	uint32_t dw0 = sweet_mdp.old[0][MR_OUT_SIZE] & 0xffff, dw1 = sweet_mdp.old[1][MR_OUT_SIZE] & 0xffff;
	uint32_t dh = sweet_mdp.old[0][MR_OUT_SIZE] >> 16, W = dw0 + dw1;
	if (ver < 0x2000 || ver != ver1 || dh == 0 || W == 0 || dh != sweet_mdp.old[1][MR_OUT_SIZE] >> 16) {
		IOLog("SWEET MDP: scale: scaler version %x/%x or outputs %x/%x not handled\n", ver, ver1,
		    sweet_mdp.old[0][MR_OUT_SIZE], sweet_mdp.old[1][MR_OUT_SIZE]);
		return false;
	}
	if (!sweet_mdp_pattern(sw, sh, SWEET_MDP_SURF_STRIDE, sw / 2 + 1)) {
		IOLog("SWEET MDP: scale: no pattern\n");
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
		sweet_mdp_wr(b + SSPP_SRC0, (uint32_t)SWEET_MDP_TEST_PA);
		sweet_mdp_wr(b + SSPP_YSTRIDE0, (sweet_mdp.old[p][MR_YSTRIDE0] & 0xffff0000u) | SWEET_MDP_SURF_STRIDE);
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
		IOLog("SWEET MDP: scale: vig%u source x %u w %u phase %#x -> %ux%u, step %#llx/%#llx op %#x\n", p, sx, cw,
		    phase, dw, dh, step_x, step_y, op);
	}
	sweet_mdp_flush("scale");
	sweet_mdp_pipes("scale");
	return true;
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
	uint32_t secs = 10;
	if (q < end && *q >= '0' && *q <= '9') {
		secs = 0;
		while (q < end && *q >= '0' && *q <= '9') {
			secs = secs * 10 + (uint32_t)(*q++ - '0');
		}
		if (secs > 600) {
			secs = 600;
		}
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
	} else if ((wl == 4 && !strncmp(w, "test", 4)) || (wl == 5 && !strncmp(w, "scale", 5))) {
		bool scale = wl == 5;
		if (sweet_mdp.timer == NULL) {
			sweet_mdp.timer = IOTimerEventSource::timerEventSource(this, sweet_mdp_timer_fired);
			if (sweet_mdp.timer != NULL && fWorkLoop->addEventSource(sweet_mdp.timer) != kIOReturnSuccess) {
				OSSafeReleaseNULL(sweet_mdp.timer);
			}
		}
		if (sweet_mdp_take(this, scale ? "scale" : "test")) {
			if (!(scale ? sweet_mdp_scale() : sweet_mdp_test())) {
				sweet_mdp_restore_locked(this, "failed");
			} else if (secs != 0 && sweet_mdp.timer != NULL) {
				sweet_mdp.timer->setTimeoutMS(secs * 1000);
				IOLog("SWEET MDP: %s on screen for %u s (p mdp restore puts it back sooner)\n",
				    scale ? "scale" : "test", secs);
			} else {
				IOLog("SWEET MDP: %s on screen until p mdp restore\n", scale ? "scale" : "test");
			}
		}
	} else if (wl == 7 && !strncmp(w, "restore", 7)) {
		sweet_mdp_restore_locked(this, "poke");
	} else {
		IOLog("SWEET MDP: p mdp dump | smmu [walk] | test [SECS] | scale [SECS] | restore\n");
	}
	IOLockUnlock(sweet_mdp.lock);
}

/*
 * Poke commands (bsd/dev/sweet_poke.c "p WORD"):
 *   p fc            INTF1's TE frame counter (and INTF frame counter) twice, 1 s apart: the panel's real rate,
 *                   with the tear-check configuration
 *   p hz 60|90 [tc] the panel's rate now (DCS 0x53), ticks and SyncRate with it; tc: tear-check line clock too
 *   p mdp ...       0948: display-engine dump and scanout tests (above)
 */
void
AppleCLCD::fbCmd(const char *s, const char *end)
{
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
	IOLog("SWEET FB: poke p: fc | hz 60|90 [tc] | fbx HEX | mdp dump|smmu|test|scale|restore\n");
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
		} else if ((src = s->kernelAddress()) == 0) {
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
	nanoseconds_to_absolutetime(500000, &lead);
	nanoseconds_to_absolutetime(300000, &early);
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
		uint32_t frames = f - fFcFrame;
		if (fFcEdge != 0 && frames >= 1 && frames < 4096) {
			uint64_t p = (t - fFcEdge) / frames;
			if (p > fVsyncAbs * 3 / 4 && p < fVsyncAbs * 5 / 4) {
				fFcPeriod = p;
				period = p;
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
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
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
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
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
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (!structDone && args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
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
