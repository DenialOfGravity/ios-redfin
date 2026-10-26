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
 * sweetfb_hz=90: the panel at 90 Hz (DCS 0x53 0x38) and 11.1 ms ticks; anything else is 60 Hz.
 */
#define SWEET_FBX_MODES         0x1
#define SWEET_FBX_NOCLEAN       0x2
#define SWEET_FBX_SWAPQ         0x4
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
	fQ = (sweet_fbx & SWEET_FBX_SWAPQ) && pt ? (uint8_t *)IOMallocData(2 * SWEET_FB_SWAP_REC_SIZE) : NULL;
	if (pt && fPresentRec != NULL && IOCreateThread(&AppleCLCD::presentLoop, this) != NULL) {
		fPresentThread = true;
		fSwapQ = fQ != NULL;
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
	for (;;) {
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
	IOLockLock(fLock);
	/* 0943: swap->swap interval (ms buckets, pauses over 500 ms not counted), and frames never shown */
	if (fStLastSwap != 0) {
		uint64_t ns;
		absolutetime_to_nanoseconds(now - fStLastSwap, &ns);
		uint64_t t = ns / 100000;       /* 0.1 ms */
		if (t <= 5000) {
			fStBucket[t <= 111 ? 0 : t <= 180 ? 1 : t <= 260 ? 2 : t <= 350 ? 3 : 4]++;
		}
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
	uint64_t blitq = 0, fence = 0, fenceN = 0;
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
	if (swaps == 0 && shown == 0) {
		return;
	}
	IOLog("SWEET FB: 5s: swaps %u shown %u dropped %u | le11 %u le18 %u le26 %u le35 %u gt35 %u | "
	    "p50 %u.%u p95 %u.%u ms | blitq %llu us fence %llu us | hz %u\n", swaps, shown, dropped,
	    b[0], b[1], b[2], b[3], b[4], p50 / 10, p50 % 10, p95 / 10, p95 % 10, blitq, fence, fHz);
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
 * Poke commands (bsd/dev/sweet_poke.c "p WORD"):
 *   p fc            INTF1's TE frame counter (and INTF frame counter) twice, 1 s apart: the panel's real rate,
 *                   with the tear-check configuration
 *   p hz 60|90 [tc] the panel's rate now (DCS 0x53), ticks and SyncRate with it; tc: tear-check line clock too
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
	IOLog("SWEET FB: poke p: fc | hz 60|90 [tc]\n");
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
			IOLog("SWEET FB: TE frame counter stopped at %u (int_count %08x): free-running ticks\n", frame, ic);
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
	uint32_t *dst = (uint32_t *)(fPixels + (uint64_t)y * fRowBytes) + x;
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

	if (sweet_gpu_ready() && presentGPU(rec)) {
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
