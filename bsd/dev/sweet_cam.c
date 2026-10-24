/*
 * redfin (Pixel 5) cameras, step 1: one sensor powered up as Android powers it, its chip ID read,
 * powered down again. After HackiOS's redwood_camera.c (cam=1 / cam=2), with this phone's numbers.
 *
 * The three sensors (live DT cam-sensor@0..2 and Android's module files
 * /vendor/lib64/camera/com.qti.sensormodule.*_lito2.bin, read as data):
 *
 *   0  rear, Sony IMX363 (metric_imx363): CCI 0 master 0 (GPIO 17 data, 18 clock), slave 0x20 (8-bit),
 *      chip ID 0x0363 in the 16-bit register 0x0016; reset GPIO 23; MCLK0 (camcc RCG 0x5004, branch
 *      0x501c, GPIO 13, 4 mA). Power-up: VIO (SLG51000 LDO7 1.8 V), VANA (LDO3 2.85 V), VAF (camera_ldo,
 *      a fixed 2.85 V regulator switched by PM8150L GPIO 8), VDIG (LDO1 2.85 V), custom1 (LDO4 1.8 V),
 *      custom2 (LDO6 1.1 V), 1 ms each; reset high, 1 ms; MCLK, 10 ms.
 *   1  rear ultra-wide, Sony IMX481 (metric_imx481): CCI 0 master 1 (GPIO 19/20), slave 0x34, ID 0x0481;
 *      reset GPIO 25; MCLK1 (0x5024/0x503c, GPIO 14, 4 mA). VIO (LDO7 1.8 V), VANA (LDO2 2.85 V), VDIG (LDO5
 *      1.1 V), MCLK, reset; 1 ms each.
 *   2  front, Sony IMX355 (primax_imx355): CCI 1 master 0 (GPIO 27/28), slave 0x34, ID 0x0355; reset GPIO
 *      21; MCLK2 (0x5044/0x505c, GPIO 15, 2 mA). VIO (LDO7 1.8 V), custom GPIO 1 (PM8150L GPIO 2) high,
 *      1 ms; MCLK, 1 ms; reset, 12 ms.
 *   Power-down: the module files' own order (the reverse, VAF given 20 ms).
 *
 * The SLG51000 (Dialog, I2C 0x75 on GENI SE9 -- the DRV2624's bus, bsd/dev/sweet_haptic.c, so
 * sweethaptic=1 is needed -- 16-bit registers): chip select PM8150L GPIO 4 high, its two enable
 * lines PM8150L GPIO 9/10 low, 10 ms, then Google's tuning (slg51000_config_tuning(): software test
 * mode, LDO3_CONF1 0x28, LDO5_VSEL 0x9f, MUXARRAY_INPUT_SEL_39 0x34, LUT_VAL_3 0xea, startup current
 * limit [7:5] 0, test mode off), as drivers/regulator/slg51000-regulator.c does at every boot.
 * An LDO: VSEL (0x2000/2200/2300/2500/2700/2900/3100) = (uV - base) / step, kept within its own
 * MINV..MAXV, then its bit in MATRIX_CONF_A (0x110d); its STATUS (+0xc1) must show VOUT_OK and no
 * ILIM within 10 ms. Only the device tree's voltages are ever set.
 *
 * MCLK here is the crystal's 19.2 MHz (RCG source 0, divider 1; Sony sensors take 6-27 MHz), not the
 * module files' 24 MHz (camcc PLL2): enough to answer on I2C. Before it: camcc's GDSC clock
 * (0xc1a0, critical in camcc-lito.c) and the titan top GDSC (0xc1c4), as the sensor's cam_clk supply.
 *
 * The sensor's I2C is done by hand on its CCI pins (GPIO function 0, pull-up, open drain by the
 * output enable), at most 100 kHz, the pins given back as they were found: no CCI block, no CCI clock.
 *
 * r16g (2026-10-24): all three answer -- IMX355 0x0355, IMX481 0x0481, IMX363 0x0363 -- each powered up in its
 * module file's order (LDOs ready within 1 ms) and down again.
 *
 * Boot-arg sweetcam=1: a look only (clocks, GDSC, pins, the PMIC GPIOs, the SLG51000 if it answers).
 * sweetcam=2: and the probe on request: poke/command line "c N" (N = 0, 1, 2; 9 = the look again),
 * tools/flame-cmd.py 'c 0'; 10 = the PMIC GPIOs, 11 = the SLG51000's ID register, 12 = camcc's
 * clocks (after the camera XO clock is switched on). sweetcam=3: and all three probed once at boot.
 * Any write that does not read back, a rail that does not come up or reports over-current, a byte
 * nobody acknowledges on the PMIC's bus, and everything switched on so far is switched off again in
 * the power-down order. Whatever was found on is left on.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int sweet_se9_xfer(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr);
void sweet_cam_init(void);
int sweet_cam_cmd(unsigned what);

/*
 * Only the pages used are mapped, once: XNU's I/O map is 8 MiB (osfmk/arm/io_map.c, raised by 0920) and
 * nothing is ever unmapped; r16a-r16e reset ~8 s after this driver mapped ~600 KB (GCC 384 KB, camcc 64 KB,
 * TLMM 128 KB), when SpringBoard's first frames needed mappings of their own (io_map is KMA_NOFAIL).
 */
#define GCC_CAM_PA      0x0010b000u     /* GCC page with the camera AHB and XO branches */
#define CAMCC_MCLK_PA   0x0ad05000u     /* camcc page with MCLK0-4 */
#define CAMCC_TOP_PA    0x0ad0c000u     /* camcc page with the GDSC clock and the titan top GDSC */
#define TLMM_CAM_PA     0x0f90d000u     /* TLMM EAST tile from GPIO 13: GPIO 13..28 */
#define TLMM_FIRST      13u
#define SP_CORE_PA      0x0c440000u
#define SP_CHNLS_PA     0x0c600000u
#define SP_OBS_PA       0x0e600000u
#define SP_CNFG_PA      0x0c40a000u

#define GCC_CAMERA_AHB  0x008u          /* GCC 0xb008, critical in gcc-lito.c */
#define GCC_CAMERA_XO   0x03cu          /* GCC 0xb03c */
#define CAMCC_GDSC_CLK  0x1a0u          /* camcc 0xc1a0 */
#define TITAN_GDSCR     0x1c4u          /* camcc 0xc1c4: [0] collapse, [31] on */
#define CBCR_OFF        (1u << 31)

#define GPIO_CFG(n)     (0x1000u * ((n) - TLMM_FIRST))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)      (0x1000u * ((n) - TLMM_FIRST) + 4u)    /* [0] in, [1] out */
#define CFG_PULL_UP     3u
#define CFG_OUTPUT      (1u << 9)
#define CFG_FUNC(f)     ((uint32_t)(f) << 2)
#define CFG_MA(ma)      ((uint32_t)((ma) / 2 - 1) << 6)

#define SLG_ADDR        0x75
#define PGPIO_SID       4u              /* PM8150L */
#define PGPIO(n)        (0xc0u + (n) - 1u)      /* GPIO n's peripheral */

enum { ST_END, ST_LDO, ST_PGPIO, ST_RESET, ST_MCLK };
struct step {
	uint8_t kind, n;                /* LDO number / PMIC GPIO number */
	uint16_t mv;
	uint16_t ms;                    /* the delay after the step */
};
struct sensor {
	const char *name;
	unsigned sda, scl, reset, mclk_pin, mclk_ma;
	uint32_t mclk_rcg, mclk_cbcr;   /* offsets in camcc's MCLK page (camcc 0x5000 + ...) */
	uint8_t slave;                  /* 7-bit */
	uint16_t id;
	struct step up[10], down[10];
};
static const struct sensor sensors[] = {
	{ "rear IMX363", 17, 18, 23, 13, 4, 0x004, 0x01c, 0x10, 0x0363,
	  { { ST_LDO, 7, 1800, 1 }, { ST_LDO, 3, 2850, 1 }, { ST_PGPIO, 8, 0, 0 }, { ST_LDO, 1, 2850, 1 },
	    { ST_LDO, 4, 1800, 1 }, { ST_LDO, 6, 1100, 1 }, { ST_RESET, 0, 0, 1 }, { ST_MCLK, 0, 0, 10 }, { ST_END, 0, 0, 0 } },
	  { { ST_MCLK, 0, 0, 1 }, { ST_RESET, 0, 0, 1 }, { ST_LDO, 6, 0, 1 }, { ST_LDO, 4, 0, 1 }, { ST_LDO, 1, 0, 1 },
	    { ST_PGPIO, 8, 0, 20 }, { ST_LDO, 3, 0, 1 }, { ST_LDO, 7, 0, 1 }, { ST_END, 0, 0, 0 } } },
	{ "rear ultra-wide IMX481", 19, 20, 25, 14, 4, 0x024, 0x03c, 0x1a, 0x0481,
	  { { ST_LDO, 7, 1800, 1 }, { ST_LDO, 2, 2850, 1 }, { ST_LDO, 5, 1100, 1 }, { ST_MCLK, 0, 0, 1 },
	    { ST_RESET, 0, 0, 1 }, { ST_END, 0, 0, 0 } },
	  { { ST_RESET, 0, 0, 1 }, { ST_MCLK, 0, 0, 1 }, { ST_LDO, 5, 0, 1 }, { ST_LDO, 2, 0, 1 }, { ST_LDO, 7, 0, 1 },
	    { ST_END, 0, 0, 0 } } },
	{ "front IMX355", 27, 28, 21, 15, 2, 0x044, 0x05c, 0x1a, 0x0355,
	  { { ST_LDO, 7, 1800, 0 }, { ST_PGPIO, 2, 0, 1 }, { ST_MCLK, 0, 0, 1 }, { ST_RESET, 0, 0, 12 }, { ST_END, 0, 0, 0 } },
	  { { ST_RESET, 0, 0, 0 }, { ST_MCLK, 0, 0, 1 }, { ST_PGPIO, 2, 0, 1 }, { ST_LDO, 7, 0, 1 }, { ST_END, 0, 0, 0 } } },
};

/* SLG51000 LDOs: VSEL register, base uV, step uV (slg51000-regulator.c regls_desc) */
static const struct { uint16_t vsel; uint32_t base, step; } ldos[8] = {
	{ 0, 0, 0 }, { 0x2000, 2200000, 5000 }, { 0x2200, 2200000, 5000 }, { 0x2300, 1200000, 10000 },
	{ 0x2500, 1200000, 10000 }, { 0x2700, 400000, 5000 }, { 0x2900, 400000, 5000 }, { 0x3100, 1200000, 10000 },
};
#define SLG_MATRIX_CONF_A       0x110d
#define SLG_SYSCTL_STATUS       0x1117
#define SLG_FAULT_LOG1          0x1115

static struct {
	vm_offset_t gcc, camcc, mclk, tlmm, sp_core, sp_cnfg;   /* camcc = its 0xc000 page, mclk = its 0x5000 page */
	unsigned mode;
	bool broken, slg_up, slg_cs_raised, gdsc_switched, gdsc_clk_switched;
	uint8_t ldo_on;                 /* bits we switched on */
	uint32_t found_mclk_cfg, found_mclk_cbcr, found_mclk_pin, found_reset_cfg, found_reset_io;
	bool mclk_on, reset_taken;
	const struct sensor *s;
} cm;
static LCK_GRP_DECLARE(sweet_cam_grp, "sweet_cam");
static LCK_MTX_DECLARE(sweet_cam_lock, &sweet_cam_grp);
static int cam_sleep_chan;

static uint32_t
rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
sleep_ms(unsigned ms)
{
	if (ms == 0) {
		return;
	}
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&cam_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
wait_bits(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned t = 0; t <= us; t += 10) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return false;
}

static void
give_up(const char *why)
{
	if (!cm.broken) {
		printf("SWEET CAM: giving up: %s\n", why);
	}
	cm.broken = true;
}

/* ---- PM8150L GPIOs through the APPS EE's arbiter channels (as sweet_haptic.c / sweet_torch.c) ---- */

static struct { uint16_t apid; bool write; vm_offset_t va; } chan_cache[16];

static bool
spmi_channel(unsigned sid, unsigned periph, bool write, vm_offset_t *ch)
{
	unsigned ppid = (sid << 8) | periph, n = rd(cm.sp_core, 4) & 0x7ffu, apid = 0xffff, own = 7;
	for (unsigned i = 0; i < n && i < 512; i++) {
		uint32_t map = rd(cm.sp_core, 0x900u + 4u * i);
		if (!map || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		unsigned o = rd(cm.sp_cnfg, 0x700u + 4u * i) & 7u;
		if (apid == 0xffff || o == 0) {
			apid = i;
			own = o;
		}
	}
	if (apid == 0xffff || (write && own != 0)) {
		printf("SWEET CAM: spmi %x:%x %s refused (apid %#x owner %u)\n", sid, periph << 8, write ? "write" : "read", apid, own);
		return false;
	}
	for (unsigned i = 0; i < sizeof(chan_cache) / sizeof(chan_cache[0]); i++) {
		if (chan_cache[i].va && chan_cache[i].apid == apid && chan_cache[i].write == write) {
			*ch = chan_cache[i].va;
			return true;
		}
	}
	for (unsigned i = 0; i < sizeof(chan_cache) / sizeof(chan_cache[0]); i++) {
		if (!chan_cache[i].va) {
			chan_cache[i].va = ml_io_map(write ? SP_CHNLS_PA + 0x10000u * apid : SP_OBS_PA + 0x80u * apid, 0x80);
			chan_cache[i].apid = (uint16_t)apid;
			chan_cache[i].write = write;
			*ch = chan_cache[i].va;
			return *ch != 0;
		}
	}
	printf("SWEET CAM: no room for another SPMI channel mapping\n");
	return false;
}

static int
spmi(unsigned sid, unsigned addr, bool write, uint8_t *val)
{
	vm_offset_t ch;
	if (!spmi_channel(sid, (addr >> 8) & 0xffu, write, &ch)) {
		return -1;
	}
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (write) {
			wr(ch, 0x10, *val);
		}
		wr(ch, 0x00, ((write ? 0u : 1u) << 27) | ((addr & 0xffu) << 4));
		uint32_t st = 0;
		unsigned i;
		for (i = 0; i < 1000 && !((st = rd(ch, 0x08)) & 1u); i++) {
			delay(1);
		}
		if (i == 1000) {
			printf("SWEET CAM: spmi %x:%x timeout\n", sid, addr);
			return -1;
		}
		if (st & 0x8u) {
			continue;
		}
		if (st & 0x6u) {
			printf("SWEET CAM: spmi %x:%x status %#x\n", sid, addr, st);
			return -1;
		}
		if (!write) {
			*val = (uint8_t)rd(ch, 0x18);
		}
		return 0;
	}
	return -1;
}

/* GPIO n: MODE_CTL 0x40, DIG_VIN_CTL 0x41, OUT_SOURCE_CTL 0x44 ([7] level), EN_CTL 0x46 */
static void
pgpio_show(unsigned n, const char *what)
{
	uint8_t r[8] = { 0 };
	unsigned base = PGPIO(n) << 8;
	bool ok = !spmi(PGPIO_SID, base + 0x04, false, &r[0]) && !spmi(PGPIO_SID, base + 0x05, false, &r[1]) &&
	    !spmi(PGPIO_SID, base + 0x08, false, &r[2]) && !spmi(PGPIO_SID, base + 0x40, false, &r[3]) &&
	    !spmi(PGPIO_SID, base + 0x41, false, &r[4]) && !spmi(PGPIO_SID, base + 0x44, false, &r[5]) &&
	    !spmi(PGPIO_SID, base + 0x46, false, &r[6]) && !spmi(PGPIO_SID, base + 0x10, false, &r[7]);
	printf("SWEET CAM: PM8150L gpio%u (%s): %s type %#x sub %#x status %#x mode %#x vin %#x out %#x en %#x rt %#x\n",
	    n, what, ok ? "" : "(read failed)", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

static bool
pgpio_set(unsigned n, bool high)
{
	unsigned base = PGPIO(n) << 8;
	uint8_t type = 0;
	if (spmi(PGPIO_SID, base + 0x04, false, &type) || type != 0x10) {
		printf("SWEET CAM: PM8150L gpio%u: type %#x, not a GPIO\n", n, type);
		give_up("a PMIC GPIO that is not one");
		return false;
	}
	/*
	 * The supply (DIG_VIN_CTL) only where Android's DT sets one: power-source 1 for GPIO 8 (camera_rear_vcm_en)
	 * and 10 (en_rwcam). GPIO 2, 4 and 9 have no pinctrl state there and keep what they have (r16f: VIN0 for
	 * 2 and 4, VIN1 for 9).
	 */
	bool set_vin = n == 8 || n == 10;
	static const uint8_t regs[4] = { 0x41, 0x44, 0x40, 0x46 };
	uint8_t vals[4] = { 0x01, (uint8_t)(high ? 0x80 : 0x00), 0x01, 0x80 };      /* VIN1, level, output, on */
	for (unsigned i = set_vin ? 0 : 1; i < 4; i++) {
		uint8_t v = vals[i], back = 0;
		if (spmi(PGPIO_SID, base + regs[i], true, &v) || spmi(PGPIO_SID, base + regs[i], false, &back) ||
		    (back & 0x87) != (vals[i] & 0x87)) {
			printf("SWEET CAM: PM8150L gpio%u reg %#x <- %#x reads %#x\n", n, regs[i], vals[i], back);
			give_up("a PMIC GPIO write that did not read back");
			return false;
		}
	}
	return true;
}

/* ---- the SLG51000 ---- */

static int
slg_read(uint16_t reg, uint8_t *val)
{
	uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	return sweet_se9_xfer(SLG_ADDR, a, 2, val, 1);
}

static int
slg_write(uint16_t reg, uint8_t val, bool check)
{
	uint8_t a[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, val }, back = 0;
	int err = sweet_se9_xfer(SLG_ADDR, a, 3, NULL, 0);
	if (!err && check && (err = slg_read(reg, &back)) == 0 && back != val) {
		printf("SWEET CAM: SLG51000 %#x <- %#x reads %#x\n", reg, val, back);
		err = EIO;
	}
	if (err) {
		give_up("an SLG51000 write");
	}
	return err;
}

static bool
slg_up(void)
{
	if (cm.slg_up) {
		return true;
	}
	uint8_t id[3] = { 0 };
	if (slg_read(0x1105, &id[0]) != 0) {
		/* not selected yet: as slg51000_i2c_probe(): cs high, both enable lines low, 10 ms */
		if (!pgpio_set(9, false) || !pgpio_set(10, false) || !pgpio_set(4, true)) {
			return false;
		}
		cm.slg_cs_raised = true;
		sleep_ms(10);
	}
	if (slg_read(0x1105, &id[0]) || slg_read(0x1106, &id[1]) || slg_read(0x1107, &id[2])) {
		give_up("no SLG51000 at SE9 0x75");
		return false;
	}
	uint8_t fault = 0, status = 0, matrix = 0;
	(void)slg_read(SLG_FAULT_LOG1, &fault);
	(void)slg_read(SLG_SYSCTL_STATUS, &status);
	(void)slg_read(SLG_MATRIX_CONF_A, &matrix);
	printf("SWEET CAM: SLG51000 id %02x %02x %02x, fault log %#x, status %#x, LDOs on %#x\n", id[0], id[1], id[2],
	    fault, status, matrix);
	/* slg51000_config_tuning() */
	static const uint8_t test_on[4] = { 0x45, 0x53, 0x54, 0x4d };
	for (unsigned i = 0; i < 4; i++) {
		if (slg_write((uint16_t)(0x111a + i), test_on[i], false)) {
			return false;
		}
	}
	uint8_t ilim = 0;
	int err = slg_write(0x2364, 0x28, false);               /* LDO3_CONF1 */
	err |= slg_write(0x2700, 0x9f, false);                  /* LDO5_VSEL */
	err |= slg_write(0x1727, 0x34, false);                  /* MUXARRAY_INPUT_SEL_39 */
	err |= slg_write(0x1603, 0xea, false);                  /* LUTARRAY_LUT_VAL_3 */
	err |= slg_read(0x1133, &ilim);
	err |= slg_write(0x1133, (uint8_t)(ilim & 0x1f), false);        /* LDO_HP_STARTUP_ILIM [7:5] = 0 */
	for (unsigned i = 0; i < 4; i++) {
		err |= slg_write((uint16_t)(0x111a + i), 0, false);
	}
	if (err) {
		give_up("the SLG51000's tuning");
		return false;
	}
	cm.slg_up = true;
	return true;
}

static bool
ldo_set(unsigned n, unsigned mv, bool on)
{
	uint8_t matrix = 0;
	if (n < 1 || n > 7 || slg_read(SLG_MATRIX_CONF_A, &matrix)) {
		give_up("the SLG51000's enable register");
		return false;
	}
	uint8_t bit = (uint8_t)(1u << (n - 1));
	if (!on) {
		if (!(cm.ldo_on & bit)) {
			return true;    /* found on, or never ours */
		}
		(void)slg_write(SLG_MATRIX_CONF_A, (uint8_t)(matrix & ~bit), false);
		cm.ldo_on &= (uint8_t)~bit;
		return true;
	}
	if (matrix & bit) {
		printf("SWEET CAM: SLG51000 LDO%u was on already: left as it is\n", n);
		return true;
	}
	uint8_t minv = 0, maxv = 0, trim2 = 0;
	uint16_t vsel = ldos[n].vsel;
	if (slg_read(vsel + 0x60, &minv) || slg_read(vsel + 0x61, &maxv) || slg_read(vsel + 0x63, &trim2)) {
		give_up("an SLG51000 LDO's range");
		return false;
	}
	uint32_t sel = (mv * 1000u - ldos[n].base) / ldos[n].step;
	if ((n == 5 || n == 6) && (trim2 & 1u)) {
		printf("SWEET CAM: SLG51000 LDO%u is in bypass mode (trim2 %#x)\n", n, trim2);
		give_up("an LDO in bypass mode");
		return false;
	}
	if (mv * 1000u < ldos[n].base || sel < minv || sel > maxv) {
		printf("SWEET CAM: SLG51000 LDO%u: %u mV is selector %u, outside %u..%u\n", n, mv, sel, minv, maxv);
		give_up("a voltage the LDO does not have");
		return false;
	}
	if (slg_write(vsel, (uint8_t)sel, true) || slg_write(SLG_MATRIX_CONF_A, (uint8_t)(matrix | bit), true)) {
		return false;
	}
	cm.ldo_on |= bit;
	uint8_t st = 0;
	for (unsigned t = 0; t < 10; t++) {
		sleep_ms(1);
		if (slg_read(vsel + 0xc1, &st)) {
			break;
		}
		if (st & 1u) {
			printf("SWEET CAM: SLG51000 LDO%u over current (status %#x)\n", n, st);
			give_up("an LDO over current");
			return false;
		}
		if (st & 2u) {
			printf("SWEET CAM: SLG51000 LDO%u at %u mV (selector %u) after %u ms\n", n, mv, sel, t + 1);
			return true;
		}
	}
	printf("SWEET CAM: SLG51000 LDO%u not ready (status %#x)\n", n, st);
	give_up("an LDO that does not come up");
	return false;
}

/* ---- clocks ---- */

/*
 * GCC's camera clocks, which camcc needs to be read at all: AHB (enabled, hardware gated, so CLK_OFF while
 * idle: r16a 0x80000003) and XO (r16a: 0x80000000, off; gcc-lito.c marks both critical, so Linux keeps them on).
 * (The resets of r16a-r16e were the full I/O map, not these: 0920.)
 */
static bool
camera_xo_on(void)
{
	uint32_t ahb = rd(cm.gcc, GCC_CAMERA_AHB), xo = rd(cm.gcc, GCC_CAMERA_XO);
	if (!(ahb & 1u)) {
		printf("SWEET CAM: GCC camera AHB %#x not enabled: camcc not touched\n", ahb);
		return false;
	}
	if (!(xo & 1u)) {
		wr(cm.gcc, GCC_CAMERA_XO, xo | 1u);
		if (!wait_bits(cm.gcc, GCC_CAMERA_XO, CBCR_OFF, 0, 500)) {
			printf("SWEET CAM: GCC camera XO %#x does not run: camcc not touched\n", rd(cm.gcc, GCC_CAMERA_XO));
			return false;
		}
		printf("SWEET CAM: GCC camera XO on (%#x); left on, as Linux keeps it\n", rd(cm.gcc, GCC_CAMERA_XO));
	}
	return true;
}

static bool
titan_up(void)
{
	if (!camera_xo_on()) {
		give_up("camcc's GCC clocks");
		return false;
	}
	uint32_t gclk = rd(cm.camcc, CAMCC_GDSC_CLK);
	if (!(gclk & 1u)) {
		wr(cm.camcc, CAMCC_GDSC_CLK, gclk | 1u);
		cm.gdsc_clk_switched = true;
		if (!wait_bits(cm.camcc, CAMCC_GDSC_CLK, CBCR_OFF, 0, 500)) {
			give_up("camcc's GDSC clock does not run");
			return false;
		}
	}
	uint32_t g = rd(cm.camcc, TITAN_GDSCR);
	if (!(g & 1u) && (g & (1u << 31))) {
		return true;
	}
	wr(cm.camcc, TITAN_GDSCR, g & ~1u);
	cm.gdsc_switched = true;
	if (!wait_bits(cm.camcc, TITAN_GDSCR, 1u << 31, 1u << 31, 1000)) {
		printf("SWEET CAM: titan top GDSC %#x does not come on\n", rd(cm.camcc, TITAN_GDSCR));
		give_up("a power domain that does not come on");
		return false;
	}
	printf("SWEET CAM: titan top GDSC on (%#x)\n", rd(cm.camcc, TITAN_GDSCR));
	return true;
}

static void
titan_down(void)
{
	if (cm.gdsc_switched) {
		wr(cm.camcc, TITAN_GDSCR, rd(cm.camcc, TITAN_GDSCR) | 1u);
		(void)wait_bits(cm.camcc, TITAN_GDSCR, 1u << 31, 0, 1000);
		printf("SWEET CAM: titan top GDSC off (%#x)\n", rd(cm.camcc, TITAN_GDSCR));
		cm.gdsc_switched = false;
	}
	if (cm.gdsc_clk_switched) {
		wr(cm.camcc, CAMCC_GDSC_CLK, rd(cm.camcc, CAMCC_GDSC_CLK) & ~1u);
		cm.gdsc_clk_switched = false;
	}
}

static bool
mclk_set(bool on)
{
	const struct sensor *s = cm.s;
	if (!on) {
		if (!cm.mclk_on) {
			return true;
		}
		wr(cm.tlmm, GPIO_CFG(s->mclk_pin), cm.found_mclk_pin);
		wr(cm.mclk, s->mclk_cbcr, rd(cm.mclk, s->mclk_cbcr) & ~1u);
		(void)wait_bits(cm.mclk, s->mclk_cbcr, CBCR_OFF, CBCR_OFF, 500);
		wr(cm.mclk, s->mclk_rcg + 4, cm.found_mclk_cfg);
		wr(cm.mclk, s->mclk_rcg, rd(cm.mclk, s->mclk_rcg) | 1u);
		(void)wait_bits(cm.mclk, s->mclk_rcg, 1u, 0, 500);
		cm.mclk_on = false;
		return true;
	}
	cm.found_mclk_cfg = rd(cm.mclk, s->mclk_rcg + 4);
	cm.found_mclk_cbcr = rd(cm.mclk, s->mclk_cbcr);
	cm.found_mclk_pin = rd(cm.tlmm, GPIO_CFG(s->mclk_pin));
	if (cm.found_mclk_cbcr & 1u) {
		give_up("MCLK on already (somebody else's)");
		return false;
	}
	cm.mclk_on = true;
	wr(cm.mclk, s->mclk_rcg + 4, (cm.found_mclk_cfg & ~0x371fu) | 1u);    /* XO, divider 1, no M/N */
	wr(cm.mclk, s->mclk_rcg, rd(cm.mclk, s->mclk_rcg) | 1u);
	if (!wait_bits(cm.mclk, s->mclk_rcg, 1u, 0, 500) || (rd(cm.mclk, s->mclk_rcg + 4) & 0x371fu) != 1u) {
		printf("SWEET CAM: MCLK RCG cmd %#x cfg %#x\n", rd(cm.mclk, s->mclk_rcg), rd(cm.mclk, s->mclk_rcg + 4));
		give_up("an RCG that does not update");
		return false;
	}
	wr(cm.mclk, s->mclk_cbcr, cm.found_mclk_cbcr | 1u);
	if (!wait_bits(cm.mclk, s->mclk_cbcr, CBCR_OFF, 0, 500)) {
		printf("SWEET CAM: MCLK branch %#x\n", rd(cm.mclk, s->mclk_cbcr));
		give_up("an MCLK branch that does not run");
		return false;
	}
	wr(cm.tlmm, GPIO_CFG(s->mclk_pin), CFG_FUNC(1) | CFG_MA(s->mclk_ma));
	return true;
}

static void
reset_set(bool high)
{
	const struct sensor *s = cm.s;
	if (!cm.reset_taken) {
		cm.found_reset_cfg = rd(cm.tlmm, GPIO_CFG(s->reset));
		cm.found_reset_io = rd(cm.tlmm, GPIO_IO(s->reset));
		cm.reset_taken = true;
	}
	wr(cm.tlmm, GPIO_IO(s->reset), high ? 2u : 0u);
	wr(cm.tlmm, GPIO_CFG(s->reset), CFG_OUTPUT | CFG_MA(2));
}

/* ---- the sensor's I2C by hand (HackiOS redwood_camera.c) ---- */

static void
half_bit(void)
{
	delay(5);
}

static void
line(unsigned pin, bool high)
{
	wr(cm.tlmm, GPIO_IO(pin), 0);
	wr(cm.tlmm, GPIO_CFG(pin), CFG_PULL_UP | CFG_MA(2) | (high ? 0 : CFG_OUTPUT));
}

static bool
level(unsigned pin)
{
	return rd(cm.tlmm, GPIO_IO(pin)) & 1u;
}

static bool
i2c_bit(unsigned sda, unsigned scl, bool out)
{
	line(sda, out);
	half_bit();
	line(scl, true);
	for (unsigned stretched = 0; !level(scl) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	bool in = level(sda);
	line(scl, false);
	return in;
}

static bool
i2c_byte(unsigned sda, unsigned scl, uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		(void)i2c_bit(sda, scl, (b >> i) & 1u);
	}
	return !i2c_bit(sda, scl, true);
}

static void
i2c_start(unsigned sda, unsigned scl)
{
	line(sda, true);
	line(scl, true);
	half_bit();
	line(sda, false);
	half_bit();
	line(scl, false);
	half_bit();
}

static void
i2c_stop(unsigned sda, unsigned scl)
{
	line(sda, false);
	half_bit();
	line(scl, true);
	half_bit();
	line(sda, true);
	half_bit();
}

/* a 16-bit register's two bytes; false if nobody answered */
static bool
sensor_read16(uint8_t slave, uint16_t reg, uint16_t *val, const char **why)
{
	const struct sensor *s = cm.s;
	uint32_t cfg[2] = { rd(cm.tlmm, GPIO_CFG(s->sda)), rd(cm.tlmm, GPIO_CFG(s->scl)) };
	uint32_t io[2] = { rd(cm.tlmm, GPIO_IO(s->sda)), rd(cm.tlmm, GPIO_IO(s->scl)) };
	bool ok = false;
	line(s->sda, true);
	line(s->scl, true);
	half_bit();
	if (!level(s->sda) || !level(s->scl)) {
		*why = "bus not at rest";
	} else {
		i2c_start(s->sda, s->scl);
		ok = i2c_byte(s->sda, s->scl, (uint8_t)(slave << 1)) && i2c_byte(s->sda, s->scl, (uint8_t)(reg >> 8)) &&
		    i2c_byte(s->sda, s->scl, (uint8_t)reg);
		*why = ok ? "" : "no acknowledgement";
		if (ok) {
			i2c_start(s->sda, s->scl);
			ok = i2c_byte(s->sda, s->scl, (uint8_t)(slave << 1 | 1));
			if (ok) {
				uint8_t b[2] = { 0, 0 };
				for (unsigned k = 0; k < 2; k++) {
					for (int i = 7; i >= 0; i--) {
						b[k] = (uint8_t)(b[k] << 1 | i2c_bit(s->sda, s->scl, true));
					}
					(void)i2c_bit(s->sda, s->scl, k == 1);  /* ACK the first, NACK the last */
				}
				*val = (uint16_t)(b[0] << 8 | b[1]);
			} else {
				*why = "no acknowledgement of the read";
			}
		}
		i2c_stop(s->sda, s->scl);
	}
	wr(cm.tlmm, GPIO_IO(s->sda), io[0] & 2u);
	wr(cm.tlmm, GPIO_IO(s->scl), io[1] & 2u);
	wr(cm.tlmm, GPIO_CFG(s->sda), cfg[0]);
	wr(cm.tlmm, GPIO_CFG(s->scl), cfg[1]);
	return ok;
}

/* ---- the look, the probe ---- */

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(cm.tlmm, GPIO_CFG(pin)), io = rd(cm.tlmm, GPIO_IO(pin));
	printf("SWEET CAM: gpio%u (%s): function %u pull %u %u mA %s, reads %u\n", pin, what, (cfg >> 2) & 15u, cfg & 3u,
	    (((cfg >> 6) & 7u) + 1u) * 2u, (cfg & CFG_OUTPUT) ? ((io & 2u) ? "drives high" : "drives low") : "input", io & 1u);
}

static void
look(void)
{
	printf("SWEET CAM: GCC camera AHB %#x XO %#x\n", rd(cm.gcc, GCC_CAMERA_AHB), rd(cm.gcc, GCC_CAMERA_XO));
	for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
		const struct sensor *s = &sensors[i];
		printf("SWEET CAM: %u %s:\n", i, s->name);
		show_pin(s->sda, "CCI data");
		show_pin(s->scl, "CCI clock");
		show_pin(s->reset, "reset");
		show_pin(s->mclk_pin, "MCLK");
	}
}

/*
 * r16a: the phone reset right after the look's pin lines (the PMIC GPIO and SLG51000 reads followed, and
 * the UDP log loses the last lines before a reset). Those reads are separate commands now, each line
 * given 300 ms to reach the log: "c 10" the PMIC GPIOs, "c 11" the SLG51000's first register.
 */
static void
look_pmic(void)
{
	static const struct { unsigned n; const char *what; } g[] = {
		{ 4, "SLG51000 cs" }, { 9, "SLG51000 enable 1" }, { 10, "SLG51000 enable 0" }, { 8, "camera_ldo (rear VAF)" },
		{ 2, "front custom 1" },
	};
	for (unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
		printf("SWEET CAM: reading PM8150L gpio%u ...\n", g[i].n);
		sleep_ms(300);
		pgpio_show(g[i].n, g[i].what);
		sleep_ms(300);
	}
}

static void
look_camcc(void)
{
	printf("SWEET CAM: camcc: switching the camera XO on first ...\n");
	sleep_ms(300);
	if (!camera_xo_on()) {
		return;
	}
	sleep_ms(300);
	printf("SWEET CAM: camcc GDSC clock %#x, titan top GDSC %#x\n", rd(cm.camcc, CAMCC_GDSC_CLK), rd(cm.camcc, TITAN_GDSCR));
	for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
		const struct sensor *s = &sensors[i];
		printf("SWEET CAM: %u %s: MCLK RCG cmd %#x cfg %#x branch %#x\n", i, s->name, rd(cm.mclk, s->mclk_rcg),
		    rd(cm.mclk, s->mclk_rcg + 4), rd(cm.mclk, s->mclk_cbcr));
	}
	sleep_ms(300);
}

static void
look_slg(void)
{
	uint8_t id = 0;
	printf("SWEET CAM: reading the SLG51000 (SE9 0x75) ...\n");
	sleep_ms(300);
	int err = slg_read(0x1105, &id);
	printf("SWEET CAM: SLG51000 at SE9 0x75: %s (err %d, id0 %#x)\n", err ? "no answer" : "answers", err, id);
	sleep_ms(300);
}

static void
run_steps(const struct step *st, bool up)
{
	for (; st->kind != ST_END; st++) {
		if (up && cm.broken) {
			return;
		}
		switch (st->kind) {
		case ST_LDO:
			if (!ldo_set(st->n, st->mv, up) && up) {
				return;
			}
			break;
		case ST_PGPIO:
			if (!pgpio_set(st->n, up) && up) {
				return;
			}
			break;
		case ST_RESET:
			reset_set(up);
			break;
		case ST_MCLK:
			if (!mclk_set(up) && up) {
				return;
			}
			break;
		}
		sleep_ms(st->ms);
	}
}

static int
probe(unsigned i)
{
	const struct sensor *s = &sensors[i];
	cm.s = s;
	cm.broken = false;
	cm.reset_taken = false;
	printf("SWEET CAM: %u %s: powering up\n", i, s->name);
	reset_set(false);
	sleep_ms(1);
	if (titan_up() && slg_up()) {
		run_steps(s->up, true);
	}
	int result = EIO;
	if (!cm.broken) {
		uint16_t id = 0, alt = 0;
		const char *why = "", *why2 = "";
		bool ok = sensor_read16(s->slave, 0x0016, &id, &why);
		if (ok) {
			printf("SWEET CAM: %u %s: chip ID %#06x at 0x%02x (8-bit 0x%02x) %s\n", i, s->name, id, s->slave, s->slave << 1,
			    id == s->id ? "-- as expected" : "-- NOT the expected one");
			result = id == s->id ? 0 : ENODEV;
		} else {
			/* the other usual Sony address, in case the module answers there */
			uint8_t other = s->slave == 0x1a ? 0x10 : 0x1a;
			bool ok2 = sensor_read16(other, 0x0016, &alt, &why2);
			printf("SWEET CAM: %u %s: nothing at 0x%02x (%s); at 0x%02x: %s %#06x\n", i, s->name, s->slave, why, other,
			    ok2 ? "answers" : why2, alt);
			result = ENODEV;
		}
	}
	printf("SWEET CAM: %u %s: powering down\n", i, s->name);
	bool was_broken = cm.broken;
	cm.broken = false;
	run_steps(s->down, false);
	mclk_set(false);
	if (cm.ldo_on) {
		/* anything a failed power-up left on */
		for (unsigned n = 1; n <= 7; n++) {
			(void)ldo_set(n, 0, false);
		}
	}
	if (cm.reset_taken) {
		wr(cm.tlmm, GPIO_IO(s->reset), cm.found_reset_io & 2u);
		wr(cm.tlmm, GPIO_CFG(s->reset), cm.found_reset_cfg);
	}
	titan_down();
	cm.broken = was_broken;
	return result;
}

/* ==== 0921: the capture path. Step 1: the CSID's test pattern through IFE-lite 0 into memory ("c 20") ==== */

/*
 * HackiOS redwood_camera.c's cam=3 (Titan 165, yupik), with lito's (Titan 175) numbers. The CSID-lite and
 * IFE-lite register layouts are the same files in both kernels (cam_ife_csid_lite17x.h, cam_vfe_lite17x.h with
 * cam_vfe_bus_ver2.c), and so are the base addresses: CSID-lite 0 at 0xacc8000, IFE-lite 0 at 0xacc4000.
 * Different here:
 *   clocks   camcc-lito.c: PLL0 (Lucid) at 1200 MHz (L 0x3e, alpha 0x8000; even /2 600, odd /3 400), then at the
 *            device tree's lowest levels: slow_ahb 80 (PLL0 odd /5), camnoc_axi 150 (even /4), cphy_rx 300 (even
 *            /2), csiNphytimer 300, ife_lite_csid 300, ife_lite 400 (odd /1); the branches cpas_ahb, camnoc_axi,
 *            csiphyN, csiNphytimer, ife_lite_csid, ife_lite_cphy_rx, ife_lite; GCC camera_hf_axi and _sf_axi
 *   rails    none asked for: wland holds cx.lvl at its top level on the apps RSC (phone/wifi/wland.c
 *            boot_modem()), and MX stays at or above CX. (r17a voted mx.lvl to SVS_L1 and back to 0, which
 *            replaces the apps RSC's vote the boot loader left, and the phone went down at the first "c 20":
 *            the vote is gone, and the stages say what comes next, 300 ms apart, so a hang shows where.)
 *   memory   the GPU's pool (sweet_gpu_pool_alloc(): RAM below 4 GB that XNU never maps), because the write
 *            master takes 32-bit addresses
 *   SMMU     the apps SMMU (0x15000000) sends the IFEs' streams (live DT msm_cam_smmu_ife: 0x820..0xd40, mask
 *            0x5e0: one match register 0x800/0x5e0 takes them all) to a context bank with translation off, as
 *            phone/wifi/radio.c does for the WLAN: the IFE's addresses are physical
 * The frames stay in the pool after the run; tools/flame-files.py read /dev/pmem PA LEN fetches them
 * (tools/redfin-camraw.py makes a picture). Everything else is taken back in the reverse order.
 */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern int sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa);

#define CAMCC_PA                0x0ad00000u
#define CSID_LITE_PA            0x0acc8000u
#define IFE_LITE_PA             0x0acc4000u
#define CSIPHY_PA(n)            (0x0ace0000u + 0x2000u * (n))
#define SMMU_PA                 0x15000000u
#define GCC_CAMERA_HF_AXI       0x028u  /* GCC 0xb028 (in the GCC page mapped above) */
#define GCC_CAMERA_SF_AXI       0x02cu

#define CAP_POOL_BYTES          (32u << 20)     /* two of the largest frames (IMX363 4032x3024 RAW10: 15.3 MB) */
#define IFE_STREAM              0x800u
#define IFE_STREAM_MASK         0x5e0u

static struct {
	vm_offset_t camcc, csid, ife, smmu, smmu_gr1, smmu_cb, frames_va;
	uint64_t frames_pa;
	uint32_t smr_n, cb_n, smr_old, s2cr_old, cbar_old;
	bool pll0_on, smmu_set, csid_on, ife_on, tpg_on;
} cap = { .smr_n = ~0u, .cb_n = ~0u };

static uint32_t
crd(uint32_t off)
{
	return rd(cap.camcc, off);
}

static bool
cwr(uint32_t off, uint32_t value, uint32_t check)
{
	wr(cap.camcc, off, value);
	if ((rd(cap.camcc, off) & check) != (value & check)) {
		printf("SWEET CAM: camcc %#x <- %#x reads %#x\n", off, value, rd(cap.camcc, off));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

/* ---- camcc's PLL0 (Lucid): clk_lucid_pll_configure() + alpha_pll_lucid_enable() with cam_cc_pll0_config ---- */

#define PLL_MODE        0x00            /* [0] outputs, [1] bypass off, [2] out of reset, [20] FSM vote, [23] update bypass, [31] lock */
#define PLL_L           0x04
#define PLL_CAL_L       0x08
#define PLL_USER        0x0c
#define PLL_USER_U      0x10
#define PLL_USER_U1     0x14
#define PLL_CONFIG      0x18
#define PLL_CONFIG_U    0x1c
#define PLL_CONFIG_U1   0x20
#define PLL_TEST_U1     0x2c
#define PLL_OPMODE      0x38
#define PLL_ALPHA       0x40

static bool
pll0_up(void)
{
	uint32_t mode = crd(PLL_MODE);
	if (mode & (1u | 1u << 20)) {
		printf("SWEET CAM: camcc PLL0 mode %#x: running or voted (somebody else's)\n", mode);
		give_up("PLL0 in use");
		return false;
	}
	static const struct { uint32_t off, val; } cfg[] = {
		{ PLL_L, 0x3e }, { PLL_CAL_L, 0x44 }, { PLL_ALPHA, 0x8000 }, { PLL_CONFIG, 0x20485699 },
		{ PLL_CONFIG_U, 0x00002261 }, { PLL_CONFIG_U1, 0x329a699c },
		{ PLL_USER, 0x00003101 },       /* main on; even /2 ([11:8] 1), odd /3 ([15:12] 3): the post-dividers' tables */
		{ PLL_USER_U, 0x00000805 }, { PLL_TEST_U1, 0x01800000 },
	};
	for (unsigned i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
		if (!cwr(cfg[i].off, cfg[i].val, ~0u)) {
			return false;
		}
	}
	cap.pll0_on = true;
	if (!cwr(PLL_MODE, crd(PLL_MODE) | 1u << 23, 1u << 23) || !cwr(PLL_MODE, crd(PLL_MODE) & ~1u, 1) ||
	    !cwr(PLL_OPMODE, 0, 1) || !cwr(PLL_MODE, crd(PLL_MODE) | 1u << 2, 1u << 2) || !cwr(PLL_OPMODE, 1, 1)) {
		return false;
	}
	if (!wait_bits(cap.camcc, PLL_MODE, 1u << 31, 1u << 31, 2000)) {
		printf("SWEET CAM: camcc PLL0 does not lock (mode %#x)\n", crd(PLL_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!cwr(PLL_USER, crd(PLL_USER) | 7u, 7u) || !cwr(PLL_MODE, crd(PLL_MODE) | 1u, 1u)) {
		return false;
	}
	printf("SWEET CAM: camcc PLL0 locked at 1200 MHz (mode %#x user %#x)\n", crd(PLL_MODE), crd(PLL_USER));
	return true;
}

static void
pll0_down(void)
{
	if (!cap.pll0_on) {
		return;
	}
	wr(cap.camcc, PLL_MODE, crd(PLL_MODE) & ~1u);
	wr(cap.camcc, PLL_USER, crd(PLL_USER) & ~7u);
	wr(cap.camcc, PLL_OPMODE, 0);
	wr(cap.camcc, PLL_MODE, crd(PLL_MODE) & ~(1u << 2));
	cap.pll0_on = false;
	printf("SWEET CAM: camcc PLL0 off (mode %#x)\n", crd(PLL_MODE));
}

/* ---- RCGs and branches ---- */

struct cap_rcg { const char *name; uint32_t cmd, src, div; uint32_t found; bool set; };
static struct cap_rcg cap_rcgs[] = {
	{ "slow_ahb 80", 0x7058, 3, 9 },                /* PLL0 odd 400 / 5 (field 2n - 1) */
	{ "camnoc_axi 150", 0xc12c, 2, 7 },             /* PLL0 even 600 / 4 */
	{ "cphy_rx 300", 0xa064, 2, 3 },                /* even / 2 */
	{ "csiNphytimer 300", 0, 2, 3 },                /* even / 2: the sensor's PHY (0x6004 + 0x24 n) */
	{ "ife_lite_csid 300", 0xc020, 2, 3 },          /* even / 2 */
	{ "ife_lite 400", 0xc004, 3, 1 },               /* odd / 1 */
};
struct cap_branch { const char *name; bool gcc; uint32_t cbcr; bool set; };
static struct cap_branch cap_branches[] = {
	{ "gcc camera_hf_axi", true, GCC_CAMERA_HF_AXI }, { "gcc camera_sf_axi", true, GCC_CAMERA_SF_AXI },
	{ "cpas_ahb", false, 0xc124 }, { "camnoc_axi", false, 0xc148 }, { "csiphyN", false, 0 }, { "csiNphytimer", false, 0 },
	{ "ife_lite_csid", false, 0xc038 }, { "ife_lite_cphy_rx", false, 0xc040 }, { "ife_lite", false, 0xc01c },
};

static bool
cap_clocks_up(unsigned phy)
{
	cap_rcgs[3].cmd = 0x6004u + 0x24u * phy;
	cap_branches[4].cbcr = 0x6020u + 0x24u * phy;   /* csiphyN */
	cap_branches[5].cbcr = 0x601cu + 0x24u * phy;   /* csiNphytimer */
	for (unsigned i = 0; i < sizeof(cap_rcgs) / sizeof(cap_rcgs[0]); i++) {
		struct cap_rcg *r = &cap_rcgs[i];
		r->found = crd(r->cmd + 4);
		if (!cwr(r->cmd + 4, (r->found & ~0x371fu) | r->src << 8 | r->div, 0x371f)) {
			return false;
		}
		r->set = true;
		wr(cap.camcc, r->cmd, crd(r->cmd) | 1u);
		if (!wait_bits(cap.camcc, r->cmd, 1u, 0, 500)) {
			printf("SWEET CAM: RCG %s cmd %#x does not update\n", r->name, crd(r->cmd));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(cap_branches) / sizeof(cap_branches[0]); i++) {
		struct cap_branch *b = &cap_branches[i];
		vm_offset_t base = b->gcc ? cm.gcc : cap.camcc;
		if (rd(base, b->cbcr) & 1u) {
			continue;
		}
		wr(base, b->cbcr, rd(base, b->cbcr) | 1u);
		b->set = true;
		if (!wait_bits(base, b->cbcr, CBCR_OFF, 0, 500)) {
			printf("SWEET CAM: branch %s %#x does not run\n", b->name, rd(base, b->cbcr));
			give_up("a branch that does not come on");
			return false;
		}
	}
	printf("SWEET CAM: capture clocks on (PHY %u)\n", phy);
	return true;
}

static void
cap_clocks_down(void)
{
	for (int i = (int)(sizeof(cap_branches) / sizeof(cap_branches[0])) - 1; i >= 0; i--) {
		struct cap_branch *b = &cap_branches[i];
		if (b->set) {
			vm_offset_t base = b->gcc ? cm.gcc : cap.camcc;
			wr(base, b->cbcr, rd(base, b->cbcr) & ~1u);
			b->set = false;
		}
	}
	for (int i = (int)(sizeof(cap_rcgs) / sizeof(cap_rcgs[0])) - 1; i >= 0; i--) {
		struct cap_rcg *r = &cap_rcgs[i];
		if (r->set) {
			wr(cap.camcc, r->cmd + 4, r->found);
			wr(cap.camcc, r->cmd, crd(r->cmd) | 1u);
			(void)wait_bits(cap.camcc, r->cmd, 1u, 0, 500);
			r->set = false;
		}
	}
}

/* ---- the apps SMMU: the IFEs' streams through a context bank with translation off (phone/wifi/radio.c) ---- */

static bool
cap_smmu_up(void)
{
	uint32_t id0 = rd(cap.smmu, 0x20), id1 = rd(cap.smmu, 0x24);
	uint32_t nsmr = id0 & 0xff, ncb = id1 & 0xff, psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	uint64_t used[4] = { 0 };
	uint32_t free_smr = ~0u;
	if ((uint64_t)npage * psize * 2 != 0x100000) {
		printf("SWEET CAM: SMMU ID1 %#x: a layout not known here\n", id1);
		return false;
	}
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd(cap.smmu, 0x800 + 4 * i), s2cr = rd(cap.smmu, 0xc00 + 4 * i);
		if (!(smr >> 31)) {
			if (free_smr == ~0u) {
				free_smr = i;
			}
			continue;
		}
		if (((s2cr >> 16) & 3) == 0) {
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		}
		uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
		if (((id ^ IFE_STREAM) & ~(m | IFE_STREAM_MASK) & 0x7fff) == 0) {
			printf("SWEET CAM: SMR%u %#x (S2CR %#x) already takes the IFE streams: left alone\n", i, smr, s2cr);
			give_up("the IFE streams are somebody's");
			return false;
		}
	}
	uint32_t cb = ~0u;
	for (uint32_t c = 0; c < ncb && c < 256; c++) {
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd(cap.smmu, (npage + c) * psize) & 1u) && rd(cap.smmu, psize + 4 * c) == 0) {
			cb = c;
			break;
		}
	}
	if (free_smr == ~0u || cb == ~0u) {
		printf("SWEET CAM: SMMU: no free match register (%d) or context bank (%d)\n", (int)free_smr, (int)cb);
		give_up("no room in the SMMU");
		return false;
	}
	cap.smmu_cb = ml_io_map_unmappable(SMMU_PA + (npage + cb) * psize, 0x1000, VM_WIMG_IO);
	if (!cap.smmu_cb) {
		return false;
	}
	cap.smr_n = free_smr;
	cap.cb_n = cb;
	cap.smr_old = rd(cap.smmu, 0x800 + 4 * free_smr);
	cap.s2cr_old = rd(cap.smmu, 0xc00 + 4 * free_smr);
	cap.cbar_old = rd(cap.smmu, psize + 4 * cb);
	cap.smmu_gr1 = psize;
	wr(cap.smmu_cb, 0x58, 0xffffffffu);     /* FSR */
	wr(cap.smmu_cb, 0x20, 0);
	wr(cap.smmu_cb, 0x24, 0);
	wr(cap.smmu_cb, 0x30, 0);
	wr(cap.smmu_cb, 0x10, 0);
	wr(cap.smmu_cb, 0x38, 0);
	wr(cap.smmu_cb, 0x3c, 0);
	wr(cap.smmu, psize + 0x800 + 4 * cb, 1);        /* CBA2R: AArch64 */
	wr(cap.smmu, psize + 4 * cb, 0x1f000);          /* CBAR: S1, S2 bypass, as ABL's banks */
	wr(cap.smmu_cb, 0, 0xe0);                       /* SCTLR: fault config, M = 0 (no translation) */
	wr(cap.smmu, 0xc00 + 4 * free_smr, cb);         /* S2CR: translate through it */
	wr(cap.smmu, 0x800 + 4 * free_smr, 1u << 31 | IFE_STREAM_MASK << 16 | IFE_STREAM);
	wr(cap.smmu, 0x70, 0);                          /* sTLBGSYNC */
	for (int i = 0; i < 1000 && (rd(cap.smmu, 0x74) & 1u); i++) {
		delay(10);
	}
	cap.smmu_set = true;
	printf("SWEET CAM: SMMU: IFE streams %#x/%#x -> CB%u (translation off) via SMR%u: SMR %#x S2CR %#x\n", IFE_STREAM,
	    IFE_STREAM_MASK, cb, free_smr, rd(cap.smmu, 0x800 + 4 * free_smr), rd(cap.smmu, 0xc00 + 4 * free_smr));
	return true;
}

static void
cap_smmu_faults(void)
{
	if (!cap.smmu_set) {
		return;
	}
	uint32_t fsr = rd(cap.smmu_cb, 0x58), gfsr = rd(cap.smmu, 0x48);
	printf("SWEET CAM: SMMU CB%u FSR %#x FAR %#x%08x FSYNR0 %#x; global FSR %#x FSYNR1 %#x\n", cap.cb_n, fsr,
	    rd(cap.smmu_cb, 0x64), rd(cap.smmu_cb, 0x60), rd(cap.smmu_cb, 0x68), gfsr, rd(cap.smmu, 0x54));
}

static void
cap_smmu_down(void)
{
	if (!cap.smmu_set) {
		return;
	}
	cap_smmu_faults();
	wr(cap.smmu, 0x800 + 4 * cap.smr_n, cap.smr_old);
	wr(cap.smmu, 0xc00 + 4 * cap.smr_n, cap.s2cr_old);
	wr(cap.smmu_cb, 0, 0);
	wr(cap.smmu, cap.smmu_gr1 + 4 * cap.cb_n, cap.cbar_old);
	wr(cap.smmu, 0x70, 0);
	for (int i = 0; i < 1000 && (rd(cap.smmu, 0x74) & 1u); i++) {
		delay(10);
	}
	cap.smmu_set = false;
}

/* ---- CSID-lite 0 and IFE-lite 0 (HackiOS's code, the same register files) ---- */

#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_RX_IRQ_STATUS      0x020
#define CSID_RX_IRQ_MASK        0x024
#define CSID_RX_IRQ_CLEAR       0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_RX_CFG0            0x100
#define CSID_RX_CFG1            0x104
#define CSID_RX_PKTS            0x160
#define CSID_RX_ECC             0x164
#define CSID_RX_CRC             0x168
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PAT  0x20c
#define CSID_RDI0_FRM_DROP_PER  0x210
#define CSID_RDI0_IRQ_SUB_PAT   0x214
#define CSID_RDI0_IRQ_SUB_PER   0x218
#define CSID_RDI0_PIX_DROP_PAT  0x224
#define CSID_RDI0_PIX_DROP_PER  0x228
#define CSID_RDI0_LINE_DROP_PAT 0x22c
#define CSID_RDI0_LINE_DROP_PER 0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_RDI0_BYTES_PONG    0x2e4
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS     0x640
#define CSID_TPG_COMMON_GEN     0x648
#define TPG_VC                  0xa
#define RAW10_DT                0x2b

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_COMP_ERROR          0x206c
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static bool
bput(vm_offset_t base, const char *what, uint32_t off, uint32_t val, bool check)
{
	wr(base, off, val);
	if (check && rd(base, off) != val) {
		printf("SWEET CAM: %s %#x <- %#x reads %#x\n", what, off, val, rd(base, off));
		give_up("a camera register that did not read back");
		return false;
	}
	return true;
}

static bool
csid_reset(void)
{
	vm_offset_t c = cap.csid;
	wr(c, CSID_RX_IRQ_MASK, 0);
	for (unsigned r = 0; r < 4; r++) {
		wr(c, CSID_RDI0_IRQ_MASK + 0x10 * r, 0);
	}
	wr(c, CSID_TOP_IRQ_CLEAR, 1);
	wr(c, CSID_RX_IRQ_CLEAR, 0xfffffff);
	for (unsigned r = 0; r < 4; r++) {
		wr(c, CSID_RDI0_IRQ_CLEAR + 0x10 * r, 0x7fff);
	}
	wr(c, CSID_IRQ_CMD, 1);
	wr(c, CSID_RX_CFG1, 0x80);
	for (unsigned r = 0; r < 4; r++) {
		wr(c, CSID_RDI0_CFG0 + 0x100 * r, 2);
	}
	for (unsigned hw = 0; hw < 2; hw++) {
		wr(c, CSID_TOP_IRQ_CLEAR, 1);
		wr(c, CSID_IRQ_CMD, 1);
		if (hw) {
			wr(c, CSID_TOP_IRQ_MASK, 1);
			wr(c, CSID_IRQ_CMD, 1);
		}
		wr(c, CSID_RST_STROBES, hw ? 0x1e : 0x1);
		if (!hw) {
			wr(c, CSID_TOP_IRQ_MASK, 1);
			wr(c, CSID_IRQ_CMD, 1);
		}
		if (!wait_bits(c, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
			printf("SWEET CAM: CSID %s reset does not finish (top irq %#x)\n", hw ? "hardware" : "register",
			    rd(c, CSID_TOP_IRQ_STATUS));
			give_up("a CSID that does not reset");
			return false;
		}
		wr(c, CSID_TOP_IRQ_CLEAR, 1);
		wr(c, CSID_IRQ_CMD, 1);
	}
	printf("SWEET CAM: CSID-lite 0 reset (version %#x)\n", rd(c, CSID_HW_VERSION));
	return true;
}

/* RX from PHY `phy' (-1: the test pattern), RDI 0 for RAW10 of vc, written packed as it comes */
static bool
csid_config(int phy, unsigned lanes, uint32_t lane_cfg, uint32_t vc, uint32_t width, uint32_t height)
{
	vm_offset_t c = cap.csid;
	uint32_t cfg0 = vc << 22 | RAW10_DT << 16 | 0xfu << 12 | 1u << 2 | 3u;
	if (!bput(c, "CSID", CSID_RX_CFG0, (lanes - 1) | lane_cfg << 4 | (uint32_t)(phy < 0 ? 0 : phy & 3) << 20, true) ||
	    !bput(c, "CSID", CSID_RX_CFG1, 1u << 6 | 1u, true)) {
		return false;
	}
	if (phy < 0 && (!bput(c, "CSID", CSID_TPG_VC_CFG0, 1u << 10 | vc, true) ||
	    !bput(c, "CSID", CSID_TPG_VC_CFG1, 0x3ffu << 12 | 0x740, true) || !bput(c, "CSID", CSID_TPG_LFSR_SEED, 0x12345678, true) ||
	    !bput(c, "CSID", CSID_TPG_DT0_CFG0, width << 16 | height, true) || !bput(c, "CSID", CSID_TPG_DT0_CFG1, RAW10_DT, true) ||
	    !bput(c, "CSID", CSID_TPG_DT0_CFG2, (3u - 1) << 16 | 0x8, true) || !bput(c, "CSID", CSID_TPG_COLOR_BARS, 1u << 5, true) ||
	    !bput(c, "CSID", CSID_TPG_COMMON_GEN, 0, true))) {
		return false;
	}
	if (!bput(c, "CSID", CSID_RDI0_CFG0, cfg0, true) || !bput(c, "CSID", CSID_RDI0_CFG1, 2, true) ||
	    !bput(c, "CSID", CSID_RDI0_FRM_DROP_PER, 1, true) || !bput(c, "CSID", CSID_RDI0_FRM_DROP_PAT, 0, true) ||
	    !bput(c, "CSID", CSID_RDI0_IRQ_SUB_PER, 1, true) || !bput(c, "CSID", CSID_RDI0_IRQ_SUB_PAT, 0, true) ||
	    !bput(c, "CSID", CSID_RDI0_PIX_DROP_PAT, 0, true) || !bput(c, "CSID", CSID_RDI0_PIX_DROP_PER, 1, true) ||
	    !bput(c, "CSID", CSID_RDI0_LINE_DROP_PAT, 0, true) || !bput(c, "CSID", CSID_RDI0_LINE_DROP_PER, 1, true) ||
	    !bput(c, "CSID", CSID_RDI0_CTRL, 0, true) || !bput(c, "CSID", CSID_RDI0_CFG0, cfg0 | 1u << 31, true)) {
		return false;
	}
	cap.csid_on = true;
	return bput(c, "CSID", CSID_RDI0_CTRL, 1, true) &&
	       bput(c, "CSID", CSID_RDI0_IRQ_MASK, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6, true);
}

/* cam_vfe_top_reset(), cam_vfe_bus_init_hw(), start_wm() and update_wm() for RDI 0 in frame mode */
static bool
ife_config(uint32_t address, uint32_t frame_inc)
{
	vm_offset_t f = cap.ife;
	wr(f, IFE_IRQ_MASK0, 1u << 31);
	wr(f, IFE_GLOBAL_RESET, 0x3f9f);
	if (!wait_bits(f, IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100000)) {
		printf("SWEET CAM: IFE-lite does not reset (irq %#x)\n", rd(f, IFE_IRQ_STATUS0));
		give_up("an IFE that does not reset");
		return false;
	}
	wr(f, IFE_IRQ_CLEAR0, 0xffffffffu);
	wr(f, IFE_IRQ_CLEAR1, 0xffffffffu);
	wr(f, IFE_IRQ_GLOBAL_CLEAR, 1);
	printf("SWEET CAM: IFE-lite 0 reset (version %#x, bus %#x)\n", rd(f, IFE_HW_VERSION), rd(f, BUS_HW_VERSION));
	if (!bput(f, "IFE", BUS_ADDR_SYNC_FRAME_HDR, 0, true) || !bput(f, "IFE", BUS_CGC_OVD, 0xfffff, false) ||
	    !bput(f, "IFE", BUS_TEST_CTRL, 0, false) || !bput(f, "IFE", BUS_ADDR_SYNC_NO_SYNC, 0xffffff, false) ||
	    !bput(f, "IFE", WM0_BURST_LIMIT, 0xf, true) || !bput(f, "IFE", WM0_WIDTH, 0xff01, true) ||
	    !bput(f, "IFE", WM0_HEIGHT, 0, true) || !bput(f, "IFE", WM0_PACKER, 0, true) ||
	    !bput(f, "IFE", WM0_STRIDE, 0xff01, true) || !bput(f, "IFE", WM0_FRAMEDROP_PERIOD, 0, true) ||
	    !bput(f, "IFE", WM0_FRAMEDROP_PATTERN, 1, true) || !bput(f, "IFE", WM0_IRQ_SUB_PERIOD, 0, true) ||
	    !bput(f, "IFE", WM0_IRQ_SUB_PATTERN, 1, true) || !bput(f, "IFE", BUS_IRQ_MASK1, 1, false) ||
	    !bput(f, "IFE", WM0_IMAGE_ADDR, address, false) || !bput(f, "IFE", WM0_FRAME_INC, frame_inc, true) ||
	    !bput(f, "IFE", WM0_CFG, 3, true)) {
		return false;
	}
	cap.ife_on = true;
	return bput(f, "IFE", IFE_REG_UPDATE, 0x2, false);
}

static void
show_paths(const char *when)
{
	vm_offset_t c = cap.csid, f = cap.ife;
	printf("SWEET CAM: %s: CSID rx irq %#x pkts %u ecc %#x crc %u; RDI0 irq %#x status %#x bytes %u/%u; IFE irq %#x %#x "
	    "violation %#x; bus irq %#x %#x %#x comp err %#x; WM0 status %#x %#x\n", when, rd(c, CSID_RX_IRQ_STATUS),
	    rd(c, CSID_RX_PKTS), rd(c, CSID_RX_ECC), rd(c, CSID_RX_CRC), rd(c, CSID_RDI0_IRQ_STATUS), rd(c, CSID_RDI0_STATUS),
	    rd(c, CSID_RDI0_BYTES_PING), rd(c, CSID_RDI0_BYTES_PONG), rd(f, IFE_IRQ_STATUS0), rd(f, IFE_IRQ_STATUS1),
	    rd(f, IFE_VIOLATION_STATUS), rd(f, BUS_IRQ_STATUS0), rd(f, BUS_IRQ_STATUS1), rd(f, BUS_IRQ_STATUS2),
	    rd(f, BUS_COMP_ERROR), rd(f, WM0_STATUS0), rd(f, WM0_STATUS1));
}

static void
paths_down(void)
{
	if (cap.tpg_on) {
		wr(cap.csid, CSID_TPG_CTRL, 0);
		cap.tpg_on = false;
	}
	if (cap.csid_on) {
		wr(cap.csid, CSID_RDI0_IRQ_MASK, 0);
		wr(cap.csid, CSID_RDI0_CTRL, (rd(cap.csid, CSID_RDI0_CTRL) & ~3u) | 2u);
		wr(cap.csid, CSID_RDI0_CFG0, rd(cap.csid, CSID_RDI0_CFG0) & ~(1u << 31));
		wr(cap.csid, CSID_RX_CFG0, 0);
		cap.csid_on = false;
	}
	if (cap.ife_on) {
		wr(cap.ife, WM0_CFG, 0);
		wr(cap.ife, IFE_REG_UPDATE, 0x2);
		delay(1000);
		cap.ife_on = false;
	}
}

/*
 * r17a: "c 20" took the phone down with nothing in the log (the UDP log loses what a hang leaves
 * unsent). Each stage now says what comes next and gives the log 300 ms to carry it out.
 */
static void
cstep(const char *what)
{
	printf("SWEET CAM: next: %s\n", what);
	sleep_ms(300);
}

/* everything up to the clocks; false on a fault (cap_down() takes back what was done) */
static bool
cap_up(unsigned phy)
{
	cstep("mappings");
	if (!cap.camcc) {
		cap.camcc = ml_io_map_unmappable(CAMCC_PA, 0x10000, VM_WIMG_IO);
		cap.csid = ml_io_map_unmappable(CSID_LITE_PA, 0x1000, VM_WIMG_IO);
		cap.ife = ml_io_map_unmappable(IFE_LITE_PA, 0x4000, VM_WIMG_IO);
		cap.smmu = ml_io_map_unmappable(SMMU_PA, 0x20000, VM_WIMG_IO);  /* page 0 and GR1 (4 or 64 KB pages) */
	}
	if (!cap.camcc || !cap.csid || !cap.ife || !cap.smmu) {
		give_up("mappings");
		return false;
	}
	cstep("frame memory");
	if (!cap.frames_pa) {
		if (sweet_gpu_pool_alloc(CAP_POOL_BYTES, &cap.frames_pa) != 0 || cap.frames_pa + CAP_POOL_BYTES > 0x100000000ull) {
			cap.frames_pa = 0;
			give_up("no frame memory below 4 GB (GPU pool)");
			return false;
		}
		cap.frames_va = ml_io_map_unmappable(cap.frames_pa, CAP_POOL_BYTES, VM_WIMG_WCOMB);
		printf("SWEET CAM: %u MB for frames at %#llx (GPU pool)\n", CAP_POOL_BYTES >> 20, (unsigned long long)cap.frames_pa);
	}
	cm.broken = false;
	cstep("titan");
	if (!titan_up()) {
		return false;
	}
	cstep("PLL0");
	if (!pll0_up()) {
		return false;
	}
	cstep("clocks");
	if (!cap_clocks_up(phy)) {
		return false;
	}
	cstep("SMMU");
	return cap_smmu_up();
}

static void
cap_down(void)
{
	bool was = cm.broken;
	paths_down();
	cap_smmu_down();
	cap_clocks_down();
	pll0_down();
	titan_down();
	cm.broken = was;
}

/* the frame memory: a marker pattern before, and a look at what is there after */
static void
frames_mark(uint32_t bytes)
{
	volatile uint32_t *w = (volatile uint32_t *)cap.frames_va;
	for (uint32_t i = 0; i < bytes / 4; i++) {
		w[i] = 0x5a5a5a5au;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
frames_look(uint32_t bytes)
{
	volatile uint32_t *w = (volatile uint32_t *)cap.frames_va;
	uint32_t untouched = 0;
	for (uint32_t i = 0; i < bytes / 4; i += 64) {
		untouched += w[i] == 0x5a5a5a5au;
	}
	printf("SWEET CAM: frame at %#llx: first words %08x %08x %08x %08x, middle %08x, last %08x; %u of %u samples untouched\n",
	    (unsigned long long)cap.frames_pa, w[0], w[1], w[2], w[3], w[bytes / 8], w[bytes / 4 - 1], untouched, bytes / 256);
}

/* "c 20": the CSID's test pattern, 1600 x 1200 RAW10 colour bars, one frame */
static int
capture_tpg(void)
{
	const uint32_t width = 1600, height = 1200, stride = width * 10 / 8, bytes = stride * height;
	int result = EIO;
	cm.s = &sensors[2];     /* (any: titan_up() wants none of it) */
	if (cap_up(2)) {
		cstep("marking the frame memory");
		frames_mark(bytes);
		cstep("CSID reset");
		bool ok = csid_reset();
		if (ok) {
			cstep("CSID configuration");
			ok = csid_config(-1, 4, 0x3210, TPG_VC, width, height);
		}
		if (ok) {
			cstep("IFE configuration");
			ok = ife_config((uint32_t)cap.frames_pa, bytes);
		}
		if (ok) {
			show_paths("configured");
			cstep("test pattern on");
			if (bput(cap.csid, "CSID", CSID_TPG_CTRL, 4u << 20 | 0x80u << 8 | 3u << 4 | 7u, true)) {
				cap.tpg_on = true;
				for (unsigned ms = 0; ms < 2000; ms += 50) {
					sleep_ms(50);
					if (rd(cap.ife, BUS_IRQ_STATUS1) & 1u) {
						printf("SWEET CAM: write master 0 done after %u ms\n", ms + 50);
						result = 0;
						break;
					}
				}
				show_paths(result ? "no frame done" : "frame done");
			}
		}
	}
	cstep("taking it all down");
	cap_smmu_faults();
	cap_down();
	if (cap.frames_va) {
		frames_look(bytes);
		printf("SWEET CAM: TPG frame: files read /dev/pmem %llu %u (1600x1200 RAW10, stride %u)\n", (unsigned long long)cap.frames_pa, bytes, stride);
	}
	return result;
}

int
sweet_cam_cmd(unsigned what)
{
	if (cm.mode < 2 || !cm.tlmm) {
		printf("SWEET CAM: not ready (sweetcam=%u)\n", cm.mode);
		return ENXIO;
	}
	lck_mtx_lock(&sweet_cam_lock);
	int err = 0;
	if (what == 9) {
		look();
	} else if (what == 10) {
		look_pmic();
	} else if (what == 11) {
		look_slg();
	} else if (what == 12) {
		look_camcc();
	} else if (what == 20) {
		err = capture_tpg();
	} else if (what < sizeof(sensors) / sizeof(sensors[0])) {
		err = probe(what);
	} else {
		err = EINVAL;
	}
	lck_mtx_unlock(&sweet_cam_lock);
	return err;
}

static void
cam_thread(void *arg __unused, wait_result_t wr __unused)
{
	sleep_ms(12000);        /* after the haptic driver has SE9 up (8 s) */
	cm.gcc = ml_io_map(GCC_CAM_PA, 0x40);
	cm.camcc = ml_io_map(CAMCC_TOP_PA, 0x200);
	cm.mclk = ml_io_map(CAMCC_MCLK_PA, 0x100);
	cm.tlmm = ml_io_map(TLMM_CAM_PA, 0x10000);
	cm.sp_core = ml_io_map(SP_CORE_PA, 0x1100);
	cm.sp_cnfg = ml_io_map(SP_CNFG_PA, 0x1000);
	if (!cm.gcc || !cm.camcc || !cm.mclk || !cm.tlmm || !cm.sp_core || !cm.sp_cnfg) {
		printf("SWEET CAM: mapping failed\n");
		cm.tlmm = 0;
		goto out;
	}
	/*
	 * sweetcam=2 does nothing at boot ("c 9" is the look); the look at boot is sweetcam=1 or 3.
	 */
	if (cm.mode == 1 || cm.mode >= 3) {
		lck_mtx_lock(&sweet_cam_lock);
		look();
		lck_mtx_unlock(&sweet_cam_lock);
	}
	if (cm.mode >= 3) {
		for (unsigned i = 0; i < sizeof(sensors) / sizeof(sensors[0]); i++) {
			(void)sweet_cam_cmd(i);
			sleep_ms(200);
		}
	}
	printf("SWEET CAM: %s (sweetcam=%u)\n", cm.mode >= 2 ? "ready: 'c N' probes sensor N" : "look only", cm.mode);
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_cam_init(void)
{
	PE_parse_boot_argn("sweetcam", &cm.mode, sizeof(cm.mode));
	if (!cm.mode) {
		return;
	}
	thread_t t;
	if (kernel_thread_start(cam_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
