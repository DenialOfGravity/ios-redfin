/*
 * SWEET internal UFS root device.
 *
 * The Qualcomm UFS host (UFSHCI 2.1) is left enabled with the link up by the
 * bootloader, which loaded our boot image from it. This driver reuses that
 * state: it points the transfer request list at its own memory (verifying the
 * host took it before ringing any doorbell) and issues polled SCSI commands,
 * one at a time, to LUN 0. It finds one GPT partition by name (boot-arg
 * ufsroot=, default "userdata") and exposes it as the BSD disk ufsroot0
 * (rufsroot0), with 512-byte logical sectors over the device's 4 KiB blocks.
 * Root on it with rd=ufsroot0.
 *
 * All DMA goes through one physically contiguous allocation (descriptors and
 * a 1 MiB bounce buffer); the host is not cache coherent, so caches are
 * cleaned/invalidated around each command.
 *
 * Writes are refused unless boot-arg ufsrw=1 is set. Every write is checked
 * against the partition bounds again right before it is issued, so nothing
 * outside the named partition can be written.
 *
 * Write guard (1032, idea from HakiOS hakios.ufs.c): inside the partition a
 * file system / raw-device write must also lie inside the HFS+ volume (size
 * from its volume header, re-read whenever block 0 is written), and only the
 * kernel's own SWEETLOG thread may write the reserved tail (shim rescue +
 * scratch + console ring, the last UFS_TAIL_BLOCKS blocks). sysctl
 * kern.sweet_ufs_guard: 0 off, 1 log only (default), 2 refuse with EPERM.
 * kern.sweet_ufs_guard_hits counts violations.
 *
 * /dev/rufsroot0 read/write copy through a kernel buffer with uiomove (no
 * physio: its B_PHYS buffers map to the USER address, which the strategy
 * routine must not memcpy to -> PAN fault).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <kern/kalloc.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <machine/atomic.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* osfmk/arm/cpu_data.h, not exported to the BSD layer */
extern int get_preemption_level(void);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;
static void sweet_log_start(void);
extern void sweet_sample_start(void);
extern void sweet_keys_init(void);
extern void sweet_touch_init(void);
extern void sweet_motion_init(void);
extern void sweet_fts_init(void);
extern void sweet_sects_init(void);
extern void sweet_haptic_init(void);
extern void sweet_torch_init(void);
extern void sweet_cam_init(void);
extern void sweet_audio_init(void);
extern void redwood_adsp_init(void);
extern void sweet_geniuart_init(void);
extern void sweet_btbridge_init(void);
extern void sweet_abp_init(void);
extern void sweet_cpufreq_init(void);
extern void sweet_pmem_init(void);
extern void flame_wlan_init(void);
extern void sweet_gpu_init(void);
extern void sweet_usb_probe(void);

#define REG_CAP         0x00
#define REG_IS          0x20
#define REG_IE          0x24
#define REG_HCS         0x30
#define REG_HCE         0x34
#define REG_UTRLBA      0x50
#define REG_UTRLBAU     0x54
#define REG_UTRLDBR     0x58
#define REG_UTRLCLR     0x5c
#define REG_UTRLRSR     0x60
/* 1069 (kern.sweet_ufs_recover): error status, UIC commands, Qualcomm vendor timers (Linux ufshci.h, ufs-qcom.h) */
#define REG_UECPA       0x38
#define REG_UECDL       0x3c
#define REG_UECN        0x40
#define REG_UECT        0x44
#define REG_UECDME      0x48
#define REG_UTMRLDBR    0x78
#define REG_UICCMD      0x90
#define REG_UCMDARG1    0x94
#define REG_UCMDARG2    0x98
#define REG_UCMDARG3    0x9c
#define REG_QC_SYS1CLK_1US      0xc0
#define REG_QC_TX_SYMBOL_CLK    0xc4
#define REG_QC_LINK_STARTUP_TMR 0xd8
#define REG_QC_CFG2             0xe0
#define IS_UPMS         (1u << 4)
#define IS_UCCS         (1u << 10)
#define IS_FATAL        ((1u << 2) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))  /* UE DFES UTPES HCFES SBFES */
#define HCS_UCRDY       (1u << 3)

#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define CAP_64AS        (1u << 24)              /* 64-bit DMA addressing */

#define UFS_BLOCK       4096u
#define BOUNCE_SIZE     (1u << 20)
#define PRDT_CHUNK      (256u << 10)            /* PRDT byte count field: 18 bits */
#define PRDT_MAX        (BOUNCE_SIZE / PRDT_CHUNK)
#define CMD_TIMEOUT_NS  (5ull * NSEC_PER_SEC)
#define CMD_SPIN_NS     (50ull * NSEC_PER_USEC)
#define CMD_SLEEP_NS    (200ull * NSEC_PER_USEC)

struct utrd {
	uint32_t dw0, dw1, dw2, dw3;
	uint32_t ucdba, ucdbau;
	uint32_t resp;          /* [31:16] response UPIU offset, [15:0] length, DWORDs */
	uint32_t prdt;          /* [31:16] PRDT offset (DWORDs), [15:0] entries */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;
	} prdt[PRDT_MAX];
};

/* Layout of the contiguous DMA allocation. */
#define DMA_UTRL        0x0000                  /* 1 KiB aligned */
#define DMA_UCD         0x1000                  /* 128 byte aligned */
#define DMA_BOUNCE      0x4000
#define DMA_SIZE        (DMA_BOUNCE + BOUNCE_SIZE)

static struct {
	vm_offset_t regs;
	vm_offset_t dma;                        /* kernel VA of the DMA allocation */
	uint64_t dma_pa;
	uint8_t lun;
	uint8_t tag;
	uint64_t first_block, nblocks;          /* partition, in 4 KiB device blocks */
	uint32_t secsize;                       /* logical sector size for the BSD layer */
	bool writable;                          /* boot-arg ufsrw=1 */
	bool cache;                             /* boot-arg ufscache=1 */
	bool wait;                              /* boot-arg ufswait=1 */
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
	uint64_t vol_blocks;                    /* HFS+ volume size in device blocks, 0 = unknown */
} su;

/*
 * Reserved tail of the partition, in 4 KiB blocks: shim rescue (64) +
 * scratch (64) + SWEETLOG console ring (1 + 1024, see sweet_log_thread).
 */
#define UFS_TAIL_BLOCKS         (64u + 64u + 1025u)
#define UFS_LOG_TAIL_BLOCKS     1025u

enum { UFS_W_FS, UFS_W_LOG };           /* who issues a write */

static int sweet_ufs_guard = 1;         /* 0 off, 1 log, 2 enforce */
static uint32_t sweet_ufs_guard_hits;

static int
sysctl_sweet_ufs_guard SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, sweet_ufs_guard, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 2) {
		return EINVAL;
	}
	sweet_ufs_guard = value;
	printf("SWEET UFS GUARD: mode %d (%s)\n", value,
	    value == 2 ? "enforce" : value == 1 ? "log" : "off");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ufs_guard, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_ufs_guard, "I", "SWEET UFS write guard: 0 off, 1 log, 2 enforce");
SYSCTL_UINT(_kern, OID_AUTO, sweet_ufs_guard_hits, CTLFLAG_RD | CTLFLAG_LOCKED,
    &sweet_ufs_guard_hits, 0, "SWEET UFS write guard violations since boot");

/* HFS+/HFSX volume header at byte 1024 of the partition (big endian). */
static uint64_t
ufs_hfs_blocks(const uint8_t *blk0)
{
	const uint8_t *h = blk0 + 1024;
	if (!((h[0] == 'H' && h[1] == '+') || (h[0] == 'H' && h[1] == 'X'))) {
		return 0;
	}
	uint32_t bsz = (uint32_t)h[40] << 24 | (uint32_t)h[41] << 16 | (uint32_t)h[42] << 8 | h[43];
	uint32_t tot = (uint32_t)h[44] << 24 | (uint32_t)h[45] << 16 | (uint32_t)h[46] << 8 | h[47];
	if (bsz < 512 || (bsz & (bsz - 1)) || tot == 0) {
		return 0;
	}
	return ((uint64_t)bsz * tot + UFS_BLOCK - 1) / UFS_BLOCK;
}

/*
 * Range rule for a write of count blocks at partition-relative block rel
 * (already known to be inside the partition). Returns 0 or EPERM.
 */
static int
ufs_guard_check(uint64_t lba, uint64_t rel, uint32_t count, int who)
{
	if (sweet_ufs_guard == 0 || su.nblocks <= UFS_TAIL_BLOCKS) {
		return 0;
	}
	uint64_t end = rel + count, lo, hi;
	if (who == UFS_W_LOG) {
		lo = su.nblocks - UFS_LOG_TAIL_BLOCKS;
		hi = su.nblocks;
	} else {
		lo = 0;
		hi = su.nblocks - UFS_TAIL_BLOCKS;
		if (su.vol_blocks && su.vol_blocks < hi) {
			hi = su.vol_blocks;
		}
	}
	if (rel >= lo && end <= hi) {
		return 0;
	}
	sweet_ufs_guard_hits++;
	if (sweet_ufs_guard_hits <= 50 || (sweet_ufs_guard_hits % 1000) == 0) {
		char name[32] = "?";
		proc_selfname(name, sizeof(name));
		printf("SWEET UFS GUARD: %s write lun %u lba 0x%llx n %u from %s%s (allowed rel 0x%llx-0x%llx, hit %u)\n",
		    sweet_ufs_guard == 2 ? "refused" : "would refuse", su.lun, lba, count, name,
		    who == UFS_W_LOG ? " [sweetlog]" : "", lo, hi, sweet_ufs_guard_hits);
	}
	return sweet_ufs_guard == 2 ? EPERM : 0;
}

static LCK_GRP_DECLARE(sweet_ufs_grp, "sweet_ufs");
static LCK_MTX_DECLARE(sweet_ufs_lock, &sweet_ufs_grp);

static inline uint32_t
ufs_rd(uint32_t off)
{
	return *(volatile uint32_t *)(su.regs + off);
}

static inline void
ufs_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(su.regs + off) = v;
}

static void
dma_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
}

static void
dma_clean(uint32_t off, uint32_t len)
{
	clean_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
}

/*
 * One SCSI command in slot 0, data (if any) to/from the bounce buffer
 * (to the device when write is set). Returns 0 or an errno.
 */
int sweet_ufs_recover;                  /* 1069: 0 off (as before), 1 log, 2 + abort/retry, 3 + host reset (sweet_ssr.c reads it) */
static void ufs_diag(const char *what, uint8_t op);
static uint32_t ufs_sim_fail;           /* kern.sweet_ufs_recover_test=1: the next command fails without being sent */

static int
ufs_scsi_once(const uint8_t *cdb, unsigned cdb_len, uint32_t len, bool write)
{
	struct utrd *utrd = (struct utrd *)(su.dma + DMA_UTRL);
	struct ucd *ucd = (struct ucd *)(su.dma + DMA_UCD);
	uint64_t ucd_pa = su.dma_pa + DMA_UCD, bounce_pa = su.dma_pa + DMA_BOUNCE;

	assert(len <= BOUNCE_SIZE);
	bzero(utrd, sizeof(*utrd));
	bzero(ucd, sizeof(*ucd));

	ucd->cmd[0] = 0x01;                     /* COMMAND UPIU */
	ucd->cmd[1] = len ? (write ? 0x20 : 0x40) : 0x00;      /* W: data out, R: data in */
	ucd->cmd[2] = su.lun;
	ucd->cmd[3] = ++su.tag;
	ucd->cmd[12] = (uint8_t)(len >> 24);
	ucd->cmd[13] = (uint8_t)(len >> 16);
	ucd->cmd[14] = (uint8_t)(len >> 8);
	ucd->cmd[15] = (uint8_t)len;
	memcpy(&ucd->cmd[16], cdb, cdb_len);

	/* DD: 1 = memory to device, 2 = device to memory. */
	utrd->dw0 = (1u << 28) | ((len ? (write ? 1u : 2u) : 0u) << 25) | (1u << 24);
	utrd->dw2 = 0x0f;                       /* OCS invalid until updated */
	utrd->ucdba = (uint32_t)ucd_pa;
	utrd->ucdbau = (uint32_t)(ucd_pa >> 32);
	utrd->resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uint32_t n = 0;
		for (uint32_t done = 0; done < len; done += PRDT_CHUNK, n++) {
			uint32_t chunk = MIN(PRDT_CHUNK, len - done);
			ucd->prdt[n].addr_lo = (uint32_t)(bounce_pa + done);
			ucd->prdt[n].addr_hi = (uint32_t)((bounce_pa + done) >> 32);
			ucd->prdt[n].dbc = chunk - 1;
		}
		utrd->prdt = (1024u / 4) << 16 | n;
	}

	if (su.cache) {
		dma_clean(DMA_UTRL, sizeof(*utrd));
		dma_clean(DMA_UCD, 16 + cdb_len);
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
		if (len) {
			dma_clean(DMA_UCD + offsetof(struct ucd, prdt),
			    (utrd->prdt & 0xffff) * sizeof(ucd->prdt[0]));
		}
	} else {
		dma_sync(DMA_UTRL, sizeof(*utrd));
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len) {
		if (su.cache && write) {
			dma_clean(DMA_BOUNCE, len);
		} else {
			dma_sync(DMA_BOUNCE, len);
		}
	}
	__builtin_arm_dsb(0xf);

	if (ufs_sim_fail && sweet_ufs_recover >= 1) {
		ufs_sim_fail--;
		printf("SWEET UFS: command 0x%02x: simulated timeout (kern.sweet_ufs_recover_test), not sent\n", cdb[0]);
		return ETIMEDOUT;
	}
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t deadline;
	nanoseconds_to_absolutetime(CMD_TIMEOUT_NS, &deadline);
	deadline += mach_absolute_time();
	bool sleepable = su.wait && su.ready && ml_get_interrupts_enabled() &&
	    !ml_at_interrupt_context() && get_preemption_level() == 0;
	uint64_t spin_until = 0, sleep_interval = 0;
	if (sleepable) {
		nanoseconds_to_absolutetime(CMD_SPIN_NS, &spin_until);
		spin_until += mach_absolute_time();
		nanoseconds_to_absolutetime(CMD_SLEEP_NS, &sleep_interval);
	}
	while (ufs_rd(REG_UTRLDBR) & 1) {
		uint64_t now = mach_absolute_time();
		if (now > deadline) {
			if (sweet_ufs_recover >= 1) {
				ufs_diag("timeout", cdb[0]);
			}
			ufs_wr(REG_UTRLCLR, ~1u);
			printf("SWEET UFS: command 0x%02x timed out\n", cdb[0]);
			if (sweet_ufs_recover >= 1) {
				/* as Linux ufshcd_clear_cmd: the slot must be free before it is used again */
				for (int i = 0; i < 1000 && (ufs_rd(REG_UTRLDBR) & 1); i++) {
					delay(100);
				}
				if (ufs_rd(REG_UTRLDBR) & 1) {
					printf("SWEET UFS: slot 0 still busy 100 ms after UTRLCLR\n");
				}
				ufs_wr(REG_IS, ufs_rd(REG_IS));
			}
			return ETIMEDOUT;
		}
		if (sleepable && now >= spin_until) {
			uint64_t wake = now + MIN(sleep_interval, deadline - now);
			/* Keep the mutex: slot 0 and the bounce buffer are still owned. */
			if (assert_wait_deadline_with_leeway((event_t)&su.wait, THREAD_UNINT,
			    TIMEOUT_URGENCY_SYS_CRITICAL, wake, 0) == THREAD_WAITING) {
				thread_block(THREAD_CONTINUE_NULL);
			}
		} else {
			delay(1);
		}
	}
	__builtin_arm_dsb(0xf);
	ufs_wr(REG_IS, ufs_rd(REG_IS));         /* acknowledge completion status */

	dma_sync(DMA_UTRL, sizeof(*utrd));
	if (su.cache) {
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
	} else {
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len && !(su.cache && write)) {
		dma_sync(DMA_BOUNCE, len);
	}

	uint32_t ocs = utrd->dw2 & 0xff;
	if (ocs || ucd->resp[0] != 0x21 || ucd->resp[6] || ucd->resp[7]) {
		printf("SWEET UFS: command 0x%02x failed: ocs 0x%x upiu 0x%x response 0x%x status 0x%x\n",
		    cdb[0], ocs, ucd->resp[0], ucd->resp[6], ucd->resp[7]);
		if (sweet_ufs_recover >= 1) {
			ufs_diag("failed", cdb[0]);
		}
		return EIO;
	}
	return 0;
}

/*
 * 1069: error handling (kern.sweet_ufs_recover, boot-arg sweetufsrec; default 0 = the driver as before 1069).
 * Reference: Linux ufshcd.c (ufshcd_print_host_regs / ufshcd_clear_cmd / ufshcd_err_handler -> ufshcd_reset_and_restore
 * -> ufshcd_hba_enable + ufshcd_link_startup + ufshcd_verify_dev_init + ufshcd_config_pwr_mode) and ufs-qcom.c.
 *   1  log: at a timeout or a failed command, the error bits (IS, HCS with its UTP error code, UEC PA/DL/N/T/DME, doorbells,
 *      the descriptor's OCS and the response UPIU); after a timeout the slot is cleared and waited for (<= 100 ms).
 *   2  + retry: the same request is issued again once (same LBA, same data in the bounce buffer: READ/WRITE(10),
 *      SYNCHRONIZE CACHE and TEST UNIT READY are idempotent; nothing is skipped or reported done that was not done).
 *   3  + host reset when the retry fails: HCE off/on, the vendor timers ABL set put back, DME_LINKSTARTUP (3 tries),
 *      our request list again, NOP OUT/IN, the power mode ABL negotiated (gears, lanes, series, termination, timeouts;
 *      read with DME_GET while the link was healthy) restored, TEST UNIT READY; then the request once more.
 *      No device reset (no GPIO; the device keeps its cache and its state), no task management.
 *      At most one reset per 2 s; after 5 failed resets in a row only 1/2 run until a request succeeds again.
 * If everything fails the error goes up as before (EIO/ETIMEDOUT). kern.sweet_ufs_recover_stats counts it all.
 * kern.sweet_ufs_recover_test (write): 1 = the next command fails as a timeout without being sent (tests 2), 2 = host reset
 * now + TEST UNIT READY (tests 3 on a healthy link; needs recover=3).
 */
#define UIC_DME_GET             0x01u
#define UIC_DME_SET             0x02u
#define UIC_DME_LINKSTARTUP     0x16u

#define PA_ACTIVETXDATALANES    0x1560u
#define PA_TXGEAR               0x1568u
#define PA_TXTERMINATION        0x1569u
#define PA_HSSERIES             0x156au
#define PA_PWRMODE              0x1571u
#define PA_ACTIVERXDATALANES    0x1580u
#define PA_RXGEAR               0x1583u
#define PA_RXTERMINATION        0x1584u
#define PA_LOCAL_TX_LCC_ENABLE  0x155eu
#define PA_PWRMODEUSERDATA0     0x15b0u

static struct {
	uint32_t timeouts, errors, retries, retry_ok, resets, reset_ok, reset_fail, fail_streak, gave_up, sim;
	uint64_t last_reset;
} ufs_ur;

/* the link as ABL left it (DME_GET at boot or when recover is set to 3) */
static const uint16_t pm_attr[] = {
	PA_RXGEAR, PA_ACTIVERXDATALANES, PA_RXTERMINATION, PA_TXGEAR, PA_ACTIVETXDATALANES, PA_TXTERMINATION, PA_HSSERIES,
	PA_PWRMODEUSERDATA0, PA_PWRMODEUSERDATA0 + 1, PA_PWRMODEUSERDATA0 + 2, PA_PWRMODEUSERDATA0 + 3,
	PA_PWRMODEUSERDATA0 + 4, PA_PWRMODEUSERDATA0 + 5, 0xd041, 0xd042, 0xd043,  /* DME_Local FC0/TC0/AFC0 timeouts */
};
#define PM_N    (sizeof(pm_attr) / sizeof(pm_attr[0]))
static struct {
	bool have;
	uint32_t v[PM_N];
	uint32_t pwrmode, lcc;
	bool lcc_ok;
	uint32_t qc[4];                 /* SYS1CLK_1US, TX_SYMBOL_CLK, LINK_STARTUP_TMR, CFG2 */
} ufs_pm;
static const uint32_t qc_reg[4] = { REG_QC_SYS1CLK_1US, REG_QC_TX_SYMBOL_CLK, REG_QC_LINK_STARTUP_TMR, REG_QC_CFG2 };

static void
ufs_diag(const char *what, uint8_t op)
{
	struct utrd *utrd = (struct utrd *)(su.dma + DMA_UTRL);
	struct ucd *ucd = (struct ucd *)(su.dma + DMA_UCD);
	uint32_t is = ufs_rd(REG_IS), hcs = ufs_rd(REG_HCS);

	if (*what == 't') {
		ufs_ur.timeouts++;
	} else {
		ufs_ur.errors++;
	}
	dma_sync(DMA_UTRL, sizeof(*utrd));
	dma_sync(DMA_UCD + offsetof(struct ucd, resp), 32);
	/* the UEC registers clear when read: read once, print once */
	uint32_t pa = ufs_rd(REG_UECPA), dl = ufs_rd(REG_UECDL), n = ufs_rd(REG_UECN), t = ufs_rd(REG_UECT), dme = ufs_rd(REG_UECDME);
	printf("SWEET UFS ERR: %s op 0x%02x: IS 0x%x%s%s%s%s%s%s HCS 0x%x (utp err %u tag %u lun %u, upmcrs %u) HCE 0x%x "
	    "DBR 0x%x RSR 0x%x TMDBR 0x%x | UEC pa 0x%x dl 0x%x n 0x%x t 0x%x dme 0x%x | OCS 0x%x upiu 0x%x resp 0x%x status 0x%x\n",
	    what, op, is, (is & (1u << 2)) ? " UE" : "", (is & (1u << 11)) ? " DFES" : "", (is & (1u << 12)) ? " UTPES" : "",
	    (is & (1u << 16)) ? " HCFES" : "", (is & (1u << 17)) ? " SBFES" : "", (is & (1u << 5 | 1u << 6)) ? " HIBERN8" : "",
	    hcs, (hcs >> 12) & 0xf, (hcs >> 16) & 0xff, (hcs >> 24) & 0xff, (hcs >> 8) & 7, ufs_rd(REG_HCE), ufs_rd(REG_UTRLDBR),
	    ufs_rd(REG_UTRLRSR), ufs_rd(REG_UTMRLDBR), pa, dl, n, t, dme, utrd->dw2 & 0xff, ucd->resp[0], ucd->resp[6], ucd->resp[7]);
}

/* one UIC command (UFSHCI 5.6.x): 0 or an errno; *val = UCMDARG3 afterwards */
static int
ufs_uic(uint32_t cmd, uint32_t attr, uint32_t val, uint32_t *out, uint32_t timeout_ms)
{
	for (int i = 0; i < 1000 && !(ufs_rd(REG_HCS) & HCS_UCRDY); i++) {
		delay(100);
	}
	if (!(ufs_rd(REG_HCS) & HCS_UCRDY)) {
		printf("SWEET UFS REC: UIC 0x%x attr 0x%x: controller not ready for UIC commands (HCS 0x%x)\n", cmd, attr, ufs_rd(REG_HCS));
		return EBUSY;
	}
	ufs_wr(REG_IS, IS_UCCS);
	ufs_wr(REG_UCMDARG1, attr << 16);
	ufs_wr(REG_UCMDARG2, 0);
	ufs_wr(REG_UCMDARG3, val);
	__builtin_arm_dsb(0xf);
	ufs_wr(REG_UICCMD, cmd);
	for (uint32_t i = 0; i < timeout_ms * 10 && !(ufs_rd(REG_IS) & IS_UCCS); i++) {
		delay(100);
	}
	if (!(ufs_rd(REG_IS) & IS_UCCS)) {
		printf("SWEET UFS REC: UIC 0x%x attr 0x%x: no completion in %u ms\n", cmd, attr, timeout_ms);
		return ETIMEDOUT;
	}
	ufs_wr(REG_IS, IS_UCCS);
	uint32_t rc = ufs_rd(REG_UCMDARG2) & 0xff;
	if (out) {
		*out = ufs_rd(REG_UCMDARG3);
	}
	if (rc) {
		printf("SWEET UFS REC: UIC 0x%x attr 0x%x: result code %u\n", cmd, attr, rc);
		return EIO;
	}
	return 0;
}

/* sweet_ufs_lock held, no request outstanding */
static void
ufs_pm_snapshot(void)
{
	bool ok = true;
	for (unsigned i = 0; i < PM_N && ok; i++) {
		ok = ufs_uic(UIC_DME_GET, pm_attr[i], 0, &ufs_pm.v[i], 100) == 0;
	}
	ok = ok && ufs_uic(UIC_DME_GET, PA_PWRMODE, 0, &ufs_pm.pwrmode, 100) == 0;
	ufs_pm.lcc_ok = ufs_uic(UIC_DME_GET, PA_LOCAL_TX_LCC_ENABLE, 0, &ufs_pm.lcc, 100) == 0;
	for (unsigned i = 0; i < 4; i++) {
		ufs_pm.qc[i] = ufs_rd(qc_reg[i]);
	}
	ufs_pm.have = ok;
	printf("SWEET UFS REC: link %s: pwrmode 0x%x gear rx %u tx %u, lanes rx %u tx %u, term rx %u tx %u, series %u, tx lcc %d; "
	    "qc 1us 0x%x sym 0x%x lst 0x%x cfg2 0x%x\n", ok ? "saved" : "NOT READ (no power mode restore after a reset)", ufs_pm.pwrmode,
	    ufs_pm.v[0], ufs_pm.v[3], ufs_pm.v[1], ufs_pm.v[4], ufs_pm.v[2], ufs_pm.v[5], ufs_pm.v[6], ufs_pm.lcc_ok ? (int)ufs_pm.lcc : -1, ufs_pm.qc[0], ufs_pm.qc[1], ufs_pm.qc[2],
	    ufs_pm.qc[3]);
}

/* Linux ufshcd_change_power_mode + ufshcd_uic_change_pwr_mode, with the values ABL had negotiated */
static int
ufs_pm_restore(void)
{
	uint32_t txm = ufs_pm.pwrmode & 0xf, rxm = (ufs_pm.pwrmode >> 4) & 0xf;
	if (!ufs_pm.have || !((txm == 1 || txm == 4) && (rxm == 1 || rxm == 4))) {
		printf("SWEET UFS REC: no saved fast mode (pwrmode 0x%x): the link stays in PWM-G1 (slow, data safe)\n", ufs_pm.pwrmode);
		return ENOENT;
	}
	for (unsigned i = 0; i < PM_N; i++) {
		if (ufs_uic(UIC_DME_SET, pm_attr[i], ufs_pm.v[i], NULL, 100) != 0) {
			return EIO;
		}
	}
	ufs_wr(REG_IS, IS_UPMS);
	if (ufs_uic(UIC_DME_SET, PA_PWRMODE, ufs_pm.pwrmode, NULL, 500) != 0) {
		return EIO;
	}
	for (int i = 0; i < 5000 && !(ufs_rd(REG_IS) & IS_UPMS); i++) {
		delay(100);
	}
	uint32_t is = ufs_rd(REG_IS), st = (ufs_rd(REG_HCS) >> 8) & 7;
	ufs_wr(REG_IS, IS_UPMS);
	if (!(is & IS_UPMS) || st != 1) {       /* PWR_LOCAL */
		printf("SWEET UFS REC: power mode change to 0x%x: %s, status %u\n", ufs_pm.pwrmode, (is & IS_UPMS) ? "done" : "no UPMS", st);
		return EIO;
	}
	return 0;
}

/* NOP OUT -> NOP IN in slot 0 (Linux ufshcd_verify_dev_init: 10 tries) */
static int
ufs_nop(void)
{
	struct utrd *utrd = (struct utrd *)(su.dma + DMA_UTRL);
	struct ucd *ucd = (struct ucd *)(su.dma + DMA_UCD);
	uint64_t ucd_pa = su.dma_pa + DMA_UCD;

	for (int t = 0; t < 10; t++) {
		bzero(utrd, sizeof(*utrd));
		bzero(ucd, offsetof(struct ucd, prdt));
		ucd->cmd[0] = 0x00;             /* NOP OUT */
		ucd->cmd[3] = ++su.tag;
		utrd->dw0 = (1u << 28) | (1u << 24);
		utrd->dw2 = 0x0f;
		utrd->ucdba = (uint32_t)ucd_pa;
		utrd->ucdbau = (uint32_t)(ucd_pa >> 32);
		utrd->resp = (512u / 4) << 16 | (512u / 4);
		dma_sync(DMA_UTRL, sizeof(*utrd));
		dma_sync(DMA_UCD, offsetof(struct ucd, prdt));
		__builtin_arm_dsb(0xf);
		ufs_wr(REG_UTRLDBR, 1);
		for (int i = 0; i < 500 && (ufs_rd(REG_UTRLDBR) & 1); i++) {
			delay(100);
		}
		if (ufs_rd(REG_UTRLDBR) & 1) {
			ufs_wr(REG_UTRLCLR, ~1u);
			for (int i = 0; i < 1000 && (ufs_rd(REG_UTRLDBR) & 1); i++) {
				delay(100);
			}
			continue;
		}
		__builtin_arm_dsb(0xf);
		ufs_wr(REG_IS, ufs_rd(REG_IS));
		dma_sync(DMA_UTRL, sizeof(*utrd));
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), 32);
		if ((utrd->dw2 & 0xff) == 0 && ucd->resp[0] == 0x20 && ucd->resp[6] == 0) {
			return 0;
		}
	}
	return EIO;
}

/* sweet_ufs_lock held: Linux ufshcd_reset_and_restore without the device reset */
static int
ufs_host_reset(const char *why)
{
	int e = 0;
	ufs_ur.resets++;
	ufs_ur.last_reset = mach_absolute_time();
	printf("SWEET UFS REC: host reset #%u (%s)\n", ufs_ur.resets, why);
	for (int round = 0; round < 2; round++) {
		ufs_wr(REG_UTRLRSR, 0);
		ufs_wr(REG_HCE, 0);
		for (int i = 0; i < 100 && (ufs_rd(REG_HCE) & 1); i++) {
			delay(100);
		}
		delay(1000);
		/* ufs-qcom's hce/link-startup notifications program these; HCE may or may not reset them: put ABL's back */
		for (unsigned i = 0; i < 4; i++) {
			uint32_t now = ufs_rd(qc_reg[i]);
			if (ufs_pm.have && now != ufs_pm.qc[i]) {
				printf("SWEET UFS REC: vendor reg 0x%x 0x%x -> 0x%x (ABL's)\n", qc_reg[i], now, ufs_pm.qc[i]);
				ufs_wr(qc_reg[i], ufs_pm.qc[i]);
			}
		}
		ufs_wr(REG_HCE, 1);
		delay(1000);
		for (int i = 0; i < 50 && !(ufs_rd(REG_HCE) & 1); i++) {
			delay(1000);
		}
		if (!(ufs_rd(REG_HCE) & 1)) {
			printf("SWEET UFS REC: HCE did not come back (HCS 0x%x)\n", ufs_rd(REG_HCS));
			e = EIO;
			continue;
		}
		ufs_wr(REG_IE, 0);
		ufs_wr(REG_IS, ufs_rd(REG_IS));
		if (ufs_pm.lcc_ok && ufs_pm.lcc == 0) {
			(void)ufs_uic(UIC_DME_SET, PA_LOCAL_TX_LCC_ENABLE, 0, NULL, 100);      /* ufs-qcom pre link startup */
		}
		e = EIO;
		for (int t = 0; t < 3 && e; t++) {
			e = ufs_uic(UIC_DME_LINKSTARTUP, 0, 0, NULL, 500);
			if (e == 0 && !(ufs_rd(REG_HCS) & HCS_DP)) {
				printf("SWEET UFS REC: link startup done but no device present (HCS 0x%x)\n", ufs_rd(REG_HCS));
				e = ENXIO;
			}
			ufs_wr(REG_IS, ufs_rd(REG_IS));
		}
		if (e == 0) {
			break;
		}
	}
	if (e == 0) {
		for (int i = 0; i < 1000 && !(ufs_rd(REG_HCS) & HCS_UTRLRDY); i++) {
			delay(100);
		}
		uint64_t list = su.dma_pa + DMA_UTRL;
		ufs_wr(REG_UTRLBA, (uint32_t)list);
		ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
		ufs_wr(REG_UTRLRSR, 1);
		if (!(ufs_rd(REG_HCS) & HCS_UTRLRDY) || ufs_rd(REG_UTRLBA) != (uint32_t)list ||
		    ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) || !(ufs_rd(REG_UTRLRSR) & 1)) {
			printf("SWEET UFS REC: host did not take our request list again (HCS 0x%x)\n", ufs_rd(REG_HCS));
			e = EIO;
		}
	}
	if (e == 0 && (e = ufs_nop()) != 0) {
		printf("SWEET UFS REC: no NOP IN from the device\n");
	}
	if (e == 0) {
		int pe = ufs_pm_restore();
		printf("SWEET UFS REC: power mode %s\n", pe == 0 ? "restored (as ABL set it)" : "NOT restored: PWM-G1");
		static const uint8_t tur[6] = { 0x00 };
		for (int i = 0; i < 3 && (e = ufs_scsi_once(tur, sizeof(tur), 0, false)); i++) {
		}
	}
	if (e == 0) {
		ufs_ur.reset_ok++;
		ufs_ur.fail_streak = 0;
	} else {
		ufs_ur.reset_fail++;
		ufs_ur.fail_streak++;
	}
	printf("SWEET UFS REC: host reset #%u %s (ok %u, failed %u)\n", ufs_ur.resets, e == 0 ? "OK" : "FAILED", ufs_ur.reset_ok, ufs_ur.reset_fail);
	return e;
}

/* every command goes through here (sweet_ufs_lock held) */
static int
ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len, bool write)
{
	int e = ufs_scsi_once(cdb, cdb_len, len, write);
	int level = sweet_ufs_recover;
	if (e == 0 || level < 2) {
		if (e == 0 && ufs_ur.fail_streak) {
			ufs_ur.fail_streak = 0;
		}
		return e;
	}
	ufs_ur.retries++;
	e = ufs_scsi_once(cdb, cdb_len, len, write);
	if (e == 0) {
		ufs_ur.retry_ok++;
		printf("SWEET UFS REC: command 0x%02x OK on the retry\n", cdb[0]);
		return 0;
	}
	if (level >= 3) {
		uint64_t gap;
		nanoseconds_to_absolutetime(2ull * NSEC_PER_SEC, &gap);
		if (ufs_ur.fail_streak >= 5) {
			if (ufs_ur.gave_up++ % 100 == 0) {
				printf("SWEET UFS REC: %u host resets in a row failed: no more resets until a command succeeds\n", ufs_ur.fail_streak);
			}
		} else if (ufs_ur.last_reset && mach_absolute_time() - ufs_ur.last_reset < gap) {
			ufs_ur.gave_up++;
		} else if (ufs_host_reset(e == ETIMEDOUT ? "timeout after retry" : "error after retry") == 0) {
			ufs_ur.retries++;
			e = ufs_scsi_once(cdb, cdb_len, len, write);
			if (e == 0) {
				ufs_ur.retry_ok++;
				printf("SWEET UFS REC: command 0x%02x OK after the host reset\n", cdb[0]);
				return 0;
			}
		}
	}
	printf("SWEET UFS REC: command 0x%02x still failing (%d): error to the caller\n", cdb[0], e);
	return e;
}

static int
sysctl_sweet_ufs_recover SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, sweet_ufs_recover, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 3) {
		return EINVAL;
	}
	if (value >= 3 && su.ready && !ufs_pm.have) {
		lck_mtx_lock(&sweet_ufs_lock);
		ufs_pm_snapshot();
		lck_mtx_unlock(&sweet_ufs_lock);
	}
	sweet_ufs_recover = value;
	printf("SWEET UFS REC: mode %d (%s)\n", value, value == 3 ? "log + retry + host reset" : value == 2 ? "log + retry" :
	    value == 1 ? "log" : "off");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ufs_recover, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_ufs_recover, "I",
    "redfin 1069: UFS errors: 0 off (as before), 1 log error bits, 2 + retry the request, 3 + host reset and retry");

static int
sysctl_sweet_ufs_recover_test SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, 0, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (proc_suser(req->p) != 0) {
		return EPERM;
	}
	if (!su.ready || sweet_ufs_recover < 2) {
		return ENXIO;           /* (a simulated failure with no retry would reach the file system) */
	}
	if (value == 1) {
		ufs_ur.sim++;
		lck_mtx_lock(&sweet_ufs_lock);
		ufs_sim_fail = 1;
		lck_mtx_unlock(&sweet_ufs_lock);
		printf("SWEET UFS REC: test: the next command fails as a timeout without being sent\n");
		return 0;
	}
	if (value == 2) {
		if (sweet_ufs_recover < 3) {
			return EPERM;
		}
		lck_mtx_lock(&sweet_ufs_lock);
		error = ufs_host_reset("kern.sweet_ufs_recover_test=2");
		lck_mtx_unlock(&sweet_ufs_lock);
		return error;
	}
	return EINVAL;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ufs_recover_test, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_ufs_recover_test,
    "I", "redfin 1069: write 1 = next UFS command fails without being sent (needs recover >= 2); 2 = host reset now (recover 3)");

static int
sysctl_sweet_ufs_recover_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char s[256];
	int n = snprintf(s, sizeof(s), "mode %d | timeouts %u errors %u | retries %u ok %u | resets %u ok %u failed %u streak %u | "
	    "gave up %u | simulated %u | link saved %d pwrmode 0x%x", sweet_ufs_recover, ufs_ur.timeouts, ufs_ur.errors, ufs_ur.retries,
	    ufs_ur.retry_ok, ufs_ur.resets, ufs_ur.reset_ok, ufs_ur.reset_fail, ufs_ur.fail_streak, ufs_ur.gave_up, ufs_ur.sim, ufs_pm.have, ufs_pm.pwrmode);
	return SYSCTL_OUT(req, s, (size_t)MIN(n, (int)sizeof(s) - 1) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ufs_recover_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    sysctl_sweet_ufs_recover_stats, "A", "redfin 1069: UFS error handling counters");

/* READ(10) of count device blocks at lba into the bounce buffer. */
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
	if (lba > UINT32_MAX || count > BOUNCE_SIZE / UFS_BLOCK) {
		return EINVAL;
	}
	uint8_t cdb[10] = {
		0x28, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);
}

/*
 * WRITE(10) of count device blocks from the bounce buffer to lba. The range
 * must lie inside the partition: this is the only place data goes out.
 */
static int
ufs_write_blocks(uint64_t lba, uint32_t count, int who)
{
	if (!su.writable || !su.nblocks || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK ||
	    lba < su.first_block || lba - su.first_block >= su.nblocks ||
	    count > su.nblocks - (lba - su.first_block) || lba + count - 1 > UINT32_MAX) {
		printf("SWEET UFS: refusing write of %u blocks at 0x%llx\n", count, lba);
		return EPERM;
	}
	uint64_t rel = lba - su.first_block;
	if (ufs_guard_check(lba, rel, count, who)) {
		return EPERM;
	}
	/* The HFS+ volume header lives in block 0: follow a resize. */
	if (who == UFS_W_FS && rel == 0) {
		uint64_t vb = ufs_hfs_blocks((const uint8_t *)(su.dma + DMA_BOUNCE));
		if (vb && vb != su.vol_blocks) {
			printf("SWEET UFS GUARD: HFS volume now %llu blocks\n", vb);
			su.vol_blocks = vb;
		}
	}
	uint8_t cdb[10] = {
		0x2a, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);
}

static int
ufs_sync_cache(void)
{
	static const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */
	return ufs_scsi(cdb, sizeof(cdb), 0, false);
}

static int
ufs_find_partition(const char *want)
{
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	int error = ufs_read_blocks(1, 1);
	if (error) {
		return error;
	}
	if (memcmp(b, "EFI PART", 8) != 0) {
		printf("SWEET UFS: LUN %u has no GPT\n", su.lun);
		return ENXIO;
	}
	uint64_t entry_lba;
	uint32_t nent, esz;
	memcpy(&entry_lba, b + 72, 8);
	memcpy(&nent, b + 80, 4);
	memcpy(&esz, b + 84, 4);
	if (esz != 128 || nent == 0 || nent * esz > BOUNCE_SIZE) {
		printf("SWEET UFS: unsupported GPT entries %u x %u\n", nent, esz);
		return ENXIO;
	}
	error = ufs_read_blocks(entry_lba, (nent * esz + UFS_BLOCK - 1) / UFS_BLOCK);
	if (error) {
		return error;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = b + i * esz;
		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
			if (ch == 0 || ch > 0x7f) {
				break;
			}
			name[c] = (char)ch;
		}
		name[c] = 0;
		if (strcmp(name, want) != 0) {
			continue;
		}
		uint64_t first, last;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (last < first) {
			return ENXIO;
		}
		su.first_block = first;
		su.nblocks = last - first + 1;
		return 0;
	}
	printf("SWEET UFS: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}

static int
ufs_take_over_host(void)
{
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		printf("SWEET UFS: host not ready (hce 0x%x hcs 0x%x)\n", hce, hcs);
		return ENXIO;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		printf("SWEET UFS: requests outstanding from the bootloader\n");
		return EBUSY;
	}
	/* With RAM above 4 GiB (mkadt.py --highmem) the DMA memory can be there too. */
	if (((su.dma_pa + DMA_SIZE - 1) >> 32) && !(ufs_rd(REG_CAP) & CAP_64AS)) {
		printf("SWEET UFS: DMA memory at 0x%llx but the host has no 64-bit addressing\n",
		    su.dma_pa);
		return ENXIO;
	}
	ufs_wr(REG_IE, 0);                      /* polled operation */
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_wr(REG_UTRLRSR, 0);
	uint64_t list = su.dma_pa + DMA_UTRL;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	/* Never ring a doorbell unless the host uses our list. */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		printf("SWEET UFS: host did not take our request list\n");
		return EIO;
	}
	return 0;
}

static int
sweet_ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!su.ready) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !su.writable) ? EROFS : 0;
}

static int
sweet_ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

/*
 * Copy count bytes at partition byte offset off to/from the KERNEL buffer
 * kaddr through the DMA bounce buffer. *donep = bytes transferred.
 */
static int
sweet_ufs_xfer(uint64_t off, uint32_t count, caddr_t kaddr, bool rd, uint32_t *donep)
{
	int error = 0;
	uint32_t done = 0;

	lck_mtx_lock(&sweet_ufs_lock);
	while (done < count) {
		uint64_t pos = off + done;
		uint64_t lba = pos / UFS_BLOCK;
		uint32_t skip = (uint32_t)(pos % UFS_BLOCK);
		uint32_t want = MIN(count - done, BOUNCE_SIZE - skip);
		uint32_t blocks = (skip + want + UFS_BLOCK - 1) / UFS_BLOCK;
		void *bounce = (void *)(su.dma + DMA_BOUNCE);
		if (rd) {
			error = ufs_read_blocks(su.first_block + lba, blocks);
			if (!error) {
				memcpy(kaddr + done, (const char *)bounce + skip, want);
			}
		} else {
			/* Partial device blocks at either end: read, merge, write back. */
			if (skip || (want % UFS_BLOCK)) {
				error = ufs_read_blocks(su.first_block + lba, blocks);
			}
			if (!error) {
				memcpy((char *)bounce + skip, kaddr + done, want);
				error = ufs_write_blocks(su.first_block + lba, blocks, UFS_W_FS);
			}
		}
		if (error) {
			break;
		}
		done += want;
	}
	lck_mtx_unlock(&sweet_ufs_lock);
	*donep = done;
	return error;
}

static void
sweet_ufs_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * su.secsize;
	uint32_t count = buf_count(bp), done = 0;
	uint64_t part_bytes = su.nblocks * UFS_BLOCK;

	bool rd = buf_flags(bp) & B_READ;

	buf_setresid(bp, count);
	if (!rd && !su.writable) {
		error = EROFS;
		goto done;
	}
	if (off > part_bytes || count > part_bytes - off) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	/*
	 * A physio buffer (B_PHYS without a UPL) maps to the USER address,
	 * which must not be memcpy'd to (PAN); the character device no longer
	 * uses physio, so refuse rather than fault.
	 */
	if ((buf_flags(bp) & (B_PHYS | B_CLUSTER)) == B_PHYS) {
		printf("SWEET UFS: B_PHYS buffer refused\n");
		error = ENOTSUP;
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}
	error = sweet_ufs_xfer(off, count, vaddr, rd, &done);
	buf_setresid(bp, count - done);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
sweet_ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)data;
	uint64_t *o = (uint64_t *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = su.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) {
			return ENODEV;
		}
		if (*f < DEV_BSIZE || *f > UFS_BLOCK || (*f & (*f - 1))) {
			return EINVAL;
		}
		su.secsize = *f;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = UFS_BLOCK;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = su.nblocks * UFS_BLOCK / su.secsize;
		return 0;
	case DKIOCISWRITABLE:
		*f = su.writable;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE: {
		if (!su.writable) {
			return 0;
		}
		lck_mtx_lock(&sweet_ufs_lock);
		int error = ufs_sync_cache();
		lck_mtx_unlock(&sweet_ufs_lock);
		return error;
	}
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = BOUNCE_SIZE / su.secsize;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = su.writable ? BOUNCE_SIZE / su.secsize : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
sweet_ufs_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return sweet_ufs_ioctl(dev, cmd, data, flag, p, 0);
}

static int
sweet_ufs_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return sweet_ufs_ioctl(dev, cmd, data, flag, p, 1);
}

static int
sweet_ufs_psize(__unused dev_t dev)
{
	return su.ready ? (int)MIN(su.nblocks * UFS_BLOCK / su.secsize, INT_MAX) : -1;
}

/*
 * Raw (character device) I/O: through a kernel buffer with uiomove, so the
 * user (or kernel) address in the uio is only touched by copyin/copyout.
 */
#define RAW_CHUNK       (128u << 10)

static int
sweet_ufs_rw(struct uio *uio, bool rd)
{
	uint64_t part_bytes = su.nblocks * UFS_BLOCK;
	int error = 0;

	if (!su.ready) {
		return ENXIO;
	}
	if (!rd && !su.writable) {
		return EROFS;
	}
	if (uio_offset(uio) < 0 || (uio_offset(uio) % su.secsize) || (uio_resid(uio) % su.secsize)) {
		return EINVAL;
	}
	char *kbuf = kalloc_data(RAW_CHUNK, Z_WAITOK);
	if (kbuf == NULL) {
		return ENOMEM;
	}
	while (uio_resid(uio) > 0) {
		uint64_t off = (uint64_t)uio_offset(uio);
		if (off >= part_bytes) {
			error = rd ? 0 : ENOSPC;
			break;
		}
		uint32_t n = (uint32_t)MIN(MIN((uint64_t)uio_resid(uio), (uint64_t)RAW_CHUNK), part_bytes - off);
		uint32_t done = 0;
		if (rd) {
			error = sweet_ufs_xfer(off, n, kbuf, true, &done);
			if (done) {
				int e2 = uiomove(kbuf, (int)done, uio);
				error = error ? error : e2;
			}
		} else {
			error = uiomove(kbuf, (int)n, uio);
			if (error) {
				break;
			}
			error = sweet_ufs_xfer(off, n, kbuf, false, &done);
			if (done < n) {
				/* Give back what was not written. */
				uio_setresid(uio, uio_resid(uio) + (n - done));
				uio_setoffset(uio, (off_t)(off + done));
			}
		}
		if (error) {
			break;
		}
	}
	kfree_data(kbuf, RAW_CHUNK);
	return error;
}

static int
sweet_ufs_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return sweet_ufs_rw(uio, true);
}

static int
sweet_ufs_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return sweet_ufs_rw(uio, false);
}

static const struct bdevsw sweet_ufs_bdevsw = {
	.d_open     = sweet_ufs_open,
	.d_close    = sweet_ufs_close,
	.d_strategy = sweet_ufs_strategy,
	.d_ioctl    = sweet_ufs_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = sweet_ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw sweet_ufs_cdevsw = {
	.d_open       = sweet_ufs_open,
	.d_close      = sweet_ufs_close,
	.d_read       = sweet_ufs_read,
	.d_write      = sweet_ufs_write,
	.d_ioctl      = sweet_ufs_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static int
sweet_ufs_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	char want[64] = "userdata";
	int error;

	if (SecureDTLookupEntry(NULL, "/sweet-ufs", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("SWEET UFS: no /sweet-ufs node in the device tree\n");
		return ENXIO;
	}
	PE_parse_boot_argn("ufsroot", want, sizeof(want));
	uint32_t rw = 0;
	PE_parse_boot_argn("ufsrw", &rw, sizeof(rw));
	uint32_t cache = 0, wait = 0;
	PE_parse_boot_argn("ufscache", &cache, sizeof(cache));
	PE_parse_boot_argn("ufswait", &wait, sizeof(wait));
	su.cache = (cache == 1);
	su.wait = (wait == 1);

	su.regs = ml_io_map(reg[0], reg[1]);
	if (kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET UFS: no contiguous DMA memory\n");
		return ENOMEM;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)su.dma);
	if (!pn) {
		return ENOMEM;
	}
	su.dma_pa = ptoa_64(pn);
	su.lun = 0;
	su.secsize = DEV_BSIZE;

	uint32_t rec = 0;
	PE_parse_boot_argn("sweetufsrec", &rec, sizeof(rec));
	sweet_ufs_recover = rec <= 3 ? (int)rec : 0;

	lck_mtx_lock(&sweet_ufs_lock);
	error = ufs_take_over_host();
	if (!error && sweet_ufs_recover >= 3) {
		ufs_pm_snapshot();              /* 1069: the link as ABL made it, before anything can go wrong */
	}
	if (!error) {
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention. */
		for (int i = 0; i < 3 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
		}
	}
	if (!error) {
		error = ufs_find_partition(want);
	}
	if (!error && ufs_read_blocks(su.first_block, 1) == 0) {
		su.vol_blocks = ufs_hfs_blocks((const uint8_t *)(su.dma + DMA_BOUNCE));
	}
	lck_mtx_unlock(&sweet_ufs_lock);
	if (error) {
		return error;
	}

	su.bmajor = bdevsw_add(-1, &sweet_ufs_bdevsw);
	if (su.bmajor < 0) {
		return ENXIO;
	}
	su.cmajor = cdevsw_add_with_bdev(-1, &sweet_ufs_cdevsw, su.bmajor);
	if (su.cmajor < 0) {
		return ENXIO;
	}
	su.bdev = makedev(su.bmajor, 0);
	devfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
	devfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
	/* Only now, with the partition bounds known, can writes be allowed. */
	su.writable = (rw != 0);
	su.ready = true;
	printf("SWEET UFS: \"%s\" on LUN %u: first block 0x%llx, %llu MiB, %s, DMA at 0x%llx\n",
	    want, su.lun, su.first_block, su.nblocks * UFS_BLOCK >> 20,
	    su.writable ? "read-write" : "read-only", su.dma_pa);
	printf("SWEET UFS GUARD: mode %d, HFS volume %llu of %llu blocks, tail %u blocks\n",
	    sweet_ufs_guard, su.vol_blocks, su.nblocks, UFS_TAIL_BLOCKS);
	sweet_log_start();
	sweet_sample_start();
	sweet_cpufreq_init();
	sweet_pmem_init();       /* /dev/pmem: bsd/dev/sweet_pmem.c */
	flame_wlan_init();       /* /dev/wlan, en0 + Apple80211 for wifid: bsd/dev/flame_wlan.c */
	sweet_gpu_init();        /* Adreno 640 (sweetgpu=1): bsd/dev/sweet_gpu.c */
	/* Input drivers (HID services in iokit/Kernel/arm/hid/SweetHID.cpp). */
	sweet_keys_init();
	sweet_touch_init();
	sweet_motion_init();	/* redfin sensors: /dev/redwood-motion, poke m (bsd/dev/sweet_motion.c) */
	sweet_fts_init();
	sweet_sects_init();	/* redfin sec_ts (sectouch=1) */
	sweet_haptic_init();	/* redfin DRV2624 (sweethaptic=1) */
	sweet_torch_init();	/* redfin flash LED (sweettorch=2): bsd/dev/sweet_torch.c */
	sweet_cam_init();	/* redfin camera sensors (sweetcam=1/2): bsd/dev/sweet_cam.c */
	sweet_audio_init();	/* /dev/redwood-audio, /dev/redwood-haptic: bsd/dev/sweet_audio.c */
	redwood_adsp_init();	/* the audio DSP (adsp=1): bsd/dev/sweet_adsp.c */
	sweet_geniuart_init();	/* /dev/btuart, the Bluetooth UART (btuart=1): bsd/dev/sweet_geniuart.c */
	sweet_btbridge_init();	/* /dev/cu.bluetooth etc. <-> btd (btbridge=1): bsd/dev/sweet_btbridge.c */
	sweet_abp_init();	/* 1061: AppleBasebandPCI stand-in + /dev/abpbridge (sweetabp=1|2): bsd/dev/sweet_abp.c */
	sweet_usb_probe();
	return 0;
}

/*
 * Persistent console log ("sweetlog"). Filming the phone screen is the only
 * other way to see the console, so every character that reaches the
 * console (kernel printf and /dev/console writes of daemons, both through
 * _cnputs) is also kept in a 4 MiB ring, and once the partition is known
 * and writable a thread copies new ring blocks to the LAST 4 MiB + 4 KiB of
 * the partition every ufslogsecs seconds (default 10), far behind the HFS
 * image at its start. Device block 0 of that area is a header:
 *   "SWEETLOG" | u32 version 1 | u32 ring size | u64 total bytes written
 *   | u64 flush sequence
 * ring offset of byte n = n % ring size. Read it from a recovery with
 * tools/sweet-log-pull.sh. Disabled with boot-arg sweetlog=0.
 */
#define SWEET_LOG_SIZE          (4u << 20)
#define SWEET_LOG_BLOCKS        (1 + SWEET_LOG_SIZE / UFS_BLOCK)
#define SWEET_LOG_SECS          10u

static char sweet_log_ring[SWEET_LOG_SIZE];
static uint64_t _Atomic sweet_log_total;
static int sweet_log_chan;
static uint32_t sweet_log_secs = SWEET_LOG_SECS;

/*
 * Uncached mirror of the ring for crash rescue (shim/ramlog.c): DRAM keeps
 * its contents across the warm reset into fastboot, but the cached ring
 * itself loses whatever was still dirty in the CPU caches. From
 * sweet_log_mirror_start() on (once ml_io_map works), every byte also goes
 * to a write-combining mirror at the top of RAM, above XNU's range:
 *   +0      "SWEETMIR" | u64 total | u32 ring size
 *   +0x1000 ring bytes (offset of byte n = n % ring size)
 * The next shim boot copies its tail to UFS (tools/flame-log-pull.sh rescue).
 *
 * redfin (1033): XNU now takes 0x100000000-0x27ec00000 (all of the 6 GiB bank
 * above 4 GiB, mkadt.py HIGH_MEM_SIZE), so the sunfish address 0x1fa000000 is
 * XNU-managed RAM: the mirror moves to 0x27f200000 (pram 0x27f000000, shim
 * mailbox 0x27f800000; 0x27fc00000-0x280000000 is left untouched).
 */
#define SWEET_MIRROR_PA         0x27f200000ULL
struct sweet_mirror_header {
	char magic[8];
	uint64_t total;
	uint32_t size;
};
static volatile struct sweet_mirror_header *sweet_mirror_hdr;
static volatile char *sweet_mirror_ring;

static void
sweet_log_put(const char *c, int size)
{
	uint64_t at = os_atomic_add_orig(&sweet_log_total, (uint64_t)size, relaxed);
	for (int i = 0; i < size; i++) {
		sweet_log_ring[(at + (uint64_t)i) % SWEET_LOG_SIZE] = c[i];
	}
	if (sweet_mirror_ring) {
		for (int i = 0; i < size; i++) {
			sweet_mirror_ring[(at + (uint64_t)i) % SWEET_LOG_SIZE] = c[i];
		}
		uint64_t end = at + (uint64_t)size;
		if (end > sweet_mirror_hdr->total) {
			sweet_mirror_hdr->total = end;
		}
	}
}

void sweet_log_mirror_start(void);

void
sweet_log_mirror_start(void)
{
	extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
	vm_offset_t m = ml_io_map_wcomb((vm_offset_t)SWEET_MIRROR_PA, 0x1000 + SWEET_LOG_SIZE);
	if (!m) {
		return;
	}
	volatile struct sweet_mirror_header *h = (volatile struct sweet_mirror_header *)m;
	volatile char *r = (volatile char *)(m + 0x1000);
	uint64_t total = os_atomic_load(&sweet_log_total, relaxed);
	for (uint64_t i = total > SWEET_LOG_SIZE ? total - SWEET_LOG_SIZE : 0; i < total; i++) {
		r[i % SWEET_LOG_SIZE] = sweet_log_ring[i % SWEET_LOG_SIZE];
	}
	h->total = total;
	h->size = SWEET_LOG_SIZE;
	const char *magic = "SWEETMIR";
	for (int i = 0; i < 8; i++) {
		h->magic[i] = magic[i];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	sweet_mirror_hdr = h;
	sweet_mirror_ring = r;
	printf("SWEET LOG: uncached mirror at 0x%llx for crash rescue\n", SWEET_MIRROR_PA);
}

/*
 * Called for all console output, any context: never blocks. Each line in
 * the ring (not on screen) starts with "[sss.mmm] ", seconds since the
 * timer started (flame: 19.2 MHz). No boot-arg: this runs from the first
 * console byte, before boot-args can be read.
 * Concurrent writers can interleave mid-line, as on the console itself.
 */
void sweet_log_append(const char *c, int size);
static bool sweet_log_sol = true;

void
sweet_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	int start = 0;
	for (int i = 0; i < size; i++) {
		if (sweet_log_sol) {
			if (i > start) {
				sweet_log_put(c + start, i - start);
			}
			char ts[24];
			uint64_t ms = mach_absolute_time() / 19200;
			int n = snprintf(ts, sizeof(ts), "[%4llu.%03llu] ", ms / 1000, ms % 1000);
			sweet_log_put(ts, n);
			start = i;
			sweet_log_sol = false;
		}
		if (c[i] == '\n') {
			sweet_log_sol = true;
		}
	}
	if (size > start) {
		sweet_log_put(c + start, size - start);
	}
}

/*
 * Copy console bytes [*pos, total) (at most max) for live log streaming
 * (sweet_usb.c). A reader that fell more than the ring behind skips ahead.
 * Returns the byte count; *pos advances.
 */
uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);

uint32_t
sweet_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint64_t total = os_atomic_load(&sweet_log_total, relaxed);
	if (total - *pos > SWEET_LOG_SIZE) {
		*pos = total - SWEET_LOG_SIZE;
	}
	uint32_t n = (uint32_t)MIN((uint64_t)max, total - *pos);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = sweet_log_ring[(*pos + i) % SWEET_LOG_SIZE];
	}
	*pos += n;
	return n;
}

uint64_t sweet_log_position(void);

uint64_t
sweet_log_position(void)
{
	return os_atomic_load(&sweet_log_total, relaxed);
}

struct sweet_log_header {
	char magic[8];
	uint32_t version, size;
	uint64_t total, seq;
};

static void
sweet_log_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t base = su.first_block + su.nblocks - SWEET_LOG_BLOCKS;
	uint64_t flushed = 0, seq = 0;
	void *bounce = (void *)(su.dma + DMA_BOUNCE);

	printf("SWEET LOG: console log to the last %u KiB of the partition (block 0x%llx)\n",
	    SWEET_LOG_BLOCKS * UFS_BLOCK >> 10, base);
	for (;;) {
		(void)tsleep(&sweet_log_chan, PZERO, "sweetlog", hz * (int)sweet_log_secs);
		uint64_t total = os_atomic_load(&sweet_log_total, relaxed);
		if (total == flushed) {
			continue;
		}
		uint64_t pos = total - flushed >= SWEET_LOG_SIZE ? total - SWEET_LOG_SIZE : flushed;
		int error = 0;

		lck_mtx_lock(&sweet_ufs_lock);
		while (pos < total && !error) {
			uint32_t off = (uint32_t)(pos % SWEET_LOG_SIZE);
			uint32_t blk = off / UFS_BLOCK;
			uint64_t need = ((uint64_t)off % UFS_BLOCK + (total - pos) + UFS_BLOCK - 1) / UFS_BLOCK;
			uint32_t n = (uint32_t)MIN(MIN(need, (uint64_t)(SWEET_LOG_SIZE / UFS_BLOCK - blk)),
			    (uint64_t)(BOUNCE_SIZE / UFS_BLOCK));
			memcpy(bounce, sweet_log_ring + (uint64_t)blk * UFS_BLOCK, n * UFS_BLOCK);
			error = ufs_write_blocks(base + 1 + blk, n, UFS_W_LOG);
			pos += (uint64_t)(blk + n) * UFS_BLOCK - off;
		}
		if (!error) {
			struct sweet_log_header h = {
				.magic = { 'S', 'W', 'E', 'E', 'T', 'L', 'O', 'G' },
				.version = 1, .size = SWEET_LOG_SIZE, .total = total, .seq = ++seq,
			};
			bzero(bounce, UFS_BLOCK);
			memcpy(bounce, &h, sizeof(h));
			error = ufs_write_blocks(base, 1, UFS_W_LOG);
		}
		if (!error) {
			error = ufs_sync_cache();
		}
		lck_mtx_unlock(&sweet_ufs_lock);
		if (error) {
			printf("SWEET LOG: write failed (%d), stopping\n", error);
			return;
		}
		flushed = total;
	}
}

static void
sweet_log_start(void)
{
	uint32_t on = 1;
	thread_t thread;

	PE_parse_boot_argn("sweetlog", &on, sizeof(on));
	if (!on || !su.writable || su.nblocks < 2 * SWEET_LOG_BLOCKS) {
		return;
	}
	PE_parse_boot_argn("ufslogsecs", &sweet_log_secs, sizeof(sweet_log_secs));
	if (sweet_log_secs == 0 || sweet_log_secs > 60) {
		sweet_log_secs = SWEET_LOG_SECS;
	}
	if (kernel_thread_start(sweet_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* Root device for rd=ufsroot0 (IOFindBSDRoot). */
int sweet_ufs_rootdev(dev_t *dev);

int
sweet_ufs_rootdev(dev_t *dev)
{
	if (!su.ready) {
		int error = sweet_ufs_init();
		if (error) {
			return error;
		}
	}
	*dev = su.bdev;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
