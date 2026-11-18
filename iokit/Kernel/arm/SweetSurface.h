/*
 * Built-in IOSurface for SWEET (see SweetSurface.cpp).
 *
 * Shared with SweetFramebuffer.cpp: the framebuffer looks surfaces up by
 * id when a swap names them and publishes the panel memory itself as the
 * default framebuffer surface.
 */
#ifndef _SWEET_SURFACE_H
#define _SWEET_SURFACE_H

#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>
#include <mach/mach_types.h>

#define SWEET_SURFACE_MAX_PLANES        20

struct sweet_surface_plane {
	uint32_t width, height, bytesPerRow, offset, size;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
};

class SweetSurface : public OSObject {
	OSDeclareDefaultStructors(SweetSurface);

public:
	uint32_t id;
	uint32_t width, height, bytesPerRow, offset, pixelFormat, allocSize;
	uint16_t bytesPerElement;
	uint8_t  elementWidth, elementHeight;
	uint32_t cacheMode, planeCount, parentID;
	struct sweet_surface_plane planes[SWEET_SURFACE_MAX_PLANES];

	IOMemoryDescriptor       *memory;       /* pixels (buffer or physical) */
	IOOptionBits              mapCache;     /* cache mode for user mappings */
	IOBufferMemoryDescriptor *state;        /* shared state page, read-only for users */
	IOMemoryMap              *kernelMap;    /* lazily created for kernel copies */
	uint32_t                  scanSlot;     /* 0975: 1 + its scanout slot (sweetscanout), 0 = ordinary memory */
	uint64_t                  scanPA;       /* 0975: the slot's physical address (contiguous, below 4 GB) */
	volatile SInt32           displayUse;   /* 0976: frames of it the display holds (queued, flipping, shown) */
	volatile SInt32           clientUse;    /* 0976: IOSurfaceIncrementUseCount - Decrement from userland */
	volatile SInt32           clientUse1;   /* 0983: ... category 1 */
	/*
	 * 1111: UBWC display surface (kern.sweet_ubwc_disp, boot-arg sweetubwc). ubwcReady: a scanout-slot surface
	 * created while the switch was on, sized for the compressed layout below (offsets from the surface's base = its
	 * slot PA; meta plane first, then the colour plane, as Mesa's fdl6 and msm_media_info's RGBA8888_UBWC put
	 * them). Its pixels stay linear (bytesPerRow, offset 0) until a GPU client asks the BO's NativeAdreno metadata
	 * (hakios-metal under HM_UBWC_DISPLAY=1): from then on ubwcLive = 1 and the display engine scans it
	 * compressed. All zero = linear, as before 1111.
	 */
	uint32_t                  ubwcMeta, ubwcMetaPitch, ubwcMetaSize, ubwcData, ubwcDataPitch;
	uint8_t                   ubwcReady;
	volatile uint8_t          ubwcLive;
	/* 1258 (kern.sweet_surf_inuse, SweetSurface.cpp sweet_inuse_*): GPU holds */
	uint64_t                  pa0;          /* physical address of byte 0 (GPU imports are matched by it) */
	volatile SInt32           gpuHold;      /* 1 while a GPU job that uses it has not landed */
	uint32_t                  gpuFence;     /* the newest such job's fence (0: being submitted) */
	uint32_t                  gpuWriteFence; /* the newest job that writes it (0: none known) */
	uint64_t                  gpuHoldAbs;   /* when the hold began or was last moved on */

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;

	/* Kernel address of the pixels (mapped on first use), or 0. */
	uintptr_t kernelAddress();
};

/* Retained surface for an id, or NULL. */
SweetSurface *sweet_surface_copy(uint32_t id);

/*
 * 0975: scanout slots (boot-arg sweetscanout, SweetSurface.cpp). The surface in slot n, retained, and the slot's
 * address (also when it is free: slots are never given back), or NULL.
 */
#define SWEET_SCAN_SLOTS                7
#define SWEET_SCAN_SLOT_LEN             0xab8000ULL     /* 4608 x 2436, 16 KB pages */
/*
 * 1111: with boot-arg sweetubwc=1|2 the slots are made this long instead: a 1125x2436 RGBA8 UBWC surface, meta
 * 128 x 624 (80 KB) + colour 4608 x 2448 (11016 KB) = 0xad6000, in 16 KB pages. Slot 0 then ends 0xa1ed8000, still
 * below the backlight packet (0xa2200000).
 */
#define SWEET_SCAN_SLOT_LEN_UBWC        0xad8000ULL
uint64_t sweet_surface_scan_len(void);
/* 1111: boot-arg sweetubwc (0 default: as 1110; 1 UBWC-sized slots, kern.sweet_ubwc_disp 0; 2 also kern.sweet_ubwc_disp 1) */
uint32_t sweet_surface_ubwc_boot(void);
extern "C" int sweet_ubwc_disp;       /* 1111: kern.sweet_ubwc_disp (bsd/dev/sweet_gpu.c) */
/*
 * 1111: the scanout-slot surface whose memory starts at physical address pa (retained), or NULL: SweetAccelerator's
 * GetMetadata finds the display surface behind an imported BO this way.
 */
SweetSurface *sweet_surface_scan_find_pa(uint64_t pa);
#define SWEET_UBWC_VERSION              3       /* SM7250: Adreno 620 UBWC 3 */
#define SWEET_UBWC_HBB                  14      /* SweetAccelerator GetDeviceInfo highest_bank_bit */
uint32_t sweet_surface_scan_mode(void);
SweetSurface *sweet_surface_scan_copy(uint32_t slot, uint64_t *pa);
/* 0979: the slots' memory made at boot (AppleCLCD::start, before userland fragments RAM); a slot's memory (retained) */
void sweet_surface_scan_reserve(void);
/*
 * 0983: a scanout-slot surface's use counts into its shared state page (+0x18: category 0 low, 1 high), as
 * IOSurfaceIsInUse reads them: category 0 = userland's + the frames the display holds. CA's swap chain then sees
 * the surface on screen as in use, as on an iPhone (IOMFB keeps a use count on what it scans out).
 */
void sweet_surface_publish_use(SweetSurface *s);
/*
 * 1258: kern.sweet_surf_inuse (boot-arg sweetsurfinuse, default 0 = 0983 exactly). Non-zero: every surface publishes
 * clientUse + displayUse + gpuHold at state page +0x18 (what IOSurfaceIsInUse reads); bit 1 (2) a 5 s klog line,
 * bit 2 (4) no GPU holds (use counts only), bit 3 (8) IOSurfaceLock waits for the GPU jobs that use the surface.
 * GPU imports (SweetAccelerator v2 BoImport) are matched to a surface by the physical address of their first byte.
 */
extern "C" int sweet_surface_inuse_mode(void);
uint32_t sweet_surface_find_pa(uint64_t pa);    /* id of the surface whose byte 0 is at pa, else 0 */
#define SWEET_SURF_FENCE_PENDING        0u              /* hold before the submission: its fence is not known yet */
#define SWEET_SURF_FENCE_CANCEL         0xffffffffu     /* the submission failed: pending holds go */
/*
 * Each surface (by id, if its byte 0 is still at pas[i]) is in use until fence lands; writes[i]: the job writes it.
 * Called with SWEET_SURF_FENCE_PENDING before the submission and with its fence (or CANCEL) after it.
 */
void sweet_surface_gpu_hold(const uint32_t *ids, const uint64_t *pas, const uint8_t *writes, uint32_t n, uint32_t fence);
void sweet_surface_import_stat(bool matched);
void sweet_surface_submit_stat(bool listed, uint32_t named, uint32_t held, bool overflow);
IOMemoryDescriptor *sweet_surface_scan_md(uint32_t slot, uint64_t *pa);

/*
 * Publish physical memory (the panel framebuffer) as a surface. Returns a
 * retained surface that stays in the id table until released by the caller
 * and every user.
 */
SweetSurface *sweet_surface_create_physical(uint64_t phys, uint32_t width,
    uint32_t height, uint32_t bytesPerRow, uint32_t pixelFormat);

/* Send right for the surface in task (IOSurfaceLookupFromMachPort). */
mach_port_name_t sweet_surface_port_in_task(SweetSurface *surface, task_t task);

#endif /* SWEET */
#endif /* _SWEET_SURFACE_H */
