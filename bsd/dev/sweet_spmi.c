/*
 * Battery-only adaptation of input/sweet_keys.c, observer EXT_READL.
 * PMIC READ ONLY. The only MMIO store issues a one-byte read command.
 * No interrupt clear, peripheral enable, charger config, or PMIC writes.
 * Init is serialized by the single built-in SweetBattery personality.
 * Battery APIDs are disjoint from the existing keys' 008/4c1 APIDs.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <IOKit/IOLocks.h>
#include "sweet_spmi.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define SP_CORE_PA 0x0c440000u
#define SP_OBS_PA  0x0e600000u
#define SP_CFG_PA  0x0c40a000u
#define SP_NO_APID 0xffffu

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
	/* Restrict access to the battery blocks; keys retain their own lanes. */
	unsigned peripheral = addr >> 8;
	if (!sp_ready || !out || sid != 0 || addr > 0xffffu ||
	    (peripheral != 0x10 && peripheral != 0x11 && peripheral != 0x12 &&
	    peripheral != 0x13 && peripheral != 0x48)) {
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
#endif /* ARM64_BOARD_CONFIG_SWEET */
