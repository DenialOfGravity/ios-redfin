/*
 * The Adreno 640 in the kernel: the environment for the shared core
 * (sweet_a6xx.c, also built into the userland tool phone/gpu), bring-up in
 * a kernel thread, and the display's per-frame GPU blit
 * (iokit/Kernel/arm/SweetFramebuffer.cpp).
 *
 * Boot-arg sweetgpu=1 enables it. Firmware comes in the device tree as
 * /chosen properties "gpufw,<file>" (tools/mkpayload.py --gpufw DIR): the
 * GMU image, the SQE microcode and the zap shader (mdt + b02).
 *
 * Present: iOS's swapped IOSurface (1125x2436 BGRA, 4608-byte rows) is
 * mapped into the GPU once per surface (4 cached slots at 4 GB + n * 16 MB)
 * and blitted, scaled to fit and bilinear, into the panel memory, whose rows
 * the display engine then reads with a 4352-byte stride (the 2D engine's
 * destination pitch is in 64-byte units; ABL's 4320 is not one).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include "sweet_a6xx.h"
#include "../../iokit/Kernel/arm/SweetDisplayMask.h"

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern boolean_t pmap_valid_address(uint64_t pa);
extern vm_map_address_t phystokv(uint64_t pa);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t);
extern int vprintf(const char *fmt, va_list ap);
extern void sweet_smc(uint64_t x[8]);

void sweet_gpu_init(void);
bool sweet_gpu_ready(void);
int sweet_gpu_surface_slot(uint64_t key);
int sweet_gpu_map_surface(uint64_t key, const uint64_t *pages, uint32_t npages, uint32_t page_size);
int sweet_gpu_present(int slot, uintptr_t kva, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
	uint32_t fb_pitch, uint32_t corner_radius, uint32_t *out_dx, uint32_t *out_dy,
	uint32_t *out_dw, uint32_t *out_dh);
void sweet_gpu_panel_stride(uint32_t stride);
int sweet_gpu_map_user(uint64_t *iova_out, const uint64_t *pages, uint32_t npages, uint32_t page_size);
int sweet_gpu_submit_user(uint64_t ib_iova, uint32_t dwords, uint32_t timeout_ms, uint32_t *fence_out);

/* Panel mapping for touch (SweetHID.cpp): zero extents = CPU crop, otherwise the GPU's fitted rectangle. */
int sweet_display_fit_x, sweet_display_fit_y, sweet_display_fit_w, sweet_display_fit_h;

static struct a6_gpu gpu;
static vm_offset_t pool_kva;
/* More GPU page tables than the pool's arena (~250 tables = 500 MB mapped): 2048 more (4 GB) */
#define PT_EXTRA_SIZE   0x800000ULL
static vm_offset_t pt_kva;
static uint64_t pt_pa;
static volatile bool gpu_ready;
static LCK_GRP_DECLARE(sweet_gpu_grp, "sweet_gpu");
static LCK_MTX_DECLARE(sweet_gpu_lock, &sweet_gpu_grp);

/* ---- environment for sweet_a6xx.c ---- */

#define GPU_WIN		0x200000ULL
#define GPU_MAPS	24

static struct {
	vm_offset_t va;
	uint64_t win;
} gpu_map[GPU_MAPS];

/* Device windows (2 MB, kept for good: only a handful of register blocks). */
static volatile uint32_t *
gpu_io(uint64_t pa)
{
	uint64_t win = pa & ~(GPU_WIN - 1);
	for (unsigned i = 0; i < GPU_MAPS; i++) {
		if (gpu_map[i].va && gpu_map[i].win == win) {
			return (volatile uint32_t *)(gpu_map[i].va + (pa - win));
		}
		if (!gpu_map[i].va) {
			gpu_map[i].va = ml_io_map_unmappable((vm_offset_t)win, GPU_WIN, VM_WIMG_IO);
			gpu_map[i].win = win;
			return gpu_map[i].va ? (volatile uint32_t *)(gpu_map[i].va + (pa - win)) : NULL;
		}
	}
	panic("sweet_gpu: out of device windows at 0x%llx", pa);
}

static bool
in_pool(uint64_t pa, size_t len)
{
	return (pool_kva && pa >= gpu.pool_pa && pa + len <= gpu.pool_pa + A6_POOL_SIZE) ||
	       (pt_kva && pa >= pt_pa && pa + len <= pt_pa + PT_EXTRA_SIZE);
}

/* kernel VA of pool or page-table memory (in_pool) */
static vm_offset_t
pool_addr(uint64_t pa)
{
	return pa >= gpu.pool_pa && pa < gpu.pool_pa + A6_POOL_SIZE ? pool_kva + (pa - gpu.pool_pa) : pt_kva + (pa - pt_pa);
}

uint64_t
a6_pt_extra(uint32_t n)
{
	return pt_kva && (n + 1) * 0x1000ULL <= PT_EXTRA_SIZE ? pt_pa + n * 0x1000ULL : 0;
}

uint32_t
a6_rd(uint64_t pa)
{
	uint32_t v;
	if (in_pool(pa, 4)) {
		(void)a6_read(pa, &v, 4);
		return v;
	}
	v = *gpu_io(pa);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
a6_wr(uint64_t pa, uint32_t v)
{
	if (in_pool(pa, 4)) {
		(void)a6_write(pa, &v, 4);
		return;
	}
	__asm__ volatile ("dmb oshst" ::: "memory");
	*gpu_io(pa) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

/*
 * Pool: through its kernel mapping, cleaned to the point of coherency (by VA:
 * physical cache operations panic for RAM outside the physmap). Other managed
 * RAM: through the physmap. MMIO: words.
 */
int
a6_write(uint64_t pa, const void *buf, size_t len)
{
	if (in_pool(pa, len)) {
		memcpy((void *)pool_addr(pa), buf, len);
		flush_dcache64((addr64_t)pool_addr(pa), (unsigned)len, FALSE);
		return 0;
	}
	if (pmap_valid_address(pa)) {
		memcpy((void *)phystokv(pa), buf, len);
		flush_dcache64((addr64_t)pa, (unsigned)len, TRUE);
		return 0;
	}
	const uint32_t *w = buf;
	for (size_t i = 0; i < len / 4; i++) {
		a6_wr(pa + 4 * i, w[i]);
	}
	return 0;
}

int
a6_read(uint64_t pa, void *buf, size_t len)
{
	if (in_pool(pa, len)) {
		flush_dcache64((addr64_t)pool_addr(pa), (unsigned)len, FALSE);
		memcpy(buf, (const void *)pool_addr(pa), len);
		return 0;
	}
	uint32_t *w = buf;
	for (size_t i = 0; i < len / 4; i++) {
		w[i] = a6_rd(pa + 4 * i);
	}
	return 0;
}

void
a6_delay_ms(unsigned ms)
{
	delay((int)ms * 1000);
}

void
a6_delay_us(unsigned us)
{
	delay((int)us);
}

void
a6_log(const char *fmt, ...)
{
	va_list ap;
	printf("SWEET GPU: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void
a6_smc(uint64_t x[8])
{
	sweet_smc(x);
}

const uint8_t *
a6_firmware(const char *name, uint32_t *len)
{
	DTEntry chosen;
	void const *data;
	unsigned size;
	char prop[32];
	snprintf(prop, sizeof(prop), "gpufw,%s", name);
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, prop, &data, &size) != kSuccess) {
		a6_log("no firmware %s in the device tree (mkpayload.py --gpufw)\n", name);
		return NULL;
	}
	*len = size;
	return data;
}

/* ---- bring-up ---- */

static void
gpu_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t t0 = mach_absolute_time(), ns;
	lck_mtx_lock(&sweet_gpu_lock);
	int rc = a6_power_up(&gpu);
	if (!rc) {
		a6_unmap_high(&gpu);
		gpu_ready = true;
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	a6_log("bring-up %s in %llu ms (pool 0x%llx)\n", rc ? "FAILED" : "done, GPU ready", ns / 1000000,
	    gpu.pool_pa);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_gpu_init(void)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sweetgpu", &on, sizeof(on));
	if (!on) {
		return;
	}
	vm_offset_t va = 0;
	if (kmem_alloc_contig(kernel_map, &va, A6_POOL_SIZE, PAGE_MASK, 0, (ppnum_t)(atop_64(A6_POOL_ALIGN) - 1),
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		a6_log("no contiguous pool\n");
		return;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)va);
	a6_init_state(&gpu, ptoa_64(pn));
	uint32_t lvl = 4;	/* 499 MHz; boot-arg sweetgpufreq=1 (257) .. 5 (585) */
	PE_parse_boot_argn("sweetgpufreq", &lvl, sizeof(lvl));
	gpu.perf_level = lvl;
	pool_kva = va;
	vm_offset_t pva = 0;
	if (kmem_alloc_contig(kernel_map, &pva, PT_EXTRA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
		pt_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)pva));
		pt_kva = pva;
	} else {
		a6_log("no contiguous memory for more page tables\n");
	}
	flush_dcache64((addr64_t)va, (unsigned)A6_POOL_SIZE, FALSE);
	thread_t thread;
	if (kernel_thread_start(gpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

bool
sweet_gpu_ready(void)
{
	return gpu_ready;
}

/* ---- hangs and resets (all with sweet_gpu_lock held) ---- */

/*
 * A hang is no progress: the completed fence has not moved for a while although work is queued.
 * progress_abs restarts whenever work is queued on an idle GPU.
 */
static uint32_t progress_fence;
static uint64_t progress_abs;
/* Resets so far, and the fences each lost: (lo, hi] (the last LOST_RING of them) */
#define LOST_RING       8
static uint64_t recoveries;
static struct { uint32_t lo, hi; } lost[LOST_RING];

/*
 * Every 5 s: how long the GPU spent on the submits that landed (their CP_ALWAYS_ON stamps, 19.2 MHz), from the
 * submit path. Fences older than the stamp ring (A6_TS_SLOTS) are skipped.
 */
static uint32_t gstat_next = 1;
static uint64_t gstat_abs;

static void
gpu_stats_locked(void)
{
	uint64_t now = mach_absolute_time(), ns;
	if (gstat_abs == 0) {
		gstat_abs = now;
	}
	absolutetime_to_nanoseconds(now - gstat_abs, &ns);
	if (ns < 5000000000ULL) {
		return;
	}
	uint32_t done = a6_fence_read(&gpu), n = 0;
	if ((int32_t)(done - gstat_next) >= (int32_t)A6_TS_SLOTS) {
		gstat_next = done - A6_TS_SLOTS + 1;
	}
	uint64_t busy = 0, longest = 0;
	for (; (int32_t)(done - gstat_next) >= 0; gstat_next++, n++) {
		uint64_t t = a6_submit_time(&gpu, gstat_next);
		busy += t;
		if (t > longest) {
			longest = t;
		}
	}
	if (n) {
		a6_log("5s: %u submits, GPU busy %llu ms (%llu%%), longest %llu us, mean %llu us\n", n, busy / 19200,
		    busy * 100 / 19200 / (ns / 1000000), longest * 10 / 192, busy * 10 / 192 / n);
	}
	gstat_abs = now;
}

static void
gpu_note_submit_locked(void)
{
	uint32_t done = a6_fence_read(&gpu);
	if (done == gpu.fence) {
		progress_fence = done;
		progress_abs = mach_absolute_time();
	}
	gpu_stats_locked();
}

static bool
gpu_hung_locked(uint32_t hang_ms)
{
	uint32_t done = a6_fence_read(&gpu);
	uint64_t now = mach_absolute_time(), ns;
	if (done != progress_fence || (int32_t)(gpu.fence - done) <= 0) {
		progress_fence = done;
		progress_abs = now;
		return false;
	}
	absolutetime_to_nanoseconds(now - progress_abs, &ns);
	return ns > hang_ms * 1000000ULL;
}

/* Soft reset + CP re-init; everything queued is lost, its fences complete at once (waiters return). */
static void
gpu_recover_locked(const char *why)
{
	uint64_t far;
	uint32_t syn, fsr = a6_fault_status(&gpu, &far, &syn), done = a6_fence_read(&gpu);
	a6_log("%s: GPU hang or fault (fence %u, done %u, smmu fsr %x far %llx fsynr0 %x): reset\n",
	    why, gpu.fence, done, fsr, far, syn);
	/* where the CP was: ring read pointer, IB1/IB2 (base + dwords left), SQE status */
	a6_log("%s: rb rptr %x wptr %x, ib1 %08x%08x rem %x, ib2 %08x%08x rem %x, rbbm status %x, sqe stat %x\n", why,
	    a6_rd(A6_GREG(0x806)), gpu.wptr, a6_rd(A6_GREG(0x929)), a6_rd(A6_GREG(0x928)), a6_rd(A6_GREG(0x92a)),
	    a6_rd(A6_GREG(0x92c)), a6_rd(A6_GREG(0x92b)), a6_rd(A6_GREG(0x92d)), a6_rd(A6_GREG(0x210)),
	    a6_rd(A6_GREG(0x908)));
	lost[recoveries % LOST_RING].lo = done;
	lost[recoveries % LOST_RING].hi = gpu.fence;
	recoveries++;
	if (a6_recover(&gpu)) {
		a6_log("%s: GPU not recovered\n", why);
		gpu_ready = false;
	}
	a6_fence_force(&gpu);
	progress_fence = gpu.fence;
	progress_abs = mach_absolute_time();
}

/* ---- the panel ---- */

#define MDP             0x0ae00000ULL
#define SSPP_VIG(n)     (MDP + 0x5000 + 0x2000ULL * (n))
#define SSPP_YSTRIDE0   0x24
#define CTL0_FLUSH      (MDP + 0x2000 + 0x18)

/* ABL scans out through VIG0 + VIG1 (540 columns each); autorefresh picks a flushed change up. */
void
sweet_gpu_panel_stride(uint32_t stride)
{
	a6_wr(SSPP_VIG(0) + SSPP_YSTRIDE0, stride);
	a6_wr(SSPP_VIG(1) + SSPP_YSTRIDE0, stride);
	a6_wr(CTL0_FLUSH, 0x3);
	a6_log("panel stride %u\n", stride);
}

/* ---- surfaces ---- */

#define SLOTS           4
#define SLOT_IOVA(n)    (0x100000000ULL + 0x1000000ULL * (n))
#define SLOT_MAX        0x1000000ULL
#define SLOT_PAGES      1024

static struct {
	uint64_t key;
	uint64_t pages[SLOT_PAGES];
	uint32_t npages, page_size;
	uint64_t used;          /* for LRU */
} slot[SLOTS];
static uint64_t slot_clock;

int
sweet_gpu_surface_slot(uint64_t key)
{
	for (int i = 0; i < SLOTS; i++) {
		if (slot[i].key == key && slot[i].npages) {
			slot[i].used = ++slot_clock;
			return i;
		}
	}
	return -1;
}

int
sweet_gpu_map_surface(uint64_t key, const uint64_t *pages, uint32_t npages, uint32_t page_size)
{
	if (!gpu_ready || npages > SLOT_PAGES || (uint64_t)npages * page_size > SLOT_MAX) {
		return -1;
	}
	int s = 0;
	for (int i = 1; i < SLOTS; i++) {
		if (slot[i].used < slot[s].used) {
			s = i;
		}
	}
	lck_mtx_lock(&sweet_gpu_lock);
	int rc = a6_map_pages(&gpu, SLOT_IOVA(s), pages, npages, page_size);
	a6_tlb_flush(&gpu);
	lck_mtx_unlock(&sweet_gpu_lock);
	if (rc) {
		slot[s].npages = 0;
		return -1;
	}
	memcpy(slot[s].pages, pages, npages * sizeof(uint64_t));
	slot[s].npages = npages;
	slot[s].page_size = page_size;
	slot[s].key = key;
	slot[s].used = ++slot_clock;
	a6_log("surface %llx -> slot %d (%u pages of %u)\n", key, s, npages, page_size);
	return s;
}

#define IB_OFF          A6_OFF_FREE     /* the present IB in the pool */
#define PANEL_W         1080
#define PANEL_H         2280
#define DISPLAY_MASK_OFF        0x500000ULL     /* 2x2 corner mattes, one per corner */
#define DISPLAY_MASK_SIDE       256u
#define DISPLAY_MASK_DIM        (DISPLAY_MASK_SIDE * 2)
#define DISPLAY_MASK_PITCH      (DISPLAY_MASK_DIM * 4)
#define DISPLAY_STAGE_OFF       0x600000ULL     /* panel-sized; only the corner bands are used */

static uint32_t display_mask_radius;

static uint64_t stat_frames, stat_clean_abs, stat_gpu_abs;

/* The four corner mattes: black, alpha = 1 - coverage of the image (SweetDisplayMask.h). */
static void
display_mask_build(uint32_t r, uint32_t ext)
{
	uint32_t *atlas = (uint32_t *)(pool_kva + DISPLAY_MASK_OFF);
	uint8_t row[DISPLAY_MASK_SIDE];
	uint32_t seg = 0;

	for (uint32_t cy = 0; cy < ext; cy++) {
		sweet_display_corner_row(r, ext, cy, &seg, row);
		for (uint32_t corner = 0; corner < 4; corner++) {
			/* Bit 0: right corner, bit 1: bottom; mirror the top-left one. */
			uint32_t ty = (corner >> 1) * DISPLAY_MASK_SIDE + ((corner & 2) ? ext - 1 - cy : cy);
			uint32_t *line = atlas + (size_t)ty * DISPLAY_MASK_DIM + (corner & 1) * DISPLAY_MASK_SIDE;
			for (uint32_t cx = 0; cx < ext; cx++) {
				line[(corner & 1) ? ext - 1 - cx : cx] = (uint32_t)(255 - row[cx]) << 24;
			}
		}
	}
	flush_dcache64((addr64_t)(uintptr_t)atlas, DISPLAY_MASK_PITCH * DISPLAY_MASK_DIM, FALSE);
}

/*
 * Blit a mapped surface, scaled to fit and centred, into the panel memory
 * (rows of fb_pitch bytes). With corner_radius, the image gets continuous
 * corners: the rows they cover go to a staging image in the pool first (same
 * blit, scissored), get the mattes blended over, and only then reach the
 * panel, which scans out continuously. The CPU rendered the source through
 * its cache: clean it through kva first (0: no clean). Waits for the GPU (a
 * few ms).
 */
int
sweet_gpu_present(int s, uintptr_t kva, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch,
	uint32_t fb_pitch, uint32_t corner_radius, uint32_t *out_dx, uint32_t *out_dy,
	uint32_t *out_dw, uint32_t *out_dh)
{
	if (!gpu_ready || s < 0 || s >= SLOTS || !slot[s].npages || w == 0 || h == 0 ||
	    (pitch & 63) || (fb_pitch & 63) || fb_pitch < PANEL_W * 4 ||
	    DISPLAY_STAGE_OFF + (uint64_t)fb_pitch * PANEL_H > A6_POOL_SIZE) {
		return -1;
	}
	uint64_t t0 = mach_absolute_time();
	if (kva) {
		clean_dcache64((addr64_t)kva, (unsigned)((uint64_t)pitch * h + offset), FALSE);
	}
	uint64_t t1 = mach_absolute_time();

	struct a6_blit frame = {
		.src_iova = SLOT_IOVA(s) + offset, .src_w = w, .src_h = h, .src_pitch = pitch,
		.sw = w, .sh = h, .dst_iova = A6_GVA_FB, .dst_pitch = fb_pitch, .filter = true,
	};
	/* Fit: full height unless the frame is relatively wider than the panel. */
	if ((uint64_t)w * PANEL_H <= (uint64_t)PANEL_W * h) {
		frame.dh = PANEL_H;
		frame.dw = (uint32_t)(((uint64_t)w * PANEL_H + h / 2) / h);
	} else {
		frame.dw = PANEL_W;
		frame.dh = (uint32_t)(((uint64_t)h * PANEL_W + w / 2) / w);
	}
	frame.dx = (PANEL_W - frame.dw) / 2;
	frame.dy = (PANEL_H - frame.dh) / 2;
	uint32_t ext = corner_radius ? sweet_display_corner_extent(corner_radius) : 0;
	if (ext > DISPLAY_MASK_SIDE || 2 * ext >= frame.dw || 2 * ext >= frame.dh) {
		ext = 0;
	}
	static uint32_t ib[10 * A6_BLIT_MAX_DW];
	lck_mtx_lock(&sweet_gpu_lock);
	uint32_t n = 0;
	if (ext) {
		if (display_mask_radius != corner_radius) {
			display_mask_build(corner_radius, ext);
			display_mask_radius = corner_radius;
		}
		uint32_t top = frame.dy, bottom = frame.dy + frame.dh - ext;
		struct a6_blit b = frame;
		b.clip = true;
		b.cx0 = 0;
		b.cx1 = PANEL_W - 1;
		/* The rows between the corners straight to the panel ... */
		b.cy0 = top + ext;
		b.cy1 = bottom - 1;
		n += a6_blit_ib(&gpu, &b, ib + n);
		/* ... the corner rows to the stage ... */
		b.dst_iova = A6_GVA_POOL + DISPLAY_STAGE_OFF;
		b.cy0 = top;
		b.cy1 = top + ext - 1;
		n += a6_blit_ib(&gpu, &b, ib + n);
		b.cy0 = bottom;
		b.cy1 = bottom + ext - 1;
		n += a6_blit_ib(&gpu, &b, ib + n);
		/* ... darkened by the mattes ... */
		for (uint32_t corner = 0; corner < 4; corner++) {
			struct a6_blit matte = {
				.src_iova = A6_GVA_POOL + DISPLAY_MASK_OFF,
				.src_w = DISPLAY_MASK_DIM, .src_h = DISPLAY_MASK_DIM, .src_pitch = DISPLAY_MASK_PITCH,
				.sx = (corner & 1) * DISPLAY_MASK_SIDE, .sy = (corner >> 1) * DISPLAY_MASK_SIDE,
				.sw = ext, .sh = ext,
				.dst_iova = A6_GVA_POOL + DISPLAY_STAGE_OFF, .dst_pitch = fb_pitch,
				.dx = (corner & 1) ? frame.dx + frame.dw - ext : frame.dx,
				.dy = (corner & 2) ? bottom : top,
				.dw = ext, .dh = ext, .blend = true,
			};
			n += a6_blit_ib(&gpu, &matte, ib + n);
		}
		/* ... then copied to the panel. */
		for (uint32_t band = 0; band < 2; band++) {
			uint32_t y = band ? bottom : top;
			struct a6_blit copy = {
				.src_iova = A6_GVA_POOL + DISPLAY_STAGE_OFF,
				.src_w = PANEL_W, .src_h = PANEL_H, .src_pitch = fb_pitch,
				.sx = frame.dx, .sy = y, .sw = frame.dw, .sh = ext,
				.dst_iova = A6_GVA_FB, .dst_pitch = fb_pitch,
				.dx = frame.dx, .dy = y, .dw = frame.dw, .dh = ext,
			};
			n += a6_blit_ib(&gpu, &copy, ib + n);
		}
	} else {
		n = a6_blit_ib(&gpu, &frame, ib);
	}
	a6_write(a6_pool_pa(&gpu, IB_OFF), ib, 4ULL * n);
	/*
	 * Queued behind whatever the Metal clients submitted: wait for our own fence, and reset the GPU only
	 * when it stops making progress (long but healthy client work must not count as a hang).
	 */
	gpu_note_submit_locked();
	uint32_t fence = a6_submit_ib_async(&gpu, a6_pool_va(IB_OFF), n, 100);
	int rc = fence ? 0 : -1;
	while (fence && !a6_fence_done(&gpu, fence)) {
		if (gpu_hung_locked(500)) {
			gpu_recover_locked("present");
			rc = -1;
			break;
		}
		delay(20);
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	uint64_t t2 = mach_absolute_time();
	stat_clean_abs += t1 - t0;
	stat_gpu_abs += t2 - t1;
	if ((++stat_frames % 300) == 0) {
		uint64_t c, g;
		absolutetime_to_nanoseconds(stat_clean_abs, &c);
		absolutetime_to_nanoseconds(stat_gpu_abs, &g);
		a6_log("present: %llu frames, avg cache clean %llu us, GPU blit %llu us\n", stat_frames,
		    c / 1000 / 300, g / 1000 / 300);
		stat_clean_abs = stat_gpu_abs = 0;
	}
	*out_dx = frame.dx;
	*out_dy = frame.dy;
	*out_dw = frame.dw;
	*out_dh = frame.dh;
	return rc;
}

/* ---- buffers and submissions for Metal (iokit/Kernel/arm/SweetAccelerator.cpp) ---- */

#define USER_VA_BASE    0x200000000ULL  /* above the surface slots */
#define USER_VA_END     0x1000000000ULL /* v1 clients; v2 windows above (sweet_gpu_window_alloc) */
#define USER_VA_ALIGN   0x10000ULL

static uint64_t user_va_next = USER_VA_BASE;

/* Freed ranges, sorted by address and merged: reused first fit, so page tables are reused too. */
#define USER_VA_FREE_MAX 1024
static struct { uint64_t va, size; } user_va_free[USER_VA_FREE_MAX];
static uint32_t user_va_nfree;

static uint64_t
user_va_alloc(uint64_t size)
{
	for (uint32_t i = 0; i < user_va_nfree; i++) {
		if (user_va_free[i].size >= size) {
			uint64_t va = user_va_free[i].va;
			user_va_free[i].va += size;
			user_va_free[i].size -= size;
			if (user_va_free[i].size == 0) {
				memmove(&user_va_free[i], &user_va_free[i + 1], (user_va_nfree - i - 1) * sizeof(user_va_free[0]));
				user_va_nfree--;
			}
			return va;
		}
	}
	if (user_va_next + size > USER_VA_END) {
		return 0;
	}
	uint64_t va = user_va_next;
	user_va_next += size;
	return va;
}

static void
user_va_release(uint64_t va, uint64_t size)
{
	uint32_t i = 0;
	while (i < user_va_nfree && user_va_free[i].va < va) {
		i++;
	}
	if (i > 0 && user_va_free[i - 1].va + user_va_free[i - 1].size == va) {
		user_va_free[i - 1].size += size;
		if (i < user_va_nfree && va + size == user_va_free[i].va) {
			user_va_free[i - 1].size += user_va_free[i].size;
			memmove(&user_va_free[i], &user_va_free[i + 1], (user_va_nfree - i - 1) * sizeof(user_va_free[0]));
			user_va_nfree--;
		}
		return;
	}
	if (i < user_va_nfree && va + size == user_va_free[i].va) {
		user_va_free[i].va = va;
		user_va_free[i].size += size;
		return;
	}
	if (user_va_nfree == USER_VA_FREE_MAX) {
		return;         /* leaks the range */
	}
	memmove(&user_va_free[i + 1], &user_va_free[i], (user_va_nfree - i) * sizeof(user_va_free[0]));
	user_va_free[i].va = va;
	user_va_free[i].size = size;
	user_va_nfree++;
}

/*
 * Map pages at a free GPU VA (64 KB granules; freed ranges are reused, and
 * with them the page tables, of which the arena has only ~250). The caller
 * cleans the CPU caches through a virtual mapping: physical cache operations
 * panic for RAM outside the physmap (kernel memory below 4 GB, "phystokv_range:
 * illegal PA").
 */
int
sweet_gpu_map_user(uint64_t *iova_out, const uint64_t *pages, uint32_t npages, uint32_t page_size)
{
	if (!gpu_ready || npages == 0) {
		return -1;
	}
	uint64_t size = ((uint64_t)npages * page_size + USER_VA_ALIGN - 1) & ~(USER_VA_ALIGN - 1);
	lck_mtx_lock(&sweet_gpu_lock);
	uint64_t iova = user_va_alloc(size);
	if (iova == 0) {
		lck_mtx_unlock(&sweet_gpu_lock);
		return -1;
	}
	int rc = a6_map_pages(&gpu, iova, pages, npages, page_size);
	if (rc) {
		a6_unmap(&gpu, iova, size);
		user_va_release(iova, size);
	}
	a6_tlb_flush(&gpu);
	lck_mtx_unlock(&sweet_gpu_lock);
	if (rc) {
		return -1;
	}
	*iova_out = iova;
	return 0;
}

/* Undo sweet_gpu_map_user. The caller makes sure no queued GPU work uses it (SweetAccelerator: the client's last fence). */
void
sweet_gpu_unmap_user(uint64_t iova, uint64_t len)
{
	uint64_t size = (len + USER_VA_ALIGN - 1) & ~(USER_VA_ALIGN - 1);
	if (iova < USER_VA_BASE || size == 0) {
		return;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	if (gpu_ready) {
		a6_unmap(&gpu, iova, size);
		a6_tlb_flush(&gpu);
	}
	user_va_release(iova, size);
	lck_mtx_unlock(&sweet_gpu_lock);
}

/* Queue an IB (user GPU memory) without waiting: its fence, or 0 if the GPU is not usable. */
uint32_t
sweet_gpu_submit_user_async(uint64_t ib_iova, uint32_t dwords)
{
	if (!gpu_ready) {
		return 0;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	gpu_note_submit_locked();
	uint32_t fence = a6_submit_ib_async(&gpu, ib_iova, dwords, 1000);
	lck_mtx_unlock(&sweet_gpu_lock);
	return fence;
}

/* Wait for a fence of sweet_gpu_submit_user_async: 0, or -1 after timeout_ms (the GPU is then recovered). */
int
sweet_gpu_wait_fence(uint32_t fence, uint32_t timeout_ms)
{
	for (unsigned i = 0; i < timeout_ms * 50; i++) {
		if (!gpu_ready) {
			return -1;
		}
		if (a6_fence_done(&gpu, fence)) {
			return 0;
		}
		delay(20);
	}
	lck_mtx_lock(&sweet_gpu_lock);
	int rc = a6_fence_done(&gpu, fence) ? 0 : -1;
	if (rc) {
		gpu_recover_locked("user fence");
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	return rc;
}

/* Run an IB (user GPU memory) and wait for it. 0, or -1 on a hang/fault (cleared, logged). */
int
sweet_gpu_submit_user(uint64_t ib_iova, uint32_t dwords, uint32_t timeout_ms, uint32_t *fence_out)
{
	if (!gpu_ready) {
		return -1;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	gpu_note_submit_locked();
	int rc = a6_submit_ib(&gpu, ib_iova, dwords, timeout_ms ? timeout_ms : 1000);
	if (rc) {
		gpu_recover_locked("user submit");
	}
	*fence_out = gpu.fence;
	lck_mtx_unlock(&sweet_gpu_lock);
	return rc;
}

/*
 * ---- v2 buffer memory: RAM that XNU does not manage ----
 *
 * SWEET has no PPL, so no __ARM_PTE_PHYSMAP__: the physmap maps all managed RAM with cacheable blocks,
 * which pmap_set_cache_attributes cannot change per page. A write-combined view of a managed page is
 * then a mismatched alias: writes got lost and reads came back stale (Turnip's command streams, phone
 * test v2test). v2 buffers come from HLOS RAM below 4 GB that XNU never maps instead (the flame memory
 * node is <0xc0000000 0x140000000>; XNU takes 4 GB up; the boot ramdisk at 0xc0000000 is dead once the
 * shim has jumped; phone/wifi's radio uses 0xe0000000-0xe1000000). Units of 16 KB (a CPU page).
 */
#define GPOOL_UNIT      0x4000ULL
#define GPOOL_FREE_MAX  4096
static const struct { uint64_t pa, size; } gpool_ranges[] = {
	{ 0xc4000000ULL, 0x1c000000ULL },       /* 448 MB (the first 64 MB left to ABL's ramdisk) */
	{ 0xe1000000ULL, 0x1f000000ULL },       /* 496 MB */
};
static struct { uint64_t pa, size; } gpool_free[GPOOL_FREE_MAX];       /* sorted by address, merged */
static uint32_t gpool_nfree;
static uint64_t gpool_used, gpool_peak;
static bool gpool_ready;
static LCK_MTX_DECLARE(sweet_gpool_lock, &sweet_gpu_grp);

/* First fit: the physical address of `bytes` (rounded to 16 KB), zeroing is the caller's. 0 = ok. */
int
sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa)
{
	uint64_t size = (bytes + GPOOL_UNIT - 1) & ~(GPOOL_UNIT - 1);
	int rc = -1;
	lck_mtx_lock(&sweet_gpool_lock);
	if (!gpool_ready) {
		for (unsigned i = 0; i < sizeof(gpool_ranges) / sizeof(gpool_ranges[0]); i++) {
			gpool_free[gpool_nfree].pa = gpool_ranges[i].pa;
			gpool_free[gpool_nfree].size = gpool_ranges[i].size;
			gpool_nfree++;
		}
		gpool_ready = true;
	}
	for (uint32_t i = 0; i < gpool_nfree; i++) {
		if (gpool_free[i].size >= size) {
			*pa = gpool_free[i].pa;
			gpool_free[i].pa += size;
			gpool_free[i].size -= size;
			if (gpool_free[i].size == 0) {
				memmove(&gpool_free[i], &gpool_free[i + 1], (gpool_nfree - i - 1) * sizeof(gpool_free[0]));
				gpool_nfree--;
			}
			gpool_used += size;
			if (gpool_used > gpool_peak) {
				gpool_peak = gpool_used;
			}
			if ((gpool_used >> 25) != ((gpool_used - size) >> 25)) {
				a6_log("pool: %llu MB in use (peak %llu)\n", gpool_used >> 20, gpool_peak >> 20);
			}
			rc = 0;
			break;
		}
	}
	lck_mtx_unlock(&sweet_gpool_lock);
	if (rc) {
		a6_log("pool: no %llu bytes free (%llu MB in use)\n", size, gpool_used >> 20);
	}
	return rc;
}

void
sweet_gpu_pool_free(uint64_t pa, uint64_t bytes)
{
	uint64_t size = (bytes + GPOOL_UNIT - 1) & ~(GPOOL_UNIT - 1);
	lck_mtx_lock(&sweet_gpool_lock);
	gpool_used -= size;
	if ((gpool_used >> 25) != ((gpool_used + size) >> 25)) {
		a6_log("pool: %llu MB in use (peak %llu)\n", gpool_used >> 20, gpool_peak >> 20);
	}
	uint32_t i = 0;
	while (i < gpool_nfree && gpool_free[i].pa < pa) {
		i++;
	}
	if (i > 0 && gpool_free[i - 1].pa + gpool_free[i - 1].size == pa) {
		gpool_free[i - 1].size += size;
		if (i < gpool_nfree && pa + size == gpool_free[i].pa) {
			gpool_free[i - 1].size += gpool_free[i].size;
			memmove(&gpool_free[i], &gpool_free[i + 1], (gpool_nfree - i - 1) * sizeof(gpool_free[0]));
			gpool_nfree--;
		}
	} else if (i < gpool_nfree && pa + size == gpool_free[i].pa) {
		gpool_free[i].pa = pa;
		gpool_free[i].size += size;
	} else if (gpool_nfree < GPOOL_FREE_MAX) {
		memmove(&gpool_free[i + 1], &gpool_free[i], (gpool_nfree - i) * sizeof(gpool_free[0]));
		gpool_free[i].pa = pa;
		gpool_free[i].size = size;
		gpool_nfree++;
	} else {
		a6_log("pool: free list full, %llu bytes at %llx leak\n", size, pa);
	}
	lck_mtx_unlock(&sweet_gpool_lock);
}

/*
 * ---- v2 clients: Turnip's darwin backend (SweetAccelerator.cpp, NativeAdreno ABI) ----
 *
 * The client picks its GPU addresses (Turnip's own VMA heap), so each connection gets a window of
 * the one GPU address space to itself: 4 GB windows from 64 GB up, above the v1 range.
 */
#define WIN_BASE        0x1000000000ULL
#define WIN_SIZE        0x100000000ULL
#define WIN_COUNT       64
static uint64_t win_used;               /* bit n: window n taken */

int
sweet_gpu_window_alloc(uint64_t *base, uint64_t *size)
{
	lck_mtx_lock(&sweet_gpu_lock);
	for (unsigned n = 0; n < WIN_COUNT; n++) {
		if (!(win_used & (1ULL << n))) {
			win_used |= 1ULL << n;
			lck_mtx_unlock(&sweet_gpu_lock);
			*base = WIN_BASE + n * WIN_SIZE;
			*size = WIN_SIZE;
			return 0;
		}
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	return -1;
}

void
sweet_gpu_window_free(uint64_t base)
{
	if (base < WIN_BASE || base >= WIN_BASE + WIN_COUNT * WIN_SIZE) {
		return;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	win_used &= ~(1ULL << ((base - WIN_BASE) / WIN_SIZE));
	lck_mtx_unlock(&sweet_gpu_lock);
}

/* 4K pages at iova (inside the caller's window), then a TLB flush. */
int
sweet_gpu_map_at(uint64_t iova, const uint64_t *pages, uint32_t npages)
{
	if (!gpu_ready || npages == 0 || iova < WIN_BASE) {
		return -1;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	int rc = a6_map_pages(&gpu, iova, pages, npages, 0x1000);
	if (rc) {
		a6_unmap(&gpu, iova, (uint64_t)npages << 12);
	}
	a6_tlb_flush(&gpu);
	lck_mtx_unlock(&sweet_gpu_lock);
	return rc;
}

void
sweet_gpu_unmap_at(uint64_t iova, uint64_t len)
{
	if (iova < WIN_BASE || len == 0) {
		return;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	if (gpu_ready) {
		a6_unmap(&gpu, iova, len);
		a6_tlb_flush(&gpu);
	}
	lck_mtx_unlock(&sweet_gpu_lock);
}

/* Queue up to A6_SUBMIT_MAX IBs: the fence, 0 when the GPU is not usable, or 0 with *busy when the ring is full. */
uint32_t
sweet_gpu_submit_v2(const uint64_t *iova, const uint32_t *dwords, unsigned n, bool *busy)
{
	*busy = false;
	if (!gpu_ready) {
		return 0;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	gpu_note_submit_locked();
	uint32_t fence = a6_submit_ibs_async(&gpu, iova, dwords, n, 0);
	lck_mtx_unlock(&sweet_gpu_lock);
	*busy = fence == 0 && n > 0 && n <= A6_SUBMIT_MAX;
	return fence;
}

bool
sweet_gpu_fence_done(uint32_t fence)
{
	return !gpu_ready || a6_fence_done(&gpu, fence);
}

/*
 * Wait up to timeout_ns (0: look once) for a fence: 0 done, 1 timeout. A GPU that makes no progress for
 * a second is reset here (its queued work is lost; sweet_gpu_lost tells the clients).
 */
int
sweet_gpu_wait_fence_ns(uint32_t fence, uint64_t timeout_ns)
{
	/* the hang check by time, not per call: Turnip's darwin backend waits in 1 ms slices */
	static uint64_t last_check;
	uint64_t t0 = mach_absolute_time(), ns = 0, check_abs;
	nanoseconds_to_absolutetime(5000000, &check_abs);
	for (;;) {
		if (sweet_gpu_fence_done(fence)) {
			return 0;
		}
		uint64_t now = mach_absolute_time();
		if (now - last_check > check_abs) {
			last_check = now;
			lck_mtx_lock(&sweet_gpu_lock);
			if (!a6_fence_done(&gpu, fence) && gpu_hung_locked(1000)) {
				gpu_recover_locked("v2 fence wait");
			}
			lck_mtx_unlock(&sweet_gpu_lock);
		}
		absolutetime_to_nanoseconds(now - t0, &ns);
		if (ns >= timeout_ns) {
			return sweet_gpu_fence_done(fence) ? 0 : 1;
		}
		delay(20);
	}
}

/*
 * Did a reset since *epoch lose work of a client whose last fence is last_fence? Moves *epoch to now.
 * (Fences are global and in submission order: the client lost work if its last one was in a lost range.)
 */
bool
sweet_gpu_lost(uint64_t *epoch, uint32_t last_fence)
{
	bool hit = false;
	lck_mtx_lock(&sweet_gpu_lock);
	if (recoveries - *epoch > LOST_RING) {
		*epoch = recoveries - LOST_RING;
	}
	for (; *epoch < recoveries; (*epoch)++) {
		uint32_t lo = lost[*epoch % LOST_RING].lo, hi = lost[*epoch % LOST_RING].hi;
		if (last_fence && (int32_t)(last_fence - lo) > 0 && (int32_t)(hi - last_fence) >= 0) {
			hit = true;
		}
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	return hit;
}

uint64_t
sweet_gpu_recoveries(void)
{
	return recoveries;
}

uint64_t
sweet_gpu_timestamp(void)
{
	return gpu_ready ? a6_timestamp(&gpu) : 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
