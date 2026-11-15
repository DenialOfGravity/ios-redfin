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
 * rows), 2 the DVFS governor (0963), N > 2 the highest entry at or below N MHz.
 * 0961: every 5 s (called from sweet_thermal.c) one "SWEET CPUFREQ:" line with
 * per-CPU busy %, the perf-state readback, the cycle-counter MHz and the LMh
 * ceiling; "w freq DOM IDX" writes one domain (bsd/dev/sweet_thermal.c).
 *
 * 0963: flamecpufreq=2 runs a DVFS governor instead of pinning: every 30 ms
 * (w gov 1 MS: 10-200) per domain, utilisation = the busiest CPU's busy share
 * of the tick; target = current frequency * util / 80 % (doubled when util
 * >= 95 %, so a saturated core ramps in a few ticks); the lowest valid row
 * at or above it (skip and boost rows are never chosen). Up at once, down only
 * after 3 ticks in a row want lower (then the highest of those wishes).
 * Floors: silver the lowest row >= 1.1 GHz, gold/prime their lowest row. Only
 * the perf-state register is written (no RPMh). "w gov 0" stops it (clocks stay
 * where they are), "w gov 1" starts it; "w freq" takes that domain out of the
 * governor until the next "w gov 1".
 *
 * 0964: sweet_cpufreq_cap() (from the thermal policy in sweet_thermal.c) puts a
 * ceiling on a domain, in every mode.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t sweet_cpu_idle_abs(int cpu);    /* osfmk/kern/sched_prim.c (0961) */
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
int sweet_cpufreq_gov(int on, unsigned period_ms);
void sweet_cpufreq_manual(unsigned dom);
int sweet_cpufreq_cap(unsigned dom, uint32_t max_khz);
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
	/* 0963 governor */
	uint8_t ch[CF_ROWS];    /* choosable rows (valid, not the skip row), ascending frequency */
	unsigned nch;
	unsigned floor_pos;     /* position in ch[] */
	unsigned down, wish;    /* consecutive "lower" ticks, highest lower wish (position) */
	bool manual;            /* "w freq" took it out of the governor */
	unsigned changes;
	/* 0964 thermal cap */
	uint32_t cap_khz;       /* 0 = none */
	bool capped;            /* not governed: req was lowered from pin */
	unsigned pin;           /* the index before the cap */
	/* 0970 big-core policy */
	uint64_t last_up;       /* when the request last went up (the down hold runs from there) */
	uint32_t at_ms[CF_ROWS]; /* time at each row in this log window */
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
static volatile int cf_gov_on;
static unsigned cf_gov_ms = 30;
static bool cf_gov_thread;
static int cf_gov_chan;

/*
 * 0970: gold and prime under the governor, when on (boot-arg sweetgov=XXYY, or "w boost 1"; default off = the
 * 0963 rule for every domain). Up at least one step when a core is >= XX % busy (default 40), proportionally
 * further; straight to the top at >= YY % (default 60). Down one step a tick, only XX-10 % or less busy and 100 ms
 * after the last step up. Boost: a touch (down or move, SweetHID) or SwapEnds less than 50 ms apart
 * (SweetFramebuffer) hold both at >= sweetboost=MHZ (default 1800, 0 = none) for sweetboostms=MS (default 200)
 * after the last one. The thermal caps (0964) stay the ceiling. "SWEET GOV:" every 5 s that saw a boost.
 */
static volatile int cf_boost_on;
static unsigned cf_up = 40, cf_jump = 60, cf_boost_ms = 200;
static uint32_t cf_boost_khz = 1800000;
static volatile uint64_t cf_boost_until;
static uint32_t cf_boosts, cf_caphits;
static uint64_t cf_log_t;
static bool cf_boost_seen;

void sweet_cpufreq_boost(int why);
extern void sweet_gpu_dcvs_boost(int why);     /* 1036: bsd/dev/sweet_gpu.c (kern.sweet_gpu_dcvs) */
void
sweet_cpufreq_boost(int why)
{
	sweet_gpu_dcvs_boost(why);      /* 1036: the GPU governor takes the same touch / frames-flowing hints */
	if (!cf_boost_on || !cf_ready || cf_boost_khz == 0) {
		return;
	}
	uint64_t now = mach_absolute_time(), d;
	nanoseconds_to_absolutetime((uint64_t)cf_boost_ms * NSEC_PER_MSEC, &d);
	bool was = now < cf_boost_until;
	cf_boost_until = now + d;
	if (!was) {
		cf_boosts++;
		thread_wakeup((event_t)&cf_gov_chan);   /* the governor acts now, not at its next tick */
	}
}

/*
 * 1037: kern.sweet_gov_framehold PCT (0..100, default 0 = off): after each SwapEnd (SweetFramebuffer swap()) the
 * governor keeps every domain at >= PCT % of its top frequency (the lowest choosable row at or above) for 300 ms,
 * on both the 0963 rule and 0970's gold/prime policy; 0964's caps stay the ceiling. UniXNU sheng-cpufreq=4 holds
 * >= 60 %. Counted in the SWEET GOV 5s line as "hold N" (ticks with the hold raising a domain).
 */
static int cf_framehold;
static volatile uint64_t cf_frame_until;
static uint32_t cf_hold_ticks;

void sweet_cpufreq_frame(void);
void
sweet_cpufreq_frame(void)
{
	if (cf_framehold <= 0 || !cf_ready) {
		return;
	}
	uint64_t now = mach_absolute_time(), d;
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &d);
	bool was = now < cf_frame_until;
	cf_frame_until = now + d;
	if (!was) {
		thread_wakeup((event_t)&cf_gov_chan);   /* up now, not at the next 30 ms tick */
	}
}

static int
sysctl_sweet_gov_framehold SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, cf_framehold, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 100) {
		return EINVAL;
	}
	cf_framehold = value;
	cf_frame_until = 0;
	printf("SWEET GOV: frame hold %d %% of top for 300 ms after each SwapEnd%s\n", value,
	    cf_gov_on ? "" : " (the governor is off: flamecpufreq=2 / w gov 1)");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_gov_framehold, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_gov_framehold, "I", "SWEET GOV: % of each domain's top held 300 ms after a SwapEnd (0 off)");

/*
 * 1041: kern.sweet_ui_bigcore MASK (osfmk/kern/sched_prim.c, choose_processor; 1046: default 1, was 0): 1 threads named
 * com.apple.coreanimation.display* (backboardd's CA display thread), 2 com.apple.coreanimation.render-server*: an idle
 * big core (CPU6/7, sweetbig order), else preempt the big core running the lowest priority below theirs, else the
 * normal choice (never waits). 1121: 32 also tags threads named hakios.metal.defer* (hakios-metal HM_DEFER) as display class. Line SWEET UI: bigcore 5s every 5 s while tagged threads run (also with 0: baseline).
 */
extern int sweet_ui_bigcore;
static int
sysctl_sweet_ui_bigcore SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, sweet_ui_bigcore, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 63) {
		return EINVAL;
	}
	sweet_ui_bigcore = value;
	printf("SWEET UI: bigcore %d (1041: 1 CA display threads, 2 render-server threads on CPU6/7: idle, else preempt lower; "
	    "1106: 4 display gold / render-server prime first, 8 no preempting the other class, 16 preempt equal priority; "
	    "1121: 32 hakios.metal.defer* threads named from now on are display class)\n",
	    value);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_ui_bigcore, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_ui_bigcore, "I", "SWEET UI: CA threads on the big cores (1 display, 2 render-server; 0 off; 1106: 4 split, 8 no cross, 16 equal pri; 1121: 32 hakios.metal.defer* as display)");

/* 1037: the lowest choosable position at or above PCT % of the top (0 = no hold) */
static unsigned
cf_hold_pos(const struct cf_dom *d, int pct)
{
	if (pct <= 0 || d->nch == 0) {
		return 0;
	}
	uint64_t want = (uint64_t)d->khz[d->ch[d->nch - 1]] * (unsigned)pct / 100;
	unsigned p = 0;
	while (p + 1 < d->nch && d->khz[d->ch[p]] < want) {
		p++;
	}
	return p;
}

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

/* Choosable rows sorted by frequency (normally the LUT is already ascending). */
static void
cf_build_choices(struct cf_dom *d)
{
	d->nch = 0;
	for (unsigned i = 0; i < d->n; ++i) {
		if (!cf_valid(d, i) || (int)i == d->skip) {
			continue;
		}
		unsigned j = d->nch++;
		while (j > 0 && d->khz[d->ch[j - 1]] > d->khz[i]) {
			d->ch[j] = d->ch[j - 1];
			j--;
		}
		d->ch[j] = (uint8_t)i;
	}
	uint32_t floor_khz = d->first == 0 ? 1100000u : 0;
	d->floor_pos = 0;
	while (d->floor_pos + 1 < d->nch && d->khz[d->ch[d->floor_pos]] < floor_khz) {
		d->floor_pos++;
	}
}

/* Highest position the governor may pick (0964: at or below the thermal cap). */
static unsigned
cf_ceiling(const struct cf_dom *d)
{
	unsigned c = d->nch ? d->nch - 1 : 0;
	while (c > 0 && d->cap_khz && d->khz[d->ch[c]] > d->cap_khz) {
		c--;
	}
	return c;
}

/* Position of the current request in ch[] (the nearest frequency at or below it). */
static unsigned
cf_pos(const struct cf_dom *d)
{
	unsigned p = 0;
	uint32_t cur = d->req < d->n ? d->khz[d->req] : 0;
	for (unsigned k = 0; k < d->nch; ++k) {
		if (d->khz[d->ch[k]] <= cur) {
			p = k;
		}
	}
	return p;
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
	char line[440];
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
	if (cf_gov_thread) {
		len += snprintf(line + len, sizeof(line) - len, "; gov %s %u/%u/%u changes", cf_gov_on ? "on" : "off",
		    cf[0].changes, cf[1].changes, cf[2].changes);
	}
	{
		extern void sweet_idle_stats(char *buf, size_t len);   /* 0965, osfmk/arm64/cpu.c */
		char idle[120];
		sweet_idle_stats(idle, sizeof(idle));
		len += snprintf(line + len, sizeof(line) - len, "; %s", idle);
	}
	printf("%s\n", line);
}

/* 0970: gold/prime position for this tick (see above). */
static unsigned
cf_big_want(struct cf_dom *d, unsigned util, unsigned cur, unsigned ceil, uint64_t now, bool boost)
{
	unsigned want = cur, top = d->nch - 1;
	if (util >= cf_jump) {
		want = top;
	} else if (util >= cf_up) {
		uint64_t target = (uint64_t)d->khz[d->ch[cur]] * util / cf_up;
		want = cur < top ? cur + 1 : cur;
		while (want < top && d->khz[d->ch[want]] < target) {
			want++;
		}
	} else if (util + 10 < cf_up && cur > d->floor_pos) {
		uint64_t hold;
		nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &hold);
		if (now - d->last_up >= hold) {
			want = cur - 1;
		}
	}
	if (boost) {
		unsigned b = 0;
		while (b < top && d->khz[d->ch[b]] < cf_boost_khz) {
			b++;
		}
		want = want < b ? b : want;
	}
	if (want > ceil) {
		cf_caphits++;
		want = ceil;
	}
	if (want > cur) {
		d->last_up = now;
	}
	return want;
}

static void
cf_gov_log(uint64_t now)
{
	uint64_t win;
	nanoseconds_to_absolutetime(5ULL * NSEC_PER_SEC, &win);
	if (cf_log_t == 0) {
		cf_log_t = now;
	}
	if (now - cf_log_t < win) {
		return;
	}
	if ((cf_boost_on || cf_framehold > 0) && cf_boost_seen) {
		char line[400];
		int len = snprintf(line, sizeof(line), "SWEET GOV: 5s: boosts %u capped %u hold %u |", cf_boosts, cf_caphits,
		    cf_hold_ticks);     /* 1037: hold */
		for (unsigned i = 1; i < CF_NDOM; ++i) {
			len += snprintf(line + len, sizeof(line) - len, " %s", cf[i].name);
			for (unsigned r = 0; r < cf[i].n && len < (int)sizeof(line) - 16; ++r) {
				if (cf[i].at_ms[r]) {
					len += snprintf(line + len, sizeof(line) - len, " %u:%u", cf[i].khz[r] / 1000, cf[i].at_ms[r]);
				}
			}
			len += snprintf(line + len, sizeof(line) - len, " ms%s", i + 1 < CF_NDOM ? " |" : "");
		}
		printf("%s\n", line);
	}
	for (unsigned i = 0; i < CF_NDOM; ++i) {
		bzero(cf[i].at_ms, sizeof(cf[i].at_ms));
	}
	cf_boosts = cf_caphits = 0;
	cf_hold_ticks = 0;
	cf_boost_seen = false;
	cf_log_t = now;
}

static void
cf_gov_tick(struct cf_load *ld)
{
	unsigned busy[CF_NCPU];
	uint64_t now = mach_absolute_time(), tick_ms = 0;
	if (ld->t) {
		absolutetime_to_nanoseconds(now - ld->t, &tick_ms);
		tick_ms /= NSEC_PER_MSEC;
	}
	cf_busy(ld, busy);
	bool boost = cf_boost_on && now < cf_boost_until;
	cf_boost_seen |= boost;
	int hold = cf_framehold > 0 && now < cf_frame_until ? cf_framehold : 0;      /* 1037 */
	cf_boost_seen |= hold != 0;
	for (unsigned i = 0; i < CF_NDOM; ++i) {
		struct cf_dom *d = &cf[i];
		if (!d->base || d->nch == 0 || d->manual) {
			continue;
		}
		unsigned util = 0;
		for (unsigned c = d->first; c < d->first + d->ncpu; ++c) {
			util = busy[c] > util ? busy[c] : util;
		}
		unsigned cur = cf_pos(d), ceil = cf_ceiling(d);
		if (d->req < d->n) {
			d->at_ms[d->req] += (uint32_t)tick_ms;
		}
		if (cf_boost_on && i > 0) {
			/* 0970 */
			unsigned bw = cf_big_want(d, util, cur, ceil, now, boost);
			unsigned hp = cf_hold_pos(d, hold);     /* 1037 */
			if (hp > bw) {
				bw = hp > ceil ? ceil : hp;
				cf_hold_ticks++;
				if (bw > cur) {
					d->last_up = now;
				}
			}
			if (d->ch[bw] != d->req) {
				(void)sweet_cpufreq_set(i, d->ch[bw], NULL);
				d->changes++;
			}
			continue;
		}
		uint64_t target = (uint64_t)d->khz[d->ch[cur]] * util / 80;
		if (util >= 95) {
			target *= 2;            /* saturated: the real demand is unknown, ramp fast */
		}
		unsigned want = d->floor_pos < ceil ? d->floor_pos : ceil;
		while (want < ceil && d->khz[d->ch[want]] < target) {
			want++;
		}
		if (want > ceil) {
			want = ceil;
		}
		if (hold) {
			unsigned hp = cf_hold_pos(d, hold);     /* 1037 */
			hp = hp > ceil ? ceil : hp;
			if (hp > want) {
				want = hp;
				cf_hold_ticks++;
			}
		}
		if (cur > ceil) {
			d->down = 0;            /* above the ceiling (a new cap): down now */
		} else if (want < cur) {
			d->wish = (d->down == 0 || want > d->wish) ? want : d->wish;
			if (++d->down < 3) {
				continue;
			}
			want = d->wish;
			d->down = 0;
		} else {
			d->down = 0;
		}
		if (d->ch[want] != d->req) {
			(void)sweet_cpufreq_set(i, d->ch[want], NULL);
			d->changes++;
		}
	}
	cf_gov_log(now);	/* 0970 */
}

static void
cf_gov_thread_fn(void *arg __unused, wait_result_t wr __unused)
{
	struct cf_load ld = { { 0 }, 0 };
	for (;;) {
		if (!cf_gov_on) {
			assert_wait((event_t)&cf_gov_on, THREAD_UNINT);
			if (!cf_gov_on) {
				thread_block(THREAD_CONTINUE_NULL);
			} else {
				clear_wait(current_thread(), THREAD_AWAKENED);
			}
			ld.t = 0;
			continue;
		}
		cf_gov_tick(&ld);
		uint64_t deadline;
		clock_interval_to_deadline(cf_gov_ms, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&cf_gov_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* on: 0 stop, 1 start (and give "w freq" domains back); period_ms 0 = keep. Returns 0 or -1. */
int
sweet_cpufreq_gov(int on, unsigned period_ms)
{
	if (!cf_ready) {
		return -1;
	}
	if (period_ms >= 10 && period_ms <= 200) {
		cf_gov_ms = period_ms;
	}
	if (on) {
		for (unsigned i = 0; i < CF_NDOM; ++i) {
			cf[i].manual = false;
			cf[i].down = 0;
		}
	}
	if (on && !cf_gov_thread) {
		thread_t t;
		if (kernel_thread_start(cf_gov_thread_fn, NULL, &t) != KERN_SUCCESS) {
			return -1;
		}
		thread_deallocate(t);
		cf_gov_thread = true;
	}
	cf_gov_on = on ? 1 : 0;
	thread_wakeup((event_t)&cf_gov_on);
	printf("SWEET CPUFREQ: governor %s, %u ms\n", on ? "on" : "off", cf_gov_ms);
	return 0;
}

/*
 * 0964: thermal cap for one domain (max_khz 0 = none). The governor honours it on
 * its next tick; otherwise (pinned, kept, "w freq") the request is lowered here
 * and put back when the cap goes.
 */
int
sweet_cpufreq_cap(unsigned dom, uint32_t max_khz)
{
	if (!cf_ready || dom >= CF_NDOM || !cf[dom].base || cf[dom].nch == 0) {
		return -1;
	}
	struct cf_dom *d = &cf[dom];
	d->cap_khz = max_khz;
	if (cf_gov_on && !d->manual) {
		d->capped = false;
		return 0;
	}
	uint32_t cur = d->req < d->n ? d->khz[d->req] : 0;
	if (max_khz && cur > max_khz) {
		if (!d->capped) {
			d->pin = d->req;
			d->capped = true;
		}
		(void)sweet_cpufreq_set(dom, d->ch[cf_ceiling(d)], "thermal cap");
	} else if (d->capped && (!max_khz || d->khz[d->pin] <= max_khz)) {
		d->capped = false;
		(void)sweet_cpufreq_set(dom, d->pin, "thermal cap released");
	} else if (d->capped && d->khz[d->ch[cf_ceiling(d)]] > cur) {
		(void)sweet_cpufreq_set(dom, d->ch[cf_ceiling(d)], "thermal cap eased");
	}
	return 0;
}

/* 0970: "w boost 0|1" (sweetgov's thresholds, or 40/60). */
void sweet_cpufreq_boost_set(int on);
void
sweet_cpufreq_boost_set(int on)
{
	cf_boost_on = on ? 1 : 0;
	cf_boost_until = 0;
	for (unsigned i = 0; i < CF_NDOM; ++i) {
		cf[i].down = 0;
	}
	printf("SWEET GOV: big-core policy and boost %s (up %u %%, top %u %%, boost %u MHz %u ms)%s\n", on ? "on" : "off",
	    cf_up, cf_jump, cf_boost_khz / 1000, cf_boost_ms, cf_gov_on ? "" : "; the governor is off (w gov 1)");
}

/* "w freq": the governor leaves this domain alone until the next "w gov 1". */
void
sweet_cpufreq_manual(unsigned dom)
{
	if (dom < CF_NDOM) {
		cf[dom].manual = true;
	}
}

void
sweet_cpufreq_init(void)
{
	unsigned want = 1;
	(void)PE_parse_boot_argn("flamecpufreq", &want, sizeof(want));
	{
		/* 0970: sweetgov=XXYY (e.g. 4060), sweetboost=MHZ, sweetboostms=MS */
		unsigned g = 0, mhz = 1800, ms = 200;
		(void)PE_parse_boot_argn("sweetgov", &g, sizeof(g));
		(void)PE_parse_boot_argn("sweetboost", &mhz, sizeof(mhz));
		(void)PE_parse_boot_argn("sweetboostms", &ms, sizeof(ms));
		cf_boost_khz = mhz * 1000;
		cf_boost_ms = ms >= 20 && ms <= 2000 ? ms : 200;
		if (g / 100 >= 5 && g / 100 < g % 100 && g % 100 <= 99) {
			cf_up = g / 100;
			cf_jump = g % 100;
		}
		if (g) {
			cf_boost_on = 1;
			printf("SWEET GOV: sweetgov=%u: up %u %%, top %u %%, boost %u MHz %u ms\n", g, cf_up, cf_jump, mhz, cf_boost_ms);
		}
	}
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
		cf_build_choices(d);
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
	if (want == 2) {
		(void)sweet_cpufreq_gov(1, 0);
	}
	extern void sweet_thermal_start(void);
	sweet_thermal_start();  /* 0960: TSENS sampling, bsd/dev/sweet_thermal.c */
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
