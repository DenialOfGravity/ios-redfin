/*
 * Minimal polled SWEET keys. Call once after the scheduler is running.
 * flame: power (KPDPWR) and volume down (RESIN) on the PM8150 PON block,
 * volume up: redfin PM8150L GPIO5 (SID 4, 0xc400, active low; sunfish PM6150L GPIO2 0xc100,
 * Pixel 4 PM8150 GPIO6). The shim sets its pull-up (shim.c redfin_pmic_gpios).
 * PMIC access is READ ONLY: the sole MMIO write issues EXT_READL on an
 * observer channel. No PMIC config, IRQ acknowledge or reset writes exist.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_key(int key, int down);
void sweet_keys_init(void);

#define KEYS_CORE_PA     0x0c440000u
#define KEYS_OBS_PA      0x0e600000u
#define KEYS_CNFG_PA     0x0c40a000u
#define KEYS_PON_RT      0x0810u
#define KEYS_GPIO_SID    4u
#define KEYS_GPIO_BASE   0xc400u
#define KEYS_GPIO_RT     (KEYS_GPIO_BASE + 0x10u)
#define KEYS_NO_APID     0xffffu

static vm_offset_t keys_core, keys_obs, keys_cnfg;
static uint16_t keys_pon_apid = KEYS_NO_APID, keys_gpio_apid = KEYS_NO_APID;
static uint32_t keys_version, keys_last_status;
static bool keys_started;
static int keys_sleep_chan;
/*
 * 1062: a key change is reported only if the peripheral still reads back as itself: TYPE (0x04) and
 * SUBTYPE (0x05) equal to what init read, and the RT_STS byte reads the same again. A read that raced
 * another driver's (r5au1f: pon=0x9, volup 0x80) is dropped. Boot-arg sweetkeyscheck=0: off.
 */
static uint8_t keys_pon_type, keys_pon_sub, keys_gpio_type, keys_gpio_sub;
static bool keys_pon_id, keys_gpio_id;
static int keys_check = 1;
static unsigned keys_dropped;

static uint32_t
keys_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* v5 can map one PPID more than once; prefer the apps-owned entry. */
static uint16_t
keys_find_apid(uint16_t ppid, uint32_t count)
{
	uint16_t found = KEYS_NO_APID;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t map = keys_rd(keys_core, 0x900u + 4u * i);
		if (map == 0 || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		uint32_t owner = keys_rd(keys_cnfg, 0x700u + 4u * i) & 7u;
		if (found == KEYS_NO_APID || owner == 0) {
			found = (uint16_t)i;
		}
	}
	return found;
}

static bool
keys_read(uint16_t apid, uint16_t addr, uint8_t *out)
{
	if (apid == KEYS_NO_APID) {
		return false;
	}
	uint32_t off = 0x80u * apid; /* obs + EE(0)*0x10000 */
	uint32_t cmd = (1u << 27) | ((uint32_t)(addr & 0xffu) << 4);
	bool ok = false;
	int bus = sweet_spmi_bus_lock();        /* 1062 */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(keys_obs + off) = cmd; /* EXT_READL, one byte */
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		keys_last_status = keys_rd(keys_obs, off + 0x08u);
		if (keys_last_status & 1u) {
			if (!(keys_last_status & 0x0eu)) {
				*out = (uint8_t)keys_rd(keys_obs, off + 0x18u);
				ok = true;
			}
			break;
		}
		delay(1);
	}
	sweet_spmi_bus_unlock(bus);
	return ok;
}

/* 1062: the peripheral's TYPE/SUBTYPE as init saw them, and the RT byte again equal to val */
static bool
keys_still(uint16_t apid, uint16_t base, uint16_t rt, bool known, uint8_t type, uint8_t sub, uint8_t val,
    uint8_t *t, uint8_t *s, uint8_t *again)
{
	if (!keys_check) {
		return true;
	}
	*t = *s = *again = 0xee;
	if (!known) {   /* init could not confirm the identity: the second read alone */
		return keys_read(apid, rt, again) && *again == val;
	}
	return keys_read(apid, base + 0x04u, t) && keys_read(apid, base + 0x05u, s) &&
	       keys_read(apid, rt, again) && *t == type && *s == sub && *again == val;
}

static void
keys_wait(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
keys_thread(void *arg, wait_result_t result)
{
	int state[3] = {0, 0, 0};
	unsigned errors = 0;
	(void)arg;
	(void)result;
	for (;;) {
		uint8_t pon = 0, gpio = 0;
		bool pok = keys_read(keys_pon_apid, KEYS_PON_RT, &pon);
		bool gok = keys_read(keys_gpio_apid, KEYS_GPIO_RT, &gpio);
		int now[3] = {!!(pon & 1u), !(gpio & 1u), !!(pon & 2u)};
		if (!pok || !gok) {
			if ((errors++ % 250u) == 0) {
				printf("SWEET KEYS: read error pon-ok=%d gpio-ok=%d arb-status=%#x\n",
				    pok, gok, keys_last_status);
			}
		} else {
			errors = 0;
		}
		/* 1062: a change only if the read is confirmed (keys_still) */
		bool pchg = pok && (now[0] != state[0] || now[2] != state[2]), gchg = gok && now[1] != state[1];
		uint8_t t, s, again;
		if (pchg && !keys_still(keys_pon_apid, 0x0800u, KEYS_PON_RT, keys_pon_id, keys_pon_type, keys_pon_sub,
		    pon, &t, &s, &again)) {
			if (keys_dropped++ < 40) {
				printf("SWEET KEYS: pon read %#x dropped: type %#x/%#x subtype %#x/%#x again %#x\n",
				    pon, t, keys_pon_type, s, keys_pon_sub, again);
			}
			pok = false;
		}
		if (gchg && !keys_still(keys_gpio_apid, KEYS_GPIO_BASE, KEYS_GPIO_RT, keys_gpio_id, keys_gpio_type,
		    keys_gpio_sub, gpio, &t, &s, &again)) {
			if (keys_dropped++ < 40) {
				printf("SWEET KEYS: volup read %#x dropped: type %#x/%#x subtype %#x/%#x again %#x\n",
				    gpio, t, keys_gpio_type, s, keys_gpio_sub, again);
			}
			gok = false;
		}
		for (int k = 0; k < 3; ++k) {
			if ((k == 1 ? gok : pok) && now[k] != state[k]) {
				state[k] = now[k];
				printf("SWEET KEYS: key=%d down=%d pon=%#x volup:c110=%#x\n",
				    k, state[k], pon, gpio);
				sweet_hid_key(k, state[k]);
			}
		}
		keys_wait();
	}
}

void
sweet_keys_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (keys_started) {
		return;
	}
	keys_started = true;
	(void)PE_parse_boot_argn("sweetkeys", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("sweetkeyscheck", &keys_check, sizeof(keys_check));
	if (!enabled) {
		printf("SWEET KEYS: disabled (sweetkeys=0)\n");
		return;
	}
	keys_core = ml_io_map(KEYS_CORE_PA, 0x2000);
	keys_cnfg = ml_io_map(KEYS_CNFG_PA, 0x1000);
	keys_obs = ml_io_map(KEYS_OBS_PA, 0x10000);
	if (!keys_core || !keys_cnfg || !keys_obs) {
		printf("SWEET KEYS: MMIO mapping failed\n");
		return;
	}
	keys_version = keys_rd(keys_core, 0);
	uint32_t count = keys_rd(keys_core, 4) & 0x7ffu;
	if (keys_version < 0x50000000u || keys_version >= 0x70000000u ||
	    count == 0 || count > 512) {
		printf("SWEET KEYS: unsupported arbiter version=%#x apids=%u\n",
		    keys_version, count);
		return;
	}
	keys_pon_apid = keys_find_apid(0x008, count);
	keys_gpio_apid = keys_find_apid((KEYS_GPIO_SID << 8) | (KEYS_GPIO_BASE >> 8), count);
	uint8_t pon = 0, gpio = 0, mode = 0, en = 0, subtype = 0;
	bool pok = keys_read(keys_pon_apid, KEYS_PON_RT, &pon);
	bool gok = keys_read(keys_gpio_apid, KEYS_GPIO_RT, &gpio);
	bool mok = keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x40u, &mode);
	bool eok = keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x46u, &en);
	bool sok = keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x05u, &subtype);
	printf("SWEET KEYS: arb=%#x apids=%u pon-apid=%u volup-apid=%u pon=%#x/%d gpio=%#x/%d mode=%#x/%d en=%#x/%d subtype=%#x/%d\n",
	    keys_version, count, keys_pon_apid, keys_gpio_apid,
	    pon, pok, gpio, gok, mode, mok, en, eok, subtype, sok);
	/* GPIO configuration belongs to firmware. Diagnose, never repair it here. */
	unsigned dir = subtype >= 0x10u && subtype <= 0x13u ?
	    mode & 3u : (mode >> 4) & 7u;
	if (!eok || !mok || !sok || !(en & 0x80u) || (dir != 0 && dir != 2)) {
		printf("SWEET KEYS: GPIO6 not confirmed enabled as input; volume-up withheld, PMIC unchanged\n");
		keys_gpio_apid = KEYS_NO_APID;
	}
	if (!pok && !gok) {
		printf("SWEET KEYS: no readable keys; polling retained for recovery\n");
	}
	/* 1062: the identity a later key change is checked against (two equal reads each) */
	uint8_t t2 = 0, s2 = 0;
	keys_pon_id = keys_read(keys_pon_apid, 0x0804u, &keys_pon_type) && keys_read(keys_pon_apid, 0x0805u, &keys_pon_sub) &&
	    keys_read(keys_pon_apid, 0x0804u, &t2) && keys_read(keys_pon_apid, 0x0805u, &s2) &&
	    t2 == keys_pon_type && s2 == keys_pon_sub;
	keys_gpio_id = keys_gpio_apid != KEYS_NO_APID && keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x04u, &keys_gpio_type) &&
	    keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x05u, &keys_gpio_sub) &&
	    keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x04u, &t2) && keys_read(keys_gpio_apid, KEYS_GPIO_BASE + 0x05u, &s2) &&
	    t2 == keys_gpio_type && s2 == keys_gpio_sub && keys_gpio_type == 0x10;
	printf("SWEET KEYS: check %s: pon type %#x subtype %#x%s, volup type %#x subtype %#x%s\n", keys_check ? "on" : "off",
	    keys_pon_type, keys_pon_sub, keys_pon_id ? "" : " (unconfirmed)", keys_gpio_type, keys_gpio_sub,
	    keys_gpio_id ? "" : " (unconfirmed)");
	kern_return_t kr = kernel_thread_start(keys_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET KEYS: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
