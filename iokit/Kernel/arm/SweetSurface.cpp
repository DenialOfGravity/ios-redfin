/*
 * Built-in IOSurface for SWEET.
 *
 * Apple's IOSurface kext is closed. IOSurface.framework (iOS 16.7) finds
 * IOServiceNameMatching("IOCoreSurfaceRoot"), opens it with type 0 and
 * talks to the user client below:
 *
 *   sel 13  limits: struct out 0x18, u16 at +0x16 must be 1 (ABI version),
 *           +0 offset alignment mask, +4 bytes-per-row alignment mask
 *   sel 0   create: scalar [parent id], struct in = serialized property
 *           dictionary (binary or XML), struct out = lock result (0xa68)
 *   sel 4   lookup by id: scalar [id], struct out = lock result
 *   sel 34  lookup by mach port: scalar [port name], struct out = lock result
 *   sel 35  create mach port: scalars [id, options], scalar out [port name]
 *   traps   0/1 use count +/- (id, category), 2 lock / 3 unlock
 *           (id, options, &seed), 4/5 release (id)
 *
 * The lock result tells the library where the surface lives in the
 * caller's address space: +0 pixels, +8 shared state page (registry id,
 * seed at +0xc), +0x10 timestamps, then id, sizes and geometry (layout in
 * sweet_surface_fill_result). Every surface is wired kernel memory mapped
 * into each task that creates or looks it up; mappings are counted per
 * client and dropped on release or close.
 *
 * Values, notifications, accelerators and shared events are accepted and
 * ignored (logged once per selector).
 */
#include "SweetSurface.h"

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/system.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

enum {
	kSweetSurfaceCreate             = 0,
	kSweetSurfaceLookup             = 4,
	kSweetSurfaceLimits             = 13,
	kSweetSurfaceLookupFromPort     = 34,
	kSweetSurfaceCreatePort         = 35,
};

enum {
	kSweetSurfaceTrapIncrementUse   = 0,
	kSweetSurfaceTrapDecrementUse   = 1,
	kSweetSurfaceTrapLock           = 2,
	kSweetSurfaceTrapUnlock         = 3,
	kSweetSurfaceTrapRelease        = 4,
	kSweetSurfaceTrapReleaseFailed  = 5,
};

#define SWEET_SURFACE_RESULT_SIZE       0xa68
#define SWEET_SURFACE_MAX_ID            4096
#define SWEET_SURFACE_MAX_DIM           16384
#define SWEET_SURFACE_MAX_ALLOC         (256u << 20)
#define SWEET_SURFACE_MAX_PROPS         (64u << 10)
#define SWEET_SURFACE_ROW_ALIGN         64
#define SWEET_SURFACE_TIMESTAMPS        0x800   /* offset in the state page */
#define SWEET_SURFACE_STATE_SEED        0xc     /* u32 in the state page */
#define kIOSurfaceLockReadOnly          0x1

static IOLock       *sweet_surface_lock;
static uint32_t      sweet_surface_align_logged;	/* 0940: planes whose rows were rounded, first 16 logged */
static SweetSurface *sweet_surface_table[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_users[SWEET_SURFACE_MAX_ID];
static uint32_t      sweet_surface_next_id = 1;
static uint32_t      sweet_surface_creates_logged;
static uint64_t      sweet_surface_wired;       /* bytes of buffer surfaces */
#define SWEET_SURFACE_WIRED_MAX         (1536ull << 20)

OSDefineMetaClassAndStructors(SweetSurface, OSObject);

/*
 * 0975: scanout slots (boot-arg sweetscanout=1 or 2; docs/redfin/scanout-plan.md step d). backboardd's
 * framebuffer-sized surfaces (1125x2436 'BGRA', one plane, offset 0) get physically contiguous memory below 4 GB,
 * which the display engine reads directly: its apps-SMMU bank (SMR3 0xb40/0x402 -> bank 2) has translation off
 * (r35a, p mdp smmu). Slot 0 is the splash carve-out at 0xa1400000 (where p mdp test/scale drew), slots 1-6 come from
 * RAM, each allocated on first use and kept for the next surface. Every other create, and every create once the
 * slots are taken, gets ordinary memory as before.
 */
#define SWEET_SCAN_CARVE_PA     0xa1400000ULL
extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
static struct {
	IOMemoryDescriptor *md;
	uint64_t            pa;
	SweetSurface       *owner;          /* not retained: SweetSurface::free gives the slot back */
} sweet_scan_slot[SWEET_SCAN_SLOTS];
static IOLock *sweet_scan_lock;
static int sweet_scan_boot = -1;
static bool sweet_scan_ram_failed;

uint32_t
sweet_surface_scan_mode(void)
{
	if (sweet_scan_boot < 0) {
		/*
		 * 1046: absent = 2 (was 0). 0 off (blit, no slots); 1 slots + direct on at AppleCLCD::start; 2 slots + direct
		 * on by itself at the first swap of a scanout-slot surface (AppleCLCD::swap, sweet_scan_auto); 3 slots only
		 * (direct off until p scanout 1: what 2 was before 1046).
		 */
		int m = 2;
		PE_parse_boot_argn("sweetscanout", &m, sizeof(m));
		sweet_scan_boot = m < 0 ? 0 : m;
	}
	return (uint32_t)sweet_scan_boot;
}

/*
 * 1111: UBWC display surfaces. boot-arg sweetubwc: 0 (absent) = everything as 1110; 1 = the scanout slots are made
 * SWEET_SCAN_SLOT_LEN_UBWC long (+128 KB each, from the GPU pool, as the slots themselves), kern.sweet_ubwc_disp
 * starts 0; 2 = the same and kern.sweet_ubwc_disp starts 1. kern.sweet_ubwc_disp (bsd/dev/sweet_gpu.c) is read at
 * each scanout-slot create: 1 makes the new surface UBWC-ready (its UBWC layout fits the slot, allocSize grows to
 * it); a ready surface turns compressed only when a GPU client asks its metadata (SweetAccelerator GetMetadata,
 * while the sysctl is still 1). With sweetubwc 0 the sysctl has no effect.
 */
static int sweet_ubwc_boot_v = -1;

uint32_t
sweet_surface_ubwc_boot(void)
{
	if (sweet_ubwc_boot_v < 0) {
		int m = 0;
		PE_parse_boot_argn("sweetubwc", &m, sizeof(m));
		sweet_ubwc_boot_v = m < 0 || m > 2 ? 0 : m;
	}
	return (uint32_t)sweet_ubwc_boot_v;
}

uint64_t
sweet_surface_scan_len(void)
{
	return sweet_surface_ubwc_boot() != 0 ? SWEET_SCAN_SLOT_LEN_UBWC : SWEET_SCAN_SLOT_LEN;
}

/*
 * 1111: the UBWC layout of a W x H RGBA8 surface, as fdl6_layout (TILE6_3, ubwc, one level) and msm_media_info's
 * COLOR_FMT_RGBA8888_UBWC both give it: 16x4-pixel blocks, one meta byte each; meta pitch align(ceil(W/16), 64),
 * meta rows align(ceil(H/4), 16), the plane 4 KB aligned; colour pitch align(W x 4, 256), rows align(H, 16).
 * Meta at 0, colour after it. Returns the total bytes.
 */
static uint64_t
sweet_ubwc_layout(SweetSurface *s)
{
	uint32_t W = s->width, H = s->height;
	uint32_t mp = (((W + 15) / 16) + 63) & ~63u, mr = (((H + 3) / 4) + 15) & ~15u;
	uint32_t msz = (mp * mr + 4095) & ~4095u;
	uint32_t dp = (W * 4 + 255) & ~255u, dr = (H + 15) & ~15u;
	s->ubwcMeta = 0;
	s->ubwcMetaPitch = mp;
	s->ubwcMetaSize = msz;
	s->ubwcData = msz;
	s->ubwcDataPitch = dp;
	return (uint64_t)msz + (((uint64_t)dp * dr + 4095) & ~4095ULL);
}

/* 1111: (with the slot claimed) a UBWC-ready layout when kern.sweet_ubwc_disp is 1; NULL or why not */
static const char *
sweet_ubwc_ready(SweetSurface *s)
{
	if (sweet_surface_ubwc_boot() == 0 || !sweet_ubwc_disp) {
		return NULL;
	}
	uint64_t total = sweet_ubwc_layout(s);
	if (total > sweet_surface_scan_len() || s->ubwcDataPitch > 0xffff || s->ubwcDataPitch < s->width * 4) {
		s->ubwcMeta = s->ubwcMetaPitch = s->ubwcMetaSize = s->ubwcData = s->ubwcDataPitch = 0;
		return "UBWC layout larger than the slot";
	}
	s->ubwcReady = 1;
	if (total > s->allocSize) {
		s->allocSize = (uint32_t)total;         /* the client's IOSurfaceGetAllocSize covers both planes */
	}
	return NULL;
}

SweetSurface *
sweet_surface_scan_find_pa(uint64_t pa)
{
	uint32_t id = 0;
	if (pa == 0 || sweet_scan_lock == NULL) {
		return NULL;
	}
	IOLockLock(sweet_scan_lock);
	for (uint32_t n = 0; n < SWEET_SCAN_SLOTS; n++) {
		if (sweet_scan_slot[n].owner != NULL && sweet_scan_slot[n].pa == pa) {
			id = sweet_scan_slot[n].owner->id;
			break;
		}
	}
	IOLockUnlock(sweet_scan_lock);
	SweetSurface *s = id != 0 ? sweet_surface_copy(id) : NULL;
	if (s != NULL && (s->scanSlot == 0 || s->scanPA != pa)) {
		s->release();
		s = NULL;
	}
	return s;
}

static void
sweet_scan_release(SweetSurface *s)
{
	uint32_t n = s->scanSlot - 1;
	if (sweet_scan_lock != NULL && n < SWEET_SCAN_SLOTS) {
		IOLockLock(sweet_scan_lock);
		if (sweet_scan_slot[n].owner == s) {
			sweet_scan_slot[n].owner = NULL;
		}
		IOLockUnlock(sweet_scan_lock);
	}
	s->scanSlot = 0;
}

/*
 * The slot's memory, made on first use: the carve-out (slot 0) or, 0979, the GPU pool: HLOS RAM below 4 GB that XNU
 * does not manage (0xc4000000.., sweet_gpu.c gpool_ranges). r36a asked XNU for contiguous RAM below 4 GB and got
 * none: on redfin XNU's RAM starts at 4 GB (mkadt.py --highmem), and SRC0_ADDR is 32 bits with the MDP's SMMU bank
 * not translating. Pool memory is unmanaged like the carve-out: mapped write-combined, no cache maintenance.
 */
extern "C" int sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa);
static bool
sweet_scan_make_locked(uint32_t n)
{
	uint64_t pa = SWEET_SCAN_CARVE_PA;
	if (n != 0) {
		if (sweet_scan_ram_failed) {
			return false;
		}
		pa = 0;
		if (sweet_gpu_pool_alloc(sweet_surface_scan_len(), &pa) != 0 || pa == 0 || pa + sweet_surface_scan_len() > (1ULL << 32)) {
			IOLog("SWEET SURF: scanout slot %u: no %#llx bytes in the GPU pool below 4 GB (%#llx): no more RAM slots\n", n,
			    sweet_surface_scan_len(), pa);
			sweet_scan_ram_failed = true;   /* (a pool block above 4 GB would leak: the ranges are all below) */
			return false;
		}
	}
	sweet_scan_slot[n].md = IOMemoryDescriptor::withPhysicalAddress(pa, sweet_surface_scan_len(), kIODirectionInOut);
	sweet_scan_slot[n].pa = pa;
	return sweet_scan_slot[n].md != NULL;
}

static bool
sweet_scan_lock_ready(void)
{
	if (sweet_scan_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (lock != NULL && !OSCompareAndSwapPtr(NULL, lock, (void * volatile *)&sweet_scan_lock)) {
			IOLockFree(lock);
		}
	}
	return sweet_scan_lock != NULL;
}

/*
 * 0979: the slots' memory at boot. r36a: the first RAM slot, asked for when backboardd created its second surface
 * (68 s), found no contiguous 11 MB below 4 GB any more, so CA's 2-surface chain had one surface in a slot and one not.
 * AppleCLCD::start runs before userland: slot 0 (carve-out) and RAM slots 1..sweetscanslots (default 6) now.
 */
void
sweet_surface_scan_reserve(void)
{
	uint32_t want = SWEET_SCAN_SLOTS - 1, got = 0;
	if (sweet_surface_scan_mode() == 0 || !sweet_scan_lock_ready()) {
		return;
	}
	PE_parse_boot_argn("sweetscanslots", &want, sizeof(want));
	if (sweet_surface_ubwc_boot() != 0) {
		if (sweet_surface_ubwc_boot() == 2) {
			sweet_ubwc_disp = 1;
		}
		IOLog("SWEET SURF: ubwc: boot-arg sweetubwc=%u: slots %#llx bytes, kern.sweet_ubwc_disp %d\n",
		    sweet_surface_ubwc_boot(), sweet_surface_scan_len(), sweet_ubwc_disp);
	}
	if (want > SWEET_SCAN_SLOTS - 1) {
		want = SWEET_SCAN_SLOTS - 1;
	}
	IOLockLock(sweet_scan_lock);
	for (uint32_t n = 0; n <= want; n++) {
		if (sweet_scan_slot[n].md == NULL && !sweet_scan_make_locked(n)) {
			break;
		}
		IOMemoryMap *map = n != 0 ? sweet_scan_slot[n].md->map(kIOMapWriteCombineCache) : NULL;
		if (map != NULL) {
			bzero((void *)map->getVirtualAddress(), (size_t)sweet_surface_scan_len());     /* black, not old pool data */
			__asm__ volatile ("dsb sy" ::: "memory");
			map->release();
		}
		IOLog("SWEET SURF: scanout slot %u reserved: %s PA %#llx\n", n, n == 0 ? "carve-out" : "GPU-pool RAM",
		    sweet_scan_slot[n].pa);
		got++;
	}
	IOLockUnlock(sweet_scan_lock);
	IOLog("SWEET SURF: scanout: %u slots at boot (asked 1 + %u)\n", got, want);
}

IOMemoryDescriptor *
sweet_surface_scan_md(uint32_t slot, uint64_t *pa)
{
	IOMemoryDescriptor *md = NULL;
	*pa = 0;
	if (slot < SWEET_SCAN_SLOTS && sweet_scan_lock_ready()) {
		IOLockLock(sweet_scan_lock);
		md = sweet_scan_slot[slot].md;
		*pa = sweet_scan_slot[slot].pa;
		if (md != NULL) {
			md->retain();
		}
		IOLockUnlock(sweet_scan_lock);
	}
	return md;
}

/* create(): a framebuffer-sized backboardd surface goes into a free slot (zeroed): NULL, or why not. */
static const char *
sweet_scan_claim(SweetSurface *s)
{
	char name[32];
	if (sweet_surface_scan_mode() == 0) {
		return "scanout off (boot-arg sweetscanout)";
	}
	if (s->pixelFormat != 0x42475241 || s->planeCount > 1 || s->bytesPerElement != 4 || s->elementWidth != 1 ||
	    s->elementHeight != 1) {
		return "not one-plane BGRA";
	}
	if (s->offset != 0 || (s->bytesPerRow & 63) != 0 || s->bytesPerRow > 0xffff || s->allocSize > sweet_surface_scan_len()) {
		return "offset, rows or size";
	}
	proc_selfname(name, sizeof(name));
	if (strcmp(name, "backboardd") != 0) {
		return "not backboardd's";
	}
	if (!sweet_scan_lock_ready()) {
		return "no lock";
	}
	IOLockLock(sweet_scan_lock);
	uint32_t n;
	for (n = 0; n < SWEET_SCAN_SLOTS; n++) {
		if (sweet_scan_slot[n].owner == NULL && (sweet_scan_slot[n].md != NULL || sweet_scan_make_locked(n))) {
			break;
		}
	}
	if (n == SWEET_SCAN_SLOTS) {
		IOLockUnlock(sweet_scan_lock);
		return "slots full (or no memory for another)";
	}
	sweet_scan_slot[n].owner = s;
	sweet_scan_slot[n].md->retain();
	s->memory = sweet_scan_slot[n].md;
	s->scanSlot = n + 1;
	s->scanPA = sweet_scan_slot[n].pa;
	IOLockUnlock(sweet_scan_lock);
	const char *notUbwc = sweet_ubwc_ready(s);      /* 1111 */
	if (sweet_surface_ubwc_boot() != 0) {
		IOLog("SWEET SURF: ubwc: %ux%u surface in slot %u: %s (meta %#x pitch %u size %#x, colour %#x pitch %u, alloc %#x, "
		    "kern.sweet_ubwc_disp %d)\n", s->width, s->height, n, s->ubwcReady ? "UBWC-ready" : notUbwc != NULL ? notUbwc : "linear",
		    s->ubwcMeta, s->ubwcMetaPitch, s->ubwcMetaSize, s->ubwcData, s->ubwcDataPitch, s->allocSize, sweet_ubwc_disp);
	}
	IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, s->memory);
	if (b != NULL) {
		s->mapCache = kIOMapDefaultCache;       /* as every surface; the scanout cleans CPU-written frames */
		bzero(b->getBytesNoCopy(), (size_t)sweet_surface_scan_len());
		flush_dcache64((addr64_t)(uintptr_t)b->getBytesNoCopy(), (unsigned)sweet_surface_scan_len(), FALSE);
	} else {
		s->mapCache = kIOMapWriteCombineCache;  /* as the panel's own surface */
		uintptr_t va = s->kernelAddress();
		if (va != 0) {
			bzero((void *)va, (size_t)sweet_surface_scan_len());
			__asm__ volatile ("dsb sy" ::: "memory");
		}
	}
	IOLog("SWEET SURF: scanout slot %u (%s, PA %#llx) for a %ux%u surface, %u-byte rows\n", n, n != 0 ? "pool RAM" : "carve-out",
	    s->scanPA, s->width, s->height, s->bytesPerRow);
	return NULL;
}

SweetSurface *
sweet_surface_scan_copy(uint32_t slot, uint64_t *pa)
{
	uint32_t id = 0;
	*pa = 0;
	if (slot >= SWEET_SCAN_SLOTS || sweet_scan_lock == NULL) {
		return NULL;
	}
	IOLockLock(sweet_scan_lock);
	if (sweet_scan_slot[slot].owner != NULL) {
		id = sweet_scan_slot[slot].owner->id;
	}
	*pa = sweet_scan_slot[slot].pa;
	IOLockUnlock(sweet_scan_lock);
	SweetSurface *s = id != 0 ? sweet_surface_copy(id) : NULL;
	if (s != NULL && s->scanSlot != slot + 1) {
		s->release();
		s = NULL;
	}
	return s;
}

void
SweetSurface::free()
{
	if (scanSlot != 0) {
		sweet_scan_release(this);       /* 0975: the slot's memory stays allocated for the next surface */
	} else if (OSDynamicCast(IOBufferMemoryDescriptor, memory) != NULL) {
		OSAddAtomic64(-(SInt64)allocSize, (volatile SInt64 *)&sweet_surface_wired);
	}
	OSSafeReleaseNULL(kernelMap);
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(state);
	OSObject::free();
}

uintptr_t
SweetSurface::kernelAddress()
{
	IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
	if (buffer != NULL) {
		return (uintptr_t)buffer->getBytesNoCopy();
	}
	IOLockLock(sweet_surface_lock);
	if (kernelMap == NULL && memory != NULL) {
		kernelMap = memory->map(kIOMapWriteCombineCache);
	}
	IOLockUnlock(sweet_surface_lock);
	return kernelMap != NULL ? (uintptr_t)kernelMap->getVirtualAddress() : 0;
}

static uint32_t *
sweet_surface_seed(SweetSurface *s)
{
	return (uint32_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_SEED);
}

/*
 * Give the surface an id and a table slot; the table holds one retain and a
 * user count (creators, client mappings, kernel holders). Lock held.
 */
static bool
sweet_surface_insert_locked(SweetSurface *s)
{
	for (uint32_t n = 0; n < SWEET_SURFACE_MAX_ID - 1; n++) {
		uint32_t id = sweet_surface_next_id;
		sweet_surface_next_id = id + 1 < SWEET_SURFACE_MAX_ID ? id + 1 : 1;
		if (sweet_surface_table[id] == NULL) {
			s->id = id;
			s->retain();
			sweet_surface_table[id] = s;
			sweet_surface_users[id] = 1;
			*(uint64_t *)s->state->getBytesNoCopy() = id;   /* registry id */
			return true;
		}
	}
	return false;
}

/* Add a user to a surface that may have left the table (port lookups). */
static bool
sweet_surface_get(SweetSurface *s)
{
	bool ok = true;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s) {
		sweet_surface_users[s->id]++;
	} else if (sweet_surface_table[s->id] == NULL) {
		s->retain();
		sweet_surface_table[s->id] = s;
		sweet_surface_users[s->id] = 1;
	} else {
		ok = false;
	}
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

/* Drop a user; the last one removes the surface from the table. */
static void
sweet_surface_put(SweetSurface *s)
{
	bool drop = false;
	IOLockLock(sweet_surface_lock);
	if (sweet_surface_table[s->id] == s && --sweet_surface_users[s->id] == 0) {
		sweet_surface_table[s->id] = NULL;
		drop = true;
	}
	IOLockUnlock(sweet_surface_lock);
	if (drop) {
		s->release();
	}
}

SweetSurface *
sweet_surface_copy(uint32_t id)
{
	SweetSurface *s = NULL;
	if (id == 0 || id >= SWEET_SURFACE_MAX_ID || sweet_surface_lock == NULL) {
		return NULL;
	}
	IOLockLock(sweet_surface_lock);
	s = sweet_surface_table[id];
	if (s != NULL) {
		s->retain();
	}
	IOLockUnlock(sweet_surface_lock);
	return s;
}

/* The table lock; the framebuffer may publish a surface before the root starts. */
static void
sweet_surface_init_lock(void)
{
	if (sweet_surface_lock == NULL) {
		IOLock *lock = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, lock, &sweet_surface_lock)) {
			IOLockFree(lock);
		}
	}
}

static SweetSurface *
sweet_surface_alloc(void)
{
	sweet_surface_init_lock();
	SweetSurface *s = OSTypeAlloc(SweetSurface);
	if (s == NULL || !s->init()) {
		OSSafeReleaseNULL(s);
		return NULL;
	}
	s->state = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, page_size, page_size);
	if (s->state == NULL) {
		s->release();
		return NULL;
	}
	bzero(s->state->getBytesNoCopy(), page_size);
	return s;
}

static bool
sweet_surface_publish(SweetSurface *s)
{
	bool ok;
	IOLockLock(sweet_surface_lock);
	ok = sweet_surface_insert_locked(s);
	IOLockUnlock(sweet_surface_lock);
	return ok;
}

SweetSurface *
sweet_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		return NULL;
	}
	s->width = width;
	s->height = height;
	s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4;
	s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat;
	s->allocSize = (uint32_t)round_page(bytesPerRow * height);
	s->memory = IOMemoryDescriptor::withPhysicalAddress(phys, s->allocSize, kIODirectionInOut);
	s->mapCache = kIOMapWriteCombineCache;
	if (s->memory == NULL || !sweet_surface_publish(s)) {
		s->release();
		return NULL;
	}
	return s;
}

mach_port_name_t
sweet_surface_port_in_task(SweetSurface *s, task_t task)
{
	mach_port_name_t name = 0;
	IOUserClient::copyPortNameForObjectInTask(task, s, &name);
	return name;
}

static uint32_t
sweet_dict_u32(OSDictionary *dict, const char *key, uint32_t def)
{
	OSNumber *n = OSDynamicCast(OSNumber, dict->getObject(key));
	return n != NULL ? n->unsigned32BitValue() : def;
}

static uint32_t
sweet_div_up(uint32_t a, uint32_t b)
{
	return b > 1 ? (a + b - 1) / b : a;
}

static uint32_t
sweet_row_bytes(uint32_t width, uint32_t ew, uint32_t bpe)
{
	uint64_t row = (uint64_t)sweet_div_up(width, ew) * bpe;
	return (uint32_t)((row + SWEET_SURFACE_ROW_ALIGN - 1) & ~(uint64_t)(SWEET_SURFACE_ROW_ALIGN - 1));
}

/*
 * redfin 1258: kern.sweet_surfkeep (bsd/dev/sweet_gpu.c) / boot-arg sweetsurfkeep, default 0. 0xffffffff = not read yet.
 */
extern "C" uint32_t sweet_surfkeep;
uint32_t sweet_surfkeep = 0xffffffffu;
static uint32_t sweet_surface_keep_logged;
extern "C" uint32_t sweet_surface_keep_row(void);
uint32_t
sweet_surface_keep_row(void)
{
	if (sweet_surfkeep == 0xffffffffu) {
		/*
		 * 1096: default 2 (was 1 since 2026-11-05): keep the explicit row only for CA's archive-decode surfaces
		 * (sweet_surface_archive_decode). sweetsurfkeep=1 = 1258 as it was (<= 9 keys), 0 = always round.
		 */
		uint32_t v = 2;
		PE_parse_boot_argn("sweetsurfkeep", &v, sizeof(v));
		sweet_surfkeep = v > 2 ? 2 : v;
	}
	return sweet_surfkeep;
}

/*
 * redfin 1096: kern.sweet_surfkeep 2. 1258 kept the row of every surface made from <= 9 keys with an explicit
 * IOSurfaceBytesPerRow. The surfaces it was written for (klogs 11-05..11-14, "SWEET SURF: 1258 ... kept") are CA's
 * archive decoder's: 6 keys, made in the client (SpringBoard, MobilePhone, InCallService, Preferences, Quick Look),
 * small (glyph/icon size), 'LA08' or 'BGRA', CPU-written from a recorded blob, never a render target or a display
 * surface. Others in the <= 9 set were not: MobileSafari's 7-key 1125x2436 'BGRA' (4512-byte rows), Quick Look's
 * 7-key thumbnails. backboardd's display surfaces have 11 keys (never kept). Mode 2 keeps the row only when all of:
 * <= 6 keys, explicit row, not backboardd, under 1 M pixels. Everything else gets 0159/0940's 64-byte rows.
 */
#define SWEET_SURFKEEP_MAX_KEYS         6
#define SWEET_SURFKEEP_MAX_PIXELS       (1u << 20)
static bool
sweet_surface_archive_decode(SweetSurface *s, OSDictionary *props, const char *who)
{
	return props->getCount() <= SWEET_SURFKEEP_MAX_KEYS && props->getObject("IOSurfaceBytesPerRow") != NULL &&
	       strcmp(who, "backboardd") != 0 && (uint64_t)s->width * s->height < SWEET_SURFKEEP_MAX_PIXELS;
}

/* The key names of a create (first 24 decisions of mode 2 logged, to check the archive-decode test on the phone). */
static void
sweet_surface_key_names(OSDictionary *props, char *buf, size_t len)
{
	size_t at = 0;
	buf[0] = 0;
	OSCollectionIterator *it = OSCollectionIterator::withCollection(props);
	if (it == NULL) {
		return;
	}
	OSSymbol *k;
	while ((k = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL && at + 2 < len) {
		const char *n = k->getCStringNoCopy();
		if (strncmp(n, "IOSurface", 9) == 0) {
			n += 9;
		}
		int w = snprintf(buf + at, len - at, "%s%s", at ? "," : "", n);
		if (w < 0) {
			break;
		}
		at += (size_t)w;
	}
	it->release();
}

/*
 * redfin 1096: kern.sweet_surfpad 0|1 (boot-arg sweetsurfpad, default 1). The A6xx GMEM blits (tile load/store,
 * hakios-metal's GMEM restore) move 16x4-pixel blocks, so a pass over an imported surface whose height is not a
 * multiple of 4 reads (and may write) up to 3 rows past its last one. The allocation covers whole 4-row groups
 * for the surface and each plane (rows, sizes and offsets the client sees do not change; only allocSize can grow).
 * New surfaces only. kern.sweet_surfpad_grew counts surfaces whose allocation grew by it.
 */
extern "C" uint32_t sweet_surfpad;
uint32_t sweet_surfpad = 0xffffffffu;
extern "C" uint32_t sweet_surfpad_grew;
uint32_t sweet_surfpad_grew;
extern "C" uint32_t sweet_surface_pad(void);
uint32_t
sweet_surface_pad(void)
{
	if (sweet_surfpad == 0xffffffffu) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetsurfpad", &v, sizeof(v));
		sweet_surfpad = v ? 1 : 0;
	}
	return sweet_surfpad;
}

/*
 * redfin 1096: kern.sweet_surfalign_limits (boot-arg sweetsurfalignlim, default 1). What IOSurfaceRootUserClient
 * selector 13 (limits, 0x18 bytes) answers as the offset (+0) and bytes-per-row (+4) alignment masks, which
 * IOSurface.framework hands to IOSurfaceAlignProperty / IOSurfaceGetPropertyAlignment: 1 = the row alignment this
 * kernel rounds to (0159's sweetsurfalign, 64: mask 63, as before 1096), 0 = no alignment (masks 0, test only),
 * a power of two N >= 2 = mask N - 1 (e.g. 256, to see whether a client follows the mask). Read at each query
 * (a process asks once, when IOSurface.framework starts in it).
 */
extern "C" uint32_t sweet_surfalign_limits;
uint32_t sweet_surfalign_limits = 0xffffffffu;
static uint32_t sweet_surface_row_align(void);
extern "C" uint32_t sweet_surface_limits_mask(void);
uint32_t
sweet_surface_limits_mask(void)
{
	if (sweet_surfalign_limits == 0xffffffffu) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetsurfalignlim", &v, sizeof(v));
		sweet_surfalign_limits = (v <= 1 || (v & (v - 1)) == 0) ? v : 1;
	}
	uint32_t v = sweet_surfalign_limits;
	if (v == 0) {
		return 0;
	}
	if (v == 1) {
		uint32_t a = sweet_surface_row_align();
		return a > 1 ? a - 1 : 0;
	}
	return v - 1;
}

/* 0159's row alignment (boot-arg sweetsurfalign=N, default 64, 0 = as asked); 1096 made it a function for selector 13. */
static uint32_t
sweet_surface_row_align(void)
{
	static uint32_t row_align = 0xffffffffu;
	if (row_align == 0xffffffffu) {
		uint32_t v = 64;
		PE_parse_boot_argn("sweetsurfalign", &v, sizeof(v));
		row_align = (v & (v - 1)) == 0 ? v : 0;
	}
	return row_align;
}

static uint32_t sweet_surface_keep2_logged;    /* 1096: mode-2 decisions logged with their key names (first 24) */

/* Geometry from the creation properties (IOSurface.h keys). */
static IOReturn
sweet_surface_layout(SweetSurface *s, OSDictionary *props)
{
	uint64_t end;

	s->width = sweet_dict_u32(props, "IOSurfaceWidth", 0);
	s->height = sweet_dict_u32(props, "IOSurfaceHeight", 0);
	s->bytesPerElement = (uint16_t)sweet_dict_u32(props, "IOSurfaceBytesPerElement", 4);
	s->elementWidth = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementWidth", 1);
	s->elementHeight = (uint8_t)sweet_dict_u32(props, "IOSurfaceElementHeight", 1);
	s->pixelFormat = sweet_dict_u32(props, "IOSurfacePixelFormat", 0);
	/*
	 * (from HackiOS) 10-bit formats a wide-gamut iPhone asks for ('w30r', 'l10r', 'w30R' and
	 * 'b3a8' with its separate alpha plane): neither these surfaces nor the software renderer
	 * draw them, and the panel is 8-bit sRGB. Made as one-plane BGRA, which the client reads back.
	 */
	bool one_plane = false;
	/*
	 * flame: only for WebKit's content process, which draws its tiles with CoreGraphics on the CPU and
	 * reads the format back (its 'w30r' tiles stayed white otherwise). Clients that render the 10-bit
	 * format they asked for (CA's software renderer in chronod, Metal in widget extensions such as
	 * NewsToday2) write it into the BGRA surface: white widgets, garbled colours. Boot-arg
	 * sweetsurf10=0 never remaps, =2 always does.
	 */
	static int remap10 = -1;
	if (remap10 < 0) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetsurf10", &v, sizeof(v));
		remap10 = (int)v;
	}
	char who[32] = "";
	proc_selfname(who, sizeof(who));
	bool remap = remap10 == 2 || (remap10 == 1 && strcmp(who, "com.apple.WebKit.WebContent") == 0);
	if (!remap) {
	} else if (s->pixelFormat == 0x77333072u || s->pixelFormat == 0x6C313072u || s->pixelFormat == 0x77333052u) {
		s->pixelFormat = 0x42475241u;           /* BGRA */
	} else if (s->pixelFormat == 0x62336138u) {
		s->pixelFormat = 0x42475241u;
		one_plane = true;
	}
	s->cacheMode = sweet_dict_u32(props, "IOSurfaceCacheMode", 0);
	s->offset = sweet_dict_u32(props, "IOSurfaceOffset", 0);
	if (s->width > SWEET_SURFACE_MAX_DIM || s->height > SWEET_SURFACE_MAX_DIM ||
	    s->bytesPerElement > 64 || s->elementWidth == 0 || s->elementHeight == 0) {
		return kIOReturnBadArgument;
	}
	s->bytesPerRow = sweet_dict_u32(props, "IOSurfaceBytesPerRow",
	    sweet_row_bytes(s->width, s->elementWidth, s->bytesPerElement));
	if (s->bytesPerRow < (uint64_t)sweet_div_up(s->width, s->elementWidth) * s->bytesPerElement) {
		return kIOReturnBadArgument;
	}
	/*
	 * 0159: single-plane rows 64-byte aligned (boot-arg sweetsurfalign=N, 0 = as asked). Core Animation
	 * asks 16/32-byte aligned rows (5088 for 1271 BGRA pixels, 96 for 96 A8 ones); Turnip makes linear
	 * images only at 64-byte pitches, so the Metal bridge copied each such layer into an image of its own
	 * every frame (6.6 ms of backboardd's ~12 ms frame). Creators read the rows back with
	 * IOSurfaceGetBytesPerRow.
	 */
	uint32_t row_align = sweet_surface_row_align();
	/*
	 * 0940: the planes' rows too (boot-arg sweetsurfplanar=0: not), and a surface with plane info of one
	 * plane like one without (Core Animation's A8/RG8/BGRA layers still reached Turnip at 96/160/224-byte
	 * pitches on r18g: ~100 "explicit layout refused" a session, each texture shadow-copied every frame).
	 * A plane whose rows grow is moved past the plane before it where the creator's offset would overlap
	 * it; the creators read rows and plane bases back (IOSurfaceGetBytesPerRowOfPlane, ...BaseAddressOfPlane:
	 * 0927's lock result). sweetsurfalign=0 turns all of it off.
	 */
	static uint32_t planar_align = 0xffffffffu;
	if (planar_align == 0xffffffffu) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetsurfplanar", &v, sizeof(v));
		planar_align = v ? row_align : 0;
	}
	OSArray *plane_info = OSDynamicCast(OSArray, props->getObject("IOSurfacePlaneInfo"));
	/*
	 * redfin 1258 (Yofuness 0251, kern.sweet_surfkeep / boot-arg sweetsurfkeep, default 0 = as before): a surface made
	 * from a recorded key set (at most 9 keys: QuartzCore's iosurface_keys_allow_list) with an explicit row keeps that
	 * row, its planes' rows too. CA's archive decoder copies a recorded blob written at the client's (32-byte) row and
	 * never reads the row back, so 0159's 64-byte rounding shears it: glyph stripes (Control Center). New surfaces
	 * only. Line SWEET SURF: 1258 ... for the first 32 such surfaces whose row 0159 would round (also with 0).
	 */
	bool recorded = props->getCount() <= 9 && props->getObject("IOSurfaceBytesPerRow") != NULL;
	uint32_t keep_mode = sweet_surface_keep_row();
	/* 1096: mode 2 keeps the row of CA's archive-decode surfaces only (sweet_surface_archive_decode) */
	bool keep_row = recorded && (keep_mode == 1 || (keep_mode == 2 && sweet_surface_archive_decode(s, props, who)));
	if (recorded && row_align > 1 && (s->bytesPerRow & (row_align - 1)) != 0) {
		if (sweet_surface_keep_logged < 32) {
			sweet_surface_keep_logged++;
			IOLog("SWEET SURF: 1258 %u keys %ux%u fmt 0x%x row %u %s (%s)\n", props->getCount(), s->width, s->height,
			    s->pixelFormat, s->bytesPerRow, keep_row ? "kept" : "rounded", who);
		}
		if (keep_mode == 2 && sweet_surface_keep2_logged < 24) {
			char keys[192];
			sweet_surface_keep2_logged++;
			sweet_surface_key_names(props, keys, sizeof(keys));
			IOLog("SWEET SURF: 1096 surfkeep 2: %s %ux%u fmt 0x%x row %u %s; keys %s\n", who, s->width, s->height,
			    s->pixelFormat, s->bytesPerRow, keep_row ? "kept (archive decode)" : "rounded to 64", keys);
		}
	}
	if (!keep_row && row_align > 1 && (one_plane || plane_info == NULL || (plane_info->getCount() == 1 && planar_align > 1))) {
		s->bytesPerRow = (s->bytesPerRow + row_align - 1) & ~(row_align - 1);
	}
	uint32_t plane_align = keep_row ? 0 : planar_align;      /* 1258 */
	end = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * sweet_div_up(s->height, s->elementHeight);

	OSArray *planes = one_plane ? NULL : plane_info;
	s->planeCount = 0;
	if (planes != NULL) {
		uint64_t next = s->offset;
		uint64_t prev_end = s->offset;		/* 0940: the end of the plane before (rows rounded) */
		bool moved = false;
		if (planes->getCount() > SWEET_SURFACE_MAX_PLANES) {
			return kIOReturnBadArgument;
		}
		for (unsigned int i = 0; i < planes->getCount(); i++) {
			OSDictionary *pd = OSDynamicCast(OSDictionary, planes->getObject(i));
			struct sweet_surface_plane *p = &s->planes[i];
			if (pd == NULL) {
				return kIOReturnBadArgument;
			}
			p->width = sweet_dict_u32(pd, "IOSurfacePlaneWidth", s->width);
			p->height = sweet_dict_u32(pd, "IOSurfacePlaneHeight", s->height);
			p->bytesPerElement = (uint16_t)sweet_dict_u32(pd, "IOSurfacePlaneBytesPerElement", 1);
			p->elementWidth = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementWidth", 1);
			p->elementHeight = (uint8_t)sweet_dict_u32(pd, "IOSurfacePlaneElementHeight", 1);
			if (p->width > SWEET_SURFACE_MAX_DIM || p->height > SWEET_SURFACE_MAX_DIM ||
			    p->bytesPerElement > 64 || p->elementWidth == 0 || p->elementHeight == 0) {
				return kIOReturnBadArgument;
			}
			p->bytesPerRow = sweet_dict_u32(pd, "IOSurfacePlaneBytesPerRow",
			    sweet_row_bytes(p->width, p->elementWidth, p->bytesPerElement));
			p->offset = sweet_dict_u32(pd, "IOSurfacePlaneOffset", (uint32_t)next);
			p->size = sweet_dict_u32(pd, "IOSurfacePlaneSize",
			    p->bytesPerRow * sweet_div_up(p->height, p->elementHeight));
			if (plane_align > 1 && (p->bytesPerRow & (plane_align - 1)) != 0) {
				uint32_t was_row = p->bytesPerRow;
				uint64_t row = ((uint64_t)p->bytesPerRow + plane_align - 1) & ~(uint64_t)(plane_align - 1);
				uint64_t size = row * sweet_div_up(p->height, p->elementHeight);
				if (row > 0xffffffffull || size > 0xffffffffull) {
					return kIOReturnBadArgument;
				}
				p->bytesPerRow = (uint32_t)row;
				if (size > p->size) {
					p->size = (uint32_t)size;
				}
				moved = true;
				if (sweet_surface_align_logged < 16) {
					sweet_surface_align_logged++;
					IOLog("SWEET SURF: 0940 fmt 0x%x %ux%u plane %u rows %u -> %u\n", s->pixelFormat, s->width,
					    s->height, i, was_row, p->bytesPerRow);
				}
			}
			if (moved && (uint64_t)p->offset < prev_end) {
				/* the plane before grew into this one: after it, page-aligned if the creator's was */
				uint64_t at = (p->offset & PAGE_MASK) == 0 ? round_page(prev_end) :
				    ((prev_end + plane_align - 1) & ~(uint64_t)(plane_align - 1));
				if (at + p->size > 0xffffffffull) {
					return kIOReturnBadArgument;
				}
				p->offset = (uint32_t)at;
			}
			if ((uint64_t)p->offset + p->size > prev_end) {
				prev_end = (uint64_t)p->offset + p->size;
			}
			next = round_page((uint64_t)p->offset + p->size);
			if ((uint64_t)p->offset + p->size > end) {
				end = (uint64_t)p->offset + p->size;
			}
		}
		s->planeCount = planes->getCount();
		/*
		 * Planar surfaces whose creator gave no top-level geometry: report the sum over
		 * the planes (b3a8 = BGRA + A8: 5 bytes per pixel), as Core Animation checks
		 * IOSurfaceGetBytesPerRow against the format's full pixel size ("unexpected
		 * bytes-per-row value of 4544, expected at least 5625": Safari pages stayed blank).
		 * The allocation comes from the planes alone.
		 */
		if (props->getObject("IOSurfaceBytesPerRow") == NULL && s->planeCount > 1) {
			uint32_t bpr = 0, bpe = 0;
			end = s->offset;
			for (uint32_t i = 0; i < s->planeCount; i++) {
				const struct sweet_surface_plane *p = &s->planes[i];
				bpr += (uint32_t)((uint64_t)p->bytesPerRow * s->width / (p->width ? p->width : 1));
				bpe += p->bytesPerElement;
				if ((uint64_t)p->offset + p->size > end) {
					end = (uint64_t)p->offset + p->size;
				}
			}
			s->bytesPerRow = bpr;
			if (props->getObject("IOSurfaceBytesPerElement") == NULL && bpe <= 64) {
				s->bytesPerElement = (uint16_t)bpe;
			}
		}
	}

	/* 1096 kern.sweet_surfpad: whole 4-row groups behind the surface and each plane (GMEM 16x4 blocks) */
	uint64_t pad_end = end;
	if (sweet_surface_pad() != 0) {
		uint32_t rows = sweet_div_up(s->height, s->elementHeight);
		uint64_t e = (uint64_t)s->offset + (uint64_t)s->bytesPerRow * ((rows + 3) & ~3u);
		if (s->planeCount == 0 && e > pad_end) {
			pad_end = e;
		}
		for (uint32_t i = 0; i < s->planeCount; i++) {
			const struct sweet_surface_plane *p = &s->planes[i];
			uint32_t prow = sweet_div_up(p->height, p->elementHeight);
			uint64_t pe = (uint64_t)p->offset + (uint64_t)p->bytesPerRow * ((prow + 3) & ~3u);
			if ((uint64_t)p->offset + p->size > pe) {
				pe = (uint64_t)p->offset + p->size;
			}
			if (pe > pad_end) {
				pad_end = pe;
			}
		}
	}
	uint64_t alloc = sweet_dict_u32(props, "IOSurfaceAllocSize", 0);
	if (alloc < end) {
		alloc = end;
	}
	if (round_page(alloc) < round_page(pad_end)) {
		OSIncrementAtomic((volatile SInt32 *)&sweet_surfpad_grew);
	}
	if (alloc < pad_end) {
		alloc = pad_end;
	}
	alloc = round_page(alloc);
	if (alloc == 0 || alloc > SWEET_SURFACE_MAX_ALLOC) {
		return kIOReturnNoMemory;
	}
	s->allocSize = (uint32_t)alloc;
	return kIOReturnSuccess;
}

/* Per-client mapping of one surface. */
class SweetSurfaceRef : public OSObject {
	OSDeclareDefaultStructors(SweetSurfaceRef);

public:
	SweetSurface *surface;
	IOMemoryMap  *data;
	IOMemoryMap  *state;
	uint32_t      count;
	int32_t       use[2];       /* 1258: what this client added to the use counts (kern.sweet_surf_inuse != 0) */

	bool init() APPLE_KEXT_OVERRIDE { return OSObject::init(); }
	void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetSurfaceRef, OSObject);

void
SweetSurfaceRef::free()
{
	OSSafeReleaseNULL(data);
	OSSafeReleaseNULL(state);
	if (surface != NULL && (use[0] != 0 || use[1] != 0)) {
		/* 1258: a client that goes (exit, crash, last release) leaves no use count behind, as IOSurface's own */
		OSAddAtomic(-use[0], &surface->clientUse);
		OSAddAtomic(-use[1], &surface->clientUse1);
		use[0] = use[1] = 0;
		sweet_surface_publish_use(surface);
	}
	if (surface != NULL) {
		sweet_surface_put(surface);
		surface->release();
		surface = NULL;
	}
	OSObject::free();
}

class IOCoreSurfaceRoot : public IOService {
	OSDeclareDefaultStructors(IOCoreSurfaceRoot);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOCoreSurfaceRoot, IOService);

static void sweet_inuse_start(void);      /* 1258 */

bool
IOCoreSurfaceRoot::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	sweet_surface_init_lock();
	if (sweet_surface_inuse_mode() != 0) {
		sweet_inuse_start();    /* 1258: boot-arg sweetsurfinuse */
	}
	setProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");
	registerService();
	IOLog("SWEET SURF: IOCoreSurfaceRoot registered\n");
	return true;
}

class IOSurfaceRootUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOSurfaceRootUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fRefs;

	SweetSurfaceRef *findRef(uint32_t id, unsigned int *index);
	IOReturn addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing);
	IOReturn returnSurface(SweetSurface *s, IOExternalMethodArguments *args);
	IOReturn create(IOExternalMethodArguments *args);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free() APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	IOExternalTrap *getTargetAndTrapForIndex(IOService **targetP, UInt32 index) APPLE_KEXT_OVERRIDE;

	IOReturn trapUseCount(void *id, void *category, void *, void *, void *, void *);
	IOReturn trapIncUse(void *id, void *category, void *, void *, void *, void *);    /* 0976 */
	IOReturn trapDecUse(void *id, void *category, void *, void *, void *, void *);
	void refUse(void *id, void *category, int delta);       /* 1258 */
	IOReturn trapLock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapUnlock(void *id, void *options, void *seedOut, void *, void *, void *);
	IOReturn trapRelease(void *id, void *, void *, void *, void *, void *);
};

OSDefineMetaClassAndStructors(IOSurfaceRootUserClient, IOUserClient);

static uint64_t sweet_surf_logged;      /* selectors / traps already logged (< 64) */

static void
sweet_surf_log_once(const char *what, uint32_t n)
{
	uint32_t bit = (what[0] == 't' ? 48 : 0) + n;
	if (bit >= 64 || (sweet_surf_logged & (1ULL << bit))) {
		return;
	}
	sweet_surf_logged |= 1ULL << bit;
	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET SURF: %s %s %u\n", name, what, n);
}

bool
IOSurfaceRootUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fRefs = OSArray::withCapacity(16);
	return fLock != NULL && fRefs != NULL;
}

void
IOSurfaceRootUserClient::free()
{
	OSSafeReleaseNULL(fRefs);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOSurfaceRootUserClient::clientClose(void)
{
	IOLockLock(fLock);
	fRefs->flushCollection();
	IOLockUnlock(fLock);
	terminate();
	return kIOReturnSuccess;
}

/* Lock held. */
SweetSurfaceRef *
IOSurfaceRootUserClient::findRef(uint32_t id, unsigned int *index)
{
	for (unsigned int i = 0; i < fRefs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)fRefs->getObject(i);
		if (ref->surface->id == id) {
			if (index != NULL) {
				*index = i;
			}
			return ref;
		}
	}
	return NULL;
}

/*
 * Count one more create/lookup of s by this client, mapping it into the
 * task the first time. Takes over the caller's user count on s.
 *
 * A lookup of a surface this client already has maps nothing and counts
 * nothing (*existing): IOSurface.framework then gets base address -1 in the
 * lock result and reuses its cached IOSurfaceClient. Counting it instead
 * made the framework build a second client for the same id; releasing that
 * one (traps 4 + 5) unmapped the memory under the first (backboardd
 * snapshot crash in CGDataProviderCreateWithData).
 */
IOReturn
IOSurfaceRootUserClient::addRef(SweetSurface *s, SweetSurfaceRef **refOut, bool *existing)
{
	*existing = false;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef(s->id, NULL);
	if (ref != NULL && ref->surface == s) {
		IOLockUnlock(fLock);
		sweet_surface_put(s);
		*refOut = ref;
		*existing = true;
		return kIOReturnSuccess;
	}
	IOLockUnlock(fLock);

	ref = OSTypeAlloc(SweetSurfaceRef);
	if (ref == NULL || !ref->init()) {
		OSSafeReleaseNULL(ref);
		sweet_surface_put(s);
		return kIOReturnNoMemory;
	}
	s->retain();
	ref->surface = s;
	ref->count = 1;
	ref->data = s->memory->createMappingInTask(fTask, 0, kIOMapAnywhere | s->mapCache);
	ref->state = s->state->createMappingInTask(fTask, 0, kIOMapAnywhere | kIOMapReadOnly);
	if (ref->data == NULL || ref->state == NULL) {
		ref->release();
		return kIOReturnVMError;
	}
	IOLockLock(fLock);
	SweetSurfaceRef *raced = findRef(s->id, NULL);
	if (raced != NULL && raced->surface == s) {
		/* Another thread mapped it meanwhile: count there, drop ours. */
		raced->count++;
		IOLockUnlock(fLock);
		ref->release();
		*refOut = raced;
		return kIOReturnSuccess;
	}
	fRefs->setObject(ref);
	IOLockUnlock(fLock);
	ref->release();         /* fRefs holds it */
	*refOut = ref;
	return kIOReturnSuccess;
}

static void
put32(uint8_t *p, uint32_t off, uint32_t v)
{
	memcpy(p + off, &v, 4);
}

static void
put64(uint8_t *p, uint32_t off, uint64_t v)
{
	memcpy(p + off, &v, 8);
}

/* IOSurface.framework's lock result (copied to IOSurfaceClient + 0x68). */
static void
sweet_surface_fill_result(SweetSurface *s, SweetSurfaceRef *ref, uint8_t *out)
{
	bzero(out, SWEET_SURFACE_RESULT_SIZE);
	put64(out, 0x00, ref->data->getAddress());
	put64(out, 0x08, ref->state->getAddress());
	put64(out, 0x10, ref->state->getAddress() + SWEET_SURFACE_TIMESTAMPS);
	put32(out, 0x18, s->id);
	put32(out, 0x1c, s->allocSize);
	put32(out, 0x20, s->width);
	put32(out, 0x24, s->height);
	put32(out, 0x28, s->bytesPerRow);
	put32(out, 0x2c, s->offset);
	put32(out, 0x30, s->pixelFormat);
	put32(out, 0x34, s->allocSize);
	put32(out, 0x38, s->planeCount);
	memcpy(out + 0x40, &s->bytesPerElement, 2);
	out[0x42] = s->elementWidth;
	out[0x43] = s->elementHeight;
	put32(out, 0x44, s->cacheMode);
	put32(out, 0x50, s->parentID);
	put32(out, 0x54, 1);
	for (uint32_t i = 0; i < s->planeCount; i++) {
		uint8_t *p = out + 0x68 + i * 0x80;
		const struct sweet_surface_plane *pl = &s->planes[i];
		put32(p, 0x00, pl->width);
		put32(p, 0x04, pl->height);
		put32(p, 0x08, pl->bytesPerRow);
		put32(p, 0x0c, pl->offset);
		/*
		 * 0927: IOSurface.framework (16.7) reads a plane's bytes per row at +0x10
		 * (IOSurfaceGetBytesPerRowOfPlane: client + 0xe0 + 0x80 * plane, the lock result
		 * being at + 0x68). Without it every plane had 0 bytes per row: the camera's
		 * pictures were drawn all into their first row (green photographs, a black
		 * preview) and VideoToolbox refused its transfers (-12902).
		 */
		put32(p, 0x10, pl->bytesPerRow);
		put32(p, 0x14, pl->size);
		memcpy(p + 0x18, &pl->bytesPerElement, 2);
		p[0x1a] = pl->elementWidth;
		p[0x1b] = pl->elementHeight;
	}
}

static IOReturn
sweet_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Map s for this client and answer with its lock result. Takes s's user count. */
IOReturn
IOSurfaceRootUserClient::returnSurface(SweetSurface *s, IOExternalMethodArguments *args)
{
	SweetSurfaceRef *ref = NULL;
	bool existing = false;
	IOReturn ret = addRef(s, &ref, &existing);
	if (ret != kIOReturnSuccess) {
		return ret;
	}
	uint8_t *out = (uint8_t *)IOMallocZeroData(SWEET_SURFACE_RESULT_SIZE);
	if (out == NULL) {
		ret = kIOReturnNoMemory;
	} else {
		sweet_surface_fill_result(s, ref, out);
		if (existing) {
			put64(out, 0x00, ~0ULL);        /* "already yours": use the cached client */
		}
		ret = sweet_struct_out(args, out, SWEET_SURFACE_RESULT_SIZE);
		IOFreeData(out, SWEET_SURFACE_RESULT_SIZE);
	}
	if (ret != kIOReturnSuccess && !existing) {
		/* The caller never learns the id: undo this create/lookup. */
		trapRelease((void *)(uintptr_t)s->id, NULL, NULL, NULL, NULL, NULL);
	}
	return ret;
}

IOReturn
IOSurfaceRootUserClient::create(IOExternalMethodArguments *args)
{
	const char *buf = (const char *)args->structureInput;
	uint32_t len = args->structureInputSize;
	char *copy = NULL;
	IOReturn ret;

	if (args->structureInputDescriptor != NULL) {
		len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len == 0 || len > SWEET_SURFACE_MAX_PROPS) {
			return kIOReturnBadArgument;
		}
		copy = (char *)IOMallocData(len);
		if (copy == NULL) {
			return kIOReturnNoMemory;
		}
		args->structureInputDescriptor->readBytes(0, copy, len);
		buf = copy;
	}
	OSObject *obj = (buf != NULL && len > 0) ? OSUnserializeXML(buf, len) : NULL;
	if (copy != NULL) {
		IOFreeData(copy, len);
	}
	OSDictionary *props = OSDynamicCast(OSDictionary, obj);
	if (props == NULL) {
		OSSafeReleaseNULL(obj);
		IOLog("SWEET SURF: create: bad property dictionary (%u bytes)\n", len);
		return kIOReturnBadArgument;
	}

	SweetSurface *s = sweet_surface_alloc();
	if (s == NULL) {
		props->release();
		return kIOReturnNoMemory;
	}
	s->parentID = args->scalarInputCount > 0 ? (uint32_t)args->scalarInput[0] : 0;
	ret = sweet_surface_layout(s, props);
	const char *notSlot = "-";
	bool panelSized = ret == kIOReturnSuccess && s->width == 1125 && s->height == 2436;
	if (panelSized) {
		notSlot = sweet_scan_claim(s);
	}
	if (panelSized && notSlot == NULL) {
		if (!sweet_surface_publish(s)) {        /* 0975: in a scanout slot */
			ret = kIOReturnNoResources;
		}
	} else if (ret == kIOReturnSuccess) {
		IOBufferMemoryDescriptor *mem = NULL;
		if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired) +
		    s->allocSize <= SWEET_SURFACE_WIRED_MAX) {
			mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
			    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
		}
		if (mem == NULL) {
			OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
			ret = kIOReturnNoMemory;
		} else {
			bzero(mem->getBytesNoCopy(), s->allocSize);
			s->memory = mem;
			s->mapCache = kIOMapDefaultCache;
			if (!sweet_surface_publish(s)) {
				ret = kIOReturnNoResources;
			}
		}
	}
	if (panelSized && sweet_surface_scan_mode() != 0) {
		/* 0979: one line per panel-sized surface: in which slot, or why not */
		char who[32];
		proc_selfname(who, sizeof(who));
		if (s->scanSlot != 0) {
			IOLog("SWEET SURF: %s surface %u (fmt %#x bpr %u): scanout slot %u\n", who, s->id, s->pixelFormat,
			    s->bytesPerRow, s->scanSlot - 1);
		} else {
			IOLog("SWEET SURF: %s surface %u (fmt %#x bpr %u): no slot: %s\n", who, s->id, s->pixelFormat, s->bytesPerRow,
			    notSlot);
		}
	}
	if (ret != kIOReturnSuccess || sweet_surface_creates_logged < 64) {
		char name[32];
		proc_selfname(name, sizeof(name));
		sweet_surface_creates_logged++;
		IOLog("SWEET SURF: %s create id %u %ux%u fmt 0x%x bpe %u bpr %u planes %u size %u keys %u -> 0x%x\n",
		    name, s->id, s->width, s->height, s->pixelFormat, s->bytesPerElement,
		    s->bytesPerRow, s->planeCount, s->allocSize, props->getCount(), ret);
	}
	props->release();
	if (ret != kIOReturnSuccess) {
		s->release();
		return ret;
	}
	ret = returnSurface(s, args);   /* takes the creator's user count */
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	uint64_t arg0 = args->scalarInputCount > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kSweetSurfaceLimits: {
		uint8_t limits[0x18] = { 0 };
		uint32_t mask = sweet_surface_limits_mask();   /* 1096 kern.sweet_surfalign_limits (1: 63, as before) */
		put32(limits, 0x0, mask);
		put32(limits, 0x4, mask);
		limits[0x16] = 1;
		sweet_surf_log_once("sel", selector);
		return sweet_struct_out(args, limits, sizeof(limits));
	}
	case kSweetSurfaceCreate:
		return create(args);
	case kSweetSurfaceLookup: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		IOReturn ret = kIOReturnNotFound;
		if (s != NULL) {
			ret = sweet_surface_get(s) ? returnSurface(s, args) : kIOReturnNotFound;
			s->release();
		}
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceLookupFromPort: {
		OSObject *obj = NULL;
		IOReturn ret = IOUserClient::copyObjectForPortNameInTask(fTask, (mach_port_name_t)arg0, &obj);
		SweetSurface *s = OSDynamicCast(SweetSurface, obj);
		if (ret == kIOReturnSuccess) {
			ret = (s != NULL && sweet_surface_get(s)) ? returnSurface(s, args) : kIOReturnBadArgument;
		}
		OSSafeReleaseNULL(obj);
		sweet_surf_log_once("sel", selector);
		return ret;
	}
	case kSweetSurfaceCreatePort: {
		SweetSurface *s = sweet_surface_copy((uint32_t)arg0);
		if (s == NULL || args->scalarOutputCount < 1) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		args->scalarOutput[0] = sweet_surface_port_in_task(s, fTask);
		args->scalarOutputCount = 1;
		s->release();
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
	default:
		/* Values, notifications, ownership, timestamps...: accept and ignore. */
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			args->scalarOutput[i] = 0;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		sweet_surf_log_once("sel", selector);
		return kIOReturnSuccess;
	}
}

IOReturn
IOSurfaceRootUserClient::trapUseCount(__unused void *id, __unused void *category,
    void *, void *, void *, void *)
{
	return kIOReturnSuccess;
}

/* 0976: with scanout slots (sweetscanout), userland's use counts are kept in clientUse (and the first logged). */
static void
sweet_surface_client_use(void *id, void *category, int delta)
{
	static uint32_t logs;
	if (sweet_surface_scan_mode() == 0 && sweet_surface_inuse_mode() == 0) {
		return;
	}
	char name[32];
	proc_selfname(name, sizeof(name));
	SweetSurface *s = sweet_surface_copy((uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return;
	}
	SInt32 v = OSAddAtomic(delta, (uintptr_t)category == 1 ? &s->clientUse1 : &s->clientUse) + delta;
	sweet_surface_publish_use(s);   /* 0983 */
	if (logs < 16 && (s->scanSlot != 0 || strcmp(name, "backboardd") == 0)) {
		logs++;     /* 0979: the display's surfaces only (SpringBoard's layer surfaces filled the 16 lines) */
		IOLog("SWEET SURF: %s use count %s surface %u (%s %u) category %#llx -> %d\n", name, delta > 0 ? "+1" : "-1",
		    s->id, s->scanSlot != 0 ? "slot" : "no slot", s->scanSlot != 0 ? s->scanSlot - 1 : 0,
		    (uint64_t)(uintptr_t)category, (int)v);
	}
	s->release();
}

#define SWEET_SURFACE_STATE_USE         0x18    /* 0983: u64 use counts, category 1 << 32 | category 0 */

/*
 * 1258: kern.sweet_surf_inuse / boot-arg sweetsurfinuse (default 0).
 *
 * IOSurfaceIsInUse (iOS 16.7 IOSurface.framework, IOSurfaceClient: [client + 0x70] = the shared state page) is
 *   ldr x8, [x0, #0x70]; ldr x8, [x8, #0x18]; cmp x8, #0; cset w0, ne
 * i.e. "the u64 at state page +0x18 is not 0" (recon/mg/iosurf2.bin +0x4d0, the function after the 0xb5 flag check;
 * the same layout as hackios MiniIOSurface's minisurf_shared.useCount, whose surf_test checks IsInUse after
 * Increment/DecrementUseCount). 0983 wrote it for the scanout slots only, so every other surface (CVPixelBufferPool
 * camera buffers, Photos' decode surfaces, CA's caches) always read "not in use" and pools handed them to the next
 * writer while backboardd's GPU work still sampled them.
 *   0      0983 exactly: scanout slots publish clientUse + displayUse, the rest never
 *   !0     every surface publishes clientUse + displayUse + gpuHold (category 0) and clientUse1 (category 1); a
 *          client's own use counts go away with it (SweetSurfaceRef::free). gpuHold: a GPU job that uses the surface
 *          has not landed. A submission that names its BOs (NativeAdrenoAbiSubmitBoListFlag, hakios-metal) holds
 *          exactly the surfaces behind its named imports; one that does not (older userland) holds every surface
 *          its client has imported and mapped (an over-estimate). The hold is taken before the job is queued and
 *          dropped when its fence lands (GPU interrupt; 5 ms poll fallback; 2 s cap; a GPU reset completes the
 *          fences, so its holds go too; a client's free() waits for its last fence, then kicks).
 *   bit 1  (2) + "SWEET SURF INUSE: 5s ..." every 5 s
 *   bit 2  (4) no GPU holds (use counts only: tells the two effects apart)
 *   bit 3  (8) IOSurfaceLock waits (<= 200 ms) for the GPU: a read-only lock for the last job that writes the
 *          surface, a read-write lock for the last job that uses it (as IOSurfaceLock syncs with the GPU on iOS)
 * One GPU ring: jobs of all processes run in order, so GPU-to-GPU use across processes needs no wait.
 */
static int sweet_inuse_mode = -1;
static IOLock *sweet_inuse_lock;
static bool sweet_inuse_thread;
static uint32_t sweet_inuse_chan;
#define SWEET_INUSE_MAX         1024
static SweetSurface *sweet_inuse_held[SWEET_INUSE_MAX];        /* retained; gpuHold set (sweet_inuse_lock) */
static uint32_t sweet_inuse_n;
static struct {
	uint32_t publish, publishUsed, hold, holdEnd, holdTimeout, holdOverflow, importHit, importMiss, maxHeld;
	uint32_t subListed, subAll, subNamed, subHeld, subOverflow, cancel, lockWait, lockWaitLate;
	uint64_t lockWaitNs;
} sweet_inuse_st;
extern "C" bool sweet_gpu_fence_done(uint32_t fence);
extern "C" int sweet_gpu_wait_fence_ns(uint32_t fence, uint64_t timeout_ns);

extern "C" int
sweet_surface_inuse_mode(void)
{
	if (sweet_inuse_mode < 0) {
		int m = 0;
		bool given = PE_parse_boot_argn("sweetsurfinuse", &m, sizeof(m));
		if (m < 0 || m > 15) {
			m = 0;
		}
		if (OSCompareAndSwap((UInt32)-1, (UInt32)m, (volatile UInt32 *)&sweet_inuse_mode) && given) {
			IOLog("SWEET SURF INUSE: sweetsurfinuse=%d (1258: 0 slots only as 0983, 1 every surface + GPU holds, "
			    "+2 5 s line, +4 no GPU holds, +8 lock waits for the GPU)\n", m);
		}
	}
	return sweet_inuse_mode;
}

void
sweet_surface_publish_use(SweetSurface *s)
{
	if (s == NULL || s->state == NULL) {
		return;
	}
	int mode = sweet_surface_inuse_mode();
	uint64_t v;
	if (mode == 0) {
		if (s->scanSlot == 0) {
			return;         /* only the display's own surfaces: the others stay as before (never in use) */
		}
		int32_t c0 = s->clientUse + s->displayUse, c1 = s->clientUse1;
		v = (uint64_t)(uint32_t)(c1 < 0 ? 0 : c1) << 32 | (uint32_t)(c0 < 0 ? 0 : c0);
	} else {
		/* 1258: each term clamped by itself (a stray decrement must not cancel a GPU hold) */
		int32_t cu = s->clientUse, du = s->displayUse, gu = s->gpuHold, c1 = s->clientUse1;
		uint32_t c0 = (uint32_t)(cu < 0 ? 0 : cu) + (uint32_t)(du < 0 ? 0 : du) + (uint32_t)(gu < 0 ? 0 : gu);
		v = (uint64_t)(uint32_t)(c1 < 0 ? 0 : c1) << 32 | c0;
		sweet_inuse_st.publish++;
		sweet_inuse_st.publishUsed += v != 0;
	}
	*(volatile uint64_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_USE) = v;
}

/* 1258: every surface in the table publishes again (after a mode change); mode 0 clears the non-slot ones */
static void
sweet_inuse_republish(int mode)
{
	if (sweet_surface_lock == NULL) {
		return;
	}
	IOLockLock(sweet_surface_lock);
	for (uint32_t id = 1; id < SWEET_SURFACE_MAX_ID; id++) {
		SweetSurface *s = sweet_surface_table[id];
		if (s == NULL || s->state == NULL) {
			continue;
		}
		if (mode == 0 && s->scanSlot == 0) {
			*(volatile uint64_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_USE) = 0;
		} else {
			sweet_surface_publish_use(s);
		}
	}
	IOLockUnlock(sweet_surface_lock);
}

uint32_t
sweet_surface_find_pa(uint64_t pa)
{
	uint32_t found = 0;
	if (pa == 0 || sweet_surface_lock == NULL) {
		return 0;
	}
	IOLockLock(sweet_surface_lock);
	for (uint32_t id = 1; id < SWEET_SURFACE_MAX_ID && found == 0; id++) {
		SweetSurface *s = sweet_surface_table[id];
		if (s == NULL || s->memory == NULL) {
			continue;
		}
		if (s->pa0 == 0) {
			IOByteCount seg = 0;    /* every surface is wired kernel memory (buffer, slot or physical) */
			s->pa0 = s->memory->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
		}
		if (s->pa0 == pa) {
			found = id;
		}
	}
	IOLockUnlock(sweet_surface_lock);
	return found;
}

void
sweet_surface_import_stat(bool matched)
{
	if (matched) {
		sweet_inuse_st.importHit++;
	} else {
		sweet_inuse_st.importMiss++;
	}
}

/* 1258: from the GPU interrupt (sweet_gpu.c gpu_irq): a fence landed, the hold thread looks now (no lock taken) */
extern "C" void sweet_surface_gpu_kick(void);
void
sweet_surface_gpu_kick(void)
{
	if (sweet_inuse_lock != NULL && sweet_inuse_n != 0) {
		IOLockWakeup(sweet_inuse_lock, &sweet_inuse_chan, false);
	}
}

static void
sweet_inuse_stats_locked(void)
{
	uint32_t total = 0, used = 0, cu = 0, du = 0, gu = 0;
	IOLockLock(sweet_surface_lock);
	for (uint32_t id = 1; id < SWEET_SURFACE_MAX_ID; id++) {
		SweetSurface *s = sweet_surface_table[id];
		if (s == NULL || s->state == NULL) {
			continue;
		}
		total++;
		used += *(volatile uint64_t *)((uint8_t *)s->state->getBytesNoCopy() + SWEET_SURFACE_STATE_USE) != 0;
		cu += s->clientUse > 0 || s->clientUse1 > 0;
		du += s->displayUse > 0;
		gu += s->gpuHold > 0;
	}
	IOLockUnlock(sweet_surface_lock);
	uint64_t oldest = 0, now = mach_absolute_time(), ns = 0;
	for (uint32_t i = 0; i < sweet_inuse_n; i++) {
		if (oldest == 0 || sweet_inuse_held[i]->gpuHoldAbs < oldest) {
			oldest = sweet_inuse_held[i]->gpuHoldAbs;
		}
	}
	if (oldest != 0) {
		absolutetime_to_nanoseconds(now - oldest, &ns);
	}
	IOLog("SWEET SURF INUSE: 5s: mode %d | publishes %u (in use %u) | gpu holds +%u -%u timeout %u overflow %u cancel %u, "
	    "held %u (max %u, oldest %llu ms) | in use now %u of %u surfaces (client %u display %u gpu %u)\n", sweet_inuse_mode,
	    sweet_inuse_st.publish, sweet_inuse_st.publishUsed, sweet_inuse_st.hold, sweet_inuse_st.holdEnd,
	    sweet_inuse_st.holdTimeout, sweet_inuse_st.holdOverflow, sweet_inuse_st.cancel, sweet_inuse_n, sweet_inuse_st.maxHeld,
	    ns / 1000000, used, total, cu, du, gu);
	IOLog("SWEET SURF INUSE: 5s: submits listed %u (named %u) all-imports %u, surfaces held %u, overflow %u | imports "
	    "matched %u unmatched %u | lock waits %u (%llu us, late %u)\n", sweet_inuse_st.subListed, sweet_inuse_st.subNamed,
	    sweet_inuse_st.subAll, sweet_inuse_st.subHeld, sweet_inuse_st.subOverflow, sweet_inuse_st.importHit,
	    sweet_inuse_st.importMiss, sweet_inuse_st.lockWait, sweet_inuse_st.lockWaitNs / 1000, sweet_inuse_st.lockWaitLate);
	bzero(&sweet_inuse_st, sizeof(sweet_inuse_st));
}

/*
 * 1258: drops GPU holds whose fence has landed (woken by the GPU interrupt, else every 5 ms while any is held); a
 * hold older than 2 s is dropped anyway (a fence lost in a GPU reset must not keep a surface in use for ever).
 */
static void
sweet_inuse_loop(void *arg __unused)
{
	uint64_t two_s, five_s, poll, next_stats;
	nanoseconds_to_absolutetime(2000000000ULL, &two_s);
	nanoseconds_to_absolutetime(5000000000ULL, &five_s);
	nanoseconds_to_absolutetime(5000000ULL, &poll);      /* a fallback: the GPU interrupt kicks */
	next_stats = mach_absolute_time() + five_s;
	IOLockLock(sweet_inuse_lock);
	for (;;) {
		uint64_t now = mach_absolute_time();
		for (uint32_t i = 0; i < sweet_inuse_n;) {
			SweetSurface *s = sweet_inuse_held[i];
			bool late = now - s->gpuHoldAbs > two_s;
			bool done = s->gpuFence != SWEET_SURF_FENCE_PENDING && sweet_gpu_fence_done(s->gpuFence);
			if (done || late || sweet_inuse_mode == 0 || (sweet_inuse_mode & 4) != 0) {
				s->gpuHold = 0;
				sweet_surface_publish_use(s);
				sweet_inuse_st.holdEnd++;
				sweet_inuse_st.holdTimeout += late;
				sweet_inuse_held[i] = sweet_inuse_held[--sweet_inuse_n];
				IOLockUnlock(sweet_inuse_lock);
				s->release();   /* maybe the last: SweetSurface::free outside our lock */
				IOLockLock(sweet_inuse_lock);
				continue;
			}
			i++;
		}
		bool logs = (sweet_inuse_mode & 2) != 0;
		if (logs && now >= next_stats) {
			sweet_inuse_stats_locked();
			next_stats = now + five_s;
		} else if (!logs) {
			next_stats = now + five_s;
		}
		uint64_t dl = sweet_inuse_n != 0 ? now + poll : next_stats;
		IOLockSleepDeadline(sweet_inuse_lock, &sweet_inuse_chan, dl, THREAD_UNINT);
	}
}

static void
sweet_inuse_start(void)
{
	if (sweet_inuse_lock == NULL) {
		IOLock *l = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, l, &sweet_inuse_lock)) {
			IOLockFree(l);
		}
	}
	IOLockLock(sweet_inuse_lock);
	bool start = !sweet_inuse_thread;
	sweet_inuse_thread = true;
	IOLockUnlock(sweet_inuse_lock);
	if (start && IOCreateThread(sweet_inuse_loop, NULL) == NULL) {
		IOLog("SWEET SURF INUSE: no hold thread\n");
		sweet_inuse_thread = false;
	}
}

/* sysctl kern.sweet_surf_inuse (bsd/dev/sweet_gpu.c) */
extern "C" int sweet_surface_inuse_set(int mode);
int
sweet_surface_inuse_set(int mode)
{
	if (mode < 0 || mode > 15) {
		return EINVAL;
	}
	int old = sweet_surface_inuse_mode();
	sweet_inuse_mode = mode;
	if (mode != 0) {
		sweet_inuse_start();
	}
	if ((old == 0) != (mode == 0)) {
		sweet_inuse_republish(mode);
	}
	if (sweet_inuse_lock != NULL) {
		IOLockWakeup(sweet_inuse_lock, &sweet_inuse_chan, false);      /* mode 0: the holds go now */
	}
	IOLog("SWEET SURF INUSE: mode %d (was %d; 1258: 0 slots only as 0983, 1 every surface + GPU holds, +2 5 s line, "
	    "+4 no GPU holds, +8 lock waits for the GPU)\n", mode, old);
	return 0;
}

/* 1258: SweetAccelerator.cpp: the surfaces a v2 submission uses, held before it is queued and until its fence lands */
void
sweet_surface_gpu_hold(const uint32_t *ids, const uint64_t *pas, const uint8_t *writes, uint32_t n, uint32_t fence)
{
	int mode = sweet_surface_inuse_mode();
	if (n == 0) {
		return;
	}
	if (mode == 0 || (mode & 4) != 0) {
		if (fence != SWEET_SURF_FENCE_PENDING && fence != SWEET_SURF_FENCE_CANCEL) {
			for (uint32_t i = 0; i < n && (mode & 8) != 0; i++) {
				/* lock waits without holds: still know the writer */
				SweetSurface *s = writes != NULL && writes[i] ? sweet_surface_copy(ids[i]) : NULL;
				if (s != NULL) {
					if (s->pa0 == pas[i]) {
						s->gpuWriteFence = fence;
					}
					s->release();
				}
			}
		}
		return;
	}
	if (!sweet_inuse_thread) {
		sweet_inuse_start();
		if (!sweet_inuse_thread) {
			return;
		}
	}
	bool added = false;
	uint64_t now = mach_absolute_time();
	for (uint32_t i = 0; i < n; i++) {
		SweetSurface *s = sweet_surface_copy(ids[i]);  /* retained: kept by the hold, else released below */
		if (s == NULL) {
			continue;
		}
		bool kept = false;
		IOLockLock(sweet_inuse_lock);
		if (s->pa0 != pas[i]) {
			/* the id was reused by another surface since the import */
		} else if (fence == SWEET_SURF_FENCE_CANCEL) {
			if (s->gpuHold != 0 && s->gpuFence == SWEET_SURF_FENCE_PENDING) {
				s->gpuHoldAbs = 0;      /* the thread drops it at its next look */
				sweet_inuse_st.cancel++;
				added = true;
			}
		} else if (s->gpuHold == 0) {
			if (sweet_inuse_n < SWEET_INUSE_MAX) {
				kept = true;
				s->gpuFence = fence;
				s->gpuHoldAbs = now;
				s->gpuHold = 1;
				sweet_inuse_held[sweet_inuse_n++] = s;
				added = true;
				sweet_inuse_st.hold++;
				if (sweet_inuse_n > sweet_inuse_st.maxHeld) {
					sweet_inuse_st.maxHeld = sweet_inuse_n;
				}
				sweet_surface_publish_use(s);
			} else {
				sweet_inuse_st.holdOverflow++;
			}
		} else if (fence != SWEET_SURF_FENCE_PENDING &&
		    (s->gpuFence == SWEET_SURF_FENCE_PENDING || (int32_t)(fence - s->gpuFence) > 0)) {
			s->gpuFence = fence;    /* one ring: the newest fence covers the older jobs */
			s->gpuHoldAbs = now;
		} else if (fence == SWEET_SURF_FENCE_PENDING) {
			s->gpuHoldAbs = now;    /* a new job is being queued: the 2 s cap starts again */
		}
		if (fence != SWEET_SURF_FENCE_PENDING && fence != SWEET_SURF_FENCE_CANCEL && writes != NULL && writes[i] &&
		    s->pa0 == pas[i]) {
			s->gpuWriteFence = fence;
		}
		IOLockUnlock(sweet_inuse_lock);
		if (!kept) {
			s->release();
		}
	}
	if (added) {
		IOLockWakeup(sweet_inuse_lock, &sweet_inuse_chan, false);
	}
}

void
sweet_surface_submit_stat(bool listed, uint32_t named, uint32_t held, bool overflow)
{
	if (listed) {
		sweet_inuse_st.subListed++;
		sweet_inuse_st.subNamed += named;
	} else {
		sweet_inuse_st.subAll++;
	}
	sweet_inuse_st.subHeld += held;
	sweet_inuse_st.subOverflow += overflow;
}

/*
 * 1258 bit 3 (8): IOSurfaceLock waits for the GPU as on iOS: read-only for the newest job that writes the surface,
 * read-write for the newest job that uses it. At most 200 ms (then the lock goes ahead, counted late).
 */
static void
sweet_inuse_lock_wait(SweetSurface *s, bool readOnly)
{
	int mode = sweet_surface_inuse_mode();
	if (mode == 0 || (mode & 8) == 0) {
		return;
	}
	uint32_t f = s->gpuWriteFence;
	if (!readOnly && s->gpuHold != 0 && s->gpuFence != SWEET_SURF_FENCE_PENDING) {
		f = s->gpuFence;
	}
	if (f == 0 || sweet_gpu_fence_done(f)) {
		return;
	}
	uint64_t t0 = mach_absolute_time(), ns = 0;
	bool late = sweet_gpu_wait_fence_ns(f, 200000000ULL) != 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	sweet_inuse_st.lockWait++;
	sweet_inuse_st.lockWaitLate += late;
	sweet_inuse_st.lockWaitNs += ns;
}

/* 1258: with kern.sweet_surf_inuse != 0, this client's own use counts (dropped with its reference) */
void
IOSurfaceRootUserClient::refUse(void *id, void *category, int delta)
{
	if (sweet_surface_inuse_mode() == 0) {
		return;
	}
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, NULL);
	if (ref != NULL) {
		int c = (uintptr_t)category == 1 ? 1 : 0;
		if (delta > 0) {
			ref->use[c]++;
		} else if (ref->use[c] > 0) {
			ref->use[c]--;
		}
	}
	IOLockUnlock(fLock);
}

IOReturn
IOSurfaceRootUserClient::trapIncUse(void *id, void *category, void *, void *, void *, void *)
{
	refUse(id, category, 1);
	sweet_surface_client_use(id, category, 1);
	return kIOReturnSuccess;
}

IOReturn
IOSurfaceRootUserClient::trapDecUse(void *id, void *category, void *, void *, void *, void *)
{
	refUse(id, category, -1);
	sweet_surface_client_use(id, category, -1);
	return kIOReturnSuccess;
}

static IOReturn
sweet_surface_seed_out(SweetSurface *s, void *seedOut, bool bump)
{
	uint32_t seed = bump ? OSIncrementAtomic((volatile SInt32 *)sweet_surface_seed(s)) + 1 :
	    *sweet_surface_seed(s);
	if (seedOut != NULL && copyout(&seed, (user_addr_t)seedOut, sizeof(seed)) != 0) {
		return kIOReturnBadArgument;
	}
	return kIOReturnSuccess;
}

/* Retained surface this client created or looked up, or NULL. */
static SweetSurface *
sweet_surface_held(IOLock *lock, OSArray *refs, uint32_t id)
{
	SweetSurface *s = NULL;
	IOLockLock(lock);
	for (unsigned int i = 0; i < refs->getCount(); i++) {
		SweetSurfaceRef *ref = (SweetSurfaceRef *)refs->getObject(i);
		if (ref->surface->id == id) {
			s = ref->surface;
			s->retain();
			break;
		}
	}
	IOLockUnlock(lock);
	return s;
}

IOReturn
IOSurfaceRootUserClient::trapLock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	sweet_inuse_lock_wait(s, ((uintptr_t)options & kIOSurfaceLockReadOnly) != 0);     /* 1258 bit 3 */
	IOReturn ret = sweet_surface_seed_out(s, seedOut, false);
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapUnlock(void *id, void *options, void *seedOut,
    void *, void *, void *)
{
	SweetSurface *s = sweet_surface_held(fLock, fRefs, (uint32_t)(uintptr_t)id);
	if (s == NULL) {
		return kIOReturnNotFound;
	}
	IOReturn ret = sweet_surface_seed_out(s, seedOut,
	    !((uintptr_t)options & kIOSurfaceLockReadOnly));
	s->release();
	return ret;
}

IOReturn
IOSurfaceRootUserClient::trapRelease(void *id, void *, void *, void *, void *, void *)
{
	unsigned int index;
	IOLockLock(fLock);
	SweetSurfaceRef *ref = findRef((uint32_t)(uintptr_t)id, &index);
	if (ref != NULL && --ref->count == 0) {
		fRefs->removeObject(index);     /* unmaps and drops the user count */
	}
	IOLockUnlock(fLock);
	return ref != NULL ? kIOReturnSuccess : kIOReturnNotFound;
}

static IOExternalTrap sweet_surface_traps[] = {
	[kSweetSurfaceTrapIncrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapIncUse },
	[kSweetSurfaceTrapDecrementUse]  = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapDecUse },
	[kSweetSurfaceTrapLock]          = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapLock },
	[kSweetSurfaceTrapUnlock]        = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapUnlock },
	[kSweetSurfaceTrapRelease]       = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
	[kSweetSurfaceTrapReleaseFailed] = { NULL, (IOTrap) & IOSurfaceRootUserClient::trapRelease },
};

IOExternalTrap *
IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **targetP, UInt32 index)
{
	if (index >= sizeof(sweet_surface_traps) / sizeof(sweet_surface_traps[0])) {
		sweet_surf_log_once("trap", index);
		return NULL;
	}
	sweet_surf_log_once("trap", index);
	*targetP = this;
	return &sweet_surface_traps[index];
}
#endif /* SWEET */
