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
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_cpufreq_log(void);
extern int sweet_cpufreq_set(unsigned dom, unsigned idx, const char *why);
extern void sweet_cpufreq_table_log(unsigned dom);
extern int sweet_cpufreq_gov(int on, unsigned period_ms);
extern void sweet_cpufreq_manual(unsigned dom);
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
static unsigned ts_period_ms = 5000;

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

static void
ts_thread(void *arg __unused, wait_result_t wr __unused)
{
	static int ts_chan;
	unsigned elapsed = 0;
	bool dumped = false;
	for (;;) {
		ts_sample();
		ts_log();
		sweet_cpufreq_log();
		if (!dumped && elapsed >= 20000) {
			dumped = true;
			sweet_thermal_pmic_dump("boot");
		}
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
	} else if (pc_is(w, "chg")) {
		sweet_thermal_pmic_dump("now");
	} else {
		printf("SWEET POWER: unknown \"w %s\" (stat, freq, table, gov, chg)\n", w);
	}
}

void
sweet_thermal_start(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sweettherm", &on, sizeof(on));
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
