/*
 * 1190 redfin (Pixel 5) fingerprint sensor (FPC1028, "fpc,fpc1020"): kernel control of its two GPIOs, nothing else.
 * The sensor's SPI bus (QUP0 SE1 0x884000, gpio0-3) belongs to TrustZone: the stock FPC TA drives it, and this file
 * never touches it. The Linux driver (redbull drivers/input/misc/fpc_fingerprint/fpc1020_platform_tee.c) only resets
 * the sensor and forwards its interrupt line to the HAL, and so does this one.
 *
 *   reset = TLMM gpio16 (EAST tile, active low; pinctrl fpc_reset_low/high)
 *   irq   = TLMM gpio112 (WEST tile, input, bias pull-down; high while a sensor event is pending)
 *
 * kern.sweet_fpc (RW, bit values 1 and 2, default 0 = untouched: gpio16 and gpio112 stay exactly as the bootloader left them and
 * nothing is logged):
 *   1  hw_reset as the stock driver: reset high 100 us, low 5 ms, high 5 ms (left released = high), gpio112
 *          as input pull-down; logs "SWEET FPC reset done irq=<gpio112 level>" (stock: "IRQ after reset 1").
 *          Runs on every write that has value 1 set.
 *   2  gpio112 rising-edge interrupt (stock: IRQF_TRIGGER_RISING) on the TLMM summary interrupt (INTID 240,
 *          owned by sec_ts touch, which hands other pins' status to sweet_fpc_tlmm_irq()). Counts events in
 *          kern.sweet_fpc_irq (RO) and logs each "SWEET FPC irq n=N t=US level=L" (at most 8 lines per wakeup).
 *          Clearing value 2 restores gpio112's boot INTR_CFG.
 * kern.sweet_fpc_wait (RW int): write N ms (1..60000) = block until gpio112 is high (the level, like the HAL's
 *   fpc_irq_wait on the sysfs "irq" attribute) or N ms pass; the same call reads back 1 (high) or 0 (timeout).
 *   Woken by the interrupt when value 2 is on and the summary interrupt is live; otherwise polls every 5 ms.
 * kern.sweet_fpc_state (RO string): "rst=gpio16 out=O oe=E cfg=C irq=gpio112 in=I cfg=C intr=C irqs=N mode=M
 *   storms=S masked=0|1" (1193: storm guard, see FPC_STORM_N).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);

#define FPC_RST_PA      (0x0f900000u + 16u * 0x1000u)   /* TLMM EAST + gpio16 (pinctrl-lito.c) */
#define FPC_IRQ_PA      (0x0f500000u + 112u * 0x1000u)  /* TLMM WEST + gpio112 */
#define TLMM_CFG        0x0u    /* [1:0] pull (1 = down), [5:2] function, [8:6] drive, [9] output enable */
#define TLMM_IO         0x4u    /* [0] in, [1] out */
#define TLMM_INTR_CFG   0x8u    /* [0] enable, [1] polarity (1 = high), [3:2] detection (1 = rising), [4] raw, [7:5] target */
#define TLMM_INTR_ST    0xcu    /* [0] status, write 0 to clear */
#define FPC_INTR_ON     ((3u << 5) | (1u << 4) | (1u << 2) | (1u << 1) | 1u)  /* apps, raw, rising edge, enabled */
/*
 * 1193: gpio112 is edge-triggered (rising), as the stock fpc1020_platform_tee driver asks (IRQF_TRIGGER_RISING |
 * IRQF_ONESHOT) and as Linux pinctrl-msm programs it on lito (detection [3:2] = 1, polarity 1, raw status, KPSS
 * target): FPC_INTR_ON above is exactly that. Storm guard: more than FPC_STORM_N interrupts within FPC_STORM_MS
 * masks gpio112 (INTR_CFG enable bit off, status cleared) from the interrupt handler itself, logs
 * "SWEET FPC irq storm, masked", and only the next kern.sweet_fpc_wait call unmasks it. The shared touch handler
 * (sweet_sects.c st_irq) still services touch first; with gpio112 masked the summary line no longer carries it.
 */
#define FPC_STORM_N     100u
#define FPC_STORM_MS    100u

static LCK_GRP_DECLARE(fpc_grp, "sweet-fpc");
static LCK_MTX_DECLARE(fpc_lock, &fpc_grp);
static vm_offset_t fpc_rst, fpc_irqp;   /* the two pins' register pages */
static int fpc_mode;                    /* kern.sweet_fpc */
static uint32_t fpc_boot_intr;          /* gpio112 INTR_CFG at the first bit-2 write */
static bool fpc_boot_saved, fpc_thread_on;
static volatile bool fpc_irq_on;        /* read by sweet_fpc_tlmm_irq in interrupt context */
static volatile uint32_t fpc_irqs, fpc_logged;
static volatile bool fpc_storm_masked;          /* 1193: set by the handler, cleared by kern.sweet_fpc_wait */
static volatile uint32_t fpc_storms, fpc_storms_logged, fpc_win_n;
static volatile uint64_t fpc_win_t;             /* start of the current FPC_STORM_MS window (mach time) */
static uint64_t fpc_storm_abs;                  /* FPC_STORM_MS in mach time units, set when the irq is enabled */
static volatile uint64_t fpc_irq_t[8];  /* mach time of event n, at [n & 7] */
static int fpc_wait_chan, fpc_log_chan;
static int fpc_wait_last;

static uint32_t
fpc_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
fpc_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* Under fpc_lock. */
static void
fpc_map(void)
{
	if (!fpc_rst) {
		fpc_rst = ml_io_map(FPC_RST_PA, 0x1000);
		fpc_irqp = ml_io_map(FPC_IRQ_PA, 0x1000);
	}
}

static unsigned
fpc_level(void)
{
	return fpc_irqp ? (fpc_rd(fpc_irqp, TLMM_IO) & 1u) : 0u;
}

static void
fpc_rst_set(bool high)
{
	fpc_wr(fpc_rst, TLMM_IO, (fpc_rd(fpc_rst, TLMM_IO) & ~2u) | (high ? 2u : 0u));
}

/* Stock hw_reset(): fpc1020_reset_active, 100 us, _reset, 5 ms, _active, 5 ms, read the irq line. */
static void
fpc_reset(void)
{
	uint32_t rcfg = fpc_rd(fpc_rst, TLMM_CFG), icfg = fpc_rd(fpc_irqp, TLMM_CFG);
	/* gpio112: function 0 (GPIO), input, pull-down (pinctrl fpc_irq_default), as the stock probe selects it */
	fpc_wr(fpc_irqp, TLMM_CFG, (icfg & ~0x3ffu) | 1u);
	/* gpio16: function 0, output, 2 mA, no pull; out level set before the output is enabled */
	fpc_rst_set(true);
	fpc_wr(fpc_rst, TLMM_CFG, (rcfg & ~0x3ffu) | 0x200u);
	delay(100);
	fpc_rst_set(false);
	delay(5000);
	fpc_rst_set(true);
	delay(5000);
	printf("SWEET FPC reset done irq=%u (gpio16 cfg %#x->%#x, gpio112 cfg %#x->%#x)\n", fpc_level(),
	    rcfg, fpc_rd(fpc_rst, TLMM_CFG), icfg, fpc_rd(fpc_irqp, TLMM_CFG));
}

/*
 * From sec_ts's TLMM summary handler (bsd/dev/sweet_sects.c, interrupt context) when its own pin is not the one
 * pending: true if gpio112's event was ours (status cleared).
 */
bool sweet_fpc_tlmm_irq(void);
bool
sweet_fpc_tlmm_irq(void)
{
	if (!fpc_irq_on || !fpc_irqp || !(fpc_rd(fpc_irqp, TLMM_INTR_ST) & 1u)) {
		return false;
	}
	fpc_wr(fpc_irqp, TLMM_INTR_ST, 0);
	uint64_t now = mach_absolute_time();
	uint32_t n = fpc_irqs + 1u;
	fpc_irq_t[n & 7u] = now;
	fpc_irqs = n;
	/* 1193: storm guard (interrupt context: register writes and counters only) */
	if (fpc_win_t == 0 || now - fpc_win_t > fpc_storm_abs) {
		fpc_win_t = now;
		fpc_win_n = 0;
	}
	if (++fpc_win_n > FPC_STORM_N) {
		fpc_wr(fpc_irqp, TLMM_INTR_CFG, FPC_INTR_ON & ~1u);
		fpc_wr(fpc_irqp, TLMM_INTR_ST, 0);
		fpc_storm_masked = true;
		fpc_storms++;
		fpc_win_t = 0;
	}
	thread_wakeup(&fpc_wait_chan);
	thread_wakeup(&fpc_log_chan);
	return true;
}

/* 1193: kern.sweet_fpc_wait unmasks gpio112 after a storm (under fpc_lock). */
static void
fpc_storm_unmask(void)
{
	if (fpc_storm_masked && fpc_irq_on && fpc_irqp) {
		fpc_win_t = 0;
		fpc_win_n = 0;
		fpc_storm_masked = false;
		fpc_wr(fpc_irqp, TLMM_INTR_ST, 0);
		fpc_wr(fpc_irqp, TLMM_INTR_CFG, FPC_INTR_ON);
		printf("SWEET FPC irq unmasked by kern.sweet_fpc_wait (storms %u) level=%u\n", fpc_storms, fpc_level());
	}
}

static void
fpc_log_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		(void)assert_wait(&fpc_log_chan, THREAD_UNINT);
		if (fpc_logged == fpc_irqs) {
			thread_block(THREAD_CONTINUE_NULL);
		} else {
			clear_wait(current_thread(), THREAD_AWAKENED);
		}
		if (fpc_storms_logged != fpc_storms) {
			fpc_storms_logged = fpc_storms;
			printf("SWEET FPC irq storm, masked (> %u interrupts in %u ms; storms %u, irqs %u, level=%u); "
			    "kern.sweet_fpc_wait unmasks\n", FPC_STORM_N, FPC_STORM_MS, fpc_storms, fpc_irqs, fpc_level());
		}
		uint32_t now = fpc_irqs, lines = 0;
		while (fpc_logged != now && lines < 8u) {
			uint32_t n = ++fpc_logged;
			uint64_t ns;
			absolutetime_to_nanoseconds(fpc_irq_t[n & 7u], &ns);
			printf("SWEET FPC irq n=%u t=%llu us level=%u\n", n, ns / 1000u, fpc_level());
			lines++;
		}
		if (fpc_logged != now) {
			printf("SWEET FPC irq n=%u (%u more not logged)\n", now, now - fpc_logged);
			fpc_logged = now;
		}
	}
}

/* Under fpc_lock. */
static void
fpc_irq_enable(bool on)
{
	if (on) {
		if (!fpc_boot_saved) {
			fpc_boot_intr = fpc_rd(fpc_irqp, TLMM_INTR_CFG);
			fpc_boot_saved = true;
		}
		if (!fpc_thread_on) {
			thread_t t;
			if (kernel_thread_start(fpc_log_thread, NULL, &t) == KERN_SUCCESS) {
				thread_deallocate(t);
				fpc_thread_on = true;
			}
		}
		fpc_wr(fpc_irqp, TLMM_INTR_CFG, FPC_INTR_ON & ~1u);
		fpc_wr(fpc_irqp, TLMM_INTR_ST, 0);
		nanoseconds_to_absolutetime((uint64_t)FPC_STORM_MS * NSEC_PER_MSEC, &fpc_storm_abs);
		fpc_win_t = 0;
		fpc_win_n = 0;
		fpc_storm_masked = false;
		fpc_irq_on = true;
		fpc_wr(fpc_irqp, TLMM_INTR_CFG, FPC_INTR_ON);
		printf("SWEET FPC irq on (gpio112 rising, INTR_CFG %#x, boot %#x) level=%u\n",
		    fpc_rd(fpc_irqp, TLMM_INTR_CFG), fpc_boot_intr, fpc_level());
	} else if (fpc_boot_saved) {
		fpc_wr(fpc_irqp, TLMM_INTR_CFG, fpc_boot_intr & ~1u);
		fpc_wr(fpc_irqp, TLMM_INTR_ST, 0);
		fpc_irq_on = false;
		fpc_storm_masked = false;
		fpc_wr(fpc_irqp, TLMM_INTR_CFG, fpc_boot_intr);
		printf("SWEET FPC irq off (INTR_CFG %#x) irqs=%u\n", fpc_rd(fpc_irqp, TLMM_INTR_CFG), fpc_irqs);
	}
}

static int
sysctl_sweet_fpc SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, fpc_mode, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value & ~3) {
		return EINVAL;
	}
	lck_mtx_lock(&fpc_lock);
	fpc_map();
	if (!fpc_rst || !fpc_irqp) {
		lck_mtx_unlock(&fpc_lock);
		return ENXIO;
	}
	if (value & 1) {
		fpc_reset();
	}
	if ((value & 2) != (fpc_mode & 2)) {
		fpc_irq_enable(value & 2);
	}
	fpc_mode = value;
	lck_mtx_unlock(&fpc_lock);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_fpc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_fpc, "I", "1190 FPC fingerprint GPIOs: 1 reset, 2 irq, 3 both (0 = untouched)");

SYSCTL_UINT(_kern, OID_AUTO, sweet_fpc_irq, CTLFLAG_RD | CTLFLAG_LOCKED, (uint32_t *)(uintptr_t)&fpc_irqs, 0,
    "1190 FPC gpio112 interrupts");

static int
sysctl_sweet_fpc_wait SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int ms = 0;
	if (req->newptr == USER_ADDR_NULL) {
		return SYSCTL_OUT(req, &fpc_wait_last, sizeof(fpc_wait_last));
	}
	int error = SYSCTL_IN(req, &ms, sizeof(ms));
	if (error) {
		return error;
	}
	if (ms < 1 || ms > 60000) {
		return EINVAL;
	}
	lck_mtx_lock(&fpc_lock);
	fpc_map();
	fpc_storm_unmask();     /* 1193 */
	lck_mtx_unlock(&fpc_lock);
	if (!fpc_irqp) {
		return ENXIO;
	}
	uint64_t deadline, now;
	clock_interval_to_deadline((uint32_t)ms, NSEC_PER_MSEC, &deadline);
	int result = 0;
	for (;;) {
		if (fpc_level()) {
			result = 1;
			break;
		}
		now = mach_absolute_time();
		if (now >= deadline) {
			break;
		}
		uint64_t step;
		clock_interval_to_deadline(5, NSEC_PER_MSEC, &step);
		(void)assert_wait_deadline(&fpc_wait_chan, THREAD_ABORTSAFE, step < deadline ? step : deadline);
		if (fpc_level()) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			result = 1;
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_INTERRUPTED) {
			return EINTR;
		}
	}
	fpc_wait_last = result;
	return SYSCTL_OUT(req, &result, sizeof(result));
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_fpc_wait, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_fpc_wait, "I", "1190 FPC: write N ms = wait for gpio112 high; reads 1 high / 0 timeout");

static int
sysctl_sweet_fpc_state SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char s[200];
	lck_mtx_lock(&fpc_lock);
	fpc_map();
	if (!fpc_rst || !fpc_irqp) {
		lck_mtx_unlock(&fpc_lock);
		return ENXIO;
	}
	uint32_t rcfg = fpc_rd(fpc_rst, TLMM_CFG), rio = fpc_rd(fpc_rst, TLMM_IO);
	uint32_t icfg = fpc_rd(fpc_irqp, TLMM_CFG), iio = fpc_rd(fpc_irqp, TLMM_IO);
	uint32_t intr = fpc_rd(fpc_irqp, TLMM_INTR_CFG);
	lck_mtx_unlock(&fpc_lock);
	snprintf(s, sizeof(s), "rst=gpio16 out=%u oe=%u cfg=%#x irq=gpio112 in=%u cfg=%#x intr=%#x irqs=%u mode=%d storms=%u masked=%d",
	    (rio >> 1) & 1u, (rcfg >> 9) & 1u, rcfg, iio & 1u, icfg, intr, fpc_irqs, fpc_mode, fpc_storms,
	    fpc_storm_masked ? 1 : 0);
	return SYSCTL_OUT(req, s, strlen(s) + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_fpc_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0,
    sysctl_sweet_fpc_state, "A", "1190 FPC: gpio16 out level, gpio112 in level, irq count");

#endif /* ARM64_BOARD_CONFIG_SWEET */
