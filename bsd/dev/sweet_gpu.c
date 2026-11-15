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
#include <sys/proc.h>
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
#include <sys/sysctl.h>
#include "sweet_a6xx.h"
#include "../../iokit/Kernel/arm/SweetDisplayMask.h"
#include "../../iokit/Kernel/arm/SweetTrace.h"  /* 0994 */

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern boolean_t pmap_valid_address(uint64_t pa);
extern vm_map_address_t phystokv(uint64_t pa);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t);
extern int vprintf(const char *fmt, va_list ap);
extern void sweet_smc(uint64_t x[8]);

void sweet_gpu_init(void);
#define GPU_TRAIL_PA	0xE2000000ull	/* sunfish/redfin: low RAM outside XNU (a6_log mirror; the GPU pool skips it) */
#define GPU_TRAIL_SIZE	0x10000u
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
static int gpu_irq_chan;                /* 0999: fence waiters asleep until the GPU interrupt */
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
	/*
	 * sunfish: also mirror into RAM XNU does not manage (GPU_TRAIL_PA, read
	 * with /dev/pmem): a stalled register access can wedge the console.
	 * Layout: "A6TRAIL\0" | u32 bytes | text.
	 */
	static volatile char *trail;
	if (trail == NULL) {
		trail = (volatile char *)ml_io_map(GPU_TRAIL_PA, GPU_TRAIL_SIZE);
		if (trail != NULL) {
			const char m[8] = "A6TRAIL";
			for (int i = 0; i < 8; i++) {
				trail[i] = m[i];
			}
			*(volatile uint32_t *)(trail + 8) = 0;
		}
	}
	if (trail != NULL) {
		char line[256];
		va_start(ap, fmt);
		int n = vsnprintf(line, sizeof(line), fmt, ap);
		va_end(ap);
		uint32_t used = *(volatile uint32_t *)(trail + 8);
		if (n > 0 && used + (uint32_t)n < GPU_TRAIL_SIZE - 16) {
			for (int i = 0; i < n && i < (int)sizeof(line) - 1; i++) {
				trail[16 + used + i] = line[i];
			}
			*(volatile uint32_t *)(trail + 8) = used + (uint32_t)MIN(n, (int)sizeof(line) - 1);
		}
		__asm__ volatile ("dsb sy" ::: "memory");
	}
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

int sweet_rpmh_write(const char *res, unsigned n, const uint32_t *offsets, const uint32_t *values);

/*
 * DDR (BCM MC0) and the memory NoC (SH0) at their top levels: nothing else votes them for the GPU (mainline
 * sm8150 has no GPU interconnect path either), and ABL's level left 8-thread CPU bandwidth at 13.7 GB/s
 * (19.6 GB/s voted up) and the present blit 35% slower. BCM command: commit | valid | x (avg) | y (peak).
 * Boot-arg sweetddr=0 leaves them alone.
 */
static void
gpu_bus_vote(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweetddr", &on, sizeof(on));
	if (!on) {
		return;
	}
	static const char *const bcm[] = { "MC0", "SH0" };
	for (unsigned i = 0; i < sizeof(bcm) / sizeof(bcm[0]); i++) {
		uint32_t off = 0, val = 1u << 30 | 1u << 29 | 0x3fffu << 14 | 0x3fffu;
		int rc = sweet_rpmh_write(bcm[i], 1, &off, &val);
		a6_log("bus vote %s top -> %d\n", bcm[i], rc);
	}
}

/*
 * ---- 0999: fence waits on the GPU interrupt (boot-arg sweetgpuirq=1, or p gpuirq 1; default off) ----
 *
 * Without it a fence wait (sweet_gpu_wait_fence_ns: Turnip's vkWaitForFences under the bridge's
 * metal2vulkan-queue-wait thread, in 1 ms IOKit calls) sleep-polls the fence word every 20 us (delay(20):
 * ~40 us a turn, ~25 wakeups per 1 ms call, ~95 ms of CPU a second in a Settings fling), and the direct
 * scanout thread looks for its frame's fence once a millisecond (GPU done -> flip 0.6 ms on average).
 * With it each submit's CACHE_FLUSH_TS also raises CP_CACHE_FLUSH_TS (CP_EVENT_WRITE IRQ bit, as Linux's
 * a6xx_submit), RBBM_INT_0_MASK lets only that through, and kgsl_3d0_irq (redfin DT: GIC SPI 300, level =
 * INTID 332, boot-arg sweetgpuirqid) wakes the fence waiters and the scanout thread. The waiters still look
 * every sweetgpuirqslice us (default 5000, the hang check's period) for the hang check and a lost interrupt; if fences keep landing
 * with no interrupt at all it goes back to polling by itself. After UniXNU cd1b9757 (whose kernel already
 * idles until the GPU interrupt inside a wait).
 */
extern bool sweet_spi_attach(uint32_t intid, bool edge, void (*fn)(void *arg), void *arg);
extern void sweet_spi_mask(uint32_t intid, bool masked);
extern void sweet_scan_gpu_kick(void);          /* SweetFramebuffer.cpp: the scanout thread looks now */
/*
 * 1040: on by default on redfin (validated on r64u with p gpuirq 1: backboardd wakeups ~5000/s -> ~200/s, 23k IRQs,
 * 113 late, no fallback, no fps loss); boot-arg sweetgpuirq=0 keeps the old polling, p gpuirq 0|1 switches live.
 */
#if A6_GPU_ID == 620
static uint32_t gpu_irq_want = 1, gpu_irq_intid = 332, gpu_irq_slice_us = 5000;
#else
static uint32_t gpu_irq_want, gpu_irq_intid = 332, gpu_irq_slice_us = 5000;
#endif
static volatile bool gpu_irq_on;                /* the waiters sleep until the interrupt (else delay(20)) */
static bool gpu_irq_attached;
static uint64_t gpu_irq_slice_abs;
static volatile uint64_t gpu_irqs, gpu_irq_empty, gpu_irq_sleeps, gpu_irq_late, gpu_irq_polls;
static volatile uint32_t gpu_irq_empty_run;

static void
gpu_irq_boot_args(void)
{
	PE_parse_boot_argn("sweetgpuirq", &gpu_irq_want, sizeof(gpu_irq_want));
	PE_parse_boot_argn("sweetgpuirqid", &gpu_irq_intid, sizeof(gpu_irq_intid));
	PE_parse_boot_argn("sweetgpuirqslice", &gpu_irq_slice_us, sizeof(gpu_irq_slice_us));
	if (gpu_irq_slice_us < 100 || gpu_irq_slice_us > 100000) {
		gpu_irq_slice_us = 5000;
	}
	nanoseconds_to_absolutetime((uint64_t)gpu_irq_slice_us * NSEC_PER_USEC, &gpu_irq_slice_abs);
}

/* Interrupt context (sleh_irq -> sweet_spi_dispatch): ack the RBBM status (level line), wake the waiters. */
static void
gpu_irq(__unused void *arg)
{
	uint32_t st = a6_irq_ack();
	if (st == 0) {
		gpu_irq_empty++;
		if (++gpu_irq_empty_run > 100000) {
			sweet_spi_mask(gpu_irq_intid, true);    /* nothing to clear and it keeps coming: give up */
			gpu_irq_on = false;
			thread_wakeup((event_t)&gpu_irq_chan);
		}
		return;
	}
	gpu_irq_empty_run = 0;
	gpu_irqs++;
	SWEET_TRACE(SWT_GPU_IRQ, st, a6_fence_read(&gpu), 0, 0);
	thread_wakeup((event_t)&gpu_irq_chan);
	sweet_scan_gpu_kick();
}

static int
gpu_irq_set(bool on, const char *why)
{
	if (!gpu_ready) {
		a6_log("gpu irq: GPU not ready (%s)\n", why);
		return -1;
	}
	if (on && !gpu_irq_attached) {
		if (gpu_irq_slice_abs == 0) {
			nanoseconds_to_absolutetime((uint64_t)gpu_irq_slice_us * NSEC_PER_USEC, &gpu_irq_slice_abs);
		}
		gpu_irq_attached = sweet_spi_attach(gpu_irq_intid, false, gpu_irq, NULL);
		if (!gpu_irq_attached) {
			a6_log("gpu irq: INTID %u not attached: fence waits poll\n", gpu_irq_intid);
			return -1;
		}
	}
	lck_mtx_lock(&sweet_gpu_lock);
	a6_irq_mask = on ? A6_INT_CP_CACHE_FLUSH_TS : 0;
	a6_irq_enable(a6_irq_mask);
	a6_submit_irq = on;
	gpu_irq_empty_run = 0;
	gpu_irq_on = on;
	lck_mtx_unlock(&sweet_gpu_lock);
	if (gpu_irq_attached) {
		sweet_spi_mask(gpu_irq_intid, !on);
	}
	thread_wakeup((event_t)&gpu_irq_chan);  /* sleepers go back to polling (off) */
	a6_log("gpu irq %s (%s): INTID %u, look every %u us\n", on ? "on" : "off", why, gpu_irq_intid, gpu_irq_slice_us);
	return 0;
}

/* p gpuirq [0|1]: switch (1 attaches kgsl_3d0_irq the first time), and the counters */
void sweet_gpu_irq_cmd(const char *s, const char *end);
void
sweet_gpu_irq_cmd(const char *s, const char *end)
{
	while (s < end && (*s == ' ' || *s == '\t')) {
		s++;
	}
	if (s < end && (*s == '0' || *s == '1')) {
		(void)gpu_irq_set(*s == '1', "p gpuirq");
	}
	printf("SWEET GPU IRQ: %s, INTID %u%s, slice %u us; irqs %llu (empty %llu); sleeps %llu, late %llu "
	    "(fence done at a slice end: a lost interrupt), poll turns %llu\n", gpu_irq_on ? "on" : "off",
	    gpu_irq_intid, gpu_irq_attached ? " attached" : "", gpu_irq_slice_us, gpu_irqs, gpu_irq_empty,
	    gpu_irq_sleeps, gpu_irq_late, gpu_irq_polls);
}

static void
gpu_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t t0, ns;
	uint32_t delay_s = 0;
	PE_parse_boot_argn("sweetgpudelay", &delay_s, sizeof(delay_s));
	if (delay_s) {
		static int gpu_delay_chan;
		uint64_t deadline;
		clock_interval_to_deadline(delay_s, NSEC_PER_SEC, &deadline);
		(void)assert_wait_deadline(&gpu_delay_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
	t0 = mach_absolute_time();
	a6_log("bring-up start (sweetgpudelay=%u)\n", delay_s);
	gpu_bus_vote();
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
	if (!rc && gpu_irq_want) {
		/* 0999; 1040: the default (klog "gpu irq on (default (1040); sweetgpuirq=0 for polling)") */
		uint32_t given = 0;
		(void)gpu_irq_set(true, PE_parse_boot_argn("sweetgpuirq", &given, sizeof(given)) ? "boot-arg sweetgpuirq" :
		    "default (1040); sweetgpuirq=0 for polling");
	} else if (!rc) {
		a6_log("gpu irq off (boot-arg sweetgpuirq=0): fence waits poll; p gpuirq 1 turns it on\n");
	}
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
#if A6_GPU_ID == 620
	uint32_t lvl = 3;	/* redfin: sweetgpufreq=1 (275), 2 (400), 3 (500), 4 (625) */
#else
	uint32_t lvl = 4;	/* boot-arg sweetgpufreq=1 (180) .. 5 (565); A618 also 6 (650), 7 (800), 8 (825) */
#endif
	PE_parse_boot_argn("sweetgpufreq", &lvl, sizeof(lvl));
	{
		extern int a6_submit_wfi;
		PE_parse_boot_argn("sweetwfi", &a6_submit_wfi, sizeof(a6_submit_wfi));
		extern int a6_submit_rbts;	/* 0988 */
		PE_parse_boot_argn("sweetgputs", &a6_submit_rbts, sizeof(a6_submit_rbts));
	}
	gpu_irq_boot_args();    /* 0999 */
	gpu.perf_level = lvl;
	a6_log("level %u = %u MHz at boot (1043: kern.sweet_gpu_mhz reads it from boot)\n", lvl, a6_level_khz(lvl) / 1000);
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

/* redfin (0964): thermal cap on the GPU level (bsd/dev/sweet_thermal.c, sweettcap bit 2); 0 = back to the boot level. */
int sweet_gpu_level_cap(unsigned max_lvl);
static uint32_t gpu_boot_lvl;           /* 1036: sweetgpufreq's level (the fixed level with kern.sweet_gpu_dcvs 0) */
static uint32_t gpu_tcap;               /* 1036: the thermal cap last asked for (0 = none) */
static int dcvs_on;                     /* 1036: kern.sweet_gpu_dcvs */
static void dcvs_fixed_locked(void);
int
sweet_gpu_level_cap(unsigned max_lvl)
{
	extern int a6_perf_vote(struct a6_gpu *g, uint32_t lvl);
	int rc = 0;
	lck_mtx_lock(&sweet_gpu_lock);
	if (!gpu_boot_lvl) {
		gpu_boot_lvl = gpu.perf_level ? gpu.perf_level : 1;
	}
	gpu_tcap = max_lvl;
	if (dcvs_on & 1) {
		/* 1036: the governor keeps the cap as its ceiling; above it now: down at once */
		if (max_lvl && gpu.perf_level > max_lvl) {
			gpu.perf_level = max_lvl;
			rc = gpu_ready ? a6_perf_vote(&gpu, max_lvl) : 0;
		}
		lck_mtx_unlock(&sweet_gpu_lock);
		return rc;
	}
	uint32_t lvl = (max_lvl && max_lvl < gpu_boot_lvl) ? max_lvl : gpu_boot_lvl;
	if (lvl != gpu.perf_level) {
		gpu.perf_level = lvl;   /* also what a reset/recovery brings the GPU up at */
		rc = gpu_ready ? a6_perf_vote(&gpu, lvl) : 0;
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	return rc;
}

/*
 * ---- 1036: GPU DCVS, kern.sweet_gpu_dcvs MASK (default 0 = the fixed sweetgpufreq level, as before) ----
 *   1  on: every 16 ms the GPU busy fraction of the last window (the GMU's XO busy counter, Linux a6xx_gpu_busy; if it
 *      does not count, the CP's per-submit stamps) picks the level for ~50 % busy: up at once to the lowest level at
 *      or above 1.1 x need (need = current MHz x busy / 50 %); the top after a window over 85 % busy or 50 ms busy
 *      without a break (windows >= 95 %); down one level after 4 windows in a row that want less; the floor after
 *      80 ms idle (windows < 3 %). Touch / SwapEnd (the 0970 boost sites) hold the top for kern.sweet_gpu_dcvs_boost_ms
 *      (default 1000, 0 none). 0964's thermal cap is the ceiling. Votes: HFI GX perf vote (msg 30), as 0964.
 *   2  no touch boost, 4 no SwapEnd boost
 * kern.sweet_gpu_dcvs_floor MHz (default 400: the lowest level at or above it), kern.sweet_gpu_mhz (read: the level
 * voted now). Line every 5 s while on: SWEET GPU DCVS 5s: MHz:ms .. | switches N | busy P% | boosts B capped C | src S.
 * 0 again: back to sweetgpufreq's level (capped as 0964 says).
 */
static int dcvs_floor_mhz = 400, dcvs_boost_ms = 1000;
static volatile uint64_t dcvs_boost_until;
static bool dcvs_thread_up;
static int dcvs_chan;
static uint64_t dcvs_cp_busy;           /* fallback source: CP stamp ticks of the submits summed (gpu_stats_locked) */
static int gpu_mhz_now;
static void gpu_stats_locked(void);

static void
dcvs_fixed_locked(void)
{
	extern int a6_perf_vote(struct a6_gpu *g, uint32_t lvl);
	if (!gpu_boot_lvl) {
		gpu_boot_lvl = gpu.perf_level ? gpu.perf_level : 1;
	}
	uint32_t lvl = (gpu_tcap && gpu_tcap < gpu_boot_lvl) ? gpu_tcap : gpu_boot_lvl;
	if (lvl != gpu.perf_level) {
		gpu.perf_level = lvl;
		if (gpu_ready) {
			(void)a6_perf_vote(&gpu, lvl);
		}
	}
	gpu_mhz_now = (int)(a6_level_khz(gpu.perf_level) / 1000);
}

void sweet_gpu_dcvs_boost(int why);
void
sweet_gpu_dcvs_boost(int why)
{
	/* why: 0 touch (SweetHID), 1 frames flowing (SwapEnds < 50 ms apart), 2 CPU present (0970's codes) */
	if (!(dcvs_on & 1) || dcvs_boost_ms <= 0 || (why == 0 && (dcvs_on & 2)) || (why != 0 && (dcvs_on & 4))) {
		return;
	}
	uint64_t now = mach_absolute_time(), d;
	nanoseconds_to_absolutetime((uint64_t)dcvs_boost_ms * NSEC_PER_MSEC, &d);
	bool was = now < dcvs_boost_until;
	dcvs_boost_until = now + d;
	if (!was) {
		thread_wakeup((event_t)&dcvs_chan);     /* up now, not at the next window */
	}
}

static void
dcvs_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t last_abs = 0, last_busy = 0, last_cp = 0, log_abs = 0;
	uint32_t quiet = 0, idle_ms = 0, busy_ms = 0;
	uint32_t at_ms[8] = { 0 }, switches = 0, boosts = 0, capped = 0, stuck = 0;
	uint64_t win_busy = 0, win_ticks = 0;
	bool use_cp = false, boosting = false, counter_on = false;
	for (;;) {
		if (!(dcvs_on & 1) || !gpu_ready) {
			last_abs = 0;
			counter_on = false;
			assert_wait((event_t)&dcvs_chan, THREAD_UNINT);
			if (!(dcvs_on & 1) || !gpu_ready) {
				thread_block(THREAD_CONTINUE_NULL);
			} else {
				clear_wait(current_thread(), THREAD_AWAKENED);  /* switched on meanwhile */
			}
			continue;
		}
		lck_mtx_lock(&sweet_gpu_lock);
		if (!counter_on) {
			a6_busy_counter_enable();
			counter_on = true;
		}
		gpu_stats_locked();     /* drains the CP stamps into dcvs_cp_busy (fallback source) */
		uint64_t now = mach_absolute_time(), busy = a6_busy_counter(), cp = dcvs_cp_busy, ns = 0;
		uint32_t nl = a6_nlevels(), cur = gpu.perf_level >= 1 && gpu.perf_level <= nl ? gpu.perf_level : nl;
		if (last_abs == 0) {
			last_abs = now;
			last_busy = busy;
			last_cp = cp;
			lck_mtx_unlock(&sweet_gpu_lock);
			goto sleep;
		}
		absolutetime_to_nanoseconds(now - last_abs, &ns);
		uint64_t ticks = ns * 192 / 10000, db = busy - last_busy, dc = cp - last_cp;   /* 19.2 MHz */
		last_abs = now;
		last_busy = busy;
		last_cp = cp;
		/* the GMU counter: no count for 2 s while the CP stamps show work: the stamps instead (logged once) */
		if (!use_cp && db == 0 && dc != 0 && ++stuck >= 125) {
			use_cp = true;
			a6_log("dcvs: GMU busy counter not counting (sel0/enable written): busy from the CP stamps\n");
		} else if (db != 0) {
			stuck = 0;
		}
		uint64_t b = use_cp ? dc : db;
		if (ticks == 0) {
			ticks = 1;
		}
		if (b > ticks) {
			b = ticks;
		}
		uint32_t pct = (uint32_t)(b * 100 / ticks), win_ms = (uint32_t)(ns / NSEC_PER_MSEC);
		win_busy += b;
		win_ticks += ticks;
		if (cur < 8) {
			at_ms[cur] += win_ms;
		}
		uint32_t top = gpu_tcap && gpu_tcap < nl ? gpu_tcap : nl, floor = 1;
		while (floor < top && a6_level_khz(floor) < (uint32_t)dcvs_floor_mhz * 1000) {
			floor++;
		}
		busy_ms = pct >= 95 ? busy_ms + win_ms : 0;
		idle_ms = pct < 3 ? idle_ms + win_ms : 0;
		uint64_t need = (uint64_t)a6_level_khz(cur) * pct / 50 * 11 / 10;
		uint32_t want = floor;
		while (want < top && a6_level_khz(want) < need) {
			want++;
		}
		bool boost = now < dcvs_boost_until;
		if (boost && !boosting) {
			boosts++;
		}
		boosting = boost;
		uint32_t lvl = cur;
		if (boost || pct > 85 || busy_ms >= 50) {
			lvl = top;
			quiet = 0;
		} else if (want > cur) {
			lvl = want;
			quiet = 0;
		} else if (idle_ms >= 80) {
			lvl = floor;
			quiet = 0;
		} else if (want < cur) {
			if (++quiet >= 4) {
				lvl = cur - 1;
				quiet = 0;
			}
		} else {
			quiet = 0;
		}
		if (lvl > top) {
			lvl = top;
			capped++;
		}
		if (lvl < floor) {
			lvl = floor;
		}
		if (lvl != cur) {
			gpu.perf_level = lvl;   /* also what a reset/recovery brings the GPU up at */
			if (a6_perf_vote_fast(&gpu, lvl) == 0) {
				switches++;
			}
		}
		gpu_mhz_now = (int)(a6_level_khz(gpu.perf_level) / 1000);
		lck_mtx_unlock(&sweet_gpu_lock);
		if (log_abs == 0) {
			log_abs = now;
		}
		absolutetime_to_nanoseconds(now - log_abs, &ns);
		if (ns >= 5ULL * NSEC_PER_SEC) {
			char line[200];
			int len = snprintf(line, sizeof(line), "SWEET GPU DCVS 5s:");
			for (uint32_t i = 1; i <= nl && i < 8; i++) {
				len += snprintf(line + len, sizeof(line) - len, " %u:%u", a6_level_khz(i) / 1000, at_ms[i]);
			}
			printf("%s ms | switches %u | busy %llu%% | boosts %u capped %u | floor %d cap %u | src %s\n", line,
			    switches, win_ticks ? win_busy * 100 / win_ticks : 0, boosts, capped, dcvs_floor_mhz, gpu_tcap,
			    use_cp ? "cp" : "gmu");
			bzero(at_ms, sizeof(at_ms));
			switches = boosts = capped = 0;
			win_busy = win_ticks = 0;
			log_abs = now;
		}
sleep:
		{
			uint64_t dl;
			clock_interval_to_deadline(16, NSEC_PER_MSEC, &dl);
			(void)assert_wait_deadline(&dcvs_chan, THREAD_UNINT, dl);
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
}

static int
sysctl_sweet_gpu_dcvs SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, dcvs_on, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 7) {
		return EINVAL;
	}
#if A6_GPU_ID == 618
	if (value & 1) {
		return ENOTSUP;         /* no runtime perf vote on the A618's legacy GMU */
	}
#endif
	if ((value & 1) && !dcvs_thread_up) {
		thread_t th;
		if (kernel_thread_start(dcvs_thread, NULL, &th) != KERN_SUCCESS) {
			return ENOMEM;
		}
		thread_deallocate(th);
		dcvs_thread_up = true;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	if (!gpu_boot_lvl) {
		gpu_boot_lvl = gpu.perf_level ? gpu.perf_level : 1;     /* sweetgpufreq's, before the governor moves it */
	}
	dcvs_on = value;
	if (!(value & 1)) {
		dcvs_fixed_locked();    /* back to sweetgpufreq's level (under the thermal cap) */
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	thread_wakeup((event_t)&dcvs_chan);
	printf("SWEET GPU: dcvs %d (%s; floor %d MHz, boost %d ms, level now %u = %u MHz)\n", value,
	    (value & 1) ? "on" : "off: fixed sweetgpufreq level", dcvs_floor_mhz, dcvs_boost_ms, gpu.perf_level,
	    a6_level_khz(gpu.perf_level) / 1000);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_dcvs, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_dcvs, "I", "SWEET GPU: DCVS (0 fixed level, 1 on, +2 no touch boost, +4 no SwapEnd boost)");

static int
sysctl_sweet_gpu_dcvs_floor SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, dcvs_floor_mhz, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 1000) {
		return EINVAL;
	}
	dcvs_floor_mhz = value;
	printf("SWEET GPU: dcvs floor %d MHz\n", value);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_dcvs_floor, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_dcvs_floor, "I", "SWEET GPU: DCVS floor, MHz (lowest level at or above)");

static int
sysctl_sweet_gpu_dcvs_boost_ms SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, dcvs_boost_ms, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 10000) {
		return EINVAL;
	}
	dcvs_boost_ms = value;
	printf("SWEET GPU: dcvs boost %d ms\n", value);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_dcvs_boost_ms, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_dcvs_boost_ms, "I", "SWEET GPU: DCVS touch/SwapEnd boost at the top, ms (0 none)");
/*
 * 1043: kern.sweet_gpu_mhz read 0 from boot until kern.sweet_gpu_dcvs was written (gpu_mhz_now was only set by the
 * governor and dcvs_fixed_locked). Now computed on each read from the level the GPU is at (or comes up at):
 * gpu.perf_level, which is the sweetgpufreq level from boot (under 0964's cap), and DCVS's level while it runs;
 * before the GPU is set up, the sweetgpufreq boot-arg's level.
 */
static int
sysctl_sweet_gpu_mhz SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint32_t lvl = gpu.perf_level;
	if (lvl == 0) {
#if A6_GPU_ID == 620
		lvl = 3;
#else
		lvl = 4;
#endif
		PE_parse_boot_argn("sweetgpufreq", &lvl, sizeof(lvl));
	}
	int mhz = (int)(a6_level_khz(lvl) / 1000);
	gpu_mhz_now = mhz;
	return sysctl_io_number(req, mhz, sizeof(mhz), NULL, NULL);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_mhz, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_gpu_mhz, "I",
    "SWEET GPU: level voted now, MHz (1043: the sweetgpufreq level from boot, DCVS's level while it runs)");

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
static struct { uint32_t lo, hi; int culprit; } lost[LOST_RING];	/* 0941: culprit = pid of the job the GPU was on (-1 unknown) */

/*
 * GPU time of the submits that landed (their CP_ALWAYS_ON stamps, 19.2 MHz), by the process that queued them
 * (pid 0 = the kernel's presents), summed on every submit and logged every 5 s. Fences that fall out of the
 * stamp ring (A6_TS_SLOTS) before they are summed only count as "missed".
 */
#define GSTAT_OWNERS    6
static uint32_t gstat_next = 1;
static uint64_t gstat_abs;
static bool gstat_force;     /* 0991: p phase: the line now, whatever the window */
static int gstat_pid[A6_TS_SLOTS];
static struct { int pid; uint32_t n; uint64_t busy; } gstat_own[GSTAT_OWNERS + 1];     /* last: everyone else */
static uint32_t gstat_n, gstat_missed;
static uint64_t gstat_busy, gstat_longest;

static void tlb_stats_locked(bool flush);       /* 1039 */

static void
gpu_owner_locked(uint32_t fence, int pid)
{
	if (fence) {
		gstat_pid[fence % A6_TS_SLOTS] = pid;
	}
}

static void
gpu_stats_locked(void)
{
	uint32_t done = a6_fence_read(&gpu);
	if ((int32_t)(done - gstat_next) >= (int32_t)A6_TS_SLOTS) {
		gstat_missed += done - gstat_next - A6_TS_SLOTS + 1;
		gstat_next = done - A6_TS_SLOTS + 1;
	}
	for (; (int32_t)(done - gstat_next) >= 0; gstat_next++) {
		uint64_t t = a6_submit_time(&gpu, gstat_next);
		int pid = gstat_pid[gstat_next % A6_TS_SLOTS], o;
		if (SWEET_TRACE_ON()) {        /* 0994: the fence's raw stamps (start, end) */
			uint64_t st[2] = { 0, 0 };
			a6_read(a6_pool_pa(&gpu, A6_OFF_SCRATCH + A6_TS_OFF) + 16ull * (gstat_next % A6_TS_SLOTS), st, 16);
			SWEET_TRACE(SWT_GPU_DONE, gstat_next, st[0], st[1], pid);
		}
		for (o = 0; o < GSTAT_OWNERS && gstat_own[o].n && gstat_own[o].pid != pid; o++) {
		}
		gstat_own[o].pid = o < GSTAT_OWNERS ? pid : -1;
		gstat_own[o].n++;
		gstat_own[o].busy += t;
		gstat_n++;
		gstat_busy += t;
		dcvs_cp_busy += t;      /* 1036: DCVS fallback source */
		if (t > gstat_longest) {
			gstat_longest = t;
		}
	}
	uint64_t now = mach_absolute_time(), ns;
	if (gstat_abs == 0) {
		gstat_abs = now;
	}
	absolutetime_to_nanoseconds(now - gstat_abs, &ns);
	if (ns < 5000000000ULL && !gstat_force) {
		return;
	}
	gstat_force = false;
	if (ns < 1000000) {
		ns = 1000000;
	}
	if (gstat_n) {
		char who[160] = "", name[24];
		size_t w = 0;
		for (int o = 0; o <= GSTAT_OWNERS && gstat_own[o].n && w < sizeof(who) - 40; o++) {
			if (gstat_own[o].pid > 0) {
				proc_name(gstat_own[o].pid, name, sizeof(name));
			} else {
				strlcpy(name, gstat_own[o].pid == 0 ? "present" : "others", sizeof(name));
			}
			w += snprintf(who + w, sizeof(who) - w, " %s %u/%llums", name, gstat_own[o].n,
			    gstat_own[o].busy / 19200);
		}
		a6_log("5s: %u submits (%u missed), GPU busy %llu ms (%llu%%), longest %llu us, mean %llu us;%s\n",
		    gstat_n, gstat_missed, gstat_busy / 19200, gstat_busy * 100 / 19200 / (ns / 1000000),
		    gstat_longest * 10 / 192, gstat_busy * 10 / 192 / gstat_n, who);
	}
	tlb_stats_locked(true);         /* 1039 */
	gstat_n = gstat_missed = 0;
	gstat_busy = gstat_longest = 0;
	bzero(gstat_own, sizeof(gstat_own));
	gstat_abs = now;
}

/* 0991: p phase (SweetFramebuffer.cpp): the GPU stats line for the phase that ends; the window restarts now */
void sweet_gpu_stats_flush(void);
void
sweet_gpu_stats_flush(void)
{
	if (!gpu_ready) {
		return;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	gstat_force = true;
	gpu_stats_locked();
	gstat_force = false;
	lck_mtx_unlock(&sweet_gpu_lock);
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
	/*
	 * 0152: a hang is no progress at all, not just no fence: a long client IB (WebKit) keeps the CP moving
	 * (ring rptr, IB1 base and dwords left, IB2 left change) with the fence still behind. Those count as
	 * progress; and never less than sweetgpuhang ms (default 2000).
	 */
	static uint32_t sig[4];
	static uint32_t min_ms;
	if (!min_ms && !PE_parse_boot_argn("sweetgpuhang", &min_ms, sizeof(min_ms)))
		min_ms = 2000;
	if (hang_ms < min_ms)
		hang_ms = min_ms;
	uint32_t now_sig[4] = { a6_rd(A6_GREG(0x806)), a6_rd(A6_GREG(0x928)), a6_rd(A6_GREG(0x92a)), a6_rd(A6_GREG(0x92d)) };
	uint32_t done = a6_fence_read(&gpu);
	uint64_t now = mach_absolute_time(), ns;
	if (done != progress_fence || (int32_t)(gpu.fence - done) <= 0 || memcmp(sig, now_sig, sizeof(sig))) {
		progress_fence = done;
		progress_abs = now;
		memcpy(sig, now_sig, sizeof(sig));
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
	/* 0941: whose job hung (the first not done); the fence-owner ring holds it while it is in flight */
	lost[recoveries % LOST_RING].culprit = (int32_t)(gpu.fence - done) > 0 && (int32_t)(gpu.fence - done) <= (int32_t)A6_TS_SLOTS ?
	    gstat_pid[(done + 1) % A6_TS_SLOTS] : -1;
	a6_log("%s: the job of pid %d hung or faulted\n", why, lost[recoveries % LOST_RING].culprit);
	recoveries++;
	if (a6_recover(&gpu)) {
		a6_log("%s: GPU not recovered\n", why);
		gpu_ready = false;
	}
	a6_fence_force(&gpu);
	progress_fence = gpu.fence;
	progress_abs = mach_absolute_time();
	thread_wakeup((event_t)&gpu_irq_chan);  /* 0999: interrupt waiters see the forced fence now */
}

/*
 * ---- 1039: kern.sweet_gpu_tlb (default 0 = a full context-bank flush, TLBIALL + TLBSYNC, after every map/unmap) ----
 *   1  unmaps of <= 512 4K pages: one TLBIVA per page on the GPU context bank (MMU-500 CB + 0x600, 64-bit store,
 *      AArch64 format VA[55:12], ASID 0 = the bank's TTBR0 ASID; our PTEs are global, nG clear, which TLBIVA matches
 *      too), then TLBSYNC and TLBSTATUS polled; maps whose 4K entries were all invalid before: no invalidate (the
 *      SMMU caches no invalid entries; page tables only gain entries; new tables hang off invalid slots). A map that
 *      replaced a valid entry (surface slot reuse, a BO mapped over another) or failed: the full flush as before.
 *   2  only the unmap part, 3 only the map part (to isolate a fault)
 * Risk: MMU-500's next-page prefetcher (CB ACTLR.CPRE; Linux turns it off for errata) could keep a prefetched entry
 * TLBIVA misses: ACTLR is logged when the mode is set. Line SWEET GPU: tlb 5s: full F | va V pages in U unmaps |
 * map skipped S (only while nonzero).
 */
static int tlb_mode;
static uint32_t tlb_full, tlb_va, tlb_unmaps, tlb_skip;

static void
tlb_after_map_locked(int rc)
{
	if (rc == 0 && (tlb_mode == 1 || tlb_mode == 3) && !a6_map_replaced) {
		tlb_skip++;
		return;
	}
	a6_tlb_flush(&gpu);
	tlb_full++;
}

static void
tlb_after_unmap_locked(uint64_t iova, uint64_t size)
{
	uint64_t n = (size + 0xfff) >> 12;
	if ((tlb_mode == 1 || tlb_mode == 2) && n != 0 && n <= 512) {
		uint64_t cb = A6_SMMU_CB(gpu.gpu_cb);
		volatile uint64_t *tlbiva = (volatile uint64_t *)(uintptr_t)gpu_io(cb + 0x600);
		__asm__ volatile ("dsb osh" ::: "memory");      /* the cleared PTEs before the invalidates */
		for (uint64_t i = 0; i < n; i++) {
			*tlbiva = ((iova & ~0xfffULL) + (i << 12)) >> 12;
		}
		a6_wr(cb + 0x7f0, 0);                           /* TLBSYNC */
		for (unsigned i = 0; i < 1000 && (a6_rd(cb + 0x7f4) & 1); i++) {
			delay(10);
		}
		tlb_va += (uint32_t)n;
		tlb_unmaps++;
		return;
	}
	a6_tlb_flush(&gpu);
	tlb_full++;
}

static void
tlb_stats_locked(bool flush)
{
	if (flush && (tlb_mode || tlb_va || tlb_skip)) {
		a6_log("tlb 5s: full %u | va %u pages in %u unmaps | map skipped %u | mode %d\n", tlb_full, tlb_va,
		    tlb_unmaps, tlb_skip, tlb_mode);
	}
	if (flush) {
		tlb_full = tlb_va = tlb_unmaps = tlb_skip = 0;
	}
}

static int
sysctl_sweet_gpu_tlb SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, tlb_mode, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 3) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	tlb_mode = value;
	a6_map_check = value == 1 || value == 3;
	uint32_t actlr = gpu_ready && value ? a6_rd(A6_SMMU_CB(gpu.gpu_cb) + 0x4) : 0;
	if (gpu_ready) {
		a6_tlb_flush(&gpu);     /* start from a clean TLB either way */
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	printf("SWEET GPU: tlb %d (1 per-page unmap + no flush on fresh maps, 2 unmap only, 3 map only); cb%d actlr %x%s\n",
	    value, gpu.gpu_cb, actlr, (actlr & 2) ? " (CPRE prefetch on)" : "");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_tlb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_tlb, "I", "SWEET GPU: TLB maintenance (0 full flush, 1 per-page/skip, 2 unmap, 3 map)");

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

/*
 * 0905: double-buffered GPU present (boot-arg sweetfbflip=1; ios-sailfish 0164 does this on the Pixel 1).
 * The GPU blits each frame into the buffer the panel is not scanning (A6_FB_PA or A6_FB_PA + A6_FB_BUF_OFF);
 * once that blit's fence has landed, VIG0 and VIG1 are pointed at it and CTL0 flushed: with ABL's
 * autorefresh the next frame start latches both pipes at once, so the panel never shows a half-drawn frame
 * (the single-buffer path blits into the buffer being scanned out). Until the flip has latched (CTL0's
 * VIG flush bits read back clear, or two 60 Hz frames passed) nothing is written into the old buffer:
 * the present reports "busy" and the framebuffer retries on its next tick.
 */
#define SSPP_SRC0_ADDR  0x14
static int fb_flip = -1;                /* boot-arg sweetfbflip */
static uint32_t scan_buf;               /* 0 = A6_FB_PA (ABL's), 1 = the second buffer */
static uint32_t back_fence;             /* latest present queued into the back buffer, 0 = none */
static uint64_t flip_abs;               /* when the last flip was flushed */
static uint64_t stat_flips;
static uint32_t stat_flip_late;         /* flips taken as latched by time, not by CTL0's flush bits */
static int mdp_owned;                   /* 0948: the poke p mdp test/scale owns VIG0/VIG1 (sweet_gpu_mdp_own) */

static bool
fb_flip_on(void)
{
	if (fb_flip < 0) {
		int on = 0;
		PE_parse_boot_argn("sweetfbflip", &on, sizeof(on));
		fb_flip = on && A6_FB_BUF_OFF != 0;
		if (on) {
			a6_log("present: double-buffered (%s)\n", fb_flip ? "on" : "no second buffer on this GPU");
		}
	}
	return fb_flip > 0;
}

static bool
flip_latched_locked(void)
{
	if (!flip_abs || !(a6_rd(CTL0_FLUSH) & 0x3)) {
		return true;
	}
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - flip_abs, &ns);
	if (ns <= 34000000ULL) {
		return false;
	}
	stat_flip_late++;
	flip_abs = 0;
	return true;
}

static void
flip_to_locked(uint32_t buf)
{
	if (mdp_owned) {
		return;         /* 0948: the pipes show a test pattern; p mdp restore puts this buffer back */
	}
	uint64_t from = A6_FB_PA + scan_buf * A6_FB_BUF_OFF, to = A6_FB_PA + buf * A6_FB_BUF_OFF;
	for (uint32_t p = 0; p < 2; p++) {
		uint32_t a = a6_rd(SSPP_VIG(p) + SSPP_SRC0_ADDR);
		a6_wr(SSPP_VIG(p) + SSPP_SRC0_ADDR, (uint32_t)(a - (uint32_t)from + (uint32_t)to));
	}
	a6_wr(CTL0_FLUSH, 0x3);
	flip_abs = mach_absolute_time();
	scan_buf = buf;
	if (stat_flips++ < 3) {
		a6_log("present: flip %llu to 0x%llx (vig0 %x vig1 %x)\n", stat_flips, to,
		    a6_rd(SSPP_VIG(0) + SSPP_SRC0_ADDR), a6_rd(SSPP_VIG(1) + SSPP_SRC0_ADDR));
	}
}

/* The back buffer's latest blit has landed: show it. With sweet_gpu_lock. */
static void
flip_poll_locked(void)
{
	if (back_fence && a6_fence_done(&gpu, back_fence) && flip_latched_locked()) {
		flip_to_locked(scan_buf ^ 1);
		back_fence = 0;
	}
}

/* From the framebuffer's vsync tick: a finished frame waiting in the back buffer goes on screen. */
void sweet_gpu_flip_poll(void);
void
sweet_gpu_flip_poll(void)
{
	if (!gpu_ready || fb_flip <= 0 || !back_fence) {
		return;
	}
	if (lck_mtx_try_lock(&sweet_gpu_lock)) {
		flip_poll_locked();
		lck_mtx_unlock(&sweet_gpu_lock);
	}
}

/* Before the CPU draws into A6_FB_PA again (framebuffer fallback): scan that buffer out. */
void sweet_gpu_scanout_front(void);
void
sweet_gpu_scanout_front(void)
{
	if (!gpu_ready || fb_flip <= 0 || (scan_buf == 0 && !back_fence)) {
		return;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	back_fence = 0;
	if (scan_buf != 0) {
		flip_to_locked(0);
		a6_log("present: back to the front buffer for the CPU path\n");
	}
	lck_mtx_unlock(&sweet_gpu_lock);
}

/*
 * 0946 (sweetfbx & 8): frames the CPU copies to the panel, past the GPU's ring. Which panel buffer to draw into
 * (physical address): with sweetfbflip=1 the one not on screen, once the last flip has latched (1: busy, try
 * again), else the scanned one; sweet_gpu_cpu_flip() then shows it at the next frame start.
 */
int sweet_gpu_cpu_target(uint64_t *pa);
int
sweet_gpu_cpu_target(uint64_t *pa)
{
	*pa = A6_FB_PA;
	if (!fb_flip_on()) {
		return 0;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	flip_poll_locked();
	if (back_fence || !flip_latched_locked()) {
		lck_mtx_unlock(&sweet_gpu_lock);
		return 1;
	}
	*pa = A6_FB_PA + (scan_buf ^ 1) * A6_FB_BUF_OFF;
	lck_mtx_unlock(&sweet_gpu_lock);
	return 0;
}

void sweet_gpu_cpu_flip(void);
void
sweet_gpu_cpu_flip(void)
{
	if (fb_flip <= 0) {
		return;
	}
	lck_mtx_lock(&sweet_gpu_lock);
	flip_to_locked(scan_buf ^ 1);
	lck_mtx_unlock(&sweet_gpu_lock);
}

/*
 * 0943: display engine registers for the framebuffer (SweetFramebuffer.cpp: p fc, p hz, the tear-check
 * phase). Only inside the MDP's 2 MB device window, which the panel code above already maps (gpu_io);
 * nothing past it is ever read. INTF1 and its tear check run since ABL (autorefresh), so they are clocked.
 */
#define MDP_WINDOW      0x200000u
uint32_t sweet_mdp_rd(uint32_t off);
void sweet_mdp_wr(uint32_t off, uint32_t v);
uint32_t
sweet_mdp_rd(uint32_t off)
{
	if (off >= MDP_WINDOW || (off & 3)) {
		return 0;
	}
	return a6_rd(MDP + off);
}

void
sweet_mdp_wr(uint32_t off, uint32_t v)
{
	if (off < MDP_WINDOW && !(off & 3)) {
		a6_wr(MDP + off, v);
	}
}

/*
 * 0948: the poke p mdp test/scale (SweetFramebuffer.cpp) takes VIG0/VIG1 over: no flip touches them until it gives
 * them back. Under sweet_gpu_lock, so a flip in progress finishes first.
 */
void sweet_gpu_mdp_own(int on);
void
sweet_gpu_mdp_own(int on)
{
	lck_mtx_lock(&sweet_gpu_lock);
	mdp_owned = on;
	lck_mtx_unlock(&sweet_gpu_lock);
}

/* ---- surfaces ---- */

/*
 * redfin 1048: kern.sweet_surfkeep 0|1 (default 0 = as before; boot-arg sweetsurfkeep=1): surfaces from a recorded key
 * set (<= 9 keys, explicit IOSurfaceBytesPerRow) keep their row, no 0159/0940 rounding (iokit/Kernel/arm/SweetSurface.cpp,
 * sweet_surface_layout). Takes effect for surfaces created after the change.
 */
extern uint32_t sweet_surfkeep;
extern uint32_t sweet_surface_keep_row(void);
static int
sysctl_sweet_surfkeep SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)sweet_surface_keep_row(), sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 2) {      /* 1096: 2 = CA archive-decode surfaces only (default) */
		return EINVAL;
	}
	sweet_surfkeep = (uint32_t)value;
	printf("SWEET SURF: surfkeep %d (%s; new surfaces)\n", value, value == 2 ? "1096: archive-decode surfaces keep their row" :
	    value == 1 ? "1048: recorded key sets keep their row" : "every row 64-aligned");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_surfkeep, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_surfkeep, "I", "SWEET SURF: explicit rows kept: 0 none, 1 <= 9 keys (1048), 2 CA archive decode only (1096, default)");

/* redfin 1096: kern.sweet_surfpad (iokit/Kernel/arm/SweetSurface.cpp sweet_surface_pad): allocations cover whole 4-row groups */
extern uint32_t sweet_surfpad, sweet_surfpad_grew;
extern uint32_t sweet_surface_pad(void);
static int
sysctl_sweet_surfpad SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)sweet_surface_pad(), sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 1) {
		return EINVAL;
	}
	sweet_surfpad = (uint32_t)value;
	printf("SWEET SURF: surfpad %d (1096: allocations padded to 4-row groups; new surfaces)\n", value);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_surfpad, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_surfpad, "I", "SWEET SURF: pad allocations to whole 4-row groups for GMEM 16x4 blits (1096, default 1)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_surfpad_grew, CTLFLAG_RD | CTLFLAG_LOCKED, &sweet_surfpad_grew, 0,
    "SWEET SURF: surfaces whose allocation grew by kern.sweet_surfpad (1096)");

/* redfin 1096: kern.sweet_surfalign_limits: the alignment masks of IOSurfaceRootUserClient selector 13 */
extern uint32_t sweet_surfalign_limits;
extern uint32_t sweet_surface_limits_mask(void);
static int
sysctl_sweet_surfalign_limits SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	(void)sweet_surface_limits_mask();     /* reads the boot-arg once */
	int error = sysctl_io_number(req, (int)sweet_surfalign_limits, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 65536 || (value > 1 && (value & (value - 1)) != 0)) {
		return EINVAL;
	}
	sweet_surfalign_limits = (uint32_t)value;
	printf("SWEET SURF: surfalign_limits %d: selector 13 answers row mask %u (1096; processes that start IOSurface now)\n",
	    value, sweet_surface_limits_mask());
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_surfalign_limits, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_surfalign_limits, "I",
    "SWEET SURF: IOSurface limits row alignment: 1 = 0159's (64, default), 0 = none, 2^n = that (1096)");

static int present_wait_locked(uint32_t k, const char *why);

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
	(void)present_wait_locked(0, "surface remap");
	(void)present_wait_locked(1, "surface remap");
	int rc = a6_map_pages(&gpu, SLOT_IOVA(s), pages, npages, page_size);
	tlb_after_map_locked(rc);       /* 1039 (a slot reuse replaced valid entries: full flush) */
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

#define IB_OFF          A6_OFF_FREE     /* the present IBs in the pool (two, PRESENT_IB) */
#define PRESENT_IB(k)   (IB_OFF + 0x4000ULL * (k))

static uint32_t present_fence[2];      /* of the present last queued from each IB slot */
static uint32_t present_next;
static uint32_t stat_busy;	/* 0158: presents put off a tick (slot still queued) */

/* With sweet_gpu_lock: the present from IB slot k has landed (or the GPU was reset). 0, or -1 if reset. */
static int
present_wait_locked(uint32_t k, const char *why)
{
	while (present_fence[k] && !a6_fence_done(&gpu, present_fence[k])) {
		if (gpu_hung_locked(500)) {
			gpu_recover_locked(why);
			return -1;
		}
		lck_mtx_unlock(&sweet_gpu_lock);
		delay(20);
		lck_mtx_lock(&sweet_gpu_lock);
	}
	return 0;
}

/*
 * 0943: when each present's blit landed, for the framebuffer's 5 s line (fence us = queued -> fence seen
 * signalled, as finely as it is polled: every present, every vsync tick and every 1 ms of the present thread
 * while a blit is out).
 */
static uint64_t present_queued_abs[2];  /* 0 = landed (or never queued) */
static uint64_t present_land_abs, present_land_n;
/* 0946: SwapEnd -> blit landed (panel-visible), per present: the framebuffer tags each present with its SwapEnd */
static uint64_t present_tag, present_swap_abs[2], present_vis_abs, present_vis_n;

static void
present_landed_locked(uint64_t now)
{
	for (uint32_t k = 0; k < 2; k++) {
		if (present_queued_abs[k] && (!present_fence[k] || a6_fence_done(&gpu, present_fence[k]))) {
			present_land_abs += now - present_queued_abs[k];
			present_land_n++;
			present_queued_abs[k] = 0;
			if (present_swap_abs[k] != 0 && now > present_swap_abs[k]) {	/* 0946 */
				present_vis_abs += now - present_swap_abs[k];
				present_vis_n++;
			}
			present_swap_abs[k] = 0;
		}
	}
}

/* True while a present's blit is still out. Never blocks. */
bool sweet_gpu_present_poll(void);
bool
sweet_gpu_present_poll(void)
{
	if (!gpu_ready || !(present_queued_abs[0] | present_queued_abs[1])) {
		return false;
	}
	if (lck_mtx_try_lock(&sweet_gpu_lock)) {
		present_landed_locked(mach_absolute_time());
		lck_mtx_unlock(&sweet_gpu_lock);
	}
	return (present_queued_abs[0] | present_queued_abs[1]) != 0;
}

/* Sum (absolute time) and count of blit->fence latencies since the last call (0 when the GPU lock is busy). */
void sweet_gpu_present_landed(uint64_t *sum_abs, uint64_t *n);
void
sweet_gpu_present_landed(uint64_t *sum_abs, uint64_t *n)
{
	*sum_abs = *n = 0;
	if (!gpu_ready || !lck_mtx_try_lock(&sweet_gpu_lock)) {
		return;
	}
	*sum_abs = present_land_abs;
	*n = present_land_n;
	present_land_abs = present_land_n = 0;
	lck_mtx_unlock(&sweet_gpu_lock);
}
/*
 * 0949: CPU present after the GPU. SwapEnd comes once CA has *queued* the frame's rendering, not when the GPU has
 * drawn it: the GPU blit sits behind that work on the ring by itself, a CPU copy does not. The last fence
 * submitted (at SwapEnd), and a wait for it that sleeps in 100 us steps.
 */
uint32_t sweet_gpu_fence_submitted(void);
uint32_t
sweet_gpu_fence_submitted(void)
{
	return gpu_ready ? gpu.fence : 0;
}

int sweet_gpu_fence_wait_soft(uint32_t fence, unsigned ms);
int
sweet_gpu_fence_wait_soft(uint32_t fence, unsigned ms)
{
	if (!gpu_ready || fence == 0) {
		return 0;
	}
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (!a6_fence_done(&gpu, fence)) {
		if (!gpu_ready || mach_absolute_time() > deadline) {
			return -1;
		}
		delay(100);
	}
	return 0;
}

/* 0946: the next present's SwapEnd time (from the framebuffer, before sweet_gpu_present). */
void sweet_gpu_present_tag(uint64_t swap_abs);
void
sweet_gpu_present_tag(uint64_t swap_abs)
{
	present_tag = swap_abs;
}

/* 0946: sum and count of SwapEnd -> blit landed since the last call (0 when the GPU lock is busy). */
void sweet_gpu_present_visible(uint64_t *sum_abs, uint64_t *n);
void
sweet_gpu_present_visible(uint64_t *sum_abs, uint64_t *n)
{
	*sum_abs = *n = 0;
	if (!gpu_ready || !lck_mtx_try_lock(&sweet_gpu_lock)) {
		return;
	}
	*sum_abs = present_vis_abs;
	*n = present_vis_n;
	present_vis_abs = present_vis_n = 0;
	lck_mtx_unlock(&sweet_gpu_lock);
}

#define PANEL_W         1080
#define PANEL_H         2340
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
	/* 0947: within 2 px of the panel: all of it (1125x2436 gave 1080x2339, and row 2339 was never written:
	 * it flickered between the two flip buffers' old contents) */
	frame.dh = PANEL_H - frame.dh <= 2 ? PANEL_H : frame.dh;
	frame.dw = PANEL_W - frame.dw <= 2 ? PANEL_W : frame.dw;
	frame.dx = (PANEL_W - frame.dw) / 2;
	frame.dy = (PANEL_H - frame.dh) / 2;
	uint32_t ext = corner_radius ? sweet_display_corner_extent(corner_radius) : 0;
	if (ext > DISPLAY_MASK_SIDE || 2 * ext >= frame.dw || 2 * ext >= frame.dh) {
		ext = 0;
	}
	static uint32_t ib[10 * A6_BLIT_MAX_DW];
	lck_mtx_lock(&sweet_gpu_lock);
	present_landed_locked(mach_absolute_time());	/* 0943 */
	uint64_t fb_gva = A6_GVA_FB;
	if (fb_flip_on()) {
		/* 0905: draw into the buffer that is not on screen, once the last flip has latched */
		flip_poll_locked();
		if (!flip_latched_locked()) {
			stat_busy++;
			lck_mtx_unlock(&sweet_gpu_lock);
			return 1;
		}
		fb_gva += (scan_buf ^ 1) * A6_FB_BUF_OFF;
		frame.dst_iova = fb_gva;
	}
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
				.dst_iova = fb_gva, .dst_pitch = fb_pitch,
				.dx = frame.dx, .dy = y, .dw = frame.dw, .dh = ext,
			};
			n += a6_blit_ib(&gpu, &copy, ib + n);
		}
	} else {
		n = a6_blit_ib(&gpu, &frame, ib);
	}
	/*
	 * Not waited for: the vsync timer must not block behind the Metal clients' work queued ahead of it
	 * (a waited present took 15-30 ms with metal2vulkan's frames in the ring, and the vsync notifications
	 * CA paces on came late). Two IB slots: only the one two presents back must have landed.
	 */
	uint32_t k = present_next & 1;
	/*
	 * 0158: never block the vsync tick behind client render jobs. The present of this IB slot (two frames
	 * back) still queued: 1, "busy" — the framebuffer keeps the frame and tries again on the next tick
	 * (sweetpresentwait=1: the old blocking wait).
	 */
	static int block = -1;
	if (block < 0 && !PE_parse_boot_argn("sweetpresentwait", &block, sizeof(block)))
		block = 0;
	if (!block && present_fence[k] && !a6_fence_done(&gpu, present_fence[k])) {
		if (gpu_hung_locked(500)) {
			gpu_recover_locked("present");
			lck_mtx_unlock(&sweet_gpu_lock);
			return -1;
		}
		stat_busy++;
		lck_mtx_unlock(&sweet_gpu_lock);
		return 1;
	}
	int rc = present_wait_locked(k, "present");
	if (rc == 0) {
		a6_write(a6_pool_pa(&gpu, PRESENT_IB(k)), ib, 4ULL * n);
		gpu_note_submit_locked();
		present_fence[k] = a6_submit_ib_async(&gpu, a6_pool_va(PRESENT_IB(k)), n, 100);
		gpu_owner_locked(present_fence[k], 0);
		present_queued_abs[k] = present_fence[k] ? mach_absolute_time() : 0;	/* 0943 */
		present_swap_abs[k] = present_fence[k] ? present_tag : 0;	/* 0946 */
		rc = present_fence[k] ? 0 : -1;
		present_next++;
		if (fb_flip > 0 && present_fence[k]) {
			back_fence = present_fence[k];	/* 0905: flipped to once it lands (flip_poll_locked) */
		}
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	uint64_t t2 = mach_absolute_time();
	stat_clean_abs += t1 - t0;
	stat_gpu_abs += t2 - t1;
	/*
	 * 0942: the present line per window of 300 frames, readable on its own: how long the window took and the
	 * presents a second in it, the mean cache clean and blit queueing per frame, and what happened in that window
	 * only (presents put off because the previous blit was still queued, flips, flips taken as latched by time),
	 * with the mode (single buffer / flip) and the GPU clock step. The totals since boot follow in brackets.
	 */
	static uint64_t win_abs;
	static uint32_t win_busy, win_late;
	static uint64_t win_flips;
	if (win_abs == 0) {
		win_abs = t0;
	}
	if ((++stat_frames % 300) == 0) {
		uint64_t c, g, span;
		absolutetime_to_nanoseconds(stat_clean_abs, &c);
		absolutetime_to_nanoseconds(stat_gpu_abs, &g);
		absolutetime_to_nanoseconds(t2 - win_abs, &span);
		uint32_t freq = 0;
		PE_parse_boot_argn("sweetgpufreq", &freq, sizeof(freq));
		a6_log("present: 300 frames in %llu ms = %llu.%llu/s (%s, gpufreq %u); per frame clean %llu us, blit %llu us; "
		    "put off %u, flips %llu, late %u [total %llu frames, put off %u, flips %llu, late %u]\n",
		    span / 1000000, span ? 300000000000ULL / span : 0, span ? (3000000000000ULL / span) % 10 : 0,
		    fb_flip > 0 ? "flip" : "single buffer", freq, c / 1000 / 300, g / 1000 / 300, stat_busy - win_busy,
		    stat_flips - win_flips, stat_flip_late - win_late, stat_frames, stat_busy, stat_flips, stat_flip_late);
		stat_clean_abs = stat_gpu_abs = 0;
		win_abs = t2;
		win_busy = stat_busy;
		win_flips = stat_flips;
		win_late = stat_flip_late;
	}
	*out_dx = frame.dx;
	*out_dy = frame.dy;
	*out_dw = frame.dw;
	*out_dh = frame.dh;
	return rc;
}

/*
 * 1035: kern.sweet_fb_early_retire MASK (SweetFramebuffer.cpp, the 0944 swap queue + GPU present; default 0 = as
 * before). A swap counts as retired once its present blit is accepted onto the ring (present() queued it), not when
 * the blit's fence lands or the flip latches:
 *   1  swap-complete tick (type-5 notification, as sweetswapdone 1) at acceptance, without needing sweetswapdone;
 *      no tick at the fence; at least sweetpace_min after the last notification
 *   2  re-kick: a queued frame that was put off (flip not latched, IB slot busy) is tried again at each vsync tick
 *      and as soon as the previous blit's fence is seen, instead of 1 ms later
 *   4  SwapWait polls (scalar 1 bit 31, CA's can_update_) answer kIOReturnBusy while that swap still waits in the
 *      queue or is being presented, 0 once its blit is on the ring (100 ms cap); today they answer 0 at once
 */
int sweet_fb_early_retire;
static int
sysctl_sweet_fb_early_retire SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, sweet_fb_early_retire, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 7) {
		return EINVAL;
	}
	sweet_fb_early_retire = value;
	printf("SWEET FB: early retire %d (1 tick at ring, 2 rekick, 4 honest SwapWait poll)\n", value);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_fb_early_retire, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_fb_early_retire, "I", "SWEET FB: early swap retire mask (1 tick, 2 rekick, 4 SwapWait poll)");

/*
 * 1042: kern.sweet_vsync_lock 0|1 (default 0 = as before): 0995's vsync options as a runtime switch (the cmdline is full):
 * 1 = sweetvsync 7 + sweetfb_refresh 1 (SweetFramebuffer.cpp sweet_vlock): ticks without slop on the panel's measured
 * period (INTF1 frame counter), IOMFBDisplayRefresh published from it. 0 = the values from before.
 */
extern int sweet_fb_vsync_lock(int on);         /* iokit/Kernel/arm/SweetFramebuffer.cpp */
static int sweet_vsync_lock;
static int
sysctl_sweet_vsync_lock SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, sweet_vsync_lock, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 1) {
		return EINVAL;
	}
	error = sweet_fb_vsync_lock(value);
	if (error == 0) {
		sweet_vsync_lock = value;
	}
	return error;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_vsync_lock, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_vsync_lock, "I", "SWEET FB: vsync ticks + IOMFBDisplayRefresh on the measured panel period (0995 mask 7)");

/*
 * redfin 1049: kern.sweet_vsyncphase US (0 = off, default; boot-arg sweetvsyncphase): vsync ticks US us before CA's
 * display-link grid (Yofuness 0274/0275, sunfish 5000), kern.sweet_vsyncphase_grid 0 auto | Hz (<= 1000) | ns: the grid
 * (iokit/Kernel/arm/SweetFramebuffer.cpp, vsyncPhaseNext). Live from the next tick.
 */
extern uint32_t sweet_vsyncphase, sweet_vsyncphase_grid;
static int
sysctl_sweet_vsyncphase SYSCTL_HANDLER_ARGS
{
#pragma unused(arg2)
	uint32_t *var = (uint32_t *)arg1;
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)*var, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 100000000) {
		return EINVAL;
	}
	*var = (uint32_t)value;
	printf("SWEET FB: %s %d (1049: vsync phase %u us, grid %u: 0 auto, <= 1000 Hz, else ns)\n", oidp->oid_name, value,
	    sweet_vsyncphase, sweet_vsyncphase_grid);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_vsyncphase, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sweet_vsyncphase, 0, sysctl_sweet_vsyncphase, "I", "SWEET FB: vsync ticks US before CA's display-link grid (0 off)");
SYSCTL_PROC(_kern, OID_AUTO, sweet_vsyncphase_grid, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sweet_vsyncphase_grid, 0, sysctl_sweet_vsyncphase, "I", "SWEET FB: 1049 grid: 0 auto, <= 1000 Hz, else ns");

/* redfin 1060: read-only frame counter for the FPS overlay (iokit/Kernel/arm/SweetFramebuffer.cpp) */
extern uint64_t sweet_fb_frames;
extern uint32_t sweet_fb_hz;
/*
 * 1099: the cold-boot brightness bug (SweetFramebuffer.cpp, SweetHID.cpp). kern.sweet_bl_trace bit 0 property
 * reads/writes of AppleARMBacklight + AppleCLCD, bit 1 IOMFB calls ("SWEET BLT:", rate-limited); kern.sweet_bl_fix
 * bit 0 IOMFBBrightnessLevel = factor (no DCS), bit 1 boot value 1.0, bit 2 ALS keep-alive until sensd.
 */
extern uint32_t sweet_bl_trace, sweet_bl_fix;
SYSCTL_UINT(_kern, OID_AUTO, sweet_bl_trace, CTLFLAG_RW | CTLFLAG_LOCKED, &sweet_bl_trace, 0,
    "SWEET BLT: 1 backlight/AppleCLCD property reads+writes, 2 IOMFB user-client calls (1099)");
/* 1101: kern.sweet_bl_fix is a handler: bit 8 (backlight-calibration-parameters) publishes/removes at once */
extern void sweet_bl_fix_changed(void);
static int
sysctl_sweet_bl_fix SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = (int)sweet_bl_fix, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error == 0 && changed) {
		sweet_bl_fix = (uint32_t)value;
		sweet_bl_fix_changed();
	}
	return error;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_bl_fix, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_bl_fix, "I",
    "1099: 1 IOMFBBrightnessLevel is a factor (no DCS), 2 boot IOMFBBrightnessLevel 1.0, 4 ALS keep-alive until sensd; "
    "1100/1101: 8 backlight-calibration-parameters (live; restart backboardd)");
SYSCTL_QUAD(_kern, OID_AUTO, sweet_fb_frames, CTLFLAG_RD | CTLFLAG_LOCKED, &sweet_fb_frames,
    "SWEET FB: frames that reached the panel (panel-buffer shown + direct-scanout latched)");
SYSCTL_INT(_kern, OID_AUTO, sweet_fb_hz, CTLFLAG_RD | CTLFLAG_LOCKED, &sweet_fb_hz, 0, "SWEET FB: panel refresh, Hz");

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
	tlb_after_map_locked(rc);       /* 1039 */
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
		tlb_after_unmap_locked(iova, size);     /* 1039 */
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
	gpu_owner_locked(fence, proc_selfpid());
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
	gpu_owner_locked(gpu.fence + 1, proc_selfpid());
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
#if A6_GPU_ID == 620
	/*
	 * redfin (0904): the same HLOS RAM (live DT memory 0xc0000000 + 3 GB, XNU from 4 GB), minus
	 * kregistry_mem 0xc8000000 + 64 KB (no-map in the DT), wland's rmtfs buffer 0xe1001000-0xe1281000
	 * and the GPU log trail / GMU dump area at 0xe2000000 (GPU_TRAIL_PA, 0xe2010000).
	 */
	{ 0xc4000000ULL, 0x04000000ULL },       /* 64 MB, up to kregistry */
	{ 0xc8400000ULL, 0x17c00000ULL },       /* 380 MB */
	{ 0xe2400000ULL, 0x1dc00000ULL },       /* 476 MB */
#else
	{ 0xc4000000ULL, 0x1c000000ULL },       /* 448 MB (the first 64 MB left to ABL's ramdisk) */
	{ 0xe1400000ULL, 0x1ec00000ULL },       /* 492 MB; 0xe1000000-0xe1400000 is wland's rmtfs buffer (hyp-assigned to the modem) */
#endif
};
static struct { uint64_t pa, size; } gpool_free[GPOOL_FREE_MAX];       /* sorted by address, merged */
static uint32_t gpool_nfree;
static uint64_t gpool_used, gpool_peak;

/* GPU pool bytes not in use (sweet_gpu_pool_alloc), for the per-client cap in SweetAccelerator */
uint64_t sweet_gpu_pool_avail(void);
uint64_t
sweet_gpu_pool_avail(void)
{
	uint64_t total = 0;
	for (unsigned i = 0; i < sizeof(gpool_ranges) / sizeof(gpool_ranges[0]); i++) {
		total += gpool_ranges[i].size;
	}
	return total - gpool_used;
}
static bool gpool_ready;
static LCK_MTX_DECLARE(sweet_gpool_lock, &sweet_gpu_grp);

/*
 * redfin 1097: kern.sweet_gpu_pool_fit (boot-arg sweetgpufit, default 1). 0 = first fit (0115, the lowest block that
 * is big enough); 1 = best fit (the smallest block that is big enough, lowest address on a tie), which keeps the big
 * blocks whole for big buffers (WebKit.GPU's 13.5 MB RGBA16F companions, scanout slots). The free list was already
 * kept sorted and coalesced on free. New allocations only.
 */
static uint32_t gpool_fit = 0xffffffffu;
static uint64_t gpool_fails, gpool_allocs;
static uint32_t gpool_fail_logged;
static uint32_t
gpool_fit_mode(void)
{
	if (gpool_fit == 0xffffffffu) {
		uint32_t v = 1;
		PE_parse_boot_argn("sweetgpufit", &v, sizeof(v));
		gpool_fit = v ? 1 : 0;
	}
	return gpool_fit;
}

/* Lock held: free bytes, the largest free block, blocks in the free list. */
static void
gpool_stats_locked(uint64_t *free_total, uint64_t *largest, uint32_t *nblocks)
{
	uint64_t f = 0, l = 0;
	for (uint32_t i = 0; i < gpool_nfree; i++) {
		f += gpool_free[i].size;
		if (gpool_free[i].size > l) {
			l = gpool_free[i].size;
		}
	}
	*free_total = f;
	*largest = l;
	*nblocks = gpool_nfree;
}

static void gpool_init_locked(void);
void sweet_mtl_clients_log(void);       /* iokit/Kernel/arm/SweetAccelerator.cpp (1097) */

/*
 * redfin 1097: the pool in the klog: "SWEET GPU: pool (why): total, free, largest free block, blocks, in use, peak,
 * allocations, refusals", the 8 largest free blocks, then one line per v2 client (SweetAccelerator: held, peak,
 * reuse cache, refusals). By `sctl kern.sweet_gpu_pool_dump=1` and at the first 8 refusals (then every 64th).
 */
void sweet_gpu_pool_dump(const char *why);
void
sweet_gpu_pool_dump(const char *why)
{
	uint64_t free_total, largest, top[8] = { 0 }, top_pa[8] = { 0 };
	uint32_t nb;
	uint64_t total = 0;
	for (unsigned i = 0; i < sizeof(gpool_ranges) / sizeof(gpool_ranges[0]); i++) {
		total += gpool_ranges[i].size;
	}
	lck_mtx_lock(&sweet_gpool_lock);
	gpool_init_locked();
	gpool_stats_locked(&free_total, &largest, &nb);
	for (uint32_t i = 0; i < gpool_nfree; i++) {
		uint64_t sz = gpool_free[i].size, at = gpool_free[i].pa;
		for (unsigned j = 0; j < 8; j++) {
			if (sz > top[j]) {
				uint64_t t = top[j], tp = top_pa[j];
				top[j] = sz;
				top_pa[j] = at;
				sz = t;
				at = tp;
			}
		}
	}
	uint64_t used = gpool_used, peak = gpool_peak, allocs = gpool_allocs, fails = gpool_fails;
	uint32_t fit = gpool_fit_mode();
	lck_mtx_unlock(&sweet_gpool_lock);
	printf("SWEET GPU: pool (%s): total %llu MB, free %llu MB, largest free %llu KB, %u free blocks, in use %llu MB "
	    "(peak %llu), allocs %llu, refused %llu, %s fit\n", why, total >> 20, free_total >> 20, largest >> 10, nb, used >> 20,
	    peak >> 20, allocs, fails, fit ? "best" : "first");
	printf("SWEET GPU: pool largest free blocks (KB@PA): %llu@%llx %llu@%llx %llu@%llx %llu@%llx %llu@%llx %llu@%llx %llu@%llx %llu@%llx\n",
	    top[0] >> 10, top_pa[0], top[1] >> 10, top_pa[1], top[2] >> 10, top_pa[2], top[3] >> 10, top_pa[3],
	    top[4] >> 10, top_pa[4], top[5] >> 10, top_pa[5], top[6] >> 10, top_pa[6], top[7] >> 10, top_pa[7]);
	sweet_mtl_clients_log();
}

/* Lock held: the free list from the ranges, once. */
static void
gpool_init_locked(void)
{
	if (!gpool_ready) {
		for (unsigned i = 0; i < sizeof(gpool_ranges) / sizeof(gpool_ranges[0]); i++) {
			gpool_free[gpool_nfree].pa = gpool_ranges[i].pa;
			gpool_free[gpool_nfree].size = gpool_ranges[i].size;
			gpool_nfree++;
		}
		gpool_ready = true;
	}
}

/* First or best fit (kern.sweet_gpu_pool_fit): the physical address of `bytes` (rounded to 16 KB), zeroing is the caller's. 0 = ok. */
int
sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa)
{
	uint64_t size = (bytes + GPOOL_UNIT - 1) & ~(GPOOL_UNIT - 1);
	int rc = -1;
	bool dump = false;
	uint64_t free_total = 0, largest = 0;
	uint32_t nb = 0;
	lck_mtx_lock(&sweet_gpool_lock);
	gpool_init_locked();
	uint32_t pick = gpool_nfree;
	bool best = gpool_fit_mode() != 0;
	for (uint32_t i = 0; i < gpool_nfree; i++) {
		if (gpool_free[i].size >= size) {
			if (!best) {
				pick = i;
				break;
			}
			if (pick == gpool_nfree || gpool_free[i].size < gpool_free[pick].size) {
				pick = i;
				if (gpool_free[i].size == size) {
					break;
				}
			}
		}
	}
	if (pick < gpool_nfree) {
		uint32_t i = pick;
		*pa = gpool_free[i].pa;
		gpool_free[i].pa += size;
		gpool_free[i].size -= size;
		if (gpool_free[i].size == 0) {
			memmove(&gpool_free[i], &gpool_free[i + 1], (gpool_nfree - i - 1) * sizeof(gpool_free[0]));
			gpool_nfree--;
		}
		gpool_used += size;
		gpool_allocs++;
		if (gpool_used > gpool_peak) {
			gpool_peak = gpool_used;
		}
		if ((gpool_used >> 25) != ((gpool_used - size) >> 25)) {
			a6_log("pool: %llu MB in use (peak %llu)\n", gpool_used >> 20, gpool_peak >> 20);
		}
		rc = 0;
	} else {
		gpool_fails++;
		gpool_stats_locked(&free_total, &largest, &nb);
		if (gpool_fail_logged < 8 || (gpool_fails & 63) == 0) {
			gpool_fail_logged++;
			dump = true;
		}
	}
	lck_mtx_unlock(&sweet_gpool_lock);
	if (rc) {
		/* 1097: free total and largest block tell a full pool from a fragmented one */
		a6_log("pool: no %llu bytes free (%llu MB in use, %llu MB free, largest free block %llu KB in %u blocks)\n", size,
		    gpool_used >> 20, free_total >> 20, largest >> 10, nb);
		if (dump) {
			sweet_gpu_pool_dump("refused");
		}
	}
	return rc;
}

/* 1097: the largest block the pool could give now (for the v2 refusal lines) */
uint64_t sweet_gpu_pool_largest(void);
uint64_t
sweet_gpu_pool_largest(void)
{
	uint64_t free_total, largest;
	uint32_t nb;
	lck_mtx_lock(&sweet_gpool_lock);
	gpool_init_locked();
	gpool_stats_locked(&free_total, &largest, &nb);
	lck_mtx_unlock(&sweet_gpool_lock);
	return largest;
}

/*
 * redfin 1097: the v2 per-client cap and the compositor's reserve (0122: boot-args sweetgpucap / sweetgpureserve,
 * read once) are now kern.sweet_gpu_cap_mb (0 = no cap) and kern.sweet_gpu_reserve_mb, live: the boot command line
 * is full. Defaults unchanged: 256 and 160 MB. The boot-args still set the start values.
 */
uint32_t sweet_gpu_cap_mb = 0xffffffffu, sweet_gpu_reserve_mb = 0xffffffffu;
void sweet_gpu_pool_limits(uint32_t *cap_mb, uint32_t *reserve_mb);
void
sweet_gpu_pool_limits(uint32_t *cap_mb, uint32_t *reserve_mb)
{
	if (sweet_gpu_cap_mb == 0xffffffffu) {
		uint32_t v = 256;
		PE_parse_boot_argn("sweetgpucap", &v, sizeof(v));
		uint32_t r = 160;
		PE_parse_boot_argn("sweetgpureserve", &r, sizeof(r));
		sweet_gpu_reserve_mb = r;
		sweet_gpu_cap_mb = v;
	}
	*cap_mb = sweet_gpu_cap_mb;
	*reserve_mb = sweet_gpu_reserve_mb;
}

static int
sysctl_sweet_gpu_cap_mb SYSCTL_HANDLER_ARGS
{
#pragma unused(arg2)
	uint32_t cap, reserve;
	sweet_gpu_pool_limits(&cap, &reserve);
	bool is_cap = arg1 != NULL;
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)(is_cap ? cap : reserve), sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 4096) {
		return EINVAL;
	}
	if (is_cap) {
		sweet_gpu_cap_mb = (uint32_t)value;
	} else {
		sweet_gpu_reserve_mb = (uint32_t)value;
	}
	printf("SWEET GPU: v2 pool %s %d MB (1097)%s\n", is_cap ? "cap per client" : "compositor reserve", value,
	    is_cap && value == 0 ? " = no cap" : "");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_cap_mb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    (void *)1, 0, sysctl_sweet_gpu_cap_mb, "I", "SWEET GPU: v2 GPU-pool MB per non-system client (0 = no cap; boot sweetgpucap, 256)");
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_reserve_mb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, sysctl_sweet_gpu_cap_mb, "I", "SWEET GPU: v2 GPU-pool MB kept for backboardd/SpringBoard (boot sweetgpureserve, 160)");

static int
sysctl_sweet_gpu_pool_fit SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)gpool_fit_mode(), sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 1) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_gpool_lock);
	gpool_fit = (uint32_t)value;
	lck_mtx_unlock(&sweet_gpool_lock);
	printf("SWEET GPU: pool %s fit (1097)\n", value ? "best" : "first");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_pool_fit, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_pool_fit, "I", "SWEET GPU: pool allocation 0 first fit, 1 best fit (default; boot sweetgpufit)");

/* kern.sweet_gpu_pool_dump: write 1 = the pool and every v2 client into the klog; read = free MB, largest free block KB */
static int
sysctl_sweet_gpu_pool_dump SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, 0, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value != 0) {
		sweet_gpu_pool_dump("sysctl");
	}
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_pool_dump, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_pool_dump, "I", "SWEET GPU: write 1 = GPU pool state and per-client use into the klog (1097)");

/* kern.sweet_gpu_pool_info (string, read-only): "used U free F largest L blocks N peak P allocs A refused R fit best|first" (KB) */
static int
sysctl_sweet_gpu_pool_info SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char buf[192];
	uint64_t free_total, largest;
	uint32_t nb;
	lck_mtx_lock(&sweet_gpool_lock);
	gpool_init_locked();
	gpool_stats_locked(&free_total, &largest, &nb);
	uint64_t used = gpool_used, peak = gpool_peak, allocs = gpool_allocs, fails = gpool_fails;
	lck_mtx_unlock(&sweet_gpool_lock);
	snprintf(buf, sizeof(buf), "used %llu free %llu largest %llu blocks %u peak %llu allocs %llu refused %llu fit %s (KB)",
	    used >> 10, free_total >> 10, largest >> 10, nb, peak >> 10, allocs, fails, gpool_fit_mode() ? "best" : "first");
	return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_pool_info, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_pool_info, "A", "SWEET GPU: pool state (1097)");

/*
 * 1097: kern.sweet_gpu_pool_drainall 0|1 (default 1): when a v2 allocation finds no pool block (or would dip into
 * the reserve), every client's 0156 reuse cache goes back to the pool and it is tried once more (was: only its own).
 */
uint32_t sweet_mtl_drainall = 1;
SYSCTL_UINT(_kern, OID_AUTO, sweet_gpu_pool_drainall, CTLFLAG_RW | CTLFLAG_LOCKED, &sweet_mtl_drainall, 0,
    "SWEET GPU: a refused v2 allocation takes back every client's reuse cache first (1097, default 1)");

/*
 * redfin 1098: kern.sweet_mtl 1|2|3 (start value = boot-arg sweetmtl): the Metal plug-in IOAcceleratorES publishes,
 * 1 SweetMetal/SweetMTLDevice, 2 SweetMetal2/SweetMTL2Device, 3 HakiosMetal/HakiosMetalDevice
 * (/System/Library/Extensions/HakiosMetal.bundle must be there). For processes that make their Metal device later.
 */
extern int sweet_mtl_publish(int which);
static int
sysctl_sweet_mtl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, sweet_mtl_publish(-1), sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (sweet_mtl_publish(value) < 0) {
		return EINVAL;
	}
	printf("SWEET MTL: Metal plug-in %d (%s; 1098; processes that create their device from now)\n", value,
	    value == 3 ? "HakiosMetal" : value == 2 ? "SweetMetal2" : "SweetMetal");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_mtl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_mtl, "I", "SWEET MTL: published Metal plug-in 1 SweetMetal, 2 SweetMetal2, 3 HakiosMetal (1098)");

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
	tlb_after_map_locked(rc);       /* 1039 */
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
		tlb_after_unmap_locked(iova, len);      /* 1039 */
	}
	lck_mtx_unlock(&sweet_gpu_lock);
}

uint32_t sweet_scan_submit_gate(uint32_t gf);  /* 0985: SweetFramebuffer.cpp; 0989: a GPU-side wait's reference; 1104: gf */

/*
 * 1104: kern.sweet_scangate_flag 0|1 (default 1). A v2 submission may carry NativeAdrenoAbiSubmit.flags bit 0
 * (NativeAdrenoAbiSubmitWritesDisplay: hakios-metal HM_GATE_FLAG=1 sets it on a submission that writes the display
 * surface). With 1, once a process has sent one flagged submission, the scanout gate (p scanout gate 1|2|3) holds only
 * its flagged submissions: its offscreen passes, compute and pre-buffers go into the ring at once. A process that never
 * flagged (Turnip, hm without HM_GATE_FLAG, every kernel client) is gated as before (all of the swapper's submits).
 * 0 = the flag is ignored (accepted, every submission of the swapper gated as before 1104).
 * The flagged process is one pid (the newest that flagged): only the swapper (backboardd) is ever gated, and a new
 * backboardd is gated in full until its first flagged submission.
 */
int sweet_scangate_flag = 1;
static volatile int scangate_flag_pid = -1;
SYSCTL_INT(_kern, OID_AUTO, sweet_scangate_flag, CTLFLAG_RW | CTLFLAG_LOCKED, &sweet_scangate_flag, 0,
    "SWEET SCANOUT: gate only display-writing submits of processes that flag them (1104, default 1)");

/*
 * 0989: the direct scanout's "latched frame" word (scratch + A6_SCAN_WORD_OFF), which gated submits make the CP wait
 * for (p scanout gate 2); and each process's last fence (the swapper's frame is done at its own last submit, not at
 * the latest of anyone's: r39's fence waits included other clients' work queued after it).
 */
void sweet_gpu_scan_word(uint32_t v);
void
sweet_gpu_scan_word(uint32_t v)
{
	if (gpu_ready) {
		a6_write(a6_pool_pa(&gpu, A6_OFF_SCRATCH + A6_SCAN_WORD_OFF), &v, 4);
	}
}

static struct { int pid; uint32_t fence; } pid_fence[8];
uint32_t sweet_gpu_fence_of(int pid);
uint32_t
sweet_gpu_fence_of(int pid)
{
	for (unsigned i = 0; i < 8; i++) {
		if (pid_fence[i].pid == pid) {
			return pid_fence[i].fence;
		}
	}
	return 0;
}

/*
 * 1038: kern.sweet_gpu_ibmax N (1..128, default 16 = A6_SUBMIT_MAX as before): IBs one v2 submit may carry. The v2
 * record stays NativeAdrenoAbiSubmit; a client sending more than 16 appends the extra NativeAdrenoAbiSubmitBuffer
 * entries right after the fixed array (structure input = sizeof + (count - 16) x 24 bytes; SweetAccelerator.cpp).
 * Ring room: one submit is 27 + 4 x N dwords (539 at 128) of the 8192-dword ring; a6_submit_ibs_async_wait waits for
 * used + n + 16 < 8192 as before. Each IB is one CP_INDIRECT_BUFFER (IB1) in the ring, so N has no CP limit.
 */
static int
sysctl_sweet_gpu_ibmax SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)a6_submit_max, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 1 || value > A6_SUBMIT_MAX_HW) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_gpu_lock);  /* not while a submit builds its packet */
	a6_submit_max = (unsigned)value;
	lck_mtx_unlock(&sweet_gpu_lock);
	printf("SWEET GPU: ibmax %d IBs per v2 submit\n", value);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gpu_ibmax, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gpu_ibmax, "I", "SWEET GPU: IBs per v2 submit (1..128, default 16)");

unsigned sweet_gpu_ibmax(void);
unsigned
sweet_gpu_ibmax(void)
{
	return a6_submit_max;
}

/* Queue up to kern.sweet_gpu_ibmax IBs: the fence, 0 when the GPU is not usable, or 0 with *busy when the ring is full. */
uint32_t
sweet_gpu_submit_v2(const uint64_t *iova, const uint32_t *dwords, unsigned n, uint32_t flags, bool *busy)
{
	*busy = false;
	if (!gpu_ready) {
		return 0;
	}
	/* 0153: a full ring waits here (sweetringwait ms, default 2000) instead of bouncing busy back to Turnip,
	 * whose retry gives up after ~6 ms when the ring is full of other clients' work (device lost). */
	static uint32_t ring_wait_ms;
	if (!ring_wait_ms && !PE_parse_boot_argn("sweetringwait", &ring_wait_ms, sizeof(ring_wait_ms)))
		ring_wait_ms = 2000;
	/* 0985: CA's next frame waits for its last one on screen (direct scanout): here (gate 1) or in the CP (0989, gate 2) */
	int pid = proc_selfpid();
	SWEET_TRACE_START(SWT_GPU_SUBMIT, pid, n, 0, 0);     /* 0994 */
	/* 1104: gf bit 0 = writes the display (the client's flag), bit 1 = this pid flags its display submits */
	uint32_t gf = flags & 1u;
	if (sweet_scangate_flag) {
		if (gf) {
			scangate_flag_pid = pid;
		}
		if (pid == scangate_flag_pid) {
			gf |= 2u;
		}
	}
	uint32_t wait_ref = sweet_scan_submit_gate(gf);
	lck_mtx_lock(&sweet_gpu_lock);
	gpu_note_submit_locked();
	uint64_t gpu_ticks = 0;
	if (SWEET_TRACE_ON()) {        /* 0994: the GPU clock against mach time, every 50 ms at most */
		static uint64_t cal_abs, cal_every;
		if (cal_every == 0) {
			nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &cal_every);
		}
		uint64_t t0 = mach_absolute_time();
		if (t0 - cal_abs > cal_every) {
			cal_abs = t0;
			gpu_ticks = a6_timestamp(&gpu);         /* CP_ALWAYS_ON_COUNTER, as Turnip's GetTimestamp */
			SWEET_TRACE(SWT_GPU_CAL, gpu_ticks, t0, mach_absolute_time(), 0);
		}
	}
	uint32_t fence = a6_submit_ibs_async_wait(&gpu, iova, dwords, n, ring_wait_ms,
	    wait_ref ? a6_pool_va(A6_OFF_SCRATCH + A6_SCAN_WORD_OFF) : 0, wait_ref);
	gpu_owner_locked(fence, pid);
	if (fence != 0) {
		unsigned slot = 0;      /* 0989: this pid's entry, else the oldest */
		for (unsigned i = 0; i < 8; i++) {
			if (pid_fence[i].pid == pid) {
				slot = i;
				break;
			}
			if ((int32_t)(pid_fence[i].fence - pid_fence[slot].fence) < 0) {
				slot = i;
			}
		}
		pid_fence[slot].pid = pid;
		pid_fence[slot].fence = fence;
	}
	lck_mtx_unlock(&sweet_gpu_lock);
	*busy = fence == 0 && n > 0 && n <= a6_submit_max;     /* 1038 */
	SWEET_TRACE_END(SWT_GPU_SUBMIT, fence, wait_ref, pid, gpu_ticks);    /* 0994 */
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
	if (sweet_gpu_fence_done(fence)) {
		return 0;      /* 0994: already done: not traced */
	}
	SWEET_TRACE_START(SWT_GPU_WAIT, fence, timeout_ns, 0, 0);     /* 0994 */
	for (;;) {
		if (sweet_gpu_fence_done(fence)) {
			SWEET_TRACE_END(SWT_GPU_WAIT, fence, 0, gpu_ready ? a6_fence_read(&gpu) : 0, 0);
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
			int r = sweet_gpu_fence_done(fence) ? 0 : 1;
			SWEET_TRACE_END(SWT_GPU_WAIT, fence, r, gpu_ready ? a6_fence_read(&gpu) : 0, 0);     /* 0994 */
			return r;
		}
		if (gpu_irq_on) {
			/* 0999: asleep until the GPU interrupt, the timeout, or the next look (hang check, lost interrupt) */
			uint64_t left_abs, left_ns = timeout_ns - ns, dl = now + gpu_irq_slice_abs;
			nanoseconds_to_absolutetime(left_ns < NSEC_PER_SEC ? left_ns : NSEC_PER_SEC, &left_abs);
			if (now + left_abs < dl) {
				dl = now + left_abs;
			}
			(void)assert_wait_deadline((event_t)&gpu_irq_chan, THREAD_UNINT, dl);
			if (sweet_gpu_fence_done(fence) || !gpu_irq_on) {
				clear_wait(current_thread(), THREAD_AWAKENED);  /* landed before the wait was armed */
				continue;
			}
			gpu_irq_sleeps++;
			if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT && sweet_gpu_fence_done(fence)) {
				gpu_irq_late++;
				if (gpu_irqs == 0 && gpu_irq_late >= 64) {
					a6_log("gpu irq: 64 fences landed and no interrupt came: back to polling\n");
					(void)gpu_irq_set(false, "no interrupts");
				}
			}
			continue;
		}
		gpu_irq_polls++;
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

/*
 * 0941: sweet_gpu_lost for the render server (backboardd, SpringBoard). A reset caused by another process's job
 * also throws away the frames the compositor had queued behind it; telling Turnip so loses its device for good
 * (r18e: backboardd's screen froze with the camera running). The compositor redraws its next frame anyway, so a
 * reset whose hung job was not its own is not reported to it (logged, *spared counts them). sweetgpuspare=0: as
 * sweet_gpu_lost.
 */
bool
sweet_gpu_lost_spare(uint64_t *epoch, uint32_t last_fence, int self_pid, uint32_t *spared)
{
	static int spare = -1;
	if (spare < 0 && !PE_parse_boot_argn("sweetgpuspare", &spare, sizeof(spare))) {
		spare = 1;
	}
	bool hit = false;
	lck_mtx_lock(&sweet_gpu_lock);
	if (recoveries - *epoch > LOST_RING) {
		*epoch = recoveries - LOST_RING;
	}
	for (; *epoch < recoveries; (*epoch)++) {
		uint32_t lo = lost[*epoch % LOST_RING].lo, hi = lost[*epoch % LOST_RING].hi;
		int culprit = lost[*epoch % LOST_RING].culprit;
		if (last_fence && (int32_t)(last_fence - lo) > 0 && (int32_t)(hi - last_fence) >= 0) {
			if (spare && culprit >= 0 && culprit != self_pid) {
				(*spared)++;
				a6_log("reset by pid %d's job: pid %d's queued work (fence %u) lost, not reported to it\n", culprit,
				    self_pid, last_fence);
			} else {
				hit = true;
			}
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
