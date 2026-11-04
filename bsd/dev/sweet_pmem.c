/*
 * /dev/pmem: physical memory for userland hardware experiments (the GPU
 * bring-up in work/userland/gpu runs on it over SSH, no reboot per try).
 * Root only (0600); boot-arg sweetpmem=0 leaves the node out.
 *
 *   pread/pwrite at offset = physical address.
 *     Managed DRAM goes through the physical aperture with the lines
 *     cleaned+invalidated to the point of coherency before and after, so
 *     data a device wrote is seen and CPU writes reach memory (IO is not
 *     coherent here). Anything else (MMIO, carve-outs such as the panel
 *     framebuffer) is mapped as device memory and accessed with 32-bit
 *     loads/stores only: offset and length must be multiples of 4.
 *     Reading an unclocked block hangs the SoC, as with poke scripts.
 *   ioctl PMEM_ALLOC   {size, align} -> {pa}: zeroed, physically contiguous
 *                      kernel memory (stays until PMEM_FREE, survives exit)
 *   ioctl PMEM_FREE    {pa}
 *   ioctl PMEM_SMC     x0..x7 in, x0..x3 out (SMCCC; e.g. SCM PAS calls)
 *   ioctl PMEM_LIST    allocation table, for tools that lost their state
 *   ioctl PMEM_RPMH    one RPMh active vote: resource name, 1-4 (offset, value) commands
 *                      (ARC levels, VRM rails; bsd/dev/sweet_rpmh.c)
 *
 * 1057 (redfin track V, Venus video decode; Yofuness/ios-flame 0277 + 0278 for
 * sunfish): the Venus buffer window 0xa2a00000 + 160 MB (free low-bank RAM
 * below 4 GB, outside XNU's window at 0x100000000) is read and written through
 * ONE mapping with any offset and length, instead of 2 MB device windows with
 * 32-bit accesses (~14 MB/s: a 1080p NV12 frame is 3.1 MB). Boot-arg and RW
 * sysctl kern.sweet_venus: 0 (default) = as before (device windows), 1 =
 * write-combined (uncached: no maintenance), 2 = cacheable with DC CIVAC around
 * each read and write. A change takes effect at the next access (the mapping
 * and device windows over the range are dropped). The queue block below
 * (0xa2600000-0xa2901000) stays on device windows: firmware and host poll it.
 * Guard, any mode: reads/writes into 0xa2600000-0xaca00000 are refused (EPERM,
 * logged once) when that RAM is XNU's (pmap_valid_address: e.g. given to the
 * VM by an extra-RAM option such as 1054's xram=1): Venus must not DMA there.
 *
 * Userland side: work/userland/gpu/pmem.h (same structs and numbers).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern boolean_t pmap_valid_address(uint64_t pa);
extern vm_map_address_t phystokv(uint64_t pa);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void _disable_preemption(void);
extern void _enable_preemption(void);
void sweet_pmem_init(void);
void sweet_smc(uint64_t x[8]);

struct pmem_alloc {
	uint64_t size;          /* in: bytes (rounded up to pages) */
	uint64_t align;         /* in: bytes, power of 2, 0 = page */
	uint64_t pa;            /* out */
};
struct pmem_smc {
	uint64_t x[8];
};
#define PMEM_LIST_MAX   64
struct pmem_list {
	uint32_t count;
	uint32_t pad;
	struct {
		uint64_t pa, size;
	} ent[PMEM_LIST_MAX];
};
#define PMEM_ALLOC      _IOWR('P', 1, struct pmem_alloc)
#define PMEM_FREE       _IOW('P', 2, uint64_t)
#define PMEM_SMC        _IOWR('P', 3, struct pmem_smc)
#define PMEM_LIST       _IOR('P', 4, struct pmem_list)
struct pmem_rpmh {
	char res[16];           /* cmd-db name, e.g. "mss.lvl", "ldoa1" */
	uint32_t n;             /* 1..4 commands */
	uint32_t offset[4];     /* from the resource address */
	uint32_t value[4];
};
#define PMEM_RPMH       _IOW('P', 5, struct pmem_rpmh)
extern int sweet_rpmh_write(const char *res, unsigned n, const uint32_t *offsets, const uint32_t *values);

#define PMEM_MAPS       16              /* 2 MB device windows, round robin */
#define PMEM_WIN        0x200000ULL
#define PM_VQ_PA        0xa2600000ULL   /* 1057: Venus queue block (venusd's default --phys), device windows */
#define PM_VEN_PA       0xa2a00000ULL   /* 1057: Venus buffer window, 2 MB aligned past the queue block's windows */
#define PM_VEN_LEN      0x0a000000ULL   /* 160 MB, to 0xaca00000 */

static struct {
	vm_offset_t va;
	uint64_t pa, size;
} pm_alloc[PMEM_LIST_MAX];
static struct {
	vm_offset_t va;
	uint64_t win;
} pm_map[PMEM_MAPS];
static unsigned pm_map_next;
static uint32_t pm_bounce[PAGE_SIZE / 4];
static int pm_venus;                    /* 1057: kern.sweet_venus / boot-arg sweetvenus: 0 device, 1 WC, 2 cached */
static int pm_ven_mapped;               /* the mode pm_ven_va was mapped with (0: none) */
static vm_offset_t pm_ven_va;
static bool pm_ven_guard_said;

static LCK_GRP_DECLARE(sweet_pmem_grp, "sweet_pmem");
static LCK_MTX_DECLARE(sweet_pmem_lock, &sweet_pmem_grp);

/*
 * Device mapping of the 2 MB window holding pa, from a small round-robin
 * cache. ml_io_map's mappings are permanent (unmapping one panics) and come
 * from the small io_submap (a 2 MB one panicked "kmem_alloc failed"), so
 * windows use ml_io_map_unmappable: kernel_map, freed by ml_io_unmap.
 */
static volatile uint32_t *
pm_io(uint64_t pa)
{
	uint64_t win = pa & ~(PMEM_WIN - 1);
	for (unsigned i = 0; i < PMEM_MAPS; i++) {
		if (pm_map[i].va && pm_map[i].win == win) {
			return (volatile uint32_t *)(pm_map[i].va + (pa - win));
		}
	}
	if (pm_venus && pa >= PM_VEN_PA && pa < PM_VEN_PA + PM_VEN_LEN) {
		return NULL;    /* 1057: never a device alias of the Venus window while it has its own mapping */
	}
	unsigned i = pm_map_next++ % PMEM_MAPS;
	if (pm_map[i].va) {
		ml_io_unmap(pm_map[i].va, PMEM_WIN);
	}
	pm_map[i].va = ml_io_map_unmappable((vm_offset_t)win, PMEM_WIN, VM_WIMG_IO);
	pm_map[i].win = win;
	return pm_map[i].va ? (volatile uint32_t *)(pm_map[i].va + (pa - win)) : NULL;
}

/*
 * 1057: the Venus window's single mapping in the current mode (caller holds
 * sweet_pmem_lock). A mode change drops the old mapping and every device
 * window over the range first: no two mappings of one page with different
 * memory types.
 */
static vm_offset_t
pm_ven_map(void)
{
	if (pm_ven_va && pm_ven_mapped == pm_venus) {
		return pm_ven_va;
	}
	if (pm_ven_va) {
		ml_io_unmap(pm_ven_va, PM_VEN_LEN);
		pm_ven_va = 0;
		pm_ven_mapped = 0;
	}
	for (unsigned i = 0; i < PMEM_MAPS; i++) {
		if (pm_map[i].va && pm_map[i].win + PMEM_WIN > PM_VEN_PA && pm_map[i].win < PM_VEN_PA + PM_VEN_LEN) {
			ml_io_unmap(pm_map[i].va, PMEM_WIN);
			pm_map[i].va = 0;
		}
	}
	if (!pm_venus) {
		return 0;
	}
	pm_ven_va = ml_io_map_unmappable((vm_offset_t)PM_VEN_PA, PM_VEN_LEN, pm_venus == 2 ? VM_WIMG_DEFAULT : VM_WIMG_WCOMB);
	if (pm_ven_va) {
		pm_ven_mapped = pm_venus;
		printf("SWEET PMEM: Venus window 0x%llx+%llu MB mapped %s\n", PM_VEN_PA, PM_VEN_LEN >> 20,
		    pm_venus == 2 ? "cacheable (DC CIVAC around accesses)" : "write-combined");
	}
	return pm_ven_va;
}

static int
pm_rw(struct uio *uio, bool write)
{
	int err = 0;
	lck_mtx_lock(&sweet_pmem_lock);
	while (!err && uio_resid(uio) > 0) {
		uint64_t pa = (uint64_t)uio_offset(uio);
		uint64_t off = pa & PAGE_MASK;
		unsigned n = (unsigned)MIN((user_ssize_t)(PAGE_SIZE - off), uio_resid(uio));
		if (pa >= PM_VQ_PA && pa < PM_VEN_PA + PM_VEN_LEN && pmap_valid_address(pa)) {
			if (!pm_ven_guard_said) {
				pm_ven_guard_said = true;
				printf("SWEET PMEM: refused 0x%llx: the Venus range 0x%llx-0x%llx is XNU's RAM here (extra RAM"
				    " given to the VM?): no Venus buffers there\n", pa, PM_VQ_PA, PM_VEN_PA + PM_VEN_LEN);
			}
			err = EPERM;
			break;
		}
		if (pm_venus && pa >= PM_VEN_PA && pa + n <= PM_VEN_PA + PM_VEN_LEN) {
			vm_offset_t base = pm_ven_map();
			if (!base) {
				err = ENOMEM;
				break;
			}
			caddr_t va = (caddr_t)(base + (pa - PM_VEN_PA));
			if (pm_venus == 2) {
				if (!write) {
					flush_dcache((vm_offset_t)va, n, FALSE);        /* DC CIVAC: drop lines older than the DMA */
				}
				err = uiomove(va, (int)n, uio);
				if (write) {
					flush_dcache((vm_offset_t)va, n, FALSE);        /* to the point of coherency for Venus */
				}
			} else {
				err = uiomove(va, (int)n, uio);
				__asm__ volatile ("dsb osh" ::: "memory");
			}
			continue;
		}
		if (pmap_valid_address(pa)) {
			caddr_t va = (caddr_t)phystokv(pa);
			flush_dcache64((addr64_t)pa, n, TRUE);
			err = uiomove(va, (int)n, uio);
			flush_dcache64((addr64_t)pa, n, TRUE);
			continue;
		}
		if ((pa | n) & 3) {
			err = EINVAL;
			break;
		}
		volatile uint32_t *r = pm_io(pa);
		if (r == NULL) {
			err = ENOMEM;
			break;
		}
		if (write) {
			err = uiomove((caddr_t)pm_bounce, (int)n, uio);
			__asm__ volatile ("dmb oshst" ::: "memory");
			for (unsigned i = 0; !err && i < n / 4; i++) {
				r[i] = pm_bounce[i];
			}
			__asm__ volatile ("dsb osh" ::: "memory");
		} else {
			for (unsigned i = 0; i < n / 4; i++) {
				pm_bounce[i] = r[i];
			}
			__asm__ volatile ("dmb oshld" ::: "memory");
			err = uiomove((caddr_t)pm_bounce, (int)n, uio);
		}
	}
	lck_mtx_unlock(&sweet_pmem_lock);
	return err;
}

static int
pm_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return pm_rw(uio, false);
}

static int
pm_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return pm_rw(uio, true);
}

static int
pm_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
pm_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void
pm_smc_once(uint64_t x[8])
{
	register uint64_t x0 __asm__("x0") = x[0];
	register uint64_t x1 __asm__("x1") = x[1];
	register uint64_t x2 __asm__("x2") = x[2];
	register uint64_t x3 __asm__("x3") = x[3];
	register uint64_t x4 __asm__("x4") = x[4];
	register uint64_t x5 __asm__("x5") = x[5];
	register uint64_t x6 __asm__("x6") = x[6];
	register uint64_t x7 __asm__("x7") = x[7];
	__asm__ volatile ("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	x[0] = x0;
	x[1] = x1;
	x[2] = x2;
	x[3] = x3;
	x[6] = x6;
}

/*
 * Qualcomm TZ returns QCOM_SCM_INTERRUPTED (1) when a non-secure interrupt
 * arrives during a call (the shim never saw it: interrupts off). Like
 * Linux's __scm_smc_do_quirk: call again with x0 = 1, the same arguments
 * and the x6 TZ handed back (ARM_SMCCC_QUIRK_QCOM_A6), on the same CPU,
 * until a final result.
 */
#define QCOM_SCM_INTERRUPTED    1

static void
pm_smc(struct pmem_smc *s)
{
	uint64_t x[8], fn = s->x[0];
	unsigned resumes = 0;
	memcpy(x, s->x, sizeof(x));
	_disable_preemption();
	for (;;) {
		pm_smc_once(x);
		if (x[0] != QCOM_SCM_INTERRUPTED || resumes >= 100000) {
			break;
		}
		uint64_t a6 = x[6];
		memcpy(x, s->x, sizeof(x));
		x[0] = QCOM_SCM_INTERRUPTED;
		x[6] = a6;
		resumes++;
	}
	_enable_preemption();
	memcpy(s->x, x, 4 * sizeof(uint64_t));
	if (resumes) {
		printf("SWEET PMEM: smc %llx resumed %u times -> %llx\n", fn, resumes, x[0]);
	}
}

/* For bsd/dev/sweet_gpu.c (zap shader): x0..x7 in, x0..x3 out. */
void
sweet_smc(uint64_t x[8])
{
	struct pmem_smc s;
	memcpy(s.x, x, sizeof(s.x));
	pm_smc(&s);
	memcpy(x, s.x, 4 * sizeof(uint64_t));
}

static int
pm_do_alloc(struct pmem_alloc *a)
{
	vm_size_t size = round_page(a->size);
	uint64_t align = a->align > PAGE_SIZE ? a->align : PAGE_SIZE;
	if (size == 0 || (align & (align - 1))) {
		return EINVAL;
	}
	int slot = -1;
	for (int i = 0; i < PMEM_LIST_MAX; i++) {
		if (!pm_alloc[i].va) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return ENOSPC;
	}
	vm_offset_t va = 0;
	if (kmem_alloc_contig(kernel_map, &va, size, PAGE_MASK, 0, (ppnum_t)(atop_64(align) - 1),
	    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)va);
	if (!pn) {
		kmem_free(kernel_map, va, size);
		return ENOMEM;
	}
	a->pa = ptoa_64(pn);
	flush_dcache64((addr64_t)a->pa, (unsigned)size, TRUE);
	pm_alloc[slot].va = va;
	pm_alloc[slot].pa = a->pa;
	pm_alloc[slot].size = size;
	printf("SWEET PMEM: alloc %llu bytes at 0x%llx (pid %d)\n", (uint64_t)size, a->pa, proc_selfpid());
	return 0;
}

static int
pm_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	int err = 0;
	lck_mtx_lock(&sweet_pmem_lock);
	switch (cmd) {
	case PMEM_ALLOC:
		err = pm_do_alloc((struct pmem_alloc *)data);
		break;
	case PMEM_FREE: {
		uint64_t pa = *(uint64_t *)data;
		err = ENOENT;
		for (int i = 0; i < PMEM_LIST_MAX; i++) {
			if (pm_alloc[i].va && pm_alloc[i].pa == pa) {
				kmem_free(kernel_map, pm_alloc[i].va, (vm_size_t)pm_alloc[i].size);
				pm_alloc[i].va = 0;
				err = 0;
				break;
			}
		}
		break;
	}
	case PMEM_SMC:
		pm_smc((struct pmem_smc *)data);
		break;
	case PMEM_LIST: {
		struct pmem_list *l = (struct pmem_list *)data;
		bzero(l, sizeof(*l));
		for (int i = 0; i < PMEM_LIST_MAX; i++) {
			if (pm_alloc[i].va) {
				l->ent[l->count].pa = pm_alloc[i].pa;
				l->ent[l->count].size = pm_alloc[i].size;
				l->count++;
			}
		}
		break;
	}
	case PMEM_RPMH: {
		struct pmem_rpmh *r = (struct pmem_rpmh *)data;
		r->res[sizeof(r->res) - 1] = 0;
		lck_mtx_unlock(&sweet_pmem_lock);       /* waits up to 100 ms; not under our lock */
		err = sweet_rpmh_write(r->res, r->n, r->offset, r->value);
		printf("SWEET PMEM: rpmh %s %u cmds (%#x=%u ...) -> %d\n", r->res, r->n, r->offset[0], r->value[0], err);
		return err;
	}
	default:
		err = ENOTTY;
	}
	lck_mtx_unlock(&sweet_pmem_lock);
	return err;
}

static const struct cdevsw sweet_pmem_cdevsw = {
	.d_open       = pm_open,
	.d_close      = pm_close,
	.d_read       = pm_read,
	.d_write      = pm_write,
	.d_ioctl      = pm_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

/* 1057: kern.sweet_venus, 0 device windows (default), 1 write-combined, 2 cached + DC CIVAC */
static int
sysctl_sweet_venus SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, pm_venus, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 2) {
		return EINVAL;
	}
	lck_mtx_lock(&sweet_pmem_lock);
	pm_venus = value;
	(void)pm_ven_map();     /* drops the old mapping/device aliases; maps the new mode */
	lck_mtx_unlock(&sweet_pmem_lock);
	printf("SWEET PMEM: kern.sweet_venus %d (Venus window %s)\n", value,
	    value == 2 ? "cached" : value == 1 ? "write-combined" : "device windows");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_venus, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_venus, "I", "SWEET /dev/pmem Venus buffer window: 0 device, 1 write-combined, 2 cached");

void
sweet_pmem_init(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweetpmem", &on, sizeof(on));
	if (!on) {
		return;
	}
	int major = cdevsw_add(-1, &sweet_pmem_cdevsw);
	if (major < 0) {
		printf("SWEET PMEM: cdevsw_add failed (%d)\n", major);
		return;
	}
	devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "pmem");
	if (PE_parse_boot_argn("sweetvenus", &pm_venus, sizeof(pm_venus)) && (pm_venus < 0 || pm_venus > 2)) {
		pm_venus = 0;   /* 1057 */
	}
	printf("SWEET PMEM: /dev/pmem ready (major %d), Venus window %s\n", major,
	    pm_venus == 2 ? "cached+maintenance" : pm_venus ? "write-combined" : "device (sweetvenus=0)");
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
