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
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi.h"

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
	return 0;
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
	if (!sp_ready || !out || addr > 0xffffu || !(batt || rtc)) {
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
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(sp_obs + off) = cmd; /* EXT_READL, BC=0 */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		uint32_t status = sp_rd(sp_obs, off + 0x08u);
		if (status & 1u) {
			if (status & 0x0eu) {
				IOLockUnlock(sp_lock);
				return -1;
			}
			*out = (uint8_t)sp_rd(sp_obs, off + 0x18u);
			IOLockUnlock(sp_lock);
			return 0;
		}
		delay(1);
	}
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

int
sweet_spmi_write_enabled(void)
{
	if (sp_wr_enabled < 0) {
		unsigned v = 0;
		PE_parse_boot_argn("sweetchg", &v, sizeof(v));
		sp_wr_enabled = v == 1;
		printf("SWEET CHG: SPMI charger writes %s (sweetchg=%u)\n", sp_wr_enabled ? "ENABLED" : "off", v);
	}
	return sp_wr_enabled;
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
		    sp_wr_enabled ? "not on the allow-list" : "sweetchg=0");
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
