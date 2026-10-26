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
#include "sweet_a6xx.h"
#include "../../iokit/Kernel/arm/SweetDisplayMask.h"

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
	}
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
static struct { uint32_t lo, hi; int culprit; } lost[LOST_RING];	/* 0941: culprit = pid of the job the GPU was on (-1 unknown) */

/*
 * GPU time of the submits that landed (their CP_ALWAYS_ON stamps, 19.2 MHz), by the process that queued them
 * (pid 0 = the kernel's presents), summed on every submit and logged every 5 s. Fences that fall out of the
 * stamp ring (A6_TS_SLOTS) before they are summed only count as "missed".
 */
#define GSTAT_OWNERS    6
static uint32_t gstat_next = 1;
static uint64_t gstat_abs;
static int gstat_pid[A6_TS_SLOTS];
static struct { int pid; uint32_t n; uint64_t busy; } gstat_own[GSTAT_OWNERS + 1];     /* last: everyone else */
static uint32_t gstat_n, gstat_missed;
static uint64_t gstat_busy, gstat_longest;

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
		for (o = 0; o < GSTAT_OWNERS && gstat_own[o].n && gstat_own[o].pid != pid; o++) {
		}
		gstat_own[o].pid = o < GSTAT_OWNERS ? pid : -1;
		gstat_own[o].n++;
		gstat_own[o].busy += t;
		gstat_n++;
		gstat_busy += t;
		if (t > gstat_longest) {
			gstat_longest = t;
		}
	}
	uint64_t now = mach_absolute_time(), ns;
	if (gstat_abs == 0) {
		gstat_abs = now;
	}
	absolutetime_to_nanoseconds(now - gstat_abs, &ns);
	if (ns < 5000000000ULL) {
		return;
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
	gstat_n = gstat_missed = 0;
	gstat_busy = gstat_longest = 0;
	bzero(gstat_own, sizeof(gstat_own));
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

/* ---- surfaces ---- */

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
	/* 0153: a full ring waits here (sweetringwait ms, default 2000) instead of bouncing busy back to Turnip,
	 * whose retry gives up after ~6 ms when the ring is full of other clients' work (device lost). */
	static uint32_t ring_wait_ms;
	if (!ring_wait_ms && !PE_parse_boot_argn("sweetringwait", &ring_wait_ms, sizeof(ring_wait_ms)))
		ring_wait_ms = 2000;
	lck_mtx_lock(&sweet_gpu_lock);
	gpu_note_submit_locked();
	uint32_t fence = a6_submit_ibs_async(&gpu, iova, dwords, n, ring_wait_ms);
	gpu_owner_locked(fence, proc_selfpid());
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
