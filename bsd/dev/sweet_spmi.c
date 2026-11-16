/*
 * Battery-only adaptation of input/sweet_keys.c, observer EXT_READL.
 * PMIC READ ONLY. The only MMIO store issues a one-byte read command.
 * No interrupt clear, peripheral enable, charger config, or PMIC writes.
 * Init is serialized by the single built-in SweetBattery personality.
 * Battery APIDs are disjoint from the existing keys' 008/4c1 APIDs.
 * Also the PM8150 RTC (peripheral 0x60) for the time of day (SweetPlatformExpert.cpp).
 * redfin: the battery blocks are on PM7250B (SID 2), the RTC stays on SID 0.
 *
 * redfin (0967): the one exception to "read only": sweet_spmi_write(), off unless
 * boot-arg sweetchg=1, and then only for the SMB5 charger registers in
 * sp_write_ok[] (FCC 0x1061, FV 0x1070, CMD_APSD 0x1341, CMD_ICL_OVERRIDE
 * 0x1342, USBIN_LOAD_CFG 0x1365, USBIN_ICL_OPTIONS 0x1366,
 * USBIN_CURRENT_LIMIT_CFG 0x1370). Linux spmi-pmic-arb.c v5: the APID's
 * channel at chnls 0x0c600000 + 0x10000 * APID, WDATA0 +0x10, CMD +0x00 =
 * EXT_WRITEL (opcode 0) << 27 | addr[7:0] << 4 | byte count - 1, STATUS +0x08
 * (bit 0 done, 1 failure, 2 denied, 3 dropped); the APID must be owned by EE 0
 * (cnfg 0x0c40a000 + 0x700 + 4 * APID, bits 2:0). Every write is read back
 * through the observer and logged ("SWEET CHG: write ...").
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <kern/locks.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi.h"
#include <kern/thread.h>      /* 1130 */
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <mach/mach_time.h>

/*
 * 1129 (port of Yofuness/ios-flame xnu 0197 + 0201, MIT, "Copyright (c) 2026 dipa-studio"; sunfish PM6150 ->
 * redfin PM8150, SID 0, PON peripheral 0x08): why the PMIC powered the phone on, read once when SPMI comes up
 * (the first RTC read at boot), READ ONLY (observer EXT_READL of 0x800-0x81f and 0x88f; nothing is cleared).
 * One klog line "SWEET PON: ..." and the same text as kern.sweet_pon (RD string). Boot-arg sweetpon=0 skips the
 * reads (line "SWEET PON: off (sweetpon=0)"). Decoding (Linux qpnp-power-on.c, PON gen2 = PMIC5 PM8150):
 *   0x08 PON_REASON1: hard_reset smpl rtc dc_chg usb_chg pon1 cblpwr kpdpwr (bits 0-7)
 *   0x0a WARM_RESET_REASON1, 0x0c/0x0d FAULT_REASON1/2 (gen2 poff/fault: bit 4 mbg, 5 ovlo, 6 uvlo, 7 avdd_rb,
 *   11 fault_n, 12 pbs_watchdog, 13 pbs_nack, 14 restart_pon, 15 otst3), 0x14 S3_RESET_REASON, 0x8f SOFT_RB_SPARE
 *   (the shim/ABL reboot reason, NOTES "Reboot reason"). All bytes are in the line raw, decode is a guide only.
 * smpl / hard_reset / uvlo = a PMIC-level event (rail, brown-out); warm reset reason set = a TZ/watchdog reset.
 */
static char sp_pon_line[384] = "SWEET PON: not read yet";
SYSCTL_STRING(_kern, OID_AUTO, sweet_pon, CTLFLAG_RD | CTLFLAG_LOCKED, sp_pon_line, sizeof(sp_pon_line),
    "1129: PM8150 PON reason registers at boot (read only)");
static void sp_pon_boot(void);
static void sp_crumb_boot(void);   /* 1130 */

/*
 * 1062: the shared SPMI transaction lock (sweet_spmi.h). r5au1f's boot: sweet_keys.c and sweet_torch.c
 * read at the same moment and got each other's bytes (keys pon=0x9 = the temperature alarm's TYPE,
 * torch subtype 0x1): the torch refused to start and keys saw fake presses.
 * kern.sweet_spmi_lock (boot-arg sweetspmilock, default 1): 0 = every driver on its own again.
 * kern.sweet_spmi_lock_waits (RD): transfers that found the lock taken (= would have overlapped).
 */
static LCK_GRP_DECLARE(sp_bus_grp, "sweet_spmi_bus");
static LCK_MTX_DECLARE(sp_bus_mtx, &sp_bus_grp);
static int sp_bus_on = -1;
static unsigned sp_bus_waits;

int
sweet_spmi_bus_lock(void)
{
	if (sp_bus_on < 0) {
		int v = 1;
		(void)PE_parse_boot_argn("sweetspmilock", &v, sizeof(v));
		sp_bus_on = v != 0;
		printf("SWEET SPMI: shared transaction lock %s (sweetspmilock=%d)\n", sp_bus_on ? "on" : "OFF", v);
	}
	if (!sp_bus_on) {
		return 0;
	}
	if (!lck_mtx_try_lock(&sp_bus_mtx)) {
		__atomic_fetch_add(&sp_bus_waits, 1u, __ATOMIC_RELAXED);
		lck_mtx_lock(&sp_bus_mtx);
	}
	return 1;
}

void
sweet_spmi_bus_unlock(int held)
{
	if (held) {
		lck_mtx_unlock(&sp_bus_mtx);
	}
}

static int
sysctl_sweet_spmi_lock SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (sp_bus_on < 0) {
		sweet_spmi_bus_unlock(sweet_spmi_bus_lock());   /* boot-arg default, logged once */
	}
	int value = sp_bus_on, changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value != 0 && value != 1) {
		return EINVAL;
	}
	sp_bus_on = value;      /* a holder still unlocks with its own "held" */
	printf("SWEET SPMI: shared transaction lock %s (sysctl)\n", value ? "on" : "OFF");
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_spmi_lock, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_spmi_lock, "I", "1062: one lock for all sweet SPMI transfers (0 = off)");
SYSCTL_UINT(_kern, OID_AUTO, sweet_spmi_lock_waits, CTLFLAG_RD | CTLFLAG_LOCKED, &sp_bus_waits, 0,
    "1062: SPMI transfers that waited for another driver's");

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define SP_CORE_PA 0x0c440000u
#define SP_OBS_PA  0x0e600000u
#define SP_CFG_PA  0x0c40a000u
#define SP_NO_APID 0xffffu
#define SP_CHNL_PA 0x0c600000u

static vm_offset_t sp_core, sp_obs, sp_cfg;
static IOLock *sp_lock;
static unsigned sp_count;
static bool sp_attempted, sp_ready;
/* A timed-out command may complete late: never reuse that lane this boot. */
static bool sp_poisoned[512];

static uint32_t
sp_rd(vm_offset_t base, unsigned off)
{
	uint32_t value = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static uint16_t
sp_find_apid(unsigned ppid)
{
	uint16_t found = SP_NO_APID;
	for (unsigned i = 0; i < sp_count; ++i) {
		uint32_t map = sp_rd(sp_core, 0x900u + 4u * i);
		if (!map || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		unsigned owner = sp_rd(sp_cfg, 0x700u + 4u * i) & 7u;
		if (found == SP_NO_APID || owner == 0) {
			found = (uint16_t)i;
		}
	}
	return found;
}

int
sweet_spmi_init(void)
{
	if (sp_attempted) {
		return sp_ready ? 0 : -1;
	}
	sp_attempted = true;
	sp_lock = IOLockAlloc();
	if (!sp_lock) {
		return -1;
	}
	sp_core = ml_io_map(SP_CORE_PA, 0x2000);
	sp_cfg = ml_io_map(SP_CFG_PA, 0x1000);
	sp_obs = ml_io_map(SP_OBS_PA, 0x10000); /* EE 0, 512 * 0x80 */
	if (!sp_core || !sp_cfg || !sp_obs) {
		printf("SWEET BATTERY: SPMI mapping failed\n");
		return -1;
	}
	uint32_t arb_version = sp_rd(sp_core, 0);
	sp_count = sp_rd(sp_core, 4) & 0x7ffu;
	if (arb_version < 0x50000000u || arb_version >= 0x70000000u ||
	    !sp_count || sp_count > 512) {
		printf("SWEET BATTERY: unsupported SPMI arb=%#x APIDs=%u\n",
		    arb_version, sp_count);
		return -1;
	}
	sp_ready = true;
	printf("SWEET BATTERY: SPMI arb=%#x APIDs=%u, observer reads only\n",
	    arb_version, sp_count);
	sp_pon_boot();          /* 1129 */
	sp_crumb_boot();        /* 1130 */
	return 0;
}

/*
 * 1130 (port of Yofuness/ios-flame xnu 0269 "watchdog pet crumb", MIT, "Copyright (c) 2026 dipa-studio"): a klog
 * line every N seconds, "SWEET CRUMB: t SEC.MS n COUNT cpu C", so after a silent reset the last crumb in the
 * rescued klog (0x... uncached mirror, sweet_ufs.c) says when the kernel was last alive. On sunfish it rode on the
 * watchdog pet; redfin's ABL leaves the APSS WDT off (NOTES "Watchdog"), so here it is its own thread.
 * kern.sweet_crumb (RW int, seconds 0-3600) / boot-arg sweetcrumb=N; 0 (default) = no lines, no thread. The
 * thread starts at the first nonzero value and then idles (1 s checks) while the value is 0.
 */
static int sp_crumb_s;
static int sp_crumb_started;

static void
sp_crumb_thread(__unused void *arg, __unused wait_result_t wr)
{
	unsigned n = 0;
	for (;;) {
		int s = sp_crumb_s;
		if (s <= 0) {
			delay_for_interval(1000, NSEC_PER_MSEC);
			continue;
		}
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		printf("SWEET CRUMB: t %llu.%03llu n %u cpu %d\n", ns / NSEC_PER_SEC, (ns / NSEC_PER_MSEC) % 1000, n++,
		    cpu_number());
		delay_for_interval((uint32_t)s * 1000, NSEC_PER_MSEC);
	}
}

static void
sp_crumb_start(void)
{
	thread_t thread;
	if (sp_crumb_s <= 0 || __atomic_exchange_n(&sp_crumb_started, 1, __ATOMIC_ACQ_REL)) {
		return;
	}
	if (kernel_thread_start(sp_crumb_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("SWEET CRUMB: every %d s (kern.sweet_crumb)\n", sp_crumb_s);
	} else {
		__atomic_store_n(&sp_crumb_started, 0, __ATOMIC_RELEASE);
	}
}

static void
sp_crumb_boot(void)
{
	int s = 0;
	if (PE_parse_boot_argn("sweetcrumb", &s, sizeof(s)) && s > 0 && s <= 3600) {
		sp_crumb_s = s;
		sp_crumb_start();
	}
}

static int
sysctl_sweet_crumb SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int v = sp_crumb_s;
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) {
		return err;
	}
	if (v < 0 || v > 3600) {
		return EINVAL;
	}
	sp_crumb_s = v;
	sp_crumb_start();
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_crumb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_sweet_crumb, "I",
    "1130: klog crumb every N s (0 = off)");

/* 1129: see the comment at the top (sweet_pon). Runs once, from sweet_spmi_init (thread context). */
static void
sp_pon_boot(void)
{
	int on = 1;
	(void)PE_parse_boot_argn("sweetpon", &on, sizeof(on));
	if (!on) {
		strlcpy(sp_pon_line, "SWEET PON: off (sweetpon=0)", sizeof(sp_pon_line));
		printf("%s\n", sp_pon_line);
		return;
	}
	uint8_t r[0x20], spare = 0;
	unsigned bad = 0;
	for (unsigned i = 0; i < sizeof(r); ++i) {
		if (sweet_spmi_read(0, 0x800 + i, &r[i]) != 0) {
			r[i] = 0xee;
			bad++;
			if (i < 4) {
				break;  /* no APID / timeout: do not walk the rest */
			}
		}
	}
	if (bad && sweet_spmi_read(0, 0x800, &r[0]) != 0) {
		strlcpy(sp_pon_line, "SWEET PON: read failed (no observer APID for 0:0x08?)", sizeof(sp_pon_line));
		printf("%s\n", sp_pon_line);
		return;
	}
	bool spare_ok = sweet_spmi_read(0, 0x88f, &spare) == 0;
	static const char *const why[8] = { "hard_reset", "smpl", "rtc", "dc_chg", "usb_chg", "pon1", "cblpwr", "kpdpwr" };
	static const char *const flt[16] = { "gp0", "gp1", "gp2", "gp3", "mbg", "ovlo", "uvlo", "avdd_rb", "b8", "b9",
		                               "b10", "fault_n", "pbs_wdog", "pbs_nack", "restart_pon", "otst3" };
	char dec[96] = "", fdec[96] = "";
	for (unsigned b = 0; b < 8; ++b) {
		if (r[8] & (1u << b)) {
			strlcat(dec, dec[0] ? "|" : "", sizeof(dec));
			strlcat(dec, why[b], sizeof(dec));
		}
	}
	unsigned fb = r[0x0c] | (unsigned)r[0x0d] << 8;
	for (unsigned b = 0; b < 16; ++b) {
		if (fb & (1u << b)) {
			strlcat(fdec, fdec[0] ? "|" : "", sizeof(fdec));
			strlcat(fdec, flt[b], sizeof(fdec));
		}
	}
	char raw[3 * 0x20 + 1];
	for (unsigned i = 0; i < 0x20; ++i) {
		snprintf(raw + 3 * i, 4, "%02x%s", r[i], i == 0x1f ? "" : " ");
	}
	snprintf(sp_pon_line, sizeof(sp_pon_line), "SWEET PON: reason %s | warm %#x | fault %s (%#x) | s3 %#x | spare88f %s%#x |"
	    " type %02x/%02x | 800-81f: %s%s", dec[0] ? dec : "none", r[0x0a], fdec[0] ? fdec : "none", fb, r[0x14],
	    spare_ok ? "" : "?", spare, r[4], r[5], raw, bad ? " (some reads failed: ee)" : "");
	printf("%s\n", sp_pon_line);
}

int
sweet_spmi_read(unsigned sid, unsigned addr, uint8_t *out)
{
	/* Restrict access to the battery blocks and the RTC (0x60); keys retain their own lanes. */
	unsigned peripheral = addr >> 8;
	/* redfin: battery blocks on PM7250B (SID 2), RTC on PM8150 (SID 0). */
	bool batt = sid == 2 && (peripheral == 0x10 || peripheral == 0x11 ||
	    peripheral == 0x12 || peripheral == 0x13 || peripheral == 0x48 ||
	    /* 0960: Type-C (0x15), SMB misc/watchdog (0x16), QG SDAM (0xb1): read only */
	    peripheral == 0x15 || peripheral == 0x16 || peripheral == 0xb1);
	bool rtc = sid == 0 && peripheral == 0x60;
	bool pon = sid == 0 && peripheral == 0x08;  /* 1129: PM8150 PON block, read only (reset forensics) */
	if (!sp_ready || !out || addr > 0xffffu || !(batt || rtc || pon)) {
		return -1;
	}
	IOLockLock(sp_lock);
	uint16_t apid = sp_find_apid((sid << 8) | peripheral);
	if (apid == SP_NO_APID || sp_poisoned[apid]) {
		IOLockUnlock(sp_lock);
		return -1;
	}
	unsigned off = 0x80u * apid;
	uint32_t cmd = (1u << 27) | ((addr & 0xffu) << 4);
	int bus = sweet_spmi_bus_lock();        /* 1062 (lock order: sp_lock, then the bus lock) */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(sp_obs + off) = cmd; /* EXT_READL, BC=0 */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t status = sp_rd(sp_obs, off + 0x08u);
		if (status & 1u) {
			if (status & 0x0eu) {
				sweet_spmi_bus_unlock(bus);
				IOLockUnlock(sp_lock);
				return -1;
			}
			*out = (uint8_t)sp_rd(sp_obs, off + 0x18u);
			sweet_spmi_bus_unlock(bus);
			IOLockUnlock(sp_lock);
			return 0;
		}
		delay(1);
	}
	sweet_spmi_bus_unlock(bus);
	sp_poisoned[apid] = true;
	printf("SWEET BATTERY: SPMI timeout sid=%u addr=%#x APID=%u disabled\n",
	    sid, addr, apid);
	IOLockUnlock(sp_lock);
	return -1;
}
/* ---- 0967: the charger write path ---- */
static const uint16_t sp_write_ok[] = { 0x1061, 0x1070, 0x1341, 0x1342, 0x1365, 0x1366, 0x1370 };
static int sp_wr_enabled = -1;
static struct {
	uint16_t apid;
	vm_offset_t va;
} sp_chnl[4];

/* 1123: kern.sweet_chg=1 opens the same allow-listed path at run time (sweet_charger.c); 0 closes it again. */
static int sp_wr_runtime;

void
sweet_spmi_write_runtime(int on)
{
	sp_wr_runtime = on != 0;
	printf("SWEET CHG: SPMI charger writes %s at run time (kern.sweet_chg)\n", sp_wr_runtime ? "ENABLED" : "off");
}

int
sweet_spmi_write_enabled(void)
{
	if (sp_wr_enabled < 0) {
		unsigned v = 0;
		PE_parse_boot_argn("sweetchg", &v, sizeof(v));
		sp_wr_enabled = v == 1;
		printf("SWEET CHG: SPMI charger writes %s (sweetchg=%u)\n", sp_wr_enabled ? "ENABLED" : "off", v);
	}
	return sp_wr_enabled || sp_wr_runtime;
}

static vm_offset_t
sp_chnl_va(uint16_t apid)
{
	for (unsigned i = 0; i < sizeof(sp_chnl) / sizeof(sp_chnl[0]); ++i) {
		if (sp_chnl[i].va && sp_chnl[i].apid == apid) {
			return sp_chnl[i].va;
		}
	}
	for (unsigned i = 0; i < sizeof(sp_chnl) / sizeof(sp_chnl[0]); ++i) {
		if (!sp_chnl[i].va) {
			sp_chnl[i].va = ml_io_map(SP_CHNL_PA + 0x10000u * apid, 0x1000);
			sp_chnl[i].apid = apid;
			return sp_chnl[i].va;
		}
	}
	return 0;
}

int
sweet_spmi_write(unsigned sid, unsigned addr, uint8_t val)
{
	bool listed = false;
	for (unsigned i = 0; i < sizeof(sp_write_ok) / sizeof(sp_write_ok[0]); ++i) {
		listed = listed || addr == sp_write_ok[i];
	}
	if (!sweet_spmi_write_enabled() || sid != 2 || !listed) {
		printf("SWEET CHG: write %u:%04x = %02x refused (%s)\n", sid, addr, val,
		    (sp_wr_enabled > 0 || sp_wr_runtime) ? "not on the allow-list" : "sweetchg=0, kern.sweet_chg=0");
		return -1;
	}
	uint8_t before = 0, after = 0;
	if (sweet_spmi_read(sid, addr, &before) != 0) {
		printf("SWEET CHG: write %u:%04x = %02x refused (cannot read it first)\n", sid, addr, val);
		return -1;
	}
	unsigned peripheral = addr >> 8;
	IOLockLock(sp_lock);
	uint16_t apid = sp_find_apid((sid << 8) | peripheral);
	unsigned owner = apid == SP_NO_APID ? 7 : (sp_rd(sp_cfg, 0x700u + 4u * apid) & 7u);
	vm_offset_t ch = (apid == SP_NO_APID || owner != 0 || sp_poisoned[apid]) ? 0 : sp_chnl_va(apid);
	if (!ch) {
		IOLockUnlock(sp_lock);
		printf("SWEET CHG: write %u:%04x refused: APID %u owner EE %u%s\n", sid, addr, apid, owner,
		    apid != SP_NO_APID && sp_poisoned[apid] ? " (poisoned)" : "");
		return -1;
	}
	uint32_t status = 0;
	unsigned t;
	int bus = sweet_spmi_bus_lock();        /* 1062 */
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(ch + 0x10u) = val;                       /* WDATA0 */
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(ch + 0x00u) = ((addr & 0xffu) << 4);     /* EXT_WRITEL (0), BC = 0 */
	__asm__ volatile ("dsb osh" ::: "memory");
	for (t = 0; t < 1000; ++t) {
		status = sp_rd(ch, 0x08u);
		if (status & 1u) {
			break;
		}
		delay(1);
	}
	sweet_spmi_bus_unlock(bus);
	if (!(status & 1u)) {
		sp_poisoned[apid] = true;
	}
	IOLockUnlock(sp_lock);
	if (!(status & 1u) || (status & 0x0eu)) {
		printf("SWEET CHG: write %u:%04x = %02x FAILED: status %x after %u us%s\n", sid, addr, val, status, t,
		    (status & 1u) ? "" : " (APID disabled)");
		return -1;
	}
	int rb = sweet_spmi_read(sid, addr, &after);
	bool same = rb == 0 && after == val;
	printf("SWEET CHG: write %u:%04x = %02x (was %02x) -> reads %02x %s\n", sid, addr, val, before, after,
	    same ? "ok" : "MISMATCH");
	return same ? 0 : -2;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
