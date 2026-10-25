/*
 * redfin (SM7250 "lito") CPU frequency: the cpufreq-hw (EPSS-less "OSM
 * firmware") block keeps a frequency table per domain and takes a desired
 * table index per domain; voltage (CPR) and the hardware thermal limiter
 * (LMh) stay in hardware. Linux: drivers/cpufreq/qcom-cpufreq-hw.c (redbull),
 * bases from the live DT qcom,cpufreq-hw node:
 *
 *   domain 0 silver CPUs 0-5 0x18323000  qcom,freq-domain <3 0 6>
 *   domain 1 gold   CPU 6    0x18325800  qcom,freq-domain <3 1 2>
 *   domain 2 prime  CPU 7    0x18327800  qcom,freq-domain <3 2 2>
 *
 * Registers: 0x000 enable (bit 0), 0x110 + 32*i LUT row (31:30 source,
 * 18:16 core count, 7:0 L, f = L * 19.2 MHz), 0x114 + 32*i voltage, 0x920
 * perf state (the index we ask for), 0x9c0 cycle counter (DT
 * qcom,no-accumulative-counter: one counter per domain).
 * LMh (limits-dcvs 0x18358800 silver, 0x18350800 gold+prime): +0x704 is the
 * hardware's current frequency ceiling (bits 9:0 * 19.2 MHz).
 *
 * Boot-arg flamecpufreq: 0 leave as is, 1 (default) highest all-core
 * frequency (the old rule: rows with fewer cores than the domain are boost
 * rows), N > 2 the highest entry at or below N MHz.
 * 0961: every 5 s (called from sweet_thermal.c) one "SWEET CPUFREQ:" line with
 * per-CPU busy %, the perf-state readback, the cycle-counter MHz and the LMh
 * ceiling; "w freq DOM IDX" writes one domain (bsd/dev/sweet_thermal.c).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t sweet_cpu_idle_abs(int cpu);    /* osfmk/kern/sched_prim.c (0961) */
void sweet_cpufreq_init(void);
void sweet_cpufreq_log(void);
int sweet_cpufreq_set(unsigned dom, unsigned idx, const char *why);
void sweet_cpufreq_table_log(unsigned dom);

#define CF_ENABLE       0x000u
#define CF_LUT          0x110u          /* + 32 * index */
#define CF_VOLT         0x114u
#define CF_ROW          32u
#define CF_ROWS         40u
#define CF_PERF         0x920u
#define CF_CYCLES       0x9c0u
#define CF_XO_KHZ       19200u
#define LMH_REQ         0x704u
#define CF_NDOM         3u
#define CF_NCPU         8u

struct cf_dom {
	const char *name;
	uint32_t pa;
	unsigned first, ncpu;
	unsigned old_cores;     /* the pre-0961 boost rule (kept for flamecpufreq=1) */
	unsigned max_cores;     /* DT qcom,freq-domain third cell, as Linux */
	uint32_t lmh_pa;
	vm_offset_t base, lmh;
	unsigned n;             /* table rows */
	uint32_t khz[CF_ROWS];
	uint8_t cc[CF_ROWS];
	int skip;               /* Linux "skip" row (core count == max - 1, last one), -1 none */
	unsigned boot_idx;      /* what ABL left */
	unsigned req;           /* last index written (or boot_idx) */
	uint32_t cyc_prev;
	uint64_t cyc_t;
};

static struct cf_dom cf[CF_NDOM] = {
	{ .name = "silver", .pa = 0x18323000u, .first = 0, .ncpu = 6, .old_cores = 6, .max_cores = 6,
	  .lmh_pa = 0x18358800u + LMH_REQ },
	{ .name = "gold", .pa = 0x18325800u, .first = 6, .ncpu = 1, .old_cores = 2, .max_cores = 2,
	  .lmh_pa = 0x18350800u + LMH_REQ },
	/* the 1-core prime domain: the old rule treated every row as normal */
	{ .name = "prime", .pa = 0x18327800u, .first = 7, .ncpu = 1, .old_cores = 1, .max_cores = 2,
	  .lmh_pa = 0x18350800u + LMH_REQ },
};
static bool cf_ready;

static LCK_GRP_DECLARE(cf_grp, "sweet_cpufreq");
static LCK_MTX_DECLARE(cf_lock, &cf_grp);

static uint32_t
cf_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

/* Linux qcom_cpufreq_hw_read_lut(): valid rows have core count == max or max - 1. */
static bool
cf_valid(const struct cf_dom *d, unsigned i)
{
	return i < d->n && (d->cc[i] == d->max_cores || d->cc[i] + 1u == d->max_cores);
}

static void
cf_read_lut(struct cf_dom *d)
{
	uint32_t prev_khz = 0;
	unsigned prev_cc = 0;
	d->skip = -1;
	d->n = 0;
	for (unsigned i = 0; i < CF_ROWS; ++i) {
		uint32_t data = cf_rd(d->base, CF_LUT + i * CF_ROW);
		unsigned src = data >> 30, lval = data & 0xffu, cores = (data >> 16) & 7u;
		uint32_t khz = src ? CF_XO_KHZ * lval : 300000u;
		/* two equal frequencies with equal core counts end the table (Linux) */
		if (i > 0 && khz == prev_khz && cores == prev_cc) {
			break;
		}
		d->khz[i] = khz;
		d->cc[i] = (uint8_t)cores;
		d->n = i + 1;
		if (cores + 1u == d->max_cores) {
			d->skip = (int)i;
		}
		prev_khz = khz;
		prev_cc = cores;
	}
}

void
sweet_cpufreq_table_log(unsigned dom)
{
	if (dom >= CF_NDOM || !cf[dom].base) {
		return;
	}
	struct cf_dom *d = &cf[dom];
	char line[400];
	int len = snprintf(line, sizeof(line), "SWEET CPUFREQ: %s table (idx:MHz, ~=skip row, *=boost):", d->name);
	for (unsigned i = 0; i < d->n && len < (int)sizeof(line) - 16; ++i) {
		len += snprintf(line + len, sizeof(line) - len, " %u:%u%s", i, d->khz[i] / 1000,
		    !cf_valid(d, i) ? "*" : (int)i == d->skip ? "~" : "");
	}
	printf("%s\n", line);
}

/* Write one domain's perf state; returns the readback (or -1). Linux: writing the skip row writes the next. */
int
sweet_cpufreq_set(unsigned dom, unsigned idx, const char *why)
{
	if (!cf_ready || dom >= CF_NDOM || !cf[dom].base || idx >= cf[dom].n) {
		return -1;
	}
	struct cf_dom *d = &cf[dom];
	lck_mtx_lock(&cf_lock);
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(d->base + CF_PERF) = idx;
	__asm__ volatile ("dsb osh" ::: "memory");
	d->req = idx;
	int rb = (int)cf_rd(d->base, CF_PERF);
	lck_mtx_unlock(&cf_lock);
	if (why) {
		printf("SWEET CPUFREQ: %s index %u (%u MHz) [%s], reads back %d\n", d->name, idx,
		    d->khz[idx] / 1000, why, rb);
	}
	return rb;
}

/* Per-CPU busy % since the previous call with the same state. */
struct cf_load {
	uint64_t idle[CF_NCPU];
	uint64_t t;
};

static void
cf_busy(struct cf_load *ld, unsigned busy[CF_NCPU])
{
	uint64_t now = mach_absolute_time();
	uint64_t dt = now - ld->t;
	for (unsigned c = 0; c < CF_NCPU; ++c) {
		uint64_t idle = sweet_cpu_idle_abs((int)c);
		uint64_t di = idle - ld->idle[c];
		/* idle 0: no such CPU (cpus=N) or not started yet: not busy */
		busy[c] = (idle && ld->t && dt && di <= dt) ? (unsigned)(100 - (di * 100) / dt) : 0;
		ld->idle[c] = idle;
	}
	ld->t = now;
}

void
sweet_cpufreq_log(void)
{
	static struct cf_load ld;
	if (!cf_ready) {
		return;
	}
	unsigned busy[CF_NCPU];
	cf_busy(&ld, busy);
	char line[300];
	int len = snprintf(line, sizeof(line), "SWEET CPUFREQ: busy %u %u %u %u %u %u | %u | %u %%;",
	    busy[0], busy[1], busy[2], busy[3], busy[4], busy[5], busy[6], busy[7]);
	uint64_t now = mach_absolute_time();
	for (unsigned i = 0; i < CF_NDOM; ++i) {
		struct cf_dom *d = &cf[i];
		if (!d->base) {
			continue;
		}
		uint32_t idx = cf_rd(d->base, CF_PERF), cyc = cf_rd(d->base, CF_CYCLES);
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(now - d->cyc_t, &ns);
		unsigned cyc_mhz = (d->cyc_t && ns) ? (unsigned)((uint64_t)(uint32_t)(cyc - d->cyc_prev) * 1000 / ns) : 0;
		d->cyc_prev = cyc;
		d->cyc_t = now;
		unsigned lmh = d->lmh ? (cf_rd(d->lmh, 0) & 0x3ffu) * CF_XO_KHZ / 1000 : 0;
		len += snprintf(line + len, sizeof(line) - len, " %s %u=%u MHz (cyc %u, lmh %u)", d->name, idx,
		    idx < d->n ? d->khz[idx] / 1000 : 0, cyc_mhz, lmh);
	}
	printf("%s\n", line);
}

void
sweet_cpufreq_init(void)
{
	unsigned want = 1;
	(void)PE_parse_boot_argn("flamecpufreq", &want, sizeof(want));
	for (unsigned c = 0; c < CF_NDOM; ++c) {
		struct cf_dom *d = &cf[c];
		d->base = ml_io_map(d->pa, 0x1000);
		if (!d->base) {
			printf("SWEET CPUFREQ: %s: map failed\n", d->name);
			continue;
		}
		d->lmh = (c > 0 && d->lmh_pa == cf[c - 1].lmh_pa) ? cf[c - 1].lmh : ml_io_map(d->lmh_pa, 4);
		cf_read_lut(d);
		d->boot_idx = d->req = cf_rd(d->base, CF_PERF);
		d->cyc_prev = cf_rd(d->base, CF_CYCLES);
		d->cyc_t = mach_absolute_time();
		cf_ready = true;
		sweet_cpufreq_table_log(c);
		printf("SWEET CPUFREQ: %s enable %x perf %u volt[0] %x lmh raw %x (%u MHz) skip %d\n", d->name,
		    cf_rd(d->base, CF_ENABLE), d->boot_idx, cf_rd(d->base, CF_VOLT), d->lmh ? cf_rd(d->lmh, 0) : 0,
		    d->lmh ? (cf_rd(d->lmh, 0) & 0x3ffu) * CF_XO_KHZ / 1000 : 0, d->skip);

		/* The pre-0961 selection, unchanged: highest non-boost row (old core rule) under the cap. */
		unsigned best = d->boot_idx, best_khz = 0, old_n = d->n;
		for (unsigned i = 1; i < d->n; ++i) {
			if (d->khz[i] == d->khz[i - 1]) {
				old_n = i;      /* the old rule: any repeated frequency ends the table */
				break;
			}
		}
		for (unsigned i = 0; i < old_n; ++i) {
			bool boost = d->cc[i] < d->old_cores;
			if (boost || (want > 2 && d->khz[i] / 1000 > want)) {
				continue;
			}
			if (d->khz[i] > best_khz) {
				best_khz = d->khz[i];
				best = i;
			}
		}
		if (want == 0 || want == 2 || best_khz == 0 || best >= d->n) {
			printf("SWEET CPUFREQ: %s index %u kept\n", d->name, d->boot_idx);
			continue;
		}
		(void)sweet_cpufreq_set(c, best, want == 1 ? "boot max" : "boot cap");
	}
	extern void sweet_thermal_start(void);
	sweet_thermal_start();  /* 0960: TSENS sampling, bsd/dev/sweet_thermal.c */
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
