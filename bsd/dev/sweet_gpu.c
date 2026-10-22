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
int sweet_gpu_present(int slot, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fb_pitch,
    uint32_t *out_dx, uint32_t *out_dw);
void sweet_gpu_panel_stride(uint32_t stride);

/* Panel mapping for touch (SweetHID.cpp): 0 = the crop of iOS's frame, else x offset + width of the fit. */
int sweet_display_fit_x, sweet_display_fit_w;

static struct a6_gpu gpu;
static vm_offset_t pool_kva;
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
	return pool_kva && pa >= gpu.pool_pa && pa + len <= gpu.pool_pa + A6_POOL_SIZE;
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

/* Pool (and other kernel RAM): through the physmap, cleaned to the point of coherency. MMIO: words. */
int
a6_write(uint64_t pa, const void *buf, size_t len)
{
	if (in_pool(pa, len)) {
		memcpy((void *)(pool_kva + (pa - gpu.pool_pa)), buf, len);
		flush_dcache64((addr64_t)pa, (unsigned)len, TRUE);
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
		flush_dcache64((addr64_t)pa, (unsigned)len, TRUE);
		memcpy(buf, (const void *)(pool_kva + (pa - gpu.pool_pa)), len);
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
	pool_kva = va;
	flush_dcache64((addr64_t)gpu.pool_pa, (unsigned)A6_POOL_SIZE, TRUE);
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

static uint64_t stat_frames, stat_clean_abs, stat_gpu_abs;

/*
 * Blit a mapped surface, scaled to fit and centred, into the panel memory
 * (rows of fb_pitch bytes). The CPU rendered it through the cache: clean
 * its pages to memory first. Waits for the GPU (a few ms).
 */
int
sweet_gpu_present(int s, uint32_t offset, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fb_pitch,
    uint32_t *out_dx, uint32_t *out_dw)
{
	if (!gpu_ready || s < 0 || s >= SLOTS || !slot[s].npages || (pitch & 63) || (fb_pitch & 63)) {
		return -1;
	}
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < slot[s].npages; i++) {
		clean_dcache64((addr64_t)slot[s].pages[i], slot[s].page_size, TRUE);
	}
	uint64_t t1 = mach_absolute_time();

	struct a6_blit b = {
		.src_iova = SLOT_IOVA(s) + offset, .src_w = w, .src_h = h, .src_pitch = pitch,
		.sw = w, .sh = h, .dst_iova = A6_GVA_FB, .dst_pitch = fb_pitch, .filter = true,
	};
	/* Fit: full height unless the frame is relatively wider than the panel. */
	if ((uint64_t)w * PANEL_H <= (uint64_t)PANEL_W * h) {
		b.dh = PANEL_H;
		b.dw = (uint32_t)(((uint64_t)w * PANEL_H + h / 2) / h);
	} else {
		b.dw = PANEL_W;
		b.dh = (uint32_t)(((uint64_t)h * PANEL_W + w / 2) / w);
	}
	b.dx = (PANEL_W - b.dw) / 2;
	b.dy = (PANEL_H - b.dh) / 2;
	static uint32_t dw[A6_BLIT_MAX_DW];
	lck_mtx_lock(&sweet_gpu_lock);
	uint32_t n = a6_blit_ib(&gpu, &b, dw);
	a6_write(a6_pool_pa(&gpu, IB_OFF), dw, 4ULL * n);
	int rc = a6_submit_ib(&gpu, a6_pool_va(IB_OFF), n, 100);
	if (rc) {
		uint64_t far;
		uint32_t syn, fsr = a6_fault_status(&gpu, &far, &syn);
		a6_log("present: GPU timeout (fence %u, memory %u, smmu fsr %x far %llx); back to the CPU copy\n",
		    gpu.fence, a6_fence_read(&gpu), fsr, far);
		(void)a6_clear_fault(&gpu);
		gpu_ready = false;
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
	*out_dx = b.dx;
	*out_dw = b.dw;
	return rc;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
