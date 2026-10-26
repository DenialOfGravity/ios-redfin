/*
 * redfin (SM7250 "lito") temperatures: the two TSENS controllers, read only.
 * Linux: drivers/thermal/tsens2xxx.c (qcom,tsens26xx); addresses and the
 * sensor map from the live DT (thermal-zones, cooling maps):
 *
 *   tsens0 TM 0x0c263000: 0 aoss, 1-6 cpu0-5 (A55), 7-8 cpuss, 9 + 11 gold
 *       (CPU6: cpu-1-0/cpu-1-2 isolate cpu6), 10 + 12 prime (CPU7), 13-14 gpuss
 *   tsens1 TM 0x0c265000: 3 ddr, 6 npu, 7 camera
 *
 * Status of sensor n: TM + 0xa0 + 4n, bits 11:0 signed deci-degC, bit 21
 * valid; TM + 0xe4 bit 3 = first conversion round done. Nothing is written:
 * the boot loader / TZ configured the controllers, we only sample them.
 *
 * Every 5 s: one "SWEET THERMAL:" line; sysctl kern.sweet_temp_* (deci-degC).
 * Once at boot (after 20 s, read only): the PM7250B charger / gauge registers
 * later patches act on ("SWEET BATTERY: boot ...").
 * Boot-arg sweettherm=0: off.
 *
 * 0961: the 5 s tick also prints the "SWEET CPUFREQ:" busy/readback line, and
 * the command server's "w ..." lines land here (sweet_poke.c):
 *   w stat              temperatures + CPU busy/frequency/LMh now
 *   w freq DOM IDX      write a domain's perf state (DOM 0/s silver, 1/g gold, 2/p prime)
 *   w table [DOM]       print the frequency table(s)
 *   w gov 0|1 [MS]      DVFS governor off/on, tick MS (0963)
 *   w chg               charger/gauge registers now
 *   w chg wr ADDR VAL   one allow-listed charger write (hex, sweetchg=1 only; 0967)
 *   w chg apsd          rerun charger detection; w chg pol 0|1 charger policy off/on (0968)
 *   w cap N             force thermal level N (0-3) for a test, "w cap auto" back to the sensors
 *   w gpu N             GPU level cap N (0 = none) through sweet_gpu_level_cap()
 *
 * 0964 thermal caps, sampled every 1 s (the log stays at 5 s); hot = the hottest
 * of gold, prime, cpuss and camera (die sensors; the skin ADC needs PMIC writes):
 *   level 1 > 75 C (back below 65): gold/prime <= 1.8 GHz
 *   level 2 > 85 C (back below 75): gold/prime <= 1.2 GHz, silver <= 1.5 GHz, GPU level <= 2 (400 MHz)
 *   level 3 > 100 C (back below 90): gold/prime lowest row, silver <= 1.0 GHz, GPU level 1
 * Each change is logged once ("SWEET THERMAL: level A -> B ..."). Boot-arg sweettcap: bit 0 (1, default)
 * CPU caps, bit 1 (2) GPU caps too (runtime GMU vote, off by default); 0 = log the levels only.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_cpufreq_log(void);
extern int sweet_cpufreq_set(unsigned dom, unsigned idx, const char *why);
extern void sweet_cpufreq_table_log(unsigned dom);
extern int sweet_cpufreq_gov(int on, unsigned period_ms);
extern void sweet_cpufreq_manual(unsigned dom);
extern int sweet_cpufreq_cap(unsigned dom, uint32_t max_khz);
extern int sweet_gpu_level_cap(unsigned max_lvl);
extern void sweet_charger_tick(void);
extern void sweet_charger_dump(void);
extern void sweet_charger_apsd(void);
void sweet_power_cmd(const char *s, const char *end);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
void sweet_thermal_start(void);

#define TS0_TM_PA       0x0c263000u
#define TS1_TM_PA       0x0c265000u
#define TS_SN_STATUS    0xa0u
#define TS_TRDY         0xe4u
#define TS_VALID        (1u << 21)
#define TS_NONE         (-9999)

static vm_offset_t ts_tm[2];
static unsigned ts_period_ms = 1000;    /* sample (0964 caps); log every 5th */
static unsigned ts_tcap = 1;            /* sweettcap */
static int ts_level;                    /* 0964 thermal level 0-3 */
static int ts_forced = -1;              /* "w cap N" */
static LCK_GRP_DECLARE(ts_grp, "sweet_thermal");
static LCK_MTX_DECLARE(ts_lock, &ts_grp);       /* sampling + policy: thread vs "w" commands */
static unsigned ts_chg = 1;             /* 0968 charger policy (sweetchgpol=0: off; writes need sweetchg=1) */

/* Published values, deci-degC (TS_NONE = not read). */
static int t_aoss = TS_NONE, t_silver = TS_NONE, t_gold = TS_NONE, t_prime = TS_NONE;
static int t_cpuss = TS_NONE, t_gpu = TS_NONE, t_ddr = TS_NONE, t_npu = TS_NONE;
static int t_camera = TS_NONE, t_max = TS_NONE;
static int t_cpu[8] = { TS_NONE, TS_NONE, TS_NONE, TS_NONE, TS_NONE, TS_NONE, TS_NONE, TS_NONE };

SYSCTL_INT(_kern, OID_AUTO, sweet_temp_aoss, CTLFLAG_RD | CTLFLAG_LOCKED, &t_aoss, 0, "aoss, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_silver, CTLFLAG_RD | CTLFLAG_LOCKED, &t_silver, 0, "hottest A55, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_gold, CTLFLAG_RD | CTLFLAG_LOCKED, &t_gold, 0, "CPU6 A76, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_prime, CTLFLAG_RD | CTLFLAG_LOCKED, &t_prime, 0, "CPU7 A76, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_cpuss, CTLFLAG_RD | CTLFLAG_LOCKED, &t_cpuss, 0, "CPU subsystem, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_gpu, CTLFLAG_RD | CTLFLAG_LOCKED, &t_gpu, 0, "GPU, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_ddr, CTLFLAG_RD | CTLFLAG_LOCKED, &t_ddr, 0, "DDR, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_npu, CTLFLAG_RD | CTLFLAG_LOCKED, &t_npu, 0, "NPU, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_camera, CTLFLAG_RD | CTLFLAG_LOCKED, &t_camera, 0, "camera block, deci-degC");
SYSCTL_INT(_kern, OID_AUTO, sweet_temp_max, CTLFLAG_RD | CTLFLAG_LOCKED, &t_max, 0, "hottest sensor, deci-degC");

static uint32_t
ts_rd(vm_offset_t base, unsigned off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

/* One sensor, deci-degC, or TS_NONE (controller not ready / never valid). */
static int
ts_read(unsigned ctl, unsigned n)
{
	vm_offset_t tm = ts_tm[ctl];
	if (!tm || !(ts_rd(tm, TS_TRDY) & (1u << 3))) {
		return TS_NONE;
	}
	uint32_t code = 0, prev = 0xffffffffu;
	for (unsigned i = 0; i < 3; i++) {
		code = ts_rd(tm, TS_SN_STATUS + 4u * n);
		if (code & TS_VALID) {
			break;
		}
		/* tsens2xxx: no valid bit in three reads -> take two equal samples */
		if ((code & 0xfffu) == (prev & 0xfffu)) {
			break;
		}
		prev = code;
	}
	int v = (int)(code & 0xfffu);
	if (v & 0x800) {
		v -= 0x1000;
	}
	return v;
}

static int
ts_max2(int a, int b)
{
	return a > b ? a : b;
}

static void
ts_sample(void)
{
	int s[15];
	for (unsigned i = 0; i < 15; i++) {
		s[i] = ts_read(0, i);
	}
	int silver = TS_NONE;
	for (unsigned i = 1; i <= 6; i++) {
		t_cpu[i - 1] = s[i];
		silver = ts_max2(silver, s[i]);
	}
	t_cpu[6] = ts_max2(s[9], s[11]);
	t_cpu[7] = ts_max2(s[10], s[12]);
	t_aoss = s[0];
	t_silver = silver;
	t_gold = t_cpu[6];
	t_prime = t_cpu[7];
	t_cpuss = ts_max2(s[7], s[8]);
	t_gpu = ts_max2(s[13], s[14]);
	t_ddr = ts_read(1, 3);
	t_npu = ts_read(1, 6);
	t_camera = ts_read(1, 7);
	int m = TS_NONE;
	const int all[] = { t_aoss, t_silver, t_gold, t_prime, t_cpuss, t_gpu, t_ddr, t_npu, t_camera };
	for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		m = ts_max2(m, all[i]);
	}
	t_max = m;
}

/* "41.3" or "--" */
static const char *
ts_fmt(char *buf, size_t len, int v)
{
	if (v == TS_NONE) {
		snprintf(buf, len, "--");
	} else {
		snprintf(buf, len, "%s%d.%d", v < 0 ? "-" : "", (v < 0 ? -v : v) / 10, (v < 0 ? -v : v) % 10);
	}
	return buf;
}

static void
ts_log(void)
{
	char a[9][12];
	printf("SWEET THERMAL: silver %s gold %s prime %s cpuss %s gpu %s ddr %s cam %s npu %s aoss %s C\n",
	    ts_fmt(a[0], 12, t_silver), ts_fmt(a[1], 12, t_gold), ts_fmt(a[2], 12, t_prime),
	    ts_fmt(a[3], 12, t_cpuss), ts_fmt(a[4], 12, t_gpu), ts_fmt(a[5], 12, t_ddr),
	    ts_fmt(a[6], 12, t_camera), ts_fmt(a[7], 12, t_npu), ts_fmt(a[8], 12, t_aoss));
}

/*
 * PM7250B (SID 2) registers the charger / battery patches depend on, read
 * once through the observer. Names: Linux smb5-reg.h / qg-reg.h.
 */
static const struct {
	uint16_t addr;
	const char *name;
} ts_pmic_regs[] = {
	{ 0x1006, "CHGR_STATUS_1" }, { 0x1007, "CHGR_STATUS_2" }, { 0x100d, "CHGR_STATUS_7" },
	{ 0x1042, "CHARGING_ENABLE_CMD" }, { 0x1061, "FCC_CFG" }, { 0x1070, "FV_CFG" },
	{ 0x1090, "JEITA_EN_CFG" }, { 0x1106, "ICL_MAX_STATUS" }, { 0x1107, "ICL_STATUS" },
	{ 0x1108, "AICL_ICL_STATUS" }, { 0x110a, "AICL_STATUS" }, { 0x110b, "POWER_PATH" },
	{ 0x1307, "APSD_STATUS" }, { 0x1308, "APSD_RESULT" }, { 0x1310, "USBIN_INT_RT" },
	{ 0x1340, "USBIN_CMD_IL" }, { 0x1342, "CMD_ICL_OVERRIDE" }, { 0x1365, "USBIN_LOAD_CFG" },
	{ 0x1366, "USBIN_ICL_OPTIONS" }, { 0x1370, "USBIN_CURRENT_LIMIT_CFG" },
	{ 0x1506, "TYPE_C_SNK_STATUS" }, { 0x1606, "TEMP_RANGE" }, { 0x1607, "DIE_TEMP" },
	{ 0x1609, "CONN_TEMP" }, { 0x1651, "WD_CFG" }, { 0x1653, "SNARL_BARK_BITE_WD_CFG" },
	{ 0x4805, "QG_SUBTYPE" }, { 0x48c6, "QG_BURST_AVG_I_LO" }, { 0x48c7, "QG_BURST_AVG_I_HI" },
	{ 0xb146, "SDAM_VALID" }, { 0xb147, "SDAM_SOC" }, { 0xb148, "SDAM_TEMP_LO" },
	{ 0xb149, "SDAM_TEMP_HI" },
};

/* Prints the table above, 6 registers per line. Also used by "w chg" before the charger patch. */
void sweet_thermal_pmic_dump(const char *why);
void
sweet_thermal_pmic_dump(const char *why)
{
	if (sweet_spmi_init() != 0) {
		printf("SWEET BATTERY: %s regs: SPMI unavailable\n", why);
		return;
	}
	const unsigned n = sizeof(ts_pmic_regs) / sizeof(ts_pmic_regs[0]);
	for (unsigned i = 0; i < n; i += 6) {
		char line[200];
		int len = snprintf(line, sizeof(line), "SWEET BATTERY: %s regs", why);
		for (unsigned j = i; j < n && j < i + 6; j++) {
			uint8_t v = 0;
			int rc = sweet_spmi_read(2, ts_pmic_regs[j].addr, &v);
			if (rc) {
				len += snprintf(line + len, sizeof(line) - len, " %04x=??", ts_pmic_regs[j].addr);
			} else {
				len += snprintf(line + len, sizeof(line) - len, " %04x=%02x", ts_pmic_regs[j].addr, v);
			}
		}
		printf("%s\n", line);
	}
	/* Decoded: the four facts P6-P8 need first. */
	uint8_t fcc = 0, fv = 0, jeita = 0, wd = 0, sbb = 0, icl = 0, valid = 0, soc = 0;
	if (!sweet_spmi_read(2, 0x1061, &fcc) && !sweet_spmi_read(2, 0x1070, &fv) &&
	    !sweet_spmi_read(2, 0x1090, &jeita) && !sweet_spmi_read(2, 0x1651, &wd) &&
	    !sweet_spmi_read(2, 0x1653, &sbb) && !sweet_spmi_read(2, 0x1370, &icl) &&
	    !sweet_spmi_read(2, 0xb146, &valid) && !sweet_spmi_read(2, 0xb147, &soc)) {
		printf("SWEET BATTERY: %s FCC %u mA FV %u mV ICL cfg %u mA JEITA_EN %02x WD_CFG %02x (bark int %d, "
		    "timer on plugin %d) SBB %02x (bite disables charging %d) SDAM valid %u soc %u%%\n",
		    why, fcc * 50u, 3600u + fv * 10u, icl * 50u, jeita, wd, (wd >> 6) & 1, (wd >> 1) & 1, sbb,
		    (sbb >> 7) & 1, valid, soc);
	}
}

/* 0964: caps per level. silver, gold, prime kHz (0 = none; 1 = lowest row); GPU level (0 = none). */
static const struct {
	uint32_t khz[3];
	unsigned gpu;
	int up, down;           /* enter above up, leave below down (deci-degC) */
} ts_levels[4] = {
	{ { 0, 0, 0 }, 0, 0, 0 },
	{ { 0, 1800000, 1800000 }, 0, 750, 650 },
	{ { 1500000, 1200000, 1200000 }, 2, 850, 750 },
	{ { 1000000, 1, 1 }, 1, 1000, 900 },
};

static void
ts_apply_level(int level, int hot)
{
	char b[12];
	if (level == ts_level) {
		return;
	}
	printf("SWEET THERMAL: level %d -> %d (hot %s C%s): silver %u gold %u prime %u MHz cap, gpu %u%s\n",
	    ts_level, level, ts_fmt(b, sizeof(b), hot), ts_forced >= 0 ? ", forced" : "",
	    ts_levels[level].khz[0] / 1000, ts_levels[level].khz[1] / 1000, ts_levels[level].khz[2] / 1000,
	    ts_levels[level].gpu, (ts_tcap & 1) ? "" : " (sweettcap=0: not applied)");
	ts_level = level;
	if (ts_tcap & 1) {
		for (unsigned d = 0; d < 3; d++) {
			(void)sweet_cpufreq_cap(d, ts_levels[level].khz[d]);
		}
	}
	if (ts_tcap & 2) {
		int rc = sweet_gpu_level_cap(ts_levels[level].gpu);
		if (rc) {
			printf("SWEET THERMAL: gpu cap %u -> %d\n", ts_levels[level].gpu, rc);
		}
	}
}

static void
ts_policy(void)
{
	int hot = ts_max2(ts_max2(t_gold, t_prime), ts_max2(t_cpuss, t_camera));
	int level = ts_level;
	if (ts_forced >= 0) {
		level = ts_forced;
	} else if (hot == TS_NONE) {
		return;
	} else {
		while (level < 3 && hot > ts_levels[level + 1].up) {
			level++;
		}
		while (level > 0 && hot < ts_levels[level].down) {
			level--;
		}
	}
	ts_apply_level(level, hot);
}

static void
ts_thread(void *arg __unused, wait_result_t wr __unused)
{
	static int ts_chan;
	unsigned elapsed = 0, ticks = 0;
	bool dumped = false;
	for (;;) {
		lck_mtx_lock(&ts_lock);
		ts_sample();
		ts_policy();
		lck_mtx_unlock(&ts_lock);
		if ((ticks++ % 5) != 0) {
			goto next_tick;
		}
		ts_log();
		sweet_cpufreq_log();
		if (!dumped && elapsed >= 20000) {
			dumped = true;
			sweet_thermal_pmic_dump("boot");
		}
		if (dumped && ts_chg) {
			sweet_charger_tick();   /* 0968, after the boot register dump */
		}
next_tick:;
		uint64_t deadline;
		clock_interval_to_deadline(ts_period_ms, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&ts_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
		elapsed += ts_period_ms;
	}
}

/* "w ..." command parsing */
static bool
pc_word(const char **p, const char *end, char *out, size_t len)
{
	const char *s = *p;
	while (s < end && (*s == ' ' || *s == '\t')) {
		s++;
	}
	size_t n = 0;
	while (s < end && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s) {
		if (n + 1 < len) {
			out[n++] = *s;
		}
		s++;
	}
	out[n] = 0;
	*p = s;
	return n > 0;
}

static bool
pc_num(const char **p, const char *end, unsigned *out)
{
	char w[16];
	if (!pc_word(p, end, w, sizeof(w)) || w[0] < '0' || w[0] > '9') {
		return false;
	}
	unsigned v = 0;
	for (const char *c = w; *c >= '0' && *c <= '9'; c++) {
		v = v * 10 + (unsigned)(*c - '0');
	}
	*out = v;
	return true;
}

/* hex with or without 0x */
static unsigned
pc_hex(const char *w)
{
	unsigned v = 0;
	if (w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) {
		w += 2;
	}
	for (; *w; w++) {
		unsigned c = (unsigned char)*w;
		unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
		    c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
		if (d > 15) {
			break;
		}
		v = v * 16 + d;
	}
	return v;
}

/* DOM: 0/1/2 or s(ilver)/g(old)/p(rime) */
static bool
pc_dom(const char **p, const char *end, unsigned *out)
{
	char w[16];
	if (!pc_word(p, end, w, sizeof(w))) {
		return false;
	}
	switch (w[0]) {
	case '0': case 's': *out = 0; return true;
	case '1': case 'g': *out = 1; return true;
	case '2': case 'p': *out = 2; return true;
	default: return false;
	}
}

static bool
pc_is(const char *w, const char *name)
{
	return strncmp(w, name, strlen(name) + 1) == 0;
}

void
sweet_power_cmd(const char *s, const char *end)
{
	char w[16];
	unsigned dom = 0, idx = 0;
	if (!pc_word(&s, end, w, sizeof(w))) {
		return;
	}
	if (pc_is(w, "stat") || pc_is(w, "temp")) {
		if (ts_tm[0]) {
			ts_sample();
			ts_log();
		}
		sweet_cpufreq_log();
	} else if (pc_is(w, "freq")) {
		if (!pc_dom(&s, end, &dom) || !pc_num(&s, end, &idx)) {
			printf("SWEET POWER: usage: w freq DOM(0-2|s|g|p) IDX\n");
			return;
		}
		sweet_cpufreq_manual(dom);
		if (sweet_cpufreq_set(dom, idx, "w freq") < 0) {
			printf("SWEET POWER: w freq %u %u refused (no such domain/index)\n", dom, idx);
		}
	} else if (pc_is(w, "table")) {
		if (pc_dom(&s, end, &dom)) {
			sweet_cpufreq_table_log(dom);
		} else {
			for (unsigned d = 0; d < 3; d++) {
				sweet_cpufreq_table_log(d);
			}
		}
	} else if (pc_is(w, "gov")) {
		unsigned on = 0, ms = 0;
		if (!pc_num(&s, end, &on)) {
			printf("SWEET POWER: usage: w gov 0|1 [MS]\n");
			return;
		}
		(void)pc_num(&s, end, &ms);
		if (sweet_cpufreq_gov(on != 0, ms) < 0) {
			printf("SWEET POWER: governor unavailable\n");
		}
	} else if (pc_is(w, "boost")) {
		/* 0970: w boost 0|1: the big-core policy and the touch/animation boost */
		extern void sweet_cpufreq_boost_set(int on);
		unsigned on = 0;
		if (!pc_num(&s, end, &on)) {
			printf("SWEET POWER: usage: w boost 0|1\n");
			return;
		}
		sweet_cpufreq_boost_set(on != 0);
	} else if (pc_is(w, "cap")) {
		unsigned n = 0;
		if (pc_num(&s, end, &n) && n <= 3) {
			ts_forced = (int)n;
		} else {
			ts_forced = -1;
		}
		lck_mtx_lock(&ts_lock);
		ts_sample();
		ts_policy();
		lck_mtx_unlock(&ts_lock);
		printf("SWEET POWER: thermal level %d (%s)\n", ts_level, ts_forced >= 0 ? "forced" : "sensors");
	} else if (pc_is(w, "gpu")) {
		unsigned n = 0;
		(void)pc_num(&s, end, &n);
		printf("SWEET POWER: gpu level cap %u -> %d\n", n, sweet_gpu_level_cap(n));
	} else if (pc_is(w, "chg")) {
		char sub[8];
		if (pc_word(&s, end, sub, sizeof(sub)) && pc_is(sub, "wr")) {
			char a[12], v[8];
			if (!pc_word(&s, end, a, sizeof(a)) || !pc_word(&s, end, v, sizeof(v))) {
				printf("SWEET POWER: usage: w chg wr ADDR VAL (hex)\n");
				return;
			}
			unsigned addr = pc_hex(a), val = pc_hex(v);
			printf("SWEET POWER: w chg wr %04x %02x -> %d\n", addr, val, sweet_spmi_write(2, addr, (uint8_t)val));
			return;
		}
		if (pc_is(sub, "apsd")) {
			sweet_charger_apsd();
			return;
		}
		if (pc_is(sub, "pol")) {
			unsigned n = 1;
			(void)pc_num(&s, end, &n);
			ts_chg = n;
			printf("SWEET POWER: charger policy %s\n", n ? "on" : "off");
			return;
		}
		sweet_charger_dump();
	} else {
		printf("SWEET POWER: unknown \"w %s\" (stat, freq, table, gov, boost, cap, gpu, chg)\n", w);
	}
}

void
sweet_thermal_start(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweettherm", &on, sizeof(on));
	PE_parse_boot_argn("sweettcap", &ts_tcap, sizeof(ts_tcap));
	PE_parse_boot_argn("sweetchgpol", &ts_chg, sizeof(ts_chg));
	if (!on) {
		printf("SWEET THERMAL: off (sweettherm=0)\n");
		return;
	}
	ts_tm[0] = ml_io_map(TS0_TM_PA, 0x1000);
	ts_tm[1] = ml_io_map(TS1_TM_PA, 0x1000);
	if (!ts_tm[0] || !ts_tm[1]) {
		printf("SWEET THERMAL: map failed\n");
		return;
	}
	printf("SWEET THERMAL: tsens0 trdy %08x s1 %08x, tsens1 trdy %08x s3 %08x\n",
	    ts_rd(ts_tm[0], TS_TRDY), ts_rd(ts_tm[0], TS_SN_STATUS + 4), ts_rd(ts_tm[1], TS_TRDY),
	    ts_rd(ts_tm[1], TS_SN_STATUS + 12));
	thread_t t;
	if (kernel_thread_start(ts_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
