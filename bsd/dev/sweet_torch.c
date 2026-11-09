/*
 * redfin (Pixel 5) flashlight: the PM8150L flash LED module ("qcom,qpnp-flash-led-v2"), SPMI slave 5,
 * peripheral 0xd3 (registers 0xd300..0xd3ff). The rear flash LED is on channels 0 and 1 together
 * (camera-flash@0: torch-source led:torch_0 + led:torch_1, switch-source led:switch_2, mask 0x3).
 *
 * Sources: the live DT (/soc/qcom,spmi@c440000/qcom,pm8150l@5/qcom,leds@d300: torch current-ma 300,
 * max-current 500, ires-ua 12500, hdrm 325 mV) and drivers/leds/leds-qpnp-flash-v2.c of the redbull
 * kernel (LineageOS android_kernel_google_redbull), read as data:
 *
 *   on    IRES (0x47) = 2 bits per channel (0 = 12.5 mA steps; 3 = 5 mA steps, the PM8150L workaround
 *         for <= 20 mA), STROBE_CTRL(ch) (0x49 + ch) bit 2 clear (software strobe), TGR_CURRENT(ch)
 *         (0x43 + ch) = round(mA / step) - 1, SAFETY_TMR(ch) (0x40 + ch) = 0x13 (no timer: torch),
 *         MOD_CTRL (0x46) bit 7, then EN_LED_CTRL (0x4c) bits 0-1.
 *   off   EN_LED_CTRL bits 0-1 clear, MOD_CTRL bit 7 clear, TGR_CURRENT 0, SAFETY_TMR 0x13.
 *   faults  INT_RT_STS (0x10) bit 0 (led-fault-irq), LED_STATUS1 (0x08) != 0 (open/short),
 *         LED_STATUS2 (0x09) bit 4 (VPH droop), as qpnp_flash_led_irq_handler reads them.
 *         LED_STATUS2 bits 2:0 (OTST) are not faults: Linux reads them only to pick a derating
 *         step after lowering the thresholds itself (r15a lit 50 mA and saw 0x04 after 1.4 s).
 *   heat  the PM8150L's own temperature alarm (SID 4, 0x2400, qcom-spmi-temp-alarm.c: STATUS
 *         0x08, stage [1:0] on GEN1 / state [6:4] on GEN2): any stage above 0 and the light goes off.
 *
 * Writes go through one function that only takes the registers above on that one peripheral (the
 * arbiter channel is checked to be SID 5 / 0xd3 and owned by the APPS EE), and every write is read
 * back. Any surprise turns the light off and stops the driver for the rest of the boot (the
 * writes that turn it off are always allowed, also after a failure: r15a's were not).
 *
 * Boot-arg sweettorch=1: a look only (the module's type, subtype, and registers, nothing written);
 * sweettorch=2: the light can be switched: poke/command line "t MA [MS]" (tools/flame-cmd.py 't 50 1000'),
 * sysctl kern.sweet_torch (any user) = level 0..1000, thousandths of the torch current; 0 is off.
 * sweettorch=3: as 2, plus one 400 ms flash at the lowest current when ready.
 *
 * Limits whatever asks: at most sweettorch_ma per channel (default 100, never above 200: Android's own
 * torch is 300 mA per channel, its maximum 500); on for at most sweettorch_secs at a time (default 120,
 * never above 600), then off until asked again; ten checks a second for the faults above while lit.
 *
 * 0933: the flash for a camera frame (sweet_torch_flash(), /dev/redwood-camera's FLASH): lit at once from the
 * camera's frame loop, with the module's own safety timer (SAFETY_TMR(ch) = 0x80 | (ms / 10 - 1), as
 * leds-qpnp-flash-v2.c sets it for a flash: the LED goes off by itself when the time is up, whatever the software
 * does). At most sweettorch_flash_ma per channel (default 300, never above 300 until there are thermal and VPH
 * droop numbers) for at most 100 ms; a metering pre-flash at the torch's current (<= sweettorch_ma) may last 500 ms.
 * Not while the light is on as a torch (EBUSY). One lock (torch_lock) keeps the camera's calls and this thread's
 * apart.
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
#include "sweet_spmi.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_thermal_torch_dc(void);        /* 1062: sweet_thermal.c */
extern kern_return_t thread_terminate(thread_t);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
void sweet_torch_init(void);
int sweet_torch_set(unsigned ma, unsigned ms);
int sweet_torch_flash(unsigned ma, unsigned *ms, bool pre);
void sweet_torch_flash_off(void);

#define SP_CORE_PA      0x0c440000u
#define SP_CHNLS_PA     0x0c600000u
#define SP_OBS_PA       0x0e600000u
#define SP_CNFG_PA      0x0c40a000u

#define FL_SID          5u
#define FL_PERIPH       0xd3u
#define FL_TYPE         0x04            /* 0x18 = flash LED module */
#define FL_SUBTYPE      0x05
#define FL_STATUS1      0x08
#define FL_STATUS2      0x09
#define  FL_VPH_DROOP   0x10
#define  FL_OTST        0x07
#define FL_INT_RT_STS   0x10
#define  FL_RT_FAULT    0x01
#define FL_SAFETY(ch)   (0x40 + (ch))
#define  FL_SAFETY_OFF  0x13
#define FL_TGR(ch)      (0x43 + (ch))
#define FL_MOD_CTRL     0x46
#define  FL_MOD_EN      0x80
#define FL_IRES         0x47
#define FL_STROBE(ch)   (0x49 + (ch))
#define  FL_SW_STROBE   0x04            /* HW_SW_STROBE_SEL: clear = software */
#define FL_EN_LED       0x4c
#define FL_HDRM(ch)     (0x4d + (ch))
#define FL_CHICKEN      0x87            /* bit 0: current can change while lit */
#define FL_CHANNELS     0x3u            /* led:switch_2 */

#define TORCH_MA_DEFAULT 100u
#define TORCH_MA_CAP     200u
#define TORCH_S_DEFAULT  120u
#define TORCH_S_CAP      600u
#define FLASH_MA_DEFAULT 300u            /* 0933 */
#define FLASH_MA_CAP     300u
#define FLASH_MS_CAP     100u
#define PREFLASH_MS_CAP  500u
/*
 * 1062: the temperature-alarm probe is tried ALARM_TRIES times, ALARM_GAP_MS apart (r5au1f: one read raced
 * sweet_keys.c and the torch refused everything for the whole boot). Without the alarm the torch still works,
 * at most sweettorch_noalarm_ma per channel (default 50, never above sweettorch_ma), and goes off when tsens
 * (the camera block, else the hottest sensor) reaches sweettorch_tsens_dc deci-degC (default 550).
 * sysctl kern.sweet_torch_reinit = 1: probe again (and start the driver again if it had stopped).
 */
#define ALARM_TRIES      5u
#define ALARM_GAP_MS     20u
#define NOALARM_MA_DEFAULT 50u
#define TSENS_DC_DEFAULT 550

struct tc_chan {
	vm_offset_t obs, chnl;  /* read (observer) and write channel of one peripheral */
	unsigned apid, sid, periph;
};
static struct {
	vm_offset_t core, cnfg;
	struct tc_chan fl, alarm;
	bool alarm_gen2;
	bool alarm_ok;                  /* 1062: the PMIC temperature alarm probed fine */
	unsigned noalarm_ma;            /* 1062 */
	int tsens_dc;                   /* 1062 */
	unsigned max_ma, max_s;
	bool ready, dead, lit, on_the_fly;
	bool flash;                     /* 0933: lit as a flash (the safety timer running) */
	unsigned flash_ma, flashes;
	unsigned lit_ma;
	uint64_t lit_since;
	unsigned writes;
} tc;
static int torch_chan;
static int torch_sleep_chan;
static volatile uint32_t torch_req;     /* bit 31 = new; [15:0] mA per channel; 0 = off */
static volatile uint32_t torch_req_ms;  /* 0 = until max_s */
static volatile unsigned torch_level;   /* last sysctl level */
static LCK_GRP_DECLARE(torch_grp, "sweet_torch");
static LCK_MTX_DECLARE(torch_lock, &torch_grp);        /* 0933: the camera's flash calls and this thread */
static volatile uint32_t torch_running;         /* 1062: torch_thread exists */
static volatile uint32_t torch_reinit_req;      /* 1062: kern.sweet_torch_reinit while it runs */
static unsigned torch_reinits;
static int torch_alarm_pub;                     /* 1062: kern.sweet_torch_alarm */

static uint32_t
tc_rd32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
tc_wr32(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
tc_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&torch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* The arbiter channel of one peripheral; the write channel only when the APPS EE (0) owns it. */
static bool
tc_find_channel(struct tc_chan *c, unsigned sid, unsigned periph, bool want_write)
{
	unsigned ppid = (sid << 8) | periph, n = tc_rd32(tc.core, 4) & 0x7ffu, apid = 0xffff, own = 7;
	uint32_t version = tc_rd32(tc.core, 0);
	if (version < 0x50000000u || version >= 0x70000000u) {
		printf("SWEET TORCH: SPMI arbiter %#x is not v5\n", version);
		return false;
	}
	for (unsigned i = 0; i < n && i < 512; i++) {
		uint32_t map = tc_rd32(tc.core, 0x900u + 4u * i);
		if (!map || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		unsigned o = tc_rd32(tc.cnfg, 0x700u + 4u * i) & 7u;
		if (apid == 0xffff || o == 0) {
			apid = i;
			own = o;
		}
	}
	if (apid == 0xffff) {
		printf("SWEET TORCH: no arbiter channel for %x:%x\n", sid, periph << 8);
		return false;
	}
	c->apid = apid;
	c->sid = sid;
	c->periph = periph;
	c->obs = ml_io_map(SP_OBS_PA + 0x80u * apid, 0x1000);
	c->chnl = want_write && own == 0 ? ml_io_map(SP_CHNLS_PA + 0x10000u * apid, 0x1000) : 0;
	printf("SWEET TORCH: %x:%x on APID %u, owner EE %u%s\n", sid, periph << 8, apid, own,
	    !want_write ? "" : own == 0 ? "" : " (not ours: read only)");
	return c->obs != 0;
}

/* One transfer; retried when the arbiter dropped it (as HackiOS redwood_torch.c). */
static int
tc_xfer(const struct tc_chan *c, bool write, uint8_t reg, uint8_t *val)
{
	vm_offset_t ch = write ? c->chnl : c->obs;
	unsigned at = (c->periph << 8) + reg;
	if (!ch) {
		return -1;
	}
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		int bus = sweet_spmi_bus_lock();        /* 1062: command, status and data as one */
		if (write) {
			tc_wr32(ch, 0x10, *val);
		}
		tc_wr32(ch, 0x00, ((write ? 0u : 1u) << 27) | ((uint32_t)reg << 4));
		uint32_t st = 0;
		uint8_t data = 0;
		unsigned i;
		for (i = 0; i < 1000; i++) {
			st = tc_rd32(ch, 0x08);
			if (st & 1u) {
				break;
			}
			delay(1);
		}
		if (i < 1000 && !write && !(st & 0xeu)) {
			data = (uint8_t)tc_rd32(ch, 0x18);
		}
		sweet_spmi_bus_unlock(bus);
		if (i == 1000) {
			printf("SWEET TORCH: spmi %s %x:%#x timeout\n", write ? "write" : "read", c->sid, at);
			tc.dead = true;
			return -1;
		}
		if (st & 0x8u) {
			continue;       /* dropped */
		}
		if (st & 0x6u) {
			printf("SWEET TORCH: spmi %s %x:%#x status %#x\n", write ? "write" : "read", c->sid, at, st);
			return -1;
		}
		if (!write) {
			*val = data;
		}
		return 0;
	}
	printf("SWEET TORCH: spmi %x:%#x dropped 3 times\n", c->sid, at);
	return -1;
}

static int
tc_read(uint8_t reg, uint8_t *val)
{
	return tc_xfer(&tc.fl, false, reg, val);
}

static bool
tc_writable(uint8_t reg)
{
	switch (reg) {
	case FL_SAFETY(0): case FL_SAFETY(1):
	case FL_TGR(0): case FL_TGR(1):
	case FL_MOD_CTRL: case FL_IRES:
	case FL_STROBE(0): case FL_STROBE(1):
	case FL_EN_LED:
	case 0x50: case 0x51: case 0x52: case 0x55: case 0x5b: case 0x61: case 0x62: case 0x76:
		return true;
	default:
		return false;
	}
}

/*
 * masked write, read back; any mismatch stops the driver. dark = a write that only switches the
 * light off or its current to 0: allowed after the driver stopped, and tried even if the read fails.
 */
static int
tc_write_x(uint8_t reg, uint8_t mask, uint8_t val, bool dark)
{
	uint8_t old = 0, back = 0;
	if (!tc_writable(reg) || (tc.dead && !dark)) {
		return -1;
	}
	if (tc_read(reg, &old)) {
		tc.dead = true;
		if (!dark) {
			return -1;
		}
		old = 0;
	}
	uint8_t v = (uint8_t)((old & ~mask) | (val & mask));
	if (tc_xfer(&tc.fl, true, reg, &v) || tc_read(reg, &back)) {
		tc.dead = true;
		return -1;
	}
	if ((back & mask) != (val & mask)) {
		printf("SWEET TORCH: %#x wrote %#x read back %#x: stopping\n", 0xd300 + reg, v, back);
		tc.dead = true;
		return -1;
	}
	if (tc.writes++ < 40) {
		printf("SWEET TORCH: %#x %#x -> %#x\n", 0xd300 + reg, old, back);
	}
	return 0;
}

static int
tc_write(uint8_t reg, uint8_t mask, uint8_t val)
{
	return tc_write_x(reg, mask, val, false);
}

static void
tc_dump(const char *why)
{
	static const uint8_t regs[] = { FL_TYPE, FL_SUBTYPE, FL_STATUS1, FL_STATUS2, FL_INT_RT_STS,
		                        FL_SAFETY(0), FL_SAFETY(1), FL_TGR(0), FL_TGR(1), FL_MOD_CTRL, FL_IRES,
		                        FL_STROBE(0), FL_STROBE(1), FL_EN_LED, FL_HDRM(0), FL_HDRM(1), 0x48, 0x50, 0x51, 0x52,
		                        0x55, 0x56, 0x57, 0x58, 0x5b, 0x61, 0x62, 0x67, 0x6e, 0x70, 0x71, 0x76, FL_CHICKEN };
	char line[300];
	int n = snprintf(line, sizeof(line), "SWEET TORCH: %s:", why);
	for (unsigned i = 0; i < sizeof(regs) && n < (int)sizeof(line) - 12; i++) {
		uint8_t v = 0;
		if (tc_read(regs[i], &v)) {
			n += snprintf(line + n, sizeof(line) - n, " %02x=??", regs[i]);
		} else {
			n += snprintf(line + n, sizeof(line) - n, " %02x=%02x", regs[i], v);
		}
	}
	printf("%s\n", line);
}

/*
 * qpnp_flash_led_init_settings() with this DT's values (no thermal-thrsh, derate slow/fast, droop or ramp
 * properties: the driver's defaults). r15b: lit, then INT_RT_STS led-fault after ~1.7 s with
 * STATUS1/2 clear; headroom auto mode (0x50), which Linux turns off, is the first suspect.
 */
static int
tc_linux_init(void)
{
	static const struct { uint8_t reg, mask, val; } init[] = {
		{ 0x50, 0xff, 0x00 },   /* HDRM_AUTO_MODE_CTRL: off */
		{ 0x52, 0x03, 0x03 },   /* ISC_DELAY: FLASH_LED_ISC_DELAY_DEFAULT */
		{ 0x51, 0x03, 0x02 },   /* WARMUP_DELAY: FLASH_LED_WARMUP_DELAY_DEFAULT */
		{ 0x76, 0x07, 0x07 },   /* CURRENT_DERATE_EN: vph droop, open, short circuit detection */
		{ 0x55, 0x80, 0x80 },   /* THERMAL_RMP_DN_RATE: OTST1 ramp back up */
		{ 0x5b, 0x77, 0x55 },   /* RGLR_RAMP_RATE: 6.4 us up and down */
		{ 0x62, 0x03, 0x02 },   /* VPH_DROOP_DEBOUNCE */
		{ 0x61, 0x07, 0x05 },   /* VPH_DROOP_THRESHOLD */
		{ 0x61, 0x30, 0x20 },   /* ... hysteresis */
	};
	int err = 0;
	for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]) && !err; i++) {
		err |= tc_write(init[i].reg, init[i].mask, init[i].val);
	}
	return err;
}

static void
tc_off(const char *why)
{
	bool was = tc.lit;
	int err = tc_write_x(FL_EN_LED, FL_CHANNELS, 0, true);
	err |= tc_write_x(FL_MOD_CTRL, FL_MOD_EN, 0, true);
	for (unsigned ch = 0; ch < 2; ch++) {
		err |= tc_write_x(FL_TGR(ch), 0x7f, 0, true);
		err |= tc_write_x(FL_SAFETY(ch), 0xff, FL_SAFETY_OFF, true);
	}
	if (err) {
		printf("SWEET TORCH: switching off did not read back (%s)\n", why);
	}
	tc.lit = false;
	tc.flash = false;
	tc.lit_ma = 0;
	if (was) {
		uint64_t ns;
		absolutetime_to_nanoseconds(mach_absolute_time() - tc.lit_since, &ns);
		printf("SWEET TORCH: off (%s) after %llu ms%s\n", why, ns / NSEC_PER_MSEC, tc.dead ? ", driver stopped" : "");
	}
}

/* mA per channel -> IRES bits and current code (get_current_reg_code) */
static void
tc_code(unsigned ma, uint8_t *ires, uint8_t *code)
{
	unsigned step_ua = ma <= 20 ? 5000u : 12500u;
	unsigned idx = ma <= 20 ? 3u : 0u;
	unsigned c = (ma * 1000u + step_ua / 2) / step_ua;
	*ires = (uint8_t)(idx | (idx << 2));
	*code = (uint8_t)(c ? c - 1 : 0);
}

static int
tc_on(unsigned ma)
{
	uint8_t ires, code;
	tc_code(ma, &ires, &code);
	if (tc.lit && tc.flash) {
		tc_off("the torch after a flash");      /* 0933: (the flash's safety timer would cut the torch short) */
	}
	if (tc.lit && tc.on_the_fly) {
		int err = tc_write(FL_IRES, 0x0f, ires);
		err |= tc_write(FL_TGR(0), 0x7f, code);
		err |= tc_write(FL_TGR(1), 0x7f, code);
		if (!err) {
			tc.lit_ma = ma;
			printf("SWEET TORCH: %u mA per channel (code %u, ires %#x)\n", ma, code, ires);
		}
		return err;
	}
	if (tc.lit) {
		tc_off("current change");
	}
	int err = tc_write(FL_IRES, 0x0f, ires);
	for (unsigned ch = 0; ch < 2 && !err; ch++) {
		err |= tc_write(FL_STROBE(ch), FL_SW_STROBE, 0);
		err |= tc_write(FL_TGR(ch), 0x7f, code);
		err |= tc_write(FL_SAFETY(ch), 0xff, FL_SAFETY_OFF);
	}
	if (!err) {
		err |= tc_write(FL_MOD_CTRL, FL_MOD_EN, FL_MOD_EN);
	}
	if (!err) {
		err |= tc_write(FL_EN_LED, FL_CHANNELS, FL_CHANNELS);
	}
	if (err) {
		tc_off("write failed");
		return -1;
	}
	tc.lit = true;
	tc.lit_ma = ma;
	tc.lit_since = mach_absolute_time();
	printf("SWEET TORCH: on, %u mA per channel (code %u, ires %#x)\n", ma, code, ires);
	return 0;
}

/* 0933: lit as a flash: ma per channel, the module's safety timer at ms (10 ms steps) */
static int
tc_flash_on(unsigned ma, unsigned ms)
{
	uint8_t ires, code, tmr = (uint8_t)(0x80u | (ms / 10u - 1u));
	tc_code(ma, &ires, &code);
	if (tc.lit) {
		tc_off("a new flash");
	}
	int err = tc_write(FL_IRES, 0x0f, ires);
	for (unsigned ch = 0; ch < 2 && !err; ch++) {
		err |= tc_write(FL_STROBE(ch), FL_SW_STROBE, 0);
		err |= tc_write(FL_TGR(ch), 0x7f, code);
		err |= tc_write(FL_SAFETY(ch), 0xff, tmr);
	}
	if (!err) {
		err |= tc_write(FL_MOD_CTRL, FL_MOD_EN, FL_MOD_EN);
	}
	if (!err) {
		err |= tc_write(FL_EN_LED, FL_CHANNELS, FL_CHANNELS);
	}
	if (err) {
		tc_off("flash write failed");
		return -1;
	}
	tc.lit = true;
	tc.flash = true;
	tc.lit_ma = ma;
	tc.lit_since = mach_absolute_time();
	if (tc.flashes++ < 20) {
		printf("SWEET TORCH: flash, %u mA per channel for at most %u ms (safety timer %#x)\n", ma, ms, tmr);
	}
	return 0;
}

/* faults while lit: true = keep going */
static bool
tc_check(void)
{
	uint8_t rt = 0, s1 = 0, s2 = 0, alarm = 0;
	static uint8_t otst_said = 0xff;
	if (tc_read(FL_INT_RT_STS, &rt) || tc_read(FL_STATUS1, &s1) || tc_read(FL_STATUS2, &s2) ||
	    (tc.alarm_ok && tc_xfer(&tc.alarm, false, 0x08, &alarm))) {
		tc.dead = true;
		return false;
	}
	unsigned stage = tc.alarm_gen2 ? (alarm >> 4) & 7u : alarm & 3u;
	static unsigned bad_in_a_row, said;
	if (stage) {
		printf("SWEET TORCH: PM8150L temperature stage %u: off\n", stage);
		tc.dead = true;
		return false;
	}
	if (!tc.alarm_ok) {     /* 1062: tsens instead; off (not stopped) when hot */
		int dc = sweet_thermal_torch_dc();
		if (dc != -9999 && dc >= tc.tsens_dc) {
			printf("SWEET TORCH: tsens %d.%d C >= %d.%d C (no temp alarm): off\n", dc / 10, dc % 10,
			    tc.tsens_dc / 10, tc.tsens_dc % 10);
			return false;
		}
	}
	/*
	 * r15c: STATUS1 0x1 and INT_RT_STS 0x4 for one check, 0.7 s after lighting, gone at the next read
	 * with the LED still ramped up (0xd0). Linux only logs these (qpnp_flash_led_irq_handler); the
	 * module protects itself from a real short. So only a fault that stays for 3 checks counts.
	 */
	if ((rt & FL_RT_FAULT) || s1 || (s2 & FL_VPH_DROOP)) {
		if (said++ < 20) {
			printf("SWEET TORCH: fault flags: rt_sts %#x status1 %#x status2 %#x (%u in a row)\n", rt, s1, s2,
			    bad_in_a_row + 1);
		}
		if (++bad_in_a_row >= 3) {
			tc.dead = true;
			return false;
		}
	} else {
		bad_in_a_row = 0;
	}
	if ((s2 & FL_OTST) != otst_said) {
		otst_said = s2 & FL_OTST;
		printf("SWEET TORCH: OTST %#x (derating band, not a fault) at %u mA\n", otst_said, tc.lit_ma);
	}
	return true;
}

/* 1062: the cap per channel now (lower without the temperature alarm) */
static unsigned
tc_cap_ma(void)
{
	return tc.alarm_ok || tc.noalarm_ma > tc.max_ma ? tc.max_ma : tc.noalarm_ma;
}

/* 1062: the PM8150L temperature alarm (SID 4, 0x2400): TYPE 0x09, SUBTYPE 0x08 (GEN1) / 0x09 (GEN2), STATUS */
static bool
tc_probe_alarm(const char *why)
{
	uint8_t atype = 0, asub = 0, astatus = 0;
	tc.alarm_ok = false;
	torch_alarm_pub = 0;
	for (unsigned t = 1; t <= ALARM_TRIES; t++) {
		atype = asub = astatus = 0;
		if (!tc_xfer(&tc.alarm, false, 0x04, &atype) && !tc_xfer(&tc.alarm, false, 0x05, &asub) &&
		    !tc_xfer(&tc.alarm, false, 0x08, &astatus) && atype == 0x09 && (asub == 0x08 || asub == 0x09)) {
			tc.alarm_gen2 = asub == 0x09;
			tc.alarm_ok = true;
			torch_alarm_pub = 1;
			printf("SWEET TORCH: PM8150L temperature alarm GEN%u, status %#x (%s, try %u)\n", tc.alarm_gen2 ? 2 : 1,
			    astatus, why, t);
			return true;
		}
		printf("SWEET TORCH: PM8150L temperature alarm: type %#x subtype %#x: try %u of %u failed (%s)%s\n", atype, asub,
		    t, ALARM_TRIES, why, t < ALARM_TRIES ? ", again in 20 ms" : "");
		if (t < ALARM_TRIES) {
			tc_sleep(ALARM_GAP_MS);
		}
	}
	printf("SWEET TORCH: no temp alarm: torch limited to %u mA / thermal cutoff via tsens at %d.%d C (now %d)\n",
	    tc_cap_ma(), tc.tsens_dc / 10, tc.tsens_dc % 10, sweet_thermal_torch_dc());
	return false;
}

static void
torch_thread(void *arg, wait_result_t wr __unused)
{
	uint32_t mode = 0;
	PE_parse_boot_argn("sweettorch", &mode, sizeof(mode));
	tc.max_ma = TORCH_MA_DEFAULT;
	tc.max_s = TORCH_S_DEFAULT;
	PE_parse_boot_argn("sweettorch_ma", &tc.max_ma, sizeof(tc.max_ma));
	PE_parse_boot_argn("sweettorch_secs", &tc.max_s, sizeof(tc.max_s));
	tc.max_ma = tc.max_ma == 0 ? TORCH_MA_DEFAULT : tc.max_ma > TORCH_MA_CAP ? TORCH_MA_CAP : tc.max_ma;
	tc.flash_ma = FLASH_MA_DEFAULT;
	PE_parse_boot_argn("sweettorch_flash_ma", &tc.flash_ma, sizeof(tc.flash_ma));
	tc.flash_ma = tc.flash_ma == 0 ? FLASH_MA_DEFAULT : tc.flash_ma > FLASH_MA_CAP ? FLASH_MA_CAP : tc.flash_ma;
	tc.max_s = tc.max_s == 0 ? TORCH_S_DEFAULT : tc.max_s > TORCH_S_CAP ? TORCH_S_CAP : tc.max_s;
	tc.noalarm_ma = NOALARM_MA_DEFAULT;
	PE_parse_boot_argn("sweettorch_noalarm_ma", &tc.noalarm_ma, sizeof(tc.noalarm_ma));
	tc.noalarm_ma = tc.noalarm_ma == 0 ? 1 : tc.noalarm_ma;
	tc.tsens_dc = TSENS_DC_DEFAULT;
	PE_parse_boot_argn("sweettorch_tsens_dc", &tc.tsens_dc, sizeof(tc.tsens_dc));
	if (arg == NULL) {
		tc_sleep(6000);
	} else {
		printf("SWEET TORCH: started again (kern.sweet_torch_reinit)\n");
	}
	if (!tc.core) {
		tc.core = ml_io_map(SP_CORE_PA, 0x2000);
		tc.cnfg = ml_io_map(SP_CNFG_PA, 0x1000);
	}
	if (!tc.core || !tc.cnfg || (!tc.fl.obs && !tc_find_channel(&tc.fl, FL_SID, FL_PERIPH, mode >= 2)) ||
	    (!tc.alarm.obs && !tc_find_channel(&tc.alarm, 4, 0x24, false))) {
		goto out;
	}
	(void)tc_probe_alarm("start");  /* 1062: retried; without it the torch is limited, not refused */
	uint8_t type = 0, sub = 0, chicken = 0;
	if (tc_read(FL_TYPE, &type) || tc_read(FL_SUBTYPE, &sub)) {
		goto out;
	}
	(void)tc_read(FL_CHICKEN, &chicken);
	tc.on_the_fly = sub >= 4 && (chicken & 1u);     /* PM6150L_FLASH_SUBTYPE onwards */
	tc_dump("found");
	if (type != 0x18) {
		printf("SWEET TORCH: %x:%x is type %#x, not a flash LED module\n", FL_SID, FL_PERIPH << 8, type);
		goto out;
	}
	if (mode < 2 || !tc.fl.chnl) {
		printf("SWEET TORCH: look only (sweettorch=%u)\n", mode);
		goto out;
	}
	/* Start from a known dark state, then Linux's settings. */
	tc_off("start");
	if (tc.dead || tc_linux_init()) {
		tc_off("init failed");
		goto out;
	}
	tc_dump("configured");
	tc.ready = true;
	printf("SWEET TORCH: ready: at most %u mA per channel, %u s at a time; current change while lit %s%s\n",
	    tc_cap_ma(), tc.max_s, tc.on_the_fly ? "yes" : "no", tc.alarm_ok ? "" : "; no temp alarm (tsens cutoff)");
	if (mode >= 3) {
		(void)sweet_torch_set(5, 400);
	}
	uint64_t off_at = 0;
	while (!tc.dead) {
		uint64_t deadline;
		clock_interval_to_deadline(tc.lit ? 100 : 60000, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&torch_chan, THREAD_UNINT, deadline);
		if ((torch_req & (1u << 31)) || torch_reinit_req) {
			clear_wait(current_thread(), THREAD_AWAKENED);
		} else {
			thread_block(THREAD_CONTINUE_NULL);
		}
		lck_mtx_lock(&torch_lock);
		if (__atomic_exchange_n(&torch_reinit_req, 0, __ATOMIC_ACQ_REL)) {
			(void)tc_probe_alarm("kern.sweet_torch_reinit");      /* 1062 */
			if (tc.lit && !tc.flash && tc.lit_ma > tc_cap_ma()) {
				(void)tc_on(tc_cap_ma());
			}
		}
		uint32_t req = __atomic_exchange_n(&torch_req, 0, __ATOMIC_ACQ_REL);
		uint64_t now = mach_absolute_time();
		if (req & (1u << 31)) {
			unsigned ma = req & 0xffffu, ms = torch_req_ms, cap = tc_cap_ma();
			if (ma == 0) {
				tc_off("asked");
			} else if (tc_on(ma > cap ? cap : ma) == 0) {
				uint64_t span;
				unsigned limit = ms && ms < tc.max_s * 1000u ? ms : tc.max_s * 1000u;
				nanoseconds_to_absolutetime((uint64_t)limit * NSEC_PER_MSEC, &span);
				off_at = now + span;
			}
		}
		if (tc.lit) {
			if (!tc_check() && !tc.dead) {
				tc_off("tsens");       /* 1062: hot without the alarm: off, the driver stays */
				torch_level = 0;
			} else if (tc.dead) {
				uint8_t raw[0x16];
				for (unsigned i = 0; i < sizeof(raw); i++) {
					if (tc_read((uint8_t)(0x06 + i), &raw[i])) {
						raw[i] = 0xee;
					}
				}
				tc_off("fault");
				printf("SWEET TORCH: at the fault 06..1b: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
				    "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", raw[0], raw[1], raw[2], raw[3],
				    raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14],
				    raw[15], raw[16], raw[17], raw[18], raw[19], raw[20], raw[21]);
				tc_dump("after fault");
			} else if (tc.flash) {
				/* 0933: a flash ends by itself (its safety timer); the camera switches it off */
			} else if (now >= off_at) {
				tc_off(torch_req_ms ? "time" : "time limit");
				torch_level = 0;
			}
		}
		lck_mtx_unlock(&torch_lock);
	}
	lck_mtx_lock(&torch_lock);
	tc_off("stopped");
	lck_mtx_unlock(&torch_lock);
out:
	tc.ready = false;
	__atomic_store_n(&torch_running, 0, __ATOMIC_RELEASE);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* Light the torch at ma per channel (0 = off) for ms (0 = until the time limit), from any thread. */
int
sweet_torch_set(unsigned ma, unsigned ms)
{
	if (!tc.ready || tc.dead) {
		return ENXIO;
	}
	torch_req_ms = ms;
	__atomic_store_n(&torch_req, (1u << 31) | (ma & 0xffffu), __ATOMIC_RELEASE);
	thread_wakeup(&torch_chan);
	return 0;
}

/*
 * 0933: the flash for a camera frame, lit now (synchronously, from the camera's frame loop): ma per channel (at most
 * sweettorch_flash_ma; pre: a metering pre-flash, at most the torch's current), for at most *ms (10..100 ms, a
 * pre-flash 10..500), which the module's safety timer enforces; *ms says what was set. 0 lit; EBUSY while the torch
 * is on; ENXIO without the driver; EIO a write that failed (the driver stops).
 */
int
sweet_torch_flash(unsigned ma, unsigned *ms, bool pre)
{
	if (!tc.ready || tc.dead) {
		return ENXIO;
	}
	unsigned cap_ma = pre ? tc.max_ma : tc.flash_ma, cap_ms = pre ? PREFLASH_MS_CAP : FLASH_MS_CAP;
	ma = ma == 0 ? 1 : ma > cap_ma ? cap_ma : ma;
	unsigned t = *ms < 10 ? 10 : *ms > cap_ms ? cap_ms : *ms;
	*ms = t / 10 * 10;
	lck_mtx_lock(&torch_lock);
	int err = 0;
	if (tc.lit && !tc.flash) {
		err = EBUSY;
	} else if (tc.dead || tc_flash_on(ma, *ms) != 0) {
		err = EIO;
	}
	lck_mtx_unlock(&torch_lock);
	if (!err) {
		thread_wakeup(&torch_chan);     /* (the fault checks while lit) */
	}
	return err;
}

/* 0933: the flash off (after the lit frame's end); nothing if it is not lit as a flash */
void
sweet_torch_flash_off(void)
{
	if (!tc.ready) {
		return;
	}
	lck_mtx_lock(&torch_lock);
	if (tc.lit && tc.flash) {
		tc_off("flash done");
	}
	lck_mtx_unlock(&torch_lock);
}

static int
sysctl_sweet_torch SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)torch_level, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value < 0 || value > 1000) {
		return EINVAL;
	}
	torch_level = (unsigned)value;
	unsigned ma = value == 0 ? 0 : (tc_cap_ma() * (unsigned)value + 999u) / 1000u;
	return sweet_torch_set(ma, 0);
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_torch, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_torch, "I", "SWEET flashlight: level 0..1000 (0 off)");

/* 1062: write 1 = probe the temperature alarm again; if the driver had stopped, start it again (no 6 s wait) */
static int
sysctl_sweet_torch_reinit SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int value = 0, changed = 0;
	int error = sysctl_io_number(req, (int)torch_reinits, sizeof(value), &value, &changed);
	if (error || !changed) {
		return error;
	}
	if (value != 1) {
		return EINVAL;
	}
	uint32_t mode = 0;
	PE_parse_boot_argn("sweettorch", &mode, sizeof(mode));
	if (!mode) {
		return ENODEV;
	}
	torch_reinits++;
	if (__atomic_load_n(&torch_running, __ATOMIC_ACQUIRE) && tc.ready && !tc.dead) {
		__atomic_store_n(&torch_reinit_req, 1, __ATOMIC_RELEASE);
		thread_wakeup(&torch_chan);
		return 0;
	}
	uint32_t idle = 0;
	if (!__atomic_compare_exchange_n(&torch_running, &idle, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return EBUSY;   /* still starting or stopping */
	}
	lck_mtx_lock(&torch_lock);
	tc.dead = false;
	tc.lit = tc.flash = false;
	lck_mtx_unlock(&torch_lock);
	thread_t t;
	if (kernel_thread_start(torch_thread, (void *)1, &t) != KERN_SUCCESS) {
		__atomic_store_n(&torch_running, 0, __ATOMIC_RELEASE);
		return ENOMEM;
	}
	thread_deallocate(t);
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, sweet_torch_reinit, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_sweet_torch_reinit, "I", "1062: 1 = probe the temperature alarm again / restart the torch driver");
SYSCTL_INT(_kern, OID_AUTO, sweet_torch_alarm, CTLFLAG_RD | CTLFLAG_LOCKED, &torch_alarm_pub, 0,
    "1062: 1 = the PMIC temperature alarm is in use");

void
sweet_torch_init(void)
{
	uint32_t mode = 0;
	PE_parse_boot_argn("sweettorch", &mode, sizeof(mode));
	if (!mode) {
		return;
	}
	thread_t t;
	__atomic_store_n(&torch_running, 1, __ATOMIC_RELEASE);
	if (kernel_thread_start(torch_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	} else {
		__atomic_store_n(&torch_running, 0, __ATOMIC_RELEASE);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
