/*
 * redfin (0968): PM7250B SMB5 charger policy. Called every 5 s from the thermal
 * thread (bsd/dev/sweet_thermal.c). Register meanings: Linux smb5-reg.h /
 * smb5-lib.c (redbull). Writes go through sweet_spmi_write() (0967): nothing is
 * written unless boot-arg sweetchg=1; without it the policy runs dry and logs
 * what it would do.
 *
 * On a plugged-in, APSD-done port:
 *   ICL: DCP or CDP 1500 mA; Type-C Rp 1.5 A >= 1500, Rp 3.0 A 2000 mA (cap);
 *        SDP / float / nothing advertised: left to the hardware (500 mA USB 2.0).
 *        Set the smblib HC way: USBIN_CURRENT_LIMIT_CFG 0x1370 = mA / 50, then
 *        USBIN_ICL_OPTIONS 0x1366 bit 0 (USBIN_MODE_CHG) = 1, CMD_ICL_OVERRIDE
 *        0x1342 bit 0 = 0, USBIN_LOAD_CFG 0x1365 bit 4 (ICL_OVERRIDE_AFTER_APSD) = 1
 *        (smblib_icl_override(SW_OVERRIDE_HC_MODE)).
 *   FCC 0x1061 (50 mA steps): 2000 mA (redfin battery profile fastchg 2000 mA),
 *        1000 mA while JEITA soft cold/hot (CHGR_STATUS_7 bits 5:4) or the SMB die
 *        is over its upper bound (DIE_TEMP_STATUS 0x1607 UB/RST/SHDN; the die
 *        temperature itself needs ADC writes) or TEMP_RANGE 0x1606 reports alert /
 *        above range.
 *   FV 0x1070 (3.6 V + 10 mV steps): lowered to 4.45 V if set higher, never raised.
 * The SMB watchdog (WD_CFG 0x1651 / SNARL_BARK_BITE 0x1653), USBIN suspend
 * (0x1340) and CHARGING_ENABLE_CMD (0x1042) are only read. At most 200 writes per
 * boot (a register the hardware keeps resetting must not be hammered).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <kern/locks.h>
#include "sweet_spmi.h"

void sweet_charger_tick(void);
void sweet_charger_dump(void);
void sweet_charger_apsd(void);

#define CHG_SID         2
#define CHG_FCC_MA      2000u
#define CHG_FCC_HOT_MA  1000u
#define CHG_FV_MAX      85u     /* 3600 + 85 * 10 = 4450 mV */
#define CHG_WRITES_MAX  200u

static struct {
	bool plugged;
	unsigned icl, fcc, fv;  /* last decision (mA, mA, register) */
	const char *src;
	unsigned writes;
	bool budget_gone;
	unsigned ticks;
} chg;

static bool
chg_rd(unsigned addr, uint8_t *v)
{
	return sweet_spmi_read(CHG_SID, addr, v) == 0;
}

/* Write if different; true if the register now holds the value. */
static bool
chg_set(unsigned addr, uint8_t val)
{
	uint8_t cur;
	if (!chg_rd(addr, &cur)) {
		return false;
	}
	if (cur == val) {
		return true;
	}
	if (!sweet_spmi_write_enabled() || chg.budget_gone) {
		return false;
	}
	if (++chg.writes > CHG_WRITES_MAX) {
		chg.budget_gone = true;
		printf("SWEET CHG: %u writes this boot: no more (a register keeps changing back?)\n", CHG_WRITES_MAX);
		return false;
	}
	return sweet_spmi_write(CHG_SID, addr, val) == 0;
}

static bool
chg_masked(unsigned addr, uint8_t mask, uint8_t val)
{
	uint8_t cur;
	if (!chg_rd(addr, &cur)) {
		return false;
	}
	return chg_set(addr, (uint8_t)((cur & ~mask) | (val & mask)));
}

static const char *
chg_apsd_name(uint8_t r)
{
	return (r & 0x40) ? "QC3" : (r & 0x20) ? "QC2" : (r & 0x10) ? "float" : (r & 0x08) ? "DCP" :
	       (r & 0x04) ? "CDP" : (r & 0x02) ? "OCP" : (r & 0x01) ? "SDP" : "none";
}

static const char *
chg_rp_name(uint8_t snk)
{
	return (snk & 0x02) ? "Rp3.0" : (snk & 0x04) ? "Rp1.5" : (snk & 0x08) ? "Rp-std" : "no-Rp";
}

static bool k9_tick(void);     /* 1123 */

void
sweet_charger_tick(void)
{
	if (k9_tick()) {
		return;                 /* 1123: kern.sweet_chg=1 owns the charger */
	}
	uint8_t usbin, apsd_st, apsd, snk, st7, die, range, fv;
	if (!chg_rd(0x1310, &usbin) || !chg_rd(0x1307, &apsd_st) || !chg_rd(0x1308, &apsd) ||
	    !chg_rd(0x1506, &snk) || !chg_rd(0x100d, &st7) || !chg_rd(0x1607, &die) ||
	    !chg_rd(0x1606, &range) || !chg_rd(0x1070, &fv)) {
		return;
	}
	chg.ticks++;
	bool plugged = (usbin & 0x10) != 0;
	if (!plugged) {
		if (chg.plugged) {
			printf("SWEET CHG: unplugged\n");
		}
		chg.plugged = false;
		chg.icl = chg.fcc = chg.fv = 0;
		return;
	}
	if (!(apsd_st & 0x01)) {
		return;                 /* APSD (BC1.2 detection) not done yet */
	}
	unsigned icl = 0;
	if (apsd & 0x0c) {
		icl = 1500;             /* DCP / CDP */
	}
	if ((snk & 0x04) && icl < 1500) {
		icl = 1500;
	}
	if (snk & 0x02) {
		icl = 2000;             /* Rp 3.0 A, capped at 2 A */
	}
	bool hot = (st7 & 0x30) || (die & 0x0e) || (range & 0x18);
	unsigned fcc = hot ? CHG_FCC_HOT_MA : CHG_FCC_MA;
	unsigned fv_want = fv > CHG_FV_MAX ? CHG_FV_MAX : fv;
	bool changed = !chg.plugged || icl != chg.icl || fcc != chg.fcc || fv_want != chg.fv;
	chg.plugged = true;
	chg.icl = icl;
	chg.fcc = fcc;
	chg.fv = fv_want;
	chg.src = chg_apsd_name(apsd);

	bool ok = true;
	if (icl) {
		ok &= chg_set(0x1370, (uint8_t)(icl / 50));
		ok &= chg_masked(0x1366, 0x01, 0x01);
		ok &= chg_masked(0x1342, 0x01, 0x00);
		ok &= chg_masked(0x1365, 0x10, 0x10);
	}
	ok &= chg_set(0x1061, (uint8_t)(fcc / 50));
	if (fv_want != fv) {
		ok &= chg_set(0x1070, (uint8_t)fv_want);
	}
	if (changed || (!ok && sweet_spmi_write_enabled() && (chg.ticks % 12) == 0)) {
		printf("SWEET CHG: plugged %s (APSD %02x) %s (snk %02x)%s -> ICL %s%u mA, FCC %u mA, FV %u mV: %s\n",
		    chg.src, apsd, chg_rp_name(snk), snk, hot ? " HOT" : "", icl ? "" : "hardware ", icl ? icl : 500,
		    fcc, 3600 + fv_want * 10, !sweet_spmi_write_enabled() ? "dry run (sweetchg=0)" :
		    ok ? "applied" : "NOT all applied");
	}
}

/* "w chg apsd": rerun BC1.2 detection (CMD_APSD 0x1341 bit 0). */
void
sweet_charger_apsd(void)
{
	printf("SWEET CHG: APSD rerun -> %d\n", chg_masked(0x1341, 0x01, 0x01) ? 0 : -1);
}

static int
chg_word(unsigned addr, int *out)
{
	uint8_t lo, hi;
	if (!chg_rd(addr, &lo) || !chg_rd(addr + 1, &hi)) {
		return -1;
	}
	*out = (int)(int16_t)((unsigned)lo | ((unsigned)hi << 8));
	return 0;
}

/* "w chg": the whole state, decoded. */
void
sweet_charger_dump(void)
{
	static const uint16_t regs[] = { 0x1006, 0x1007, 0x100d, 0x1042, 0x1061, 0x1070, 0x1090, 0x1106, 0x1107,
		                         0x1108, 0x110a, 0x110b, 0x1307, 0x1308, 0x1310, 0x1340, 0x1342, 0x1365,
		                         0x1366, 0x1370, 0x1506, 0x1606, 0x1607, 0x1609, 0x1651, 0x1653 };
	uint8_t v[sizeof(regs) / sizeof(regs[0])];
	char line[256];
	int len = snprintf(line, sizeof(line), "SWEET CHG: regs");
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		if (!chg_rd(regs[i], &v[i])) {
			printf("SWEET CHG: SPMI read %04x failed\n", regs[i]);
			return;
		}
		len += snprintf(line + len, sizeof(line) - len, " %04x=%02x", regs[i], v[i]);
		if (len > 200) {
			printf("%s\n", line);
			len = snprintf(line, sizeof(line), "SWEET CHG: regs");
		}
	}
	printf("%s\n", line);
	/* indices into regs[] */
	uint8_t st1 = v[0], st7 = v[2], en = v[3], fcc = v[4], fv = v[5], jeita = v[6], iclmax = v[7], iclst = v[8];
	uint8_t aicl_icl = v[9], aicl = v[10], usbin = v[14], il = v[15], snk = v[20], range = v[21], die = v[22];
	uint8_t apsd = v[13], icl = v[19], wd = v[24], sbb = v[25];
	static const char *states[] = { "inhibit", "trickle", "pre", "fast", "taper", "terminate", "pause", "disabled" };
	int vraw = 0, iraw = 0;
	uint8_t sub = 0;
	int mv = 0, ma = 0;
	if (!chg_word(0x48c0, &vraw) && !chg_word(0x48c2, &iraw) && chg_rd(0x4805, &sub)) {
		mv = (int)((int64_t)(uint16_t)vraw * 194637 / 1000000);
		ma = (int)(-((int64_t)iraw * (sub == 3 ? 152588 : 305176)) / 1000000);
	}
	printf("SWEET CHG: %s, %s %s, state %s, enable cmd %02x, USBIN suspend %d; ICL cfg %u mA, max %u, now %u, AICL %u mA "
	    "(done %d, soft-ilimit %d)\n", (usbin & 0x10) ? "plugged" : "unplugged", chg_apsd_name(apsd),
	    chg_rp_name(snk), states[st1 & 7], en, il & 1, icl * 50u, iclmax * 50u, iclst * 50u, aicl_icl * 50u,
	    aicl & 1, (aicl >> 6) & 1);
	printf("SWEET CHG: FCC %u mA, FV %u mV, JEITA_EN %02x zone %s, die %02x, temp-range %02x, WD_CFG %02x SBB %02x; "
	    "battery %d mV %d mA; writes %s, %u done; policy ICL %u FCC %u FV %u\n", fcc * 50u, 3600u + fv * 10u, jeita,
	    (st7 & 0x08) ? "too-hot" : (st7 & 0x04) ? "too-cold" : (st7 & 0x20) ? "soft-hot" : (st7 & 0x10) ? "soft-cold" :
	    "normal", die, range, wd, sbb, mv, ma, sweet_spmi_write_enabled() ? "ENABLED" : "off (sweetchg=0)", chg.writes,
	    chg.icl, chg.fcc, chg.fv ? 3600 + chg.fv * 10 : 0);
}

/*
 * ---- 1123: kern.sweet_chg (default 0 = everything above unchanged) ----
 *
 * Evidence (k-a-boot, df-1115 klog0): ABL leaves the SMB5 charging: CHGR_STATUS_1 0x1006 = 03 (fast charge),
 * CHARGING_ENABLE_CMD 0x1042 = 01, FCC 2000 mA, FV 4450 mV, JEITA_EN d0, WD_CFG 0x1651 = 00 (SMB watchdog
 * off, nothing to pet), SBB 0c (bite does not disable charging). On the PC cable APSD says SDP and Type-C
 * Rp-default, so the hardware input limit is USB 2.0 500 mA (USBIN_ICL_OPTIONS 0x1366 = 02 = USB51 500 mA mode,
 * ICL_STATUS 0x1107 = 0a = 500 mA, AICL 500 mA). With the display on iOS draws ~500-700 mA, so the battery sees
 * -50..-200 mA "while charging" (chg= in SweetBattery is external && ibat > 50 mA, not a register).
 *
 * kern.sweet_chg=1: on the first switch-on the touched registers are saved (ABL values); then every tick (5 s):
 *   APSD done + plugged: ICL = DCP / CDP / QC 1500 mA, Type-C Rp 1.5 A or 3.0 A 1500 mA, SDP kern.sweet_chg_sdp_ma
 *     (default 500 = left to the hardware, max 900 = USB 3.x), float / OCP / none: hardware. A non-hardware ICL is set
 *     the smblib SW_OVERRIDE_HC_MODE way: 0x1370 = mA / 50, 0x1366 bit 0 = 1, 0x1342 bit 0 = 0, 0x1365 bit 4 = 1;
 *     "hardware" = those four back to the saved ABL values. AICL stays on (not touched).
 *   FCC 0x1061 = min(ABL's FCC, 2000 mA); 1000 mA while JEITA soft zone / die over bound / temp-range alert.
 *   Unplugged: the four ICL registers and FCC back to the ABL values (the next port starts in hardware mode).
 *   Never written: FV 0x1070, CHARGING_ENABLE_CMD, JEITA, watchdog, USBIN suspend, anything outside SID 2 0x10/0x13.
 * kern.sweet_chg=0 again: everything back to the ABL values, run-time writes off.
 * Every write is read back and logged by sweet_spmi_write(). "SWEET CHG30:" every 30 s while kern.sweet_chg=1.
 * kern.sweet_chg_dump=1: read-only dump of SID 2 peripherals 0x10 0x11 0x12 0x13 0x15 0x16 (0x00-0xff) + "w chg".
 */
static LCK_GRP_DECLARE(k9_grp, "sweet_chg");
static LCK_MTX_DECLARE(k9_mtx, &k9_grp);
static int k9_on;
static int k9_sdp_ma = 500;
static bool k9_saved;
static unsigned k9_ticks;
static const uint16_t k9_regs[] = { 0x1370, 0x1366, 0x1342, 0x1365, 0x1061 };
static uint8_t k9_abl[sizeof(k9_regs) / sizeof(k9_regs[0])];
static struct {
	bool plugged, hc;
	unsigned icl, fcc;
	uint8_t apsd, snk;
} k9;

static bool
k9_save(void)
{
	if (k9_saved) {
		return true;
	}
	for (unsigned i = 0; i < sizeof(k9_regs) / sizeof(k9_regs[0]); i++) {
		if (!chg_rd(k9_regs[i], &k9_abl[i])) {
			printf("SWEET CHG: 1123 cannot read %04x: kern.sweet_chg stays 0\n", k9_regs[i]);
			return false;
		}
	}
	k9_saved = true;
	printf("SWEET CHG: 1123 saved ABL values 1370=%02x (ICL cfg %u mA) 1366=%02x 1342=%02x 1365=%02x 1061=%02x (FCC %u mA)\n",
	    k9_abl[0], k9_abl[0] * 50u, k9_abl[1], k9_abl[2], k9_abl[3], k9_abl[4], k9_abl[4] * 50u);
	return true;
}

/* The four ICL registers (and FCC if all) back to ABL's values. */
static bool
k9_restore(bool all, const char *why)
{
	if (!k9_saved) {
		return true;
	}
	bool ok = true;
	for (unsigned i = 0; i < sizeof(k9_regs) / sizeof(k9_regs[0]) - (all ? 0 : 1); i++) {
		uint8_t cur;
		if (chg_rd(k9_regs[i], &cur) && cur != k9_abl[i]) {
			ok &= chg_set(k9_regs[i], k9_abl[i]);
		}
	}
	k9.hc = false;
	if (all) {
		k9.fcc = 0;
	}
	printf("SWEET CHG: 1123 ABL values back (%s%s): %s\n", all ? "ICL + FCC, " : "ICL, ", why, ok ? "ok" : "NOT all");
	return ok;
}

static unsigned
k9_fcc_cap(void)
{
	unsigned abl = k9_abl[4] * 50u;
	return abl && abl < CHG_FCC_MA ? abl : CHG_FCC_MA;
}

static void
k9_line(void)
{
	uint8_t st1 = 0, en = 0, fcc = 0, fv = 0, st7 = 0, range = 0, die = 0, icfg = 0, iopt = 0, iclst = 0, aicl = 0;
	uint8_t usbin = 0, apsd = 0, snk = 0, tlo = 0, thi = 0, sub = 0;
	int vraw = 0, iraw = 0, mv = 0, ma = 0;
	if (!chg_rd(0x1006, &st1) || !chg_rd(0x1042, &en) || !chg_rd(0x1061, &fcc) || !chg_rd(0x1070, &fv) ||
	    !chg_rd(0x100d, &st7) || !chg_rd(0x1606, &range) || !chg_rd(0x1607, &die) || !chg_rd(0x1370, &icfg) ||
	    !chg_rd(0x1366, &iopt) || !chg_rd(0x1107, &iclst) || !chg_rd(0x1108, &aicl) || !chg_rd(0x1310, &usbin) ||
	    !chg_rd(0x1308, &apsd) || !chg_rd(0x1506, &snk) || !chg_rd(0xb148, &tlo) || !chg_rd(0xb149, &thi)) {
		printf("SWEET CHG30: SPMI read failed\n");
		return;
	}
	if (!chg_word(0x48c0, &vraw) && !chg_word(0x48c2, &iraw) && chg_rd(0x4805, &sub)) {
		mv = (int)((int64_t)(uint16_t)vraw * 194637 / 1000000);
		ma = (int)(-((int64_t)iraw * (sub == 3 ? 152588 : 305176)) / 1000000);
	}
	static const char *states[] = { "inhibit", "trickle", "pre", "fast", "taper", "terminate", "pause", "disabled" };
	int sdt = (int)(int16_t)((unsigned)tlo | ((unsigned)thi << 8));
	printf("SWEET CHG30: %s %s %s, ICL %s cfg %u now %u AICL %u mA, FCC %u FV %u, %s (en %02x), zone %s range %02x "
	    "die %02x, batt %d mV %+d mA, sdam-temp %d.%d C, writes %u\n",
	    (usbin & 0x10) ? "plugged" : "unplugged", chg_apsd_name(apsd), chg_rp_name(snk),
	    (iopt & 1) ? "HC" : (iopt & 2) ? "usb500" : "usb100", icfg * 50u, iclst * 50u, aicl * 50u, fcc * 50u,
	    3600u + fv * 10u, states[st1 & 7], en,
	    (st7 & 0x08) ? "too-hot" : (st7 & 0x04) ? "too-cold" : (st7 & 0x20) ? "soft-hot" : (st7 & 0x10) ? "soft-cold" :
	    "normal", range, die, mv, ma, sdt / 10, (sdt < 0 ? -sdt : sdt) % 10, chg.writes);
}

static bool
k9_tick(void)
{
	lck_mtx_lock(&k9_mtx);
	if (!k9_on) {
		lck_mtx_unlock(&k9_mtx);
		return false;
	}
	uint8_t usbin, apsd_st, apsd, snk, st7, die, range;
	if (!chg_rd(0x1310, &usbin) || !chg_rd(0x1307, &apsd_st) || !chg_rd(0x1308, &apsd) ||
	    !chg_rd(0x1506, &snk) || !chg_rd(0x100d, &st7) || !chg_rd(0x1607, &die) || !chg_rd(0x1606, &range)) {
		lck_mtx_unlock(&k9_mtx);
		return true;
	}
	bool log30 = (k9_ticks++ % 6) == 0;
	bool plugged = (usbin & 0x10) != 0;
	if (!plugged) {
		if (k9.plugged) {
			printf("SWEET CHG: 1123 unplugged\n");
			k9_restore(true, "unplugged");
		}
		k9.plugged = false;
		k9.icl = 0;
		goto out;
	}
	if (!(apsd_st & 0x01)) {
		goto out;               /* APSD not done: hardware */
	}
	unsigned icl = 0;               /* 0 = hardware (ABL's mode) */
	if (apsd & 0x6c) {
		icl = 1500;             /* DCP, CDP, QC2/QC3 (a DCP) */
	}
	if (snk & 0x06) {
		icl = 1500;             /* Type-C Rp 1.5 A / 3.0 A, capped at 1.5 A */
	}
	if (!icl && (apsd & 0x01) && k9_sdp_ma > 500) {
		icl = (unsigned)k9_sdp_ma;
	}
	bool hot = (st7 & 0x30) || (die & 0x0e) || (range & 0x18);
	unsigned fcc = k9_fcc_cap();
	if (hot && fcc > CHG_FCC_HOT_MA) {
		fcc = CHG_FCC_HOT_MA;
	}
	bool changed = !k9.plugged || icl != k9.icl || fcc != k9.fcc || apsd != k9.apsd || snk != k9.snk;
	bool ok = true;
	if (icl) {
		ok &= chg_set(0x1370, (uint8_t)(icl / 50));
		ok &= chg_masked(0x1366, 0x01, 0x01);
		ok &= chg_masked(0x1342, 0x01, 0x00);
		ok &= chg_masked(0x1365, 0x10, 0x10);
		k9.hc = true;
	} else if (k9.hc) {
		ok &= k9_restore(false, "port back to hardware ICL");
	}
	ok &= chg_set(0x1061, (uint8_t)(fcc / 50));
	if (changed || (!ok && (k9_ticks % 12) == 1)) {
		printf("SWEET CHG: 1123 %s (APSD %02x) %s (snk %02x)%s -> ICL %s%u mA, FCC %u mA (cap %u), FV untouched: %s\n",
		    chg_apsd_name(apsd), apsd, chg_rp_name(snk), snk, hot ? " HOT" : "", icl ? "" : "hardware ",
		    icl ? icl : 500, fcc, k9_fcc_cap(), ok ? "applied" : "NOT all applied");
	}
	k9.plugged = true;
	k9.icl = icl;
	k9.fcc = fcc;
	k9.apsd = apsd;
	k9.snk = snk;
out:
	if (log30) {
		k9_line();
	}
	lck_mtx_unlock(&k9_mtx);
	return true;
}

static int
sysctl_sweet_chg SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = k9_on, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value != 0 && value != 1) {
		return EINVAL;
	}
	if (sweet_spmi_init() != 0) {
		return ENXIO;
	}
	lck_mtx_lock(&k9_mtx);
	if (value && !k9_on) {
		if (!k9_save()) {
			lck_mtx_unlock(&k9_mtx);
			return EIO;
		}
		sweet_spmi_write_runtime(1);
		k9_on = 1;
		k9_ticks = 0;
		k9.plugged = false;
		k9.icl = k9.fcc = 0;
		printf("SWEET CHG: 1123 kern.sweet_chg=1 (SDP %d mA, FCC cap %u mA, FV and watchdog untouched)\n",
		    k9_sdp_ma, k9_fcc_cap());
	} else if (!value && k9_on) {
		k9_on = 0;
		k9_restore(true, "kern.sweet_chg=0");
		sweet_spmi_write_runtime(0);
		k9.plugged = false;
	}
	lck_mtx_unlock(&k9_mtx);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_chg, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_chg, "I",
    "1123: 1 = kernel keeps USB charging set up (ICL from port type, FCC <= ABL); 0 = ABL's setup (default)");

static int
sysctl_sweet_chg_sdp_ma SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = k9_sdp_ma, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 500 || value > 900 || (value % 50) != 0) {
		return EINVAL;
	}
	k9_sdp_ma = value;
	printf("SWEET CHG: 1123 SDP input limit %d mA%s\n", value, value > 500 ? " (above USB 2.0; AICL still on)" : "");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_chg_sdp_ma, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_chg_sdp_ma,
    "I", "1123: SDP (PC port) input limit with kern.sweet_chg=1: 500 (hardware, default) .. 900 mA");

static int
sysctl_sweet_chg_dump SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error || !changed || value != 1) {
		return error;
	}
	if (sweet_spmi_init() != 0) {
		return ENXIO;
	}
	static const uint8_t periph[] = { 0x10, 0x11, 0x12, 0x13, 0x15, 0x16 };
	for (unsigned p = 0; p < sizeof(periph); p++) {
		for (unsigned row = 0; row < 0x100; row += 16) {
			char line[120];
			int len = snprintf(line, sizeof(line), "SWEET CHGDUMP: %02x%02x:", periph[p], row);
			for (unsigned o = row; o < row + 16; o++) {
				uint8_t v;
				if (chg_rd(((unsigned)periph[p] << 8) | o, &v)) {
					len += snprintf(line + len, sizeof(line) - len, " %02x", v);
				} else {
					len += snprintf(line + len, sizeof(line) - len, " ??");
				}
			}
			printf("%s\n", line);
		}
	}
	sweet_charger_dump();
	printf("SWEET CHG: 1123 kern.sweet_chg=%d, ABL values %s\n", k9_on, k9_saved ? "saved" : "not saved yet");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_chg_dump, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_chg_dump, "I",
    "1123: write 1 = read-only dump of the PM7250B charger registers to the klog");
#endif /* ARM64_BOARD_CONFIG_SWEET */
