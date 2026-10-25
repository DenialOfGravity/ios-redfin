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
	bool mclk24;                    /* 0922: MCLK at 24 MHz from camcc PLL2 (running), not the crystal's 19.2 */
	uint32_t found_mclk_mnd[3];
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
		bool ok = false;
		/*
		 * 0926: three tries. r18d: after a dozen quick sensor switches (mediaserverd restarting), gpio8's MODE_CTL
		 * read back 0 once and the camera was given up for the rest of the boot.
		 */
		for (unsigned t = 0; t < 3 && !ok; t++) {
			v = vals[i];
			back = 0;
			ok = !spmi(PGPIO_SID, base + regs[i], true, &v) && !spmi(PGPIO_SID, base + regs[i], false, &back) &&
			    (back & 0x87) == (vals[i] & 0x87);
			if (!ok) {
				delay(200);
			}
		}
		if (!ok) {
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
		for (unsigned k = 0; k < 3; k++) {
			wr(cm.mclk, s->mclk_rcg + 8 + 4 * k, cm.found_mclk_mnd[k]);
		}
		wr(cm.mclk, s->mclk_rcg, rd(cm.mclk, s->mclk_rcg) | 1u);
		(void)wait_bits(cm.mclk, s->mclk_rcg, 1u, 0, 500);
		cm.mclk_on = false;
		return true;
	}
	cm.found_mclk_cfg = rd(cm.mclk, s->mclk_rcg + 4);
	for (unsigned k = 0; k < 3; k++) {
		cm.found_mclk_mnd[k] = rd(cm.mclk, s->mclk_rcg + 8 + 4 * k);
	}
	cm.found_mclk_cbcr = rd(cm.mclk, s->mclk_cbcr);
	cm.found_mclk_pin = rd(cm.tlmm, GPIO_CFG(s->mclk_pin));
	if (cm.found_mclk_cbcr & 1u) {
		give_up("MCLK on already (somebody else's)");
		return false;
	}
	cm.mclk_on = true;
	/*
	 * 19.2 MHz: XO, divider 1, no M/N. 24 MHz (0922, cm.mclk24, camcc PLL2 running): ftbl_cam_cc_mclk0_clk_src's
	 * F(24000000, P_CAM_CC_PLL2_OUT_AUX, 1, 1, 20): source 5 (PLL2 aux, 480 MHz), divider 1, M/N 1/20 dual edge,
	 * as __clk_rcg2_configure() writes them (M 1, N ~(20 - 1), D ~20, 8 bits each).
	 */
	uint32_t cfg = cm.mclk24 ? 5u << 8 | 2u << 12 | 1u : 1u;
	if (cm.mclk24) {
		wr(cm.mclk, s->mclk_rcg + 8, (cm.found_mclk_mnd[0] & ~0xffu) | 1u);
		wr(cm.mclk, s->mclk_rcg + 12, (cm.found_mclk_mnd[1] & ~0xffu) | (~(20u - 1u) & 0xffu));
		wr(cm.mclk, s->mclk_rcg + 16, (cm.found_mclk_mnd[2] & ~0xffu) | (~20u & 0xffu));
	}
	wr(cm.mclk, s->mclk_rcg + 4, (cm.found_mclk_cfg & ~0x10371fu) | cfg);
	wr(cm.mclk, s->mclk_rcg, rd(cm.mclk, s->mclk_rcg) | 1u);
	if (!wait_bits(cm.mclk, s->mclk_rcg, 1u, 0, 500) || (rd(cm.mclk, s->mclk_rcg + 4) & 0x371fu) != cfg) {
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

/*
 * The CCI pins as I2C lines (pull-up, driven low by the output enable) for a transfer or, held (0922), for a
 * whole session of writes; given back as they were found, the clock line first (the data line falling while the
 * clock is high would be a start condition).
 */
static struct { bool held, taken; uint32_t cfg[2], io[2]; } bus;

static void
bus_take(void)
{
	const struct sensor *s = cm.s;
	if (bus.taken) {
		return;
	}
	bus.cfg[0] = rd(cm.tlmm, GPIO_CFG(s->sda));
	bus.cfg[1] = rd(cm.tlmm, GPIO_CFG(s->scl));
	bus.io[0] = rd(cm.tlmm, GPIO_IO(s->sda));
	bus.io[1] = rd(cm.tlmm, GPIO_IO(s->scl));
	bus.taken = true;
	line(s->sda, true);
	line(s->scl, true);
	half_bit();
}

static void
bus_give(void)
{
	const struct sensor *s = cm.s;
	if (!bus.taken || bus.held) {
		return;
	}
	wr(cm.tlmm, GPIO_IO(s->scl), bus.io[1] & 2u);
	wr(cm.tlmm, GPIO_CFG(s->scl), bus.cfg[1]);
	wr(cm.tlmm, GPIO_IO(s->sda), bus.io[0] & 2u);
	wr(cm.tlmm, GPIO_CFG(s->sda), bus.cfg[0]);
	bus.taken = false;
}

/* a 16-bit register's two bytes; false if nobody answered */
static bool
sensor_read16(uint8_t slave, uint16_t reg, uint16_t *val, const char **why)
{
	const struct sensor *s = cm.s;
	bool ok = false;
	bus_take();
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
	bus_give();
	return ok;
}

/* 0922: one register write (n bytes: the address's and the data's), every byte acknowledged */
static bool
sensor_write(uint8_t slave, const uint8_t *b, unsigned n)
{
	const struct sensor *s = cm.s;
	bool ok = false;
	bus_take();
	if (level(s->sda) && level(s->scl)) {
		i2c_start(s->sda, s->scl);
		ok = i2c_byte(s->sda, s->scl, (uint8_t)(slave << 1));
		for (unsigned i = 0; ok && i < n; i++) {
			ok = i2c_byte(s->sda, s->scl, b[i]);
		}
		i2c_stop(s->sda, s->scl);
	}
	bus_give();
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
	uint32_t cb_last;       /* + 1: the bank of the last run, taken again (r17d/f: its CBAR does not go back to 0) */
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
	if (cap.cb_last) {
		uint32_t c = cap.cb_last - 1;
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd(cap.smmu, (npage + c) * psize) & 1u) &&
		    rd(cap.smmu, psize + 4 * c) == 0x1f000) {
			cb = c;
		}
	}
	for (uint32_t c = 0; cb == ~0u && c < ncb && c < 256; c++) {
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd(cap.smmu, (npage + c) * psize) & 1u) && rd(cap.smmu, psize + 4 * c) == 0) {
			cb = c;
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
	cap.cb_last = cb + 1;
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
	wr(cap.smmu, cap.smmu_gr1 + 0x800 + 4 * cap.cb_n, 0);
	wr(cap.smmu, 0x70, 0);
	for (int i = 0; i < 1000 && (rd(cap.smmu, 0x74) & 1u); i++) {
		delay(10);
	}
	/* r17d: each run took the next bank (CB6, 7, 8): the reads of the given-back one, to see why */
	printf("SWEET CAM: SMMU CB%u given back: CBAR %#x CBA2R %#x SCTLR %#x; SMR%u %#x S2CR %#x\n", cap.cb_n,
	    rd(cap.smmu, cap.smmu_gr1 + 4 * cap.cb_n), rd(cap.smmu, cap.smmu_gr1 + 0x800 + 4 * cap.cb_n), rd(cap.smmu_cb, 0),
	    cap.smr_n, rd(cap.smmu, 0x800 + 4 * cap.smr_n), rd(cap.smmu, 0xc00 + 4 * cap.smr_n));
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
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_IRQ_GLOBAL_CLEAR    0x2068
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
	if (phy >= 0) {
		wr(c, CSID_RX_IRQ_MASK, 0x0fffffffu);   /* 0922: the receiver's errors and events seen in show_paths() */
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

/* the frame memory of "c 20" and "c 21": CAP_POOL_BYTES of the GPU pool, below 4 GB, kept */
static bool
cap_frames(void)
{
	if (!cap.frames_pa) {
		if (sweet_gpu_pool_alloc(CAP_POOL_BYTES, &cap.frames_pa) != 0 || cap.frames_pa + CAP_POOL_BYTES > 0x100000000ull) {
			cap.frames_pa = 0;
			give_up("no frame memory below 4 GB (GPU pool)");
			return false;
		}
		cap.frames_va = ml_io_map_unmappable(cap.frames_pa, CAP_POOL_BYTES, VM_WIMG_WCOMB);
		printf("SWEET CAM: %u MB for frames at %#llx (GPU pool)\n", CAP_POOL_BYTES >> 20, (unsigned long long)cap.frames_pa);
	}
	return cap.frames_va != 0;
}

/*
 * r17a: "c 20" took the phone down with nothing in the log (the UDP log loses what a hang leaves
 * unsent). Each stage now says what comes next and gives the log 300 ms to carry it out.
 */
static unsigned cam_step_ms;            /* "c 22 MS" (r17c: the UDP log drops what is printed during a poke; the
					 * ring, read with the phone agent's klog, has it, given the time to read it); 0923: 0 by
					 * default, and then the stages are not named */

static void
cstep(const char *what)
{
	if (cam_step_ms) {
		printf("SWEET CAM: next: %s\n", what);
		sleep_ms(cam_step_ms);
	}
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
		cap.smmu = ml_io_map_unmappable(SMMU_PA, 0x100000, VM_WIMG_IO); /* all of it: cap_smmu_up() reads the banks' SCTLRs */
	}
	if (!cap.camcc || !cap.csid || !cap.ife || !cap.smmu) {
		give_up("mappings");
		return false;
	}
	cstep("frame memory");
	if (!cap_frames()) {
		return false;
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

/* ==== 0922: the capture path, step 2: a sensor's own frames ("c 21 SENSOR MODE OPTIONS") ==== */

/*
 * HackiOS redwood_camera.c's cam=4 with redfin's sensors. The sequence is Android's (cam_sensor, cam_csiphy,
 * cam_ife_csid, cam_vfe): the capture clocks and the SMMU as for "c 20"; camcc's PLL2 for the 24 MHz MCLK the
 * device tree asks for (cam-sensor clock-rates 24000000; the module files' tables start with EXCK_FREQ 0x136 =
 * 24.00 MHz); the sensor powered up as for "c N", its chip ID, its tables "init" and "resM"; CSID-lite 0 from the
 * sensor's PHY and RDI 0, IFE-lite 0's write master into the frame memory; the sensor's CSIPHY; the sensor's
 * "on", frames until the wanted one is written, the write master stopped at that frame's end, "off"; everything
 * down again.
 *
 * The tables: tools/redfin-camtab.py makes /private/var/local/fw/camera-<sensor>.tab out of the sensor's module
 * file (the vendor's: on the phone only, never in git), read at run time (redwood_adsp_fw_file(), sweet_adsp.c).
 *   SENSOR   0 rear IMX363 (CSIPHY 0), 1 rear ultra-wide IMX481 (CSIPHY 1), 2 front IMX355 (CSIPHY 2)
 *   MODE     the module file's mode (redfin-camtab.py lists them); one past the last: the one with the fewest pixels
 *   OPTIONS  [3:0] 0 the picture, N the sensor's test pattern table tpgN (tpg2: colour bars); [15:8] the CSIPHY
 *            settle count (0: the module file's, in periods of the 300 MHz timer); [31:16] the exposure in lines
 *            (0: the tables'); [47:32] the analog gain code (0x0204, 0: the tables'); [55:48] frames to wait for
 *            (0: 8, the last kept)
 *
 * The CSIPHY ("qcom,csiphy-v1.2.2": cam_csiphy_soc.c gives it the 1.2 tables of cam_csiphy_1_2_hwreg.h):
 * csiphy_reset_reg_1_2, csiphy_common_reg_1_2 with the lanes to enable, csiphy_2ph_v1_2_reg for each lane position
 * in use (data lane 0, the clock lane, data lanes 1, 2, 3) with the settle count, csiphy_irq_reg_1_2. Its supplies
 * are the titan top GDSC, refgen (0xff1000, qcom,refgen-kona-regulator: PWRDWN_CTRL5 bit 0) and PM8150 L5 (the UFS
 * and DSI PHYs' too, so on).
 */
extern const uint8_t *redwood_adsp_fw_file(const char *name, uint64_t *size);
extern void redwood_adsp_fw_free(const uint8_t *data, uint64_t size);

#define REFGEN_PA               0x00ff1000u
#define REFGEN_PWRDWN_CTRL5     0x080u

struct tab_mode { uint32_t width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, dt, cfa, table; };
struct tab_entry { uint16_t address, data; uint8_t address_bytes, data_bytes, operation, pad; uint32_t delay_us; };

static const struct { const char *file; unsigned phy; } sensor_capture[] = {
	{ "camera-imx363.tab", 0 }, { "camera-imx481.tab", 1 }, { "camera-imx355.tab", 2 },
};

static struct {
	const uint8_t *data;
	uint64_t size;
	uint32_t slave, lane_assign, modes, tables;
	const struct tab_mode *mode;
	vm_offset_t csiphy, refgen, csiphy_map[4];    /* csiphy: the one in use (r17f: one mapping kept for all, the first PHY's) */
	bool pll2_on, csiphy_on, refgen_switched;
	/* 0931: the table file's "info" (tools/redfin-camtab.py, 2026-10-25), else 0: margin in lines, black level, the most
	 * analog and digital gain (x 256), the first digital gain register (Sony: four 16-bit ones, 8.8, from 0x020e) */
	uint32_t margin, black, again_max, dgain_max, dgain_reg;
} sc;

static void
tab_free(void)
{
	if (sc.data) {
		redwood_adsp_fw_free(sc.data, sc.size);
		sc.data = NULL;
	}
}

static const struct tab_entry *tab_find(const char *name, uint32_t *count);

/* 0931: the "info" table's { key, value } entries (none in table files made before it: then all 0) */
static void
tab_info(void)
{
	uint32_t n = 0;
	const struct tab_entry *e = tab_find("info", &n);
	sc.margin = sc.black = sc.again_max = sc.dgain_max = sc.dgain_reg = 0;
	for (uint32_t i = 0; e && i < n; i++) {
		switch (e[i].address) {
		case 1: sc.margin = e[i].data; break;
		case 2: sc.black = e[i].data; break;
		case 3: sc.again_max = e[i].data; break;
		case 4: sc.dgain_max = e[i].data; break;
		case 5: sc.dgain_reg = e[i].data; break;
		default: break;
		}
	}
}

static bool
tab_load(const char *name)
{
	sc.data = redwood_adsp_fw_file(name, &sc.size);
	if (sc.data == NULL) {
		printf("SWEET CAM: no /private/var/local/fw/%s (tools/redfin-camtab.py makes it)\n", name);
		return false;
	}
	const uint32_t *h = (const uint32_t *)(const void *)(sc.data + 8);
	if (sc.size < 24 || memcmp(sc.data, "RFCTAB01", 8) != 0 || h[2] == 0 || h[2] > 32 ||
	    24 + (uint64_t)h[2] * sizeof(struct tab_mode) > sc.size) {
		printf("SWEET CAM: %s: not a table file of the kind known here\n", name);
		tab_free();
		return false;
	}
	sc.slave = h[0];
	sc.lane_assign = h[1];
	sc.modes = h[2];
	sc.tables = h[3];
	sc.mode = (const struct tab_mode *)(const void *)(sc.data + 24);
	tab_info();
	return true;
}

static const struct tab_entry *
tab_find(const char *name, uint32_t *count)
{
	uint64_t at = 24 + (uint64_t)sc.modes * sizeof(struct tab_mode);
	for (uint32_t t = 0; t < sc.tables && at + 12 <= sc.size; t++) {
		uint32_t n = *(const uint32_t *)(const void *)(sc.data + at + 8);
		if (at + 12 + (uint64_t)n * sizeof(struct tab_entry) > sc.size) {
			break;
		}
		bool same = true;
		for (unsigned k = 0; k < 8 && same; k++) {
			same = sc.data[at + k] == (uint8_t)name[k];
			if (name[k] == 0) {
				break;
			}
		}
		if (same) {
			*count = n;
			return (const struct tab_entry *)(const void *)(sc.data + at + 12);
		}
		at += 12 + (uint64_t)n * sizeof(struct tab_entry);
	}
	*count = 0;
	return NULL;
}

/* one register of the sensor's: 1 or 2 bytes of address, 1 or 2 of data */
static bool
sensor_put(uint16_t address, unsigned address_bytes, uint16_t data, unsigned data_bytes)
{
	uint8_t b[4];
	unsigned n = 0;
	if (address_bytes == 2) {
		b[n++] = (uint8_t)(address >> 8);
	}
	b[n++] = (uint8_t)address;
	if (data_bytes == 2) {
		b[n++] = (uint8_t)(data >> 8);
	}
	b[n++] = (uint8_t)data;
	return sensor_write((uint8_t)sc.slave, b, n);
}

/* one of the sensor's tables in order, as cam_sensor writes it (the writes are not read back) */
static bool
sensor_table(const char *name)
{
	uint32_t count;
	const struct tab_entry *e = tab_find(name, &count);
	if (e == NULL) {
		printf("SWEET CAM: the table file has no \"%s\"\n", name);
		give_up("a sensor table missing");
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 ||
		    e[i].data_bytes > 2) {
			printf("SWEET CAM: \"%s\" %u: operation %u, %u/%u bytes: not known here\n", name, i, e[i].operation,
			    e[i].address_bytes, e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		if (!sensor_put(e[i].address, e[i].address_bytes, e[i].data, e[i].data_bytes)) {
			printf("SWEET CAM: \"%s\" %u: %#x <- %#x not acknowledged\n", name, i, e[i].address, e[i].data);
			give_up("a sensor write nobody acknowledged");
			return false;
		}
		if (e[i].delay_us >= 1000) {
			sleep_ms(e[i].delay_us > 100000 ? 100 : e[i].delay_us / 1000);
		} else if (e[i].delay_us) {
			delay(e[i].delay_us);
		}
	}
	printf("SWEET CAM: the sensor's \"%s\": %u writes\n", name, count);
	return true;
}

/* ---- camcc's PLL2 (Zonda) for the 24 MHz MCLK: cam_cc_pll2_config, clk_zonda_pll_configure() + _enable() ---- */

#define PLL2                    0x2000u
#define Z_MODE                  (PLL2 + 0x00)   /* [0] outputs, [1] bypass off, [2] out of reset, [20] FSM vote, [29] freq lock, [31] lock */
#define Z_L                     (PLL2 + 0x04)
#define Z_USER                  (PLL2 + 0x0c)   /* [3:0] the outputs, [9:8] the post-divider (1: /2) */
#define Z_CONFIG                (PLL2 + 0x10)
#define Z_CONFIG_U              (PLL2 + 0x14)
#define Z_TEST                  (PLL2 + 0x1c)   /* [16] stays in CFA: then the frequency lock is what to wait for */
#define Z_OPMODE                (PLL2 + 0x28)

static bool
pll2_up(void)
{
	uint32_t mode = crd(Z_MODE);
	if (mode & (1u | 1u << 20)) {
		printf("SWEET CAM: camcc PLL2 mode %#x: running or voted (somebody else's)\n", mode);
		give_up("PLL2 in use");
		return false;
	}
	/* L 0x32: 960 MHz; the aux output /2: 480 MHz, MCLK's source 5 */
	if (!cwr(Z_L, 0x32, ~0u) || !cwr(Z_CONFIG, 0x08200920, ~0u) || !cwr(Z_CONFIG_U, 0x15008001, ~0u) ||
	    !cwr(Z_USER, 0xe0000101, ~0u) || !cwr(Z_TEST, 0x00010000, ~0u)) {
		return false;
	}
	sc.pll2_on = true;
	if (!cwr(Z_MODE, crd(Z_MODE) & ~2u, 2u) || !cwr(Z_MODE, crd(Z_MODE) & ~1u, 1u) || !cwr(Z_OPMODE, 0, 1u) ||
	    !cwr(Z_MODE, crd(Z_MODE) | 4u, 4u)) {
		return false;
	}
	if (!cwr(Z_MODE, crd(Z_MODE) | 2u, 2u)) {
		return false;
	}
	delay(1);
	if (!cwr(Z_MODE, crd(Z_MODE) | 4u, 4u) || !cwr(Z_OPMODE, 1, 1u)) {
		return false;
	}
	uint32_t lock = (crd(Z_TEST) & (1u << 16)) ? 1u << 29 : 1u << 31;
	if (!wait_bits(cap.camcc, Z_MODE, lock, lock, 2000)) {
		printf("SWEET CAM: camcc PLL2 does not lock (mode %#x)\n", crd(Z_MODE));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!cwr(Z_USER, crd(Z_USER) | 0xfu, 0xfu) || !cwr(Z_MODE, crd(Z_MODE) | 1u, 1u)) {
		return false;
	}
	printf("SWEET CAM: camcc PLL2 locked at 960 MHz (mode %#x user %#x)\n", crd(Z_MODE), crd(Z_USER));
	return true;
}

static void
pll2_down(void)
{
	if (!sc.pll2_on) {
		return;
	}
	wr(cap.camcc, Z_MODE, crd(Z_MODE) & ~1u);
	wr(cap.camcc, Z_USER, crd(Z_USER) & ~0xfu);
	wr(cap.camcc, Z_MODE, crd(Z_MODE) & ~6u);
	wr(cap.camcc, Z_OPMODE, 0);
	sc.pll2_on = false;
	printf("SWEET CAM: camcc PLL2 off (mode %#x)\n", crd(Z_MODE));
}

/* ---- the CSIPHY ---- */

static bool
refgen_on(void)
{
	if (!sc.refgen) {
		sc.refgen = ml_io_map_unmappable(REFGEN_PA, 0x1000, VM_WIMG_IO);
	}
	if (!sc.refgen) {
		give_up("mappings");
		return false;
	}
	uint32_t v = rd(sc.refgen, REFGEN_PWRDWN_CTRL5);
	printf("SWEET CAM: refgen PWRDWN_CTRL5 %#x (%s)\n", v, (v & 1u) ? "on: the display's" : "off: switching it on");
	if (!(v & 1u)) {
		wr(sc.refgen, REFGEN_PWRDWN_CTRL5, v | 1u);
		sc.refgen_switched = true;
		delay(5);
	}
	return true;
}

static void
refgen_restore(void)
{
	if (sc.refgen_switched) {
		wr(sc.refgen, REFGEN_PWRDWN_CTRL5, rd(sc.refgen, REFGEN_PWRDWN_CTRL5) & ~1u);
		sc.refgen_switched = false;
	}
}

static void
csiphy_reset(void)
{
	vm_offset_t p = sc.csiphy;
	wr(p, 0x814, 0x00);
	sleep_ms(5);
	wr(p, 0x818, 0x00);
	wr(p, 0x81c, 0x00);
	wr(p, 0x800, 0x01);
	sleep_ms(1);
	wr(p, 0x800, 0x00);
}

/* lanes: data lanes in use; lane_assign: the physical data lane of each, 4 bits apiece */
static bool
csiphy_up(unsigned phy, unsigned lanes, uint32_t lane_assign, unsigned settle)
{
	static const uint8_t pos_of_lane[4] = { 0, 2, 3, 4 };           /* lane positions: data 0, clock, data 1, 2, 3 */
	static const uint16_t pos_base[5] = { 0x000, 0x700, 0x200, 0x400, 0x600 };
	static const uint8_t irq_mask[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t pos_mask = 1u << 1, lane_enable = 0x80;                /* the clock lane */
	for (unsigned l = 0; l < lanes; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;
		if (n > 3) {
			give_up("a lane assignment not known here");
			return false;
		}
		pos_mask |= 1u << pos_of_lane[n];
		lane_enable |= 1u << (2 * n);
	}
	if (!sc.csiphy_map[phy & 3]) {
		sc.csiphy_map[phy & 3] = ml_io_map_unmappable(CSIPHY_PA(phy & 3), 0x1000, VM_WIMG_IO);
	}
	sc.csiphy = sc.csiphy_map[phy & 3];
	if (!sc.csiphy) {
		give_up("mappings");
		return false;
	}
	vm_offset_t p = sc.csiphy;
	csiphy_reset();
	sc.csiphy_on = true;
	/* csiphy_common_reg_1_2 */
	wr(p, 0x814, lane_enable);
	wr(p, 0x818, 0x01);
	wr(p, 0x81c, 0x5a);
	wr(p, 0x800, 0x03);
	sleep_ms(1);
	wr(p, 0x800, 0x02);
	wr(p, 0x884, 0x01);
	wr(p, 0x824, 0x72);
	/* csiphy_2ph_v1_2_reg, each position in use */
	for (unsigned pos = 0; pos < 5; pos++) {
		if (!(pos_mask & (1u << pos))) {
			continue;
		}
		uint32_t b = pos_base[pos];
		bool clock = pos == 1;
		wr(p, b + 0x30, 0x00);
		wr(p, b + 0x2c, 0x01);
		wr(p, b + 0x34, 0x0f);
		wr(p, b + 0x10, 0x52);
		wr(p, b + 0x1c, 0x0a);
		wr(p, b + 0x14, 0x60);
		if (clock) {
			wr(p, b + 0x28, 0x04);
		}
		wr(p, b + 0x3c, 0xb8);
		wr(p, b + 0x00, clock ? 0x80 : 0x91);
		wr(p, b + 0x04, 0x0c);
		wr(p, b + 0x20, 0x00);
		wr(p, b + 0x24, 0x00);
		wr(p, b + 0x08, settle & 0xff);
		if (clock) {
			wr(p, b + 0x0c, 0xff);
			wr(p, b + 0x38, 0x1f);
		} else {
			wr(p, b + 0x38, 0xfe);
			wr(p, b + 0x5c, 0xc0);
			wr(p, b + 0x60, 0x0d);
		}
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(p, 0x82c + 4 * i, irq_mask[i]);
		wr(p, 0x858 + 4 * i, 0xff);
	}
	wr(p, 0x828, 0x1);
	wr(p, 0x828, 0x0);
	printf("SWEET CAM: CSIPHY %u: %u D-PHY lanes (positions %#x, enable %#x), settle count %u\n", phy, lanes, pos_mask,
	    lane_enable, settle);
	return true;
}

static void
show_csiphy(const char *when)
{
	if (!sc.csiphy) {
		return;
	}
	printf("SWEET CAM: CSIPHY %s: status %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", when,
	    rd(sc.csiphy, 0x8b0) & 0xff, rd(sc.csiphy, 0x8b4) & 0xff, rd(sc.csiphy, 0x8b8) & 0xff, rd(sc.csiphy, 0x8bc) & 0xff,
	    rd(sc.csiphy, 0x8c0) & 0xff, rd(sc.csiphy, 0x8c4) & 0xff, rd(sc.csiphy, 0x8c8) & 0xff, rd(sc.csiphy, 0x8cc) & 0xff,
	    rd(sc.csiphy, 0x8d0) & 0xff, rd(sc.csiphy, 0x8d4) & 0xff, rd(sc.csiphy, 0x8d8) & 0xff);
}

static void
csiphy_down(void)
{
	if (!sc.csiphy_on) {
		return;
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(sc.csiphy, 0x82c + 4 * i, 0);
	}
	csiphy_reset();
	sc.csiphy_on = false;
}

/* 0931: one of the table file's tables, quietly (for the group hold, at every exposure change); *found: it has it */
static bool
sensor_table_quiet(const char *name, bool *found)
{
	uint32_t count = 0;
	const struct tab_entry *e = sc.data ? tab_find(name, &count) : NULL;
	*found = e != NULL;
	for (uint32_t i = 0; e && i < count; i++) {
		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 ||
		    e[i].data_bytes > 2 || !sensor_put(e[i].address, e[i].address_bytes, e[i].data, e[i].data_bytes)) {
			return false;
		}
	}
	return true;
}

/* 0931: the group hold from the module file's "holdon"/"holdoff" tables, SMIA 0x0104 where the file has none */
static bool
group_hold(bool on)
{
	bool found = false, ok = sensor_table_quiet(on ? "holdon" : "holdoff", &found);
	return found ? ok : sensor_put(0x0104, 2, on ? 1 : 0, 1);
}

/*
 * the sensor's exposure (SMIA: 0x0202 coarse integration in lines, 0x0204 analog gain), in its group hold; 0931: and
 * the digital gain (dgain, 256 = 1x, 0 = as it is) into the four colours' registers (Sony 0x020e, 0x0210, 0x0212,
 * 0x0214, 8.8) where the table file names the first
 */
static bool
sensor_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines, uint32_t dgain)
{
	if (lines == 0 && gain == 0 && frame_lines == 0 && (dgain == 0 || !sc.dgain_reg)) {
		return true;
	}
	bool ok = group_hold(true);
	if (ok && frame_lines) {
		ok = sensor_put(0x0340, 2, (uint16_t)frame_lines, 2);
	}
	if (ok && lines) {
		ok = sensor_put(0x0202, 2, (uint16_t)lines, 2);
	}
	if (ok && gain) {
		ok = sensor_put(0x0204, 2, (uint16_t)gain, 2);
	}
	for (unsigned k = 0; ok && dgain && sc.dgain_reg && k < 4; k++) {
		ok = sensor_put((uint16_t)(sc.dgain_reg + 2 * k), 2, (uint16_t)dgain, 2);
	}
	ok = group_hold(false) && ok;
	if (!ok || cam_step_ms) {
		printf("SWEET CAM: exposure %u lines, analog gain code %#x, frame %u lines, digital gain %#x: %s\n", lines, gain,
		    frame_lines, dgain, ok ? "written" : "NOT acknowledged");
	}
	return ok;
}

/* ==== 0929: the rear camera's lens ==== */

/*
 * The IMX363 module's lens motor is driven by an ON Semiconductor LC898129, the module's AF and OIS controller
 * (with its own flash: no firmware download). Found 2026-10-25 in metric_imx363_lito2's module file
 * (tools/redfin-camsensor.py -a): actuatorName, oisName and eepromName are all "lc898129dp"; the live DT's
 * qcom,actuator@0 and qcom,ois@0 are on CCI 0 master 0 -- the IMX363's own lines -- with the supply cam_vaf
 * (camera_ldo, PM8150L GPIO 8, switched on in the IMX363's power-up, VAF +8 ms in the actuator's own). The
 * ultra-wide (IMX481) and the front camera (IMX355) are fixed-focus: their module files name no actuator.
 *
 * LC898129: slave 0x76 (7-bit 0x3b), 16-bit registers, 32-bit data sent most significant byte first. As the module
 * file's actuator and OIS settings use it:
 *   init      poll 0xF100 (status) until 0 (CamX gives it 100 ms) -- the controller is ready
 *   servo on  0xF01F <- 0, poll, 0xF010 <- 7, poll (the OIS part's init and centering-on lists; OIS itself, 0xF012,
 *             stays off here: no gyro)
 *   position  0xF01A <- 0x00020000 | code, code 12 bits; the module file's tuning: initial code 400, one region of
 *             325 steps of 1 code (so 400 ..725 until the per-unit calibration in the controller's flash is read;
 *             boot-args sweetcam_af_inf / sweetcam_af_mac set the codes of positions 0 and 1023)
 *   servo off 0xF010 <- 0x70000, poll, 0xF01F <- 2 (the OIS disable list's last writes)
 * The table below also has the usual single-coil VCM drivers, for a module that has one (boot-arg sweetcam_af=NAME
 * forces one, sweetcam_af=none none; by default the ones a sensor's module names are probed by a read of their
 * status/ID register). Positions for iOS: 0 (infinity) to 1023 (macro), REDWOOD_CAMERA_NO_FOCUS without a lens motor.
 */
struct lens_write { uint16_t reg; uint32_t val; };
struct lens_chip {
	const char *name;
	uint8_t slave;                  /* 7-bit */
	uint8_t reg_bytes, data_bytes;
	uint16_t probe_reg;             /* read to see it answers (and, with status_poll, polled until 0) */
	bool status_poll;
	uint16_t pos_reg;
	uint32_t pos_or;                /* OR'ed into every position write */
	uint8_t pos_shift;
	uint16_t code_max, inf, mac;    /* the codes of positions 0 and 1023 by default */
	struct lens_write on[4], off[4];        /* after the init / before the power goes; reg 0xffff ends */
};
#define LENS_END        { 0xffff, 0 }
static const struct lens_chip lens_chips[] = {
	{ "lc898129", 0x3b, 2, 4, 0xf100, true, 0xf01a, 0x00020000u, 0, 0xfff, 400, 725,
	  { { 0xf01f, 0 }, { 0xf010, 7 }, LENS_END }, { { 0xf010, 0x70000 }, { 0xf01f, 2 }, LENS_END } },
	/* AKM AK7375/AK7377: 10 bits at 0x00 (data shifted up 6), 0x02 = 0 active, 0x02 = 0x20 standby */
	{ "ak737x", 0x0c, 1, 2, 0x02, false, 0x00, 0, 6, 0x3ff, 0, 1023, { { 0x02, 0x00 }, LENS_END },
	  { { 0x02, 0x20 }, LENS_END } },
	/* Dongwoon DW9800-class: 0x02 control (0 = on), 10 bits at 0x03 */
	{ "dw98xx", 0x0c, 1, 2, 0x00, false, 0x03, 0, 0, 0x3ff, 0, 1023, { { 0x02, 0x00 }, LENS_END },
	  { { 0x02, 0x01 }, LENS_END } },
};
/* the ones each sensor's module names, in probe order (sensors[] order: IMX363, IMX481, IMX355) */
static const int8_t lens_of_sensor[3] = { 0, -1, -1 };

#define LENS_MAX        1023u
#define LENS_PARK_STEP  64u             /* positions per step when parking ... */
#define LENS_PARK_MS    4               /* ... and the time between steps */

static struct {
	const struct lens_chip *chip;   /* the one found at the last power-up; NULL: none */
	bool up, forced_none, args_read;
	char forced[16];
	uint32_t inf, mac;              /* codes of positions 0 and 1023 */
	uint32_t pos;                   /* the position last written (kept between sessions: the next power-up goes back there) */
	uint64_t moving_from, moving_until;     /* mach time: a frame exposed in between carries LENS_MOVING */
} lens = { .pos = 0 };

static void
lens_args(void)
{
	if (lens.args_read) {
		return;
	}
	lens.args_read = true;
	char b[16] = { 0 };
	if (PE_parse_boot_argn("sweetcam_af", b, sizeof(b))) {
		b[sizeof(b) - 1] = 0;
		if (b[0] == 0 || strcmp(b, "none") == 0) {     /* (sweetcam_af=0 comes as the number 0: an empty string) */
			lens.forced_none = true;
		} else {
			strlcpy(lens.forced, b, sizeof(lens.forced));
		}
	}
}

/* a register write of the lens chip's: reg_bytes of address, data_bytes of data, most significant first */
static bool
lens_put(const struct lens_chip *c, uint16_t reg, uint32_t val)
{
	uint8_t b[6];
	unsigned n = 0;
	if (c->reg_bytes == 2) {
		b[n++] = (uint8_t)(reg >> 8);
	}
	b[n++] = (uint8_t)reg;
	for (int k = (int)c->data_bytes - 1; k >= 0; k--) {
		b[n++] = (uint8_t)(val >> (8 * k));
	}
	return sensor_write(c->slave, b, n);
}

/* a register read of the lens chip's (data_bytes, most significant first); false if nobody answered */
static bool
lens_get(const struct lens_chip *c, uint16_t reg, uint32_t *val)
{
	const struct sensor *s = cm.s;
	bool ok = false;
	bus_take();
	if (level(s->sda) && level(s->scl)) {
		i2c_start(s->sda, s->scl);
		ok = i2c_byte(s->sda, s->scl, (uint8_t)(c->slave << 1));
		if (ok && c->reg_bytes == 2) {
			ok = i2c_byte(s->sda, s->scl, (uint8_t)(reg >> 8));
		}
		ok = ok && i2c_byte(s->sda, s->scl, (uint8_t)reg);
		if (ok) {
			i2c_start(s->sda, s->scl);
			ok = i2c_byte(s->sda, s->scl, (uint8_t)(c->slave << 1 | 1));
		}
		if (ok) {
			uint32_t v = 0;
			for (unsigned k = 0; k < c->data_bytes; k++) {
				uint8_t byte = 0;
				for (int i = 7; i >= 0; i--) {
					byte = (uint8_t)(byte << 1 | i2c_bit(s->sda, s->scl, true));
				}
				(void)i2c_bit(s->sda, s->scl, k + 1 == c->data_bytes);   /* ACK all but the last */
				v = v << 8 | byte;
			}
			*val = v;
		}
		i2c_stop(s->sda, s->scl);
	}
	bus_give();
	return ok;
}

/* status_poll chips: the status register back to 0 within ms */
static bool
lens_ready(const struct lens_chip *c, unsigned ms)
{
	if (!c->status_poll) {
		return true;
	}
	uint32_t st = ~0u;
	for (unsigned t = 0; t <= ms; t++) {
		if (lens_get(c, c->probe_reg, &st) && st == 0) {
			return true;
		}
		delay(1000);
	}
	printf("SWEET CAM: lens %s: status %#x after %u ms\n", c->name, st, ms);
	return false;
}

static bool
lens_list(const struct lens_chip *c, const struct lens_write *w)
{
	for (; w->reg != 0xffff; w++) {
		if (!lens_ready(c, 100) || !lens_put(c, w->reg, w->val)) {
			printf("SWEET CAM: lens %s: %#x <- %#x not taken\n", c->name, w->reg, w->val);
			return false;
		}
	}
	return lens_ready(c, 100);
}

static uint32_t
lens_code(uint32_t pos)
{
	pos = pos > LENS_MAX ? LENS_MAX : pos;
	int64_t code = (int64_t)lens.inf + ((int64_t)lens.mac - (int64_t)lens.inf) * pos / LENS_MAX;
	return code < 0 ? 0 : code > lens.chip->code_max ? lens.chip->code_max : (uint32_t)code;
}

static bool
lens_write_pos(uint32_t pos)
{
	const struct lens_chip *c = lens.chip;
	uint32_t code = lens_code(pos);
	return lens_put(c, c->pos_reg, c->pos_or | code << c->pos_shift);
}

/* the frames exposed while it moves: from now for a VCM's settling time, longer the further it goes */
static void
lens_moving(uint32_t from, uint32_t to)
{
	uint32_t d = from > to ? from - to : to - from;
	uint64_t span;
	nanoseconds_to_absolutetime((uint64_t)(10 + d / 32) * NSEC_PER_MSEC, &span);
	lens.moving_from = mach_absolute_time();
	lens.moving_until = lens.moving_from + span;
}

/* after the sensor's tables (the bus held, VAF on): find the sensor's lens chip, start it, the lens where it was */
static void
lens_up(unsigned si)
{
	lens.up = false;
	lens.chip = NULL;
	lens_args();
	if (lens.forced_none || si >= sizeof(lens_of_sensor) || (lens_of_sensor[si] < 0 && !lens.forced[0])) {
		return;
	}
	sleep_ms(8);            /* VAF's settling (the actuator's power-up: VAF +8 ms) */
	for (unsigned i = 0; i < sizeof(lens_chips) / sizeof(lens_chips[0]) && !lens.chip; i++) {
		const struct lens_chip *c = &lens_chips[i];
		uint32_t v = 0;
		if (lens.forced[0] ? strcmp(lens.forced, c->name) != 0 : (int)i != lens_of_sensor[si]) {
			continue;
		}
		bool there = false;
		for (unsigned t = 0; t < 10 && !there; t++) {  /* (a controller still starting from its flash may not answer yet) */
			there = lens_get(c, c->probe_reg, &v);
			if (!there) {
				sleep_ms(5);
			}
		}
		if (!there) {
			printf("SWEET CAM: lens %s: nothing at 0x%02x\n", c->name, c->slave);
			continue;
		}
		lens.chip = c;
	}
	const struct lens_chip *c = lens.chip;
	if (!c) {
		return;
	}
	uint32_t inf = c->inf, mac = c->mac;
	(void)PE_parse_boot_argn("sweetcam_af_inf", &inf, sizeof(inf));
	(void)PE_parse_boot_argn("sweetcam_af_mac", &mac, sizeof(mac));
	lens.inf = inf > c->code_max ? c->code_max : inf;
	lens.mac = mac > c->code_max ? c->code_max : mac;
	if (!lens_ready(c, 200) || !lens_list(c, c->on) || !lens_write_pos(lens.pos)) {
		printf("SWEET CAM: lens %s did not start: no focus this session\n", c->name);
		lens.chip = NULL;
		return;
	}
	lens.up = true;
	lens_moving(0, lens.pos);
	printf("SWEET CAM: lens %s at 0x%02x ready: position %u (code %u; 0..1023 = codes %u..%u)\n", c->name, c->slave, lens.pos,
	    lens_code(lens.pos), lens.inf, lens.mac);
}

/* to position pos (0..1023) now; false if the chip did not take it (the lens is given up for the session) */
static bool
lens_move(uint32_t pos)
{
	if (!lens.up) {
		return false;
	}
	pos = pos > LENS_MAX ? LENS_MAX : pos;
	if (pos == lens.pos) {
		return true;
	}
	if (!lens_write_pos(pos)) {
		printf("SWEET CAM: lens %s: position %u not taken: no focus this session\n", lens.chip->name, pos);
		lens.up = false;
		return false;
	}
	lens_moving(lens.pos, pos);
	lens.pos = pos;
	return true;
}

/* back to infinity a step at a time (no knock at the end stop) */
static void
lens_park(void)
{
	while (lens.up && lens.pos > 0) {
		uint32_t to = lens.pos > LENS_PARK_STEP ? lens.pos - LENS_PARK_STEP : 0;
		if (!lens_move(to)) {
			break;
		}
		delay(LENS_PARK_MS * 1000);
	}
}

/* before VAF goes: parked, the servos off; lens.pos keeps the position asked for (the next power-up goes back to it) */
static void
lens_down(void)
{
	if (!lens.chip) {
		return;
	}
	if (lens.up) {
		uint32_t keep = lens.pos;
		lens_park();
		(void)lens_list(lens.chip, lens.chip->off);
		lens.pos = keep;        /* the next session goes back to it */
		printf("SWEET CAM: lens %s parked\n", lens.chip->name);
	}
	lens.up = false;
	lens.chip = NULL;
}

static void
sensor_down(void)
{
	const struct sensor *s = cm.s;
	bool was = cm.broken;
	cm.broken = false;
	lens_down();            /* 0929: on the held bus, VAF still on */
	bus.held = false;
	bus_give();
	run_steps(s->down, false);
	mclk_set(false);
	if (cm.ldo_on) {
		for (unsigned n = 1; n <= 7; n++) {
			(void)ldo_set(n, 0, false);
		}
	}
	if (cm.reset_taken) {
		wr(cm.tlmm, GPIO_IO(s->reset), cm.found_reset_io & 2u);
		wr(cm.tlmm, GPIO_CFG(s->reset), cm.found_reset_cfg);
		cm.reset_taken = false;
	}
	cm.broken = was;
}

/*
 * 0923: everything up to the sensor's "on", for "c 21" and /dev/redwood-camera alike (the table file loaded and
 * the mode checked first): the capture clocks and the SMMU, PLL2 and the 24 MHz MCLK, refgen, the sensor's power,
 * its chip ID, its tables (init, resM, the exposure, tpgN), CSID-lite 0 and IFE-lite 0 into frame_pa, the CSIPHY.
 * *powered says whether the sensor was powered (stream_down() takes it down again). Gain is the sensor's code.
 */
static bool
stream_up(unsigned si, unsigned mode_n, unsigned pattern, unsigned settle, uint32_t lines, uint32_t gain, uint32_t frame_lines,
    uint32_t dgain, uint32_t frame_pa, bool *powered)
{
	const struct sensor *s = &sensors[si];
	const struct tab_mode *m = &sc.mode[mode_n];
	unsigned phy = sensor_capture[si].phy;
	cm.s = s;
	cm.broken = false;
	cm.reset_taken = false;
	*powered = false;
	if (!cap_up(phy)) {
		return false;
	}
	cstep("PLL2 (24 MHz MCLK)");
	bool ok = pll2_up();
	cm.mclk24 = ok;
	if (ok) {
		cstep("refgen");
		ok = refgen_on();
	}
	if (ok) {
		cstep("the sensor's power");
		*powered = true;
		reset_set(false);
		sleep_ms(1);
		ok = slg_up();
		if (ok) {
			run_steps(s->up, true);
			ok = !cm.broken;
		}
	}
	if (ok) {
		uint16_t id = 0;
		const char *why = "";
		bus_take();
		bus.held = true;
		ok = sensor_read16(s->slave, 0x0016, &id, &why) && id == s->id;
		if (!ok || cam_step_ms) {
			printf("SWEET CAM: %s: chip ID %#06x%s%s\n", s->name, id, ok ? "" : " -- ", ok ? "" : (*why ? why : "not the one"));
		}
	}
	char res[8], tpg[8];
	snprintf(res, sizeof(res), "res%u", mode_n);
	snprintf(tpg, sizeof(tpg), "tpg%u", pattern);
	if (ok) {
		cstep("the sensor's tables");
		ok = sensor_table("init") && sensor_table(res) && sensor_exposure(lines, gain, frame_lines, dgain) &&
		    (!pattern || sensor_table(tpg));
	}
	if (ok) {
		lens_up(si);    /* 0929: the lens motor, where the module has one (a failure there only means no focus) */
	}
	if (ok) {
		cstep("CSID reset");
		ok = csid_reset();
	}
	if (ok) {
		cstep("CSID configuration");
		ok = csid_config((int)phy, m->lanes, sc.lane_assign & ((1u << (4 * m->lanes)) - 1), 0, m->width, m->height);
	}
	if (ok) {
		cstep("IFE configuration");
		ok = ife_config(frame_pa, 0);   /* each frame into the same place, unless told otherwise frame by frame */
	}
	if (ok) {
		cstep("CSIPHY");
		ok = csiphy_up(phy, m->lanes, sc.lane_assign, settle ? settle : m->settle);
	}
	return ok;
}

static void
stream_down(bool powered)
{
	cstep("taking it all down");
	cap_smmu_faults();
	paths_down();
	csiphy_down();
	if (powered) {
		sensor_down();
	}
	cm.mclk24 = false;
	pll2_down();
	refgen_restore();
	cap_down();
}

static int
capture_sensor(unsigned si, unsigned mode_n, uint64_t options)
{
	unsigned pattern = options & 0xf, settle = (options >> 8) & 0xff, wanted = (options >> 48) & 0xff;
	uint32_t lines = (options >> 16) & 0xffff, gain = (options >> 32) & 0xffff;
	int result = EIO;
	if (si >= sizeof(sensor_capture) / sizeof(sensor_capture[0])) {
		return EINVAL;
	}
	const struct sensor *s = &sensors[si];
	if (!tab_load(sensor_capture[si].file)) {
		return ENOENT;
	}
	if (mode_n >= sc.modes) {
		mode_n = 0;
		for (unsigned m = 1; m < sc.modes; m++) {
			if ((uint64_t)sc.mode[m].width * sc.mode[m].height < (uint64_t)sc.mode[mode_n].width * sc.mode[mode_n].height) {
				mode_n = m;
			}
		}
	}
	const struct tab_mode *m = &sc.mode[mode_n];
	/* the write master puts each line at a 16-byte boundary (r17d: 1640 pixels, 2050 bytes, every 2064) */
	uint32_t width = m->width, height = m->height, stride = (width / 4 * 5 + 15) & ~15u, bytes = stride * height;
	settle = settle ? settle : m->settle;
	wanted = wanted ? wanted : 8;
	if (sc.slave != s->slave || m->lanes < 1 || m->lanes > 4 || m->cphy || m->dt != RAW10_DT || width % 4 || bytes > CAP_POOL_BYTES) {
		printf("SWEET CAM: %s mode %u: slave %#x, %u lanes%s, data type %#x, %ux%u: not what this is written for\n", s->name,
		    mode_n, sc.slave, m->lanes, m->cphy ? " (C-PHY)" : "", m->dt, width, height);
		tab_free();
		return EINVAL;
	}
	printf("SWEET CAM: %s mode %u: %ux%u RAW10, %u lanes, %u.%03u frames/s, line %u, frame %u lines; settle %u; %s\n", s->name,
	    mode_n, width, height, m->lanes, m->fps_milli / 1000, m->fps_milli % 1000, m->line_length, m->frame_length, settle,
	    pattern ? "the sensor's test pattern" : "the picture");
	bool powered = false;
	if (!cap_frames()) {
		tab_free();
		return ENOMEM;
	}
	frames_mark(bytes);
	if (stream_up(si, mode_n, pattern, settle, lines, gain, 0, 0, (uint32_t)cap.frames_pa, &powered)) {
		show_paths("configured");
		cstep("streaming");
		bool ok = sensor_table("on");
		if (ok) {
			/* r17d: the rear sensors sent nothing at all: the sensor's own word on it (SMIA 0x0005 frame count, 0x0100 mode) */
			uint16_t fc0 = 0, fc1 = 0, md = 0;
			const char *why = "";
			sleep_ms(100);
			bool r0 = sensor_read16(s->slave, 0x0004, &fc0, &why);
			sleep_ms(100);
			bool r1 = sensor_read16(s->slave, 0x0004, &fc1, &why) && sensor_read16(s->slave, 0x0100, &md, &why);
			printf("SWEET CAM: %s streaming? frame count %u then %u (100 ms apart), mode select %#x%s%s\n", s->name, fc0 & 0xff,
			    fc1 & 0xff, md >> 8, r0 && r1 ? "" : " -- read failed: ", r0 && r1 ? "" : why);
		}
		if (ok) {
			unsigned done = 0, ms = 0;
			for (; ms < 4000 && done < wanted; ms++) {
				delay(1000);
				if (!(rd(cap.ife, BUS_IRQ_STATUS1) & 1u)) {
					continue;
				}
				done++;
				wr(cap.ife, BUS_IRQ_CLEAR1, 1);
				wr(cap.ife, BUS_IRQ_GLOBAL_CLEAR, 1);
				if (done == wanted) {
					/* stop the write master at this frame's end: the register update takes at the next frame's start */
					wr(cap.ife, WM0_CFG, 0);
				} else {
					/* the next frame's address (the same place): r17d wrote only the first frame without it */
					wr(cap.ife, WM0_IMAGE_ADDR, (uint32_t)cap.frames_pa);
				}
				wr(cap.ife, IFE_REG_UPDATE, 0x2);
				if (done <= 2 || done == wanted) {
					printf("SWEET CAM: frame %u written, %u ms after the sensor was started\n", done, ms + 1);
				}
			}
			show_paths(done ? "after the frames" : "no frame");
			show_csiphy(done ? "after the frames" : "no frame");
			(void)sensor_table("off");
			sleep_ms(40);
			result = done == wanted ? 0 : done ? EAGAIN : ETIMEDOUT;
		}
	}
	stream_down(powered);
	if (cap.frames_va && result != EINVAL) {
		frames_look(bytes);
		printf("SWEET CAM: %s mode %u frame: files read /dev/pmem %llu %u (%ux%u RAW10 packed, lines %u bytes apart, %s first)\n",
		    s->name, mode_n, (unsigned long long)cap.frames_pa, bytes, width, height, stride,
		    m->cfa == 0 ? "BGGR" : m->cfa == 1 ? "GBRG" : m->cfa == 2 ? "GRBG" : m->cfa == 3 ? "RGGB" : "mono");
	}
	tab_free();
	return result;
}

/* ==== 0923: /dev/redwood-camera, the cameras' frames for iOS ==== */

/*
 * HackiOS redwood_camera.c's cam=5 (bsd/sys/redwood_camera.h, copied below; the program on the other side is its
 * userland camera_device.c, inside mediaserverd), on top of stream_up()/stream_down():
 *   open()   marks the device open; the first read starts a session (the sensor powered up and streaming)
 *   read()   the newest frame not given yet: struct redwood_camera_frame, then its bytes (RAW10 as the IFE wrote
 *            them); waits for one (3 s while streaming, 15 s while the sensor starts)
 *   write()  struct redwood_camera_command: EXPOSURE (lines, gain, frame lines), SENSOR (another camera, its mode),
 *            FOCUS (taken, ignored: no lens motor driver yet), KEEP (not here: ENOTSUP)
 *   close()  (the last) ends the session; so does a second without a read, and any fault
 * The frames go into RING places of GPU-pool memory below 4 GB (the write master's addresses are 32 bits), mapped
 * write-combined (nothing of them in the processor's caches, so a read copies them as they are): at each start of
 * frame (IFE-lite RDI 0 SOF, polled every millisecond) the write master gets the address of the place for the frame
 * after it, taken at the next start of frame.
 * The sensors as HackiOS numbers them (camera_device.c asks by these): 1 the wide (back) camera = IMX363, 2 the front
 * = IMX355, 3 the ultra-wide = IMX481; 0 (HackiOS's macro) has none here.
 * Gain: linear for the program (256 = 1x, at most gain_max), each sensor's code written (Sony: IMX363 512 - 512/g,
 * IMX355 and IMX481 1024 - 1024/g, as the module files' realToRegGain).
 * The mode: the one asked for (SENSOR's b), else HackiOS's choice: the most pixels at 4:3, at most 31 frames a second
 * and 13 megapixels (IMX363 4032x3024, IMX355 3280x2464, IMX481 2328x1746).
 */
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>

extern void sweet_gpu_pool_free(uint64_t pa, uint64_t bytes);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);     /* (not exported to bsd/ by its header) */

#define REDWOOD_CAMERA_MAGIC            "RWFRAME1"
struct redwood_camera_frame {
	char            magic[8];
	uint32_t        header_bytes, width, height, stride, format, bayer, black;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, line_ns, gain, gain_unity, gain_max, frame_lines, frame_lines_min, frame_lines_max,
	    margin, sensor, focus, reserved[5];
};
#define REDWOOD_CAMERA_NO_FOCUS         0xffffffffu
enum { REDWOOD_CAMERA_EXPOSURE = 1, REDWOOD_CAMERA_KEEP = 2, REDWOOD_CAMERA_SENSOR = 3, REDWOOD_CAMERA_FOCUS = 4 };
/* 0929-0933 (the shared contract with phone/audio/redwood_camera.h): reserved[0] flags, [1]/[2] start of frame (mach
 * time, low/high 32 bits), [3] digital gain (unity | max << 16; 0 none), [4] the mode's frames a second x 1000 */
#define REDWOOD_CAMERA_FLAG_FLASH_LIT           (1u << 0)
#define REDWOOD_CAMERA_FLAG_LENS_MOVING         (1u << 1)
#define REDWOOD_CAMERA_FLAG_MODE_CHANGED        (1u << 2)
#define REDWOOD_CAMERA_FOCUS_MOVE               0
#define REDWOOD_CAMERA_FOCUS_PARK               1
struct redwood_camera_command { uint32_t command, bytes, a, b, c, d; };

#define RING                    3
#define PLACE_SPARE             (256u << 10)    /* room after each frame */
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define EXPOSURE_MARGIN         40
#define FRAME_LINES_MAX         0xfff0u
#define GAIN_UNITY              256u
#define RDI0_SOF                (1u << 27)      /* IFE_IRQ_STATUS0 (r17: 0x08000220 after frames) */
#define WM0_DONE                1u              /* BUS_IRQ_STATUS1 */

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

/* HackiOS's sensor numbers -> ours (sensors[]); ~0u: none */
static const unsigned ios_to_ours[4] = { ~0u, 0, 2, 1 };
static const unsigned ours_to_ios[3] = { 1, 3, 2 };

static LCK_GRP_DECLARE(ios_grp, "sweet_cam_ios");
static LCK_MTX_DECLARE(ios_lock, &ios_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
	uint64_t        sof;            /* 0929: its start of frame (mach time) */
	uint32_t        focus, flags;   /* 0929: the lens's position (or REDWOOD_CAMERA_NO_FOCUS), REDWOOD_CAMERA_FLAG_* */
} place[RING];
static uint64_t ring_pa, ring_bytes, place_bytes;
static vm_offset_t ring_va;
static uint32_t ring_frame_bytes;
static unsigned ios_sensor = 0, ios_mode_n;    /* ours */
static uint32_t ios_line_ns, ios_stride, ios_gain_max;
static uint32_t ios_margin = EXPOSURE_MARGIN, ios_black = 64, ios_dgain_max;  /* 0931: from the table file's "info" where it has one */
static struct tab_mode ios_mode_buf;   /* (a copy: the table file is loaded only while it is used) */
static const struct tab_mode *ios_mode = &ios_mode_buf;
static volatile bool ios_open, ios_streaming, ios_wanted, ios_broken;
static uint64_t ios_last_read, ios_given, ios_frames_done, ios_writes_late;
static struct { uint32_t lines, gain, frame_lines, dgain; bool pending; } ios_ask;     /* 0931: dgain 0 = as it is */
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } ios_now, ios_next;
static uint32_t ios_dgain;      /* 0931: the digital gain last asked for (256 = 1x; 0 = never: the tables' own), kept for new sessions */
static volatile unsigned ios_switch_to;        /* 1 + ours */
static uint32_t ios_switch_mode = ~0u;
static struct { uint32_t pos; bool pending, park; } ios_focus;     /* 0929: FOCUS, done after the next frame's end */

static uint64_t
ios_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

/* linear gain (GAIN_UNITY = 1x) -> the sensor's code */
static uint32_t
gain_code(uint32_t g)
{
	uint32_t base = ios_sensor == 0 ? 512u : 1024u;
	g = g < GAIN_UNITY ? GAIN_UNITY : g;
	return base - (uint32_t)((uint64_t)base * GAIN_UNITY / g);
}

static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = ios_mode->frame_length;
	*gain = *gain < GAIN_UNITY ? GAIN_UNITY : *gain > ios_gain_max ? ios_gain_max : *gain;
	*lines = *lines < 4 ? 4 : *lines > FRAME_LINES_MAX - ios_margin ? FRAME_LINES_MAX - ios_margin : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + ios_margin > shortest ? *lines + ios_margin : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > FRAME_LINES_MAX ? FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - ios_margin) {
		*lines = *frame_lines - ios_margin;
	}
}

/*
 * 0931: the digital gain asked for (256 = 1x), within what the sensor has (0: none, or nothing asked). The frames report
 * the analog gain, lines and frame lines as clamped here and in exposure_limits(): what the sensor was given, so a
 * program that compares what it asked for (clamped the same way: gain_max, frame_lines_min/_max, margin) with what the
 * frames say sees its request taken.
 */
static uint32_t
dgain_limit(uint32_t d)
{
	if (d == 0 || ios_dgain_max == 0) {
		return 0;
	}
	return d < GAIN_UNITY ? GAIN_UNITY : d > ios_dgain_max ? ios_dgain_max : d;
}

/* the places, for frames of `bytes': kept between sessions, made anew only for larger frames */
static bool
ring_alloc(uint32_t bytes)
{
	uint64_t pb = (((uint64_t)bytes + 0x3fff) & ~0x3fffull) + PLACE_SPARE, want = RING * pb;
	if (ring_va && want <= ring_bytes) {
		ring_frame_bytes = bytes;
		place_bytes = pb;
		return true;
	}
	if (ring_va) {
		ml_io_unmap(ring_va, ring_bytes);
		sweet_gpu_pool_free(ring_pa, ring_bytes);
		ring_va = 0;
		ring_pa = 0;
	}
	if (sweet_gpu_pool_alloc(want, &ring_pa) != 0 || ring_pa + want > 0x100000000ull) {
		printf("SWEET CAM: no GPU pool memory below 4 GB for %u frames (%llu KB)\n", RING, want >> 10);
		if (ring_pa) {
			sweet_gpu_pool_free(ring_pa, want);
		}
		ring_pa = 0;
		return false;
	}
	ring_va = ml_io_map_unmappable(ring_pa, want, VM_WIMG_DEFAULT);     /* (cached: see camdev_read()) */
	if (!ring_va) {
		sweet_gpu_pool_free(ring_pa, want);
		ring_pa = 0;
		return false;
	}
	ring_bytes = want;
	ring_frame_bytes = bytes;
	place_bytes = pb;
	printf("SWEET CAM: /dev/redwood-camera: %u places of %llu KB at %#llx (GPU pool)\n", RING, pb >> 10, (unsigned long long)ring_pa);
	return true;
}

/* the sensor (ours) and its mode for the next sessions: its table file kept loaded */
static bool
ios_select(unsigned si, uint32_t mode_n)
{
	tab_free();
	if (si >= sizeof(sensor_capture) / sizeof(sensor_capture[0]) || !tab_load(sensor_capture[si].file)) {
		return false;
	}
	if (mode_n >= sc.modes) {
		uint64_t best = 0;
		mode_n = 0;
		for (uint32_t m = 0; m < sc.modes; m++) {
			const struct tab_mode *o = &sc.mode[m];
			uint64_t pixels = (uint64_t)o->width * o->height;
			if (o->width * 3 == o->height * 4 && o->fps_milli <= 31000 && pixels <= 13000000 && pixels > best) {
				best = pixels;
				mode_n = m;
			}
		}
	}
	const struct tab_mode *m = &sc.mode[mode_n];
	if (sc.slave != sensors[si].slave || m->lanes < 1 || m->lanes > 4 || m->cphy || m->dt != RAW10_DT || m->width % 4 ||
	    m->fps_milli == 0 || m->frame_length == 0) {
		printf("SWEET CAM: /dev/redwood-camera: %s mode %u is not what this is written for\n", sensors[si].name, mode_n);
		tab_free();
		return false;
	}
	ios_sensor = si;
	ios_mode_n = mode_n;
	ios_mode_buf = *m;
	tab_free();
	m = ios_mode;
	ios_stride = (m->width / 4 * 5 + 15) & ~15u;
	ios_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)m->fps_milli * m->frame_length));
	ios_gain_max = (si == 0 ? 8u : 16u) * GAIN_UNITY;
	/* 0931: the module file's limits where the table file has them (IMX363: 8x analog, margin 16; IMX481/355: 16x, 16/10) */
	ios_margin = sc.margin ? sc.margin : EXPOSURE_MARGIN;
	ios_black = sc.black ? sc.black : 64;
	if (sc.again_max >= GAIN_UNITY && sc.again_max < ios_gain_max) {
		ios_gain_max = sc.again_max;
	}
	ios_dgain_max = sc.dgain_reg ? (sc.dgain_max > 0xfff || sc.dgain_max < GAIN_UNITY ? 0xfffu : sc.dgain_max) : 0;
	ios_ask.lines = m->frame_length - ios_margin;
	ios_ask.gain = 8 * GAIN_UNITY;
	ios_ask.frame_lines = 0;
	ios_ask.dgain = 0;
	ios_ask.pending = false;
	ios_dgain = 0;          /* (a new sensor starts with its tables' digital gain) */
	printf("SWEET CAM: /dev/redwood-camera: the %s (camera %u): mode %u, %ux%u, %u.%03u frames/s at the most, lines of %u ns\n",
	    sensors[si].name, ours_to_ios[si], mode_n, m->width, m->height, m->fps_milli / 1000, m->fps_milli % 1000, ios_line_ns);
	return true;
}

static void
places_reset(void)
{
	lck_mtx_lock(&ios_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	lck_mtx_unlock(&ios_lock);
}

/* one session: from the first read that wants frames to the close, the idle time, a fault or another sensor */
static unsigned ios_start_failures;

static void
ios_session(void)
{
	int writing = 0, next = 0;
	bool powered = false;
	uint32_t lines, gain, frame_lines;
	const char *why = "a fault";

	ios_frames_done = ios_given = ios_writes_late = 0;
	if (!ring_alloc(ios_stride * ios_mode->height)) {
		ios_broken = true;
		return;
	}
	places_reset();
	lck_mtx_lock(&ios_lock);
	place[0].state = PLACE_WRITING;
	lines = ios_ask.lines;
	gain = ios_ask.gain;
	frame_lines = ios_ask.frame_lines;
	ios_ask.pending = false;
	lck_mtx_unlock(&ios_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	lck_mtx_lock(&sweet_cam_lock);
	if (!tab_load(sensor_capture[ios_sensor].file)) {
		ios_broken = true;
		lck_mtx_unlock(&sweet_cam_lock);
		return;
	}
	bool ok = stream_up(ios_sensor, ios_mode_n, 0, 0, lines, gain_code(gain), frame_lines, dgain_limit(ios_dgain), (uint32_t)ring_pa,
	    &powered);
	if (ok) {
		wr(cap.ife, IFE_IRQ_CLEAR0, 0xffffffffu);
		wr(cap.ife, IFE_IRQ_GLOBAL_CLEAR, 1);
		ok = sensor_table("on");
	}
	if (!ok) {
		/* 0926: a start that failed is tried again at the next read, up to three times in a row */
		ios_start_failures++;
		printf("SWEET CAM: /dev/redwood-camera: the %s did not start (%u in a row)%s\n", sensors[ios_sensor].name,
		    ios_start_failures, ios_start_failures >= 3 ? ": no more tries" : "");
		ios_broken = ios_start_failures >= 3;
		stream_down(powered);
		tab_free();
		lck_mtx_unlock(&sweet_cam_lock);
		sleep_ms(200);
		return;
	}
	ios_start_failures = 0;
	ios_now.lines = ios_next.lines = lines;
	ios_now.gain = ios_next.gain = gain;
	ios_now.frame_lines = ios_next.frame_lines = frame_lines;
	ios_next.from = 0;
	printf("SWEET CAM: /dev/redwood-camera: streaming the %s, mode %u %ux%u; exposure %u lines, gain %u/256, frame %u lines\n",
	    sensors[ios_sensor].name, ios_mode_n, ios_mode->width, ios_mode->height, lines, gain, frame_lines);
	uint64_t last_frame = ios_ms();
	lck_mtx_lock(&ios_lock);
	ios_streaming = true;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	for (;;) {
		if (!ios_open) {
			why = "closed";
			break;
		}
		if (ios_switch_to != 0) {
			why = "another sensor asked for";
			break;
		}
		sleep_ms(1);
		uint64_t now = ios_ms();
		uint32_t top = rd(cap.ife, IFE_IRQ_STATUS0), bus_st = rd(cap.ife, BUS_IRQ_STATUS1);
		if (!(top & RDI0_SOF) && !(bus_st & WM0_DONE)) {
			if (now - last_frame > FRAME_TIMEOUT_MS) {
				show_paths("no frame");
				why = "no frame for 1.5 s";
				ios_broken = true;
				break;
			}
			if (now - ios_last_read > IDLE_MS) {
				why = "nobody reads";
				break;
			}
			continue;
		}
		wr(cap.ife, IFE_IRQ_CLEAR0, top & RDI0_SOF);
		wr(cap.ife, IFE_IRQ_GLOBAL_CLEAR, 1);
		wr(cap.ife, BUS_IRQ_CLEAR1, bus_st & WM0_DONE);
		wr(cap.ife, BUS_IRQ_GLOBAL_CLEAR, 1);
		lck_mtx_lock(&ios_lock);
		if (bus_st & WM0_DONE) {
			last_frame = now;
			ios_frames_done++;
			if (ios_next.from != 0 && ios_frames_done >= ios_next.from) {
				ios_now = ios_next;
				ios_next.from = 0;
			}
			place[writing].state = PLACE_READY;
			place[writing].sequence = ios_frames_done;
			place[writing].time = mach_absolute_time();
			place[writing].exposure_lines = ios_now.lines;
			place[writing].gain = ios_now.gain;
			place[writing].frame_lines = ios_now.frame_lines;
			/* 0929: the lens; moving if it moved while any of the frame's lines were exposed (first line: SOF - exposure) */
			place[writing].focus = lens.up ? lens.pos : REDWOOD_CAMERA_NO_FOCUS;
			place[writing].flags = 0;
			uint64_t exposed;
			nanoseconds_to_absolutetime((uint64_t)ios_now.lines * ios_line_ns, &exposed);
			if (lens.up && lens.moving_until > place[writing].sof - MIN(exposed, place[writing].sof) &&
			    lens.moving_from < place[writing].time) {
				place[writing].flags |= REDWOOD_CAMERA_FLAG_LENS_MOVING;
			}
		}
		if (top & RDI0_SOF) {
			if (next >= 0) {
				writing = next;         /* (the address given at the last SOF, taken at this one) */
				next = -1;
			} else {
				ios_writes_late++;      /* no address waiting: this frame goes where the last one went */
			}
			place[writing].state = PLACE_WRITING;
			place[writing].sof = mach_absolute_time();
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;
				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				wr(cap.ife, WM0_IMAGE_ADDR, (uint32_t)(ring_pa + (uint64_t)next * place_bytes));
				wr(cap.ife, IFE_REG_UPDATE, 0x2);
			}
		}
		bool ask = (bus_st & WM0_DONE) && ios_ask.pending;
		uint32_t al = ios_ask.lines, ag = ios_ask.gain, af = ios_ask.frame_lines, ad = ios_ask.dgain;
		ios_ask.pending = ios_ask.pending && !ask;
		/* 0929: a lens move, written at a frame's end like the exposure (on the same held bus) */
		bool focus = (bus_st & WM0_DONE) && ios_focus.pending;
		uint32_t fpos = ios_focus.pos;
		bool fpark = ios_focus.park;
		ios_focus.pending = ios_focus.pending && !focus;
		lck_mtx_unlock(&ios_lock);
		if (focus && lens.up) {
			if (fpark) {
				/* a step a frame back to infinity, the next step asked for again until it is there */
				uint32_t to = lens.pos > 4 * LENS_PARK_STEP ? lens.pos - 4 * LENS_PARK_STEP : 0;
				(void)lens_move(to);
				if (lens.up && lens.pos > 0) {
					lck_mtx_lock(&ios_lock);
					if (!ios_focus.pending) {
						ios_focus.pending = true;
						ios_focus.park = true;
					}
					lck_mtx_unlock(&ios_lock);
				}
			} else {
				(void)lens_move(fpos);
			}
		}
		if (bus_st & WM0_DONE) {
			wakeup(&ios_frames_done);
		}
		if (ask) {
			exposure_limits(&al, &ag, &af);
			uint32_t dg = dgain_limit(ad);
			bool dchange = dg != 0 && dg != ios_dgain;
			if (al != ios_now.lines || ag != ios_now.gain || af != ios_now.frame_lines || dchange) {
				if (!sensor_exposure(al, gain_code(ag), af, dchange ? dg : 0)) {
					why = "an exposure the sensor did not take";
					ios_broken = true;
					break;
				}
				if (dchange) {
					ios_dgain = dg;
				}
				ios_next.lines = al;
				ios_next.gain = ag;
				ios_next.frame_lines = af;
				ios_next.from = ios_frames_done + 2;
			}
		}
		if ((bus_st & WM0_DONE) && (ios_frames_done == 30 || ios_frames_done % 900 == 0)) {
			show_paths("streaming");
		}
	}
	lck_mtx_lock(&ios_lock);
	ios_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_frames_done);
	(void)sensor_table("off");
	sleep_ms(40);
	stream_down(powered);
	tab_free();
	lck_mtx_unlock(&sweet_cam_lock);
	printf("SWEET CAM: /dev/redwood-camera: the frames stopped (%s): %llu done, %llu given, %llu written over\n", why,
	    ios_frames_done, ios_given, ios_writes_late);
}

static void
ios_thread(void *arg __unused, wait_result_t w __unused)
{
	uint32_t mode_n = ~0u, which = 1;       /* (HackiOS numbering: the back camera first) */
	(void)PE_parse_boot_argn("sweetcam_mode", &mode_n, sizeof(mode_n));
	(void)PE_parse_boot_argn("sweetcam_sensor", &which, sizeof(which));
	lck_mtx_lock(&sweet_cam_lock);
	if (!ios_select(ios_to_ours[which & 3] == ~0u ? 0 : ios_to_ours[which & 3], mode_n)) {
		ios_broken = true;
	}
	lck_mtx_unlock(&sweet_cam_lock);
	while (!ios_broken) {
		lck_mtx_lock(&ios_lock);
		while (!ios_broken && !(ios_open && ios_wanted) && ios_switch_to == 0) {
			(void)msleep(&ios_open, &ios_lock, PZERO, "redwood-camera-idle", NULL);
		}
		unsigned to = ios_switch_to;
		ios_switch_to = 0;
		if (!to) {
			ios_wanted = false;
		}
		lck_mtx_unlock(&ios_lock);
		if (ios_broken) {
			break;
		}
		if (to != 0) {
			lck_mtx_lock(&sweet_cam_lock);
			bool ok = ios_select(to - 1, ios_switch_mode);
			lck_mtx_unlock(&sweet_cam_lock);
			if (!ok) {
				ios_broken = true;
				break;
			}
			continue;
		}
		ios_session();
	}
	wakeup(&ios_frames_done);
	printf("SWEET CAM: /dev/redwood-camera: no more frames in this run\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (ios_broken) {
		return EIO;
	}
	ios_open = true;
	return 0;
}

static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_open = false;
	ios_wanted = false;
	wakeup(&ios_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_frame f;
	uint64_t since = ios_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&ios_lock);
	ios_last_read = ios_ms();
	for (;;) {
		if (ios_broken) {
			lck_mtx_unlock(&ios_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > ios_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (ios_ms() - since > (ios_streaming ? 3000u : 15000u)) {
			lck_mtx_unlock(&ios_lock);
			return EAGAIN;
		}
		if (!ios_streaming && !ios_wanted) {
			ios_wanted = true;
			wakeup(&ios_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&ios_frames_done, &ios_lock, PZERO | PCATCH, "redwood-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&ios_lock);
			return error;
		}
		ios_last_read = ios_ms();
	}
	place[b].state = PLACE_READING;
	ios_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = ios_mode->width;
	f.height = ios_mode->height;
	f.stride = ios_stride;
	f.format = 1;
	f.bayer = ios_mode->cfa == 0 ? 3 : ios_mode->cfa == 1 ? 2 : ios_mode->cfa == 2 ? 1 : 0;      /* CamX's order -> the header's */
	f.black = ios_black;                            /* 0931 */
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = ios_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = GAIN_UNITY;
	f.gain_max = ios_gain_max;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = ios_mode->frame_length;
	f.frame_lines_max = FRAME_LINES_MAX;
	f.margin = ios_margin;                          /* 0931 */
	f.reserved[3] = ios_dgain_max ? ios_dgain_max << 16 | GAIN_UNITY : 0;
	f.reserved[4] = ios_mode->fps_milli;
	f.sensor = ours_to_ios[ios_sensor];
	f.focus = place[b].focus;                       /* 0929 */
	f.reserved[0] = place[b].flags;
	vm_offset_t at = ring_va + (uint64_t)b * place_bytes;
	uint32_t frame_bytes = ring_frame_bytes;
	lck_mtx_unlock(&ios_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		uint32_t n = (uint32_t)MIN((uint64_t)frame_bytes, (uint64_t)uio_resid(uio));
		uint64_t t0 = mach_absolute_time(), t1, t2, ns1, ns2;
		/*
		 * r18a: write-combined (uncached) places read at ~120 MB/s (a 15 MB frame in 120 ms): they are cached now,
		 * and what the IFE wrote past the caches is made visible by forgetting the frame's lines first (HackiOS's way)
		 */
		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		t1 = mach_absolute_time();
		error = uiomove((caddr_t)at, (int)n, uio);
		t2 = mach_absolute_time();
		if (f.sequence <= 3 || f.sequence % 300 == 0) {
			absolutetime_to_nanoseconds(t1 - t0, &ns1);
			absolutetime_to_nanoseconds(t2 - t1, &ns2);
			printf("SWEET CAM: /dev/redwood-camera: frame %llu read: %u bytes, caches %llu us, copy %llu us\n", f.sequence, n,
			    ns1 / 1000, ns2 / 1000);
		}
	}
	lck_mtx_lock(&ios_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case REDWOOD_CAMERA_EXPOSURE:
		lck_mtx_lock(&ios_lock);
		ios_ask.lines = c.a;
		ios_ask.gain = c.b;
		ios_ask.frame_lines = c.c;
		ios_ask.dgain = c.d;    /* 0931: digital gain, 256 = 1x, 0 = as it is */
		ios_ask.pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	case REDWOOD_CAMERA_FOCUS:
		/* 0929: a position 0 (infinity) .. 1023 (macro), b = 1 park; taken at the next frame's end (the frames say
		 * REDWOOD_CAMERA_NO_FOCUS where there is no lens motor, and then this does nothing) */
		if (c.b > REDWOOD_CAMERA_FOCUS_PARK) {
			return EINVAL;
		}
		lck_mtx_lock(&ios_lock);
		ios_focus.pos = c.a > LENS_MAX ? LENS_MAX : c.a;
		ios_focus.park = c.b == REDWOOD_CAMERA_FOCUS_PARK;
		ios_focus.pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	case REDWOOD_CAMERA_SENSOR:
		if (c.a >= 4 || ios_to_ours[c.a] == ~0u) {
			return EINVAL;
		}
		lck_mtx_lock(&ios_lock);
		ios_switch_mode = c.b;
		ios_switch_to = ios_to_ours[c.a] + 1;
		lck_mtx_unlock(&ios_lock);
		wakeup(&ios_open);
		return 0;
	case REDWOOD_CAMERA_KEEP:
		return ENOTSUP;
	}
	return EINVAL;
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = 0
};

/* sweetcam=3 and up: the device (made at once: mediaserverd looks for it when it starts) and its server thread */
static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") == NULL) {
		printf("SWEET CAM: /dev/redwood-camera cannot be made (%d)\n", major);
		return;
	}
	printf("SWEET CAM: /dev/redwood-camera made\n");
}

/*
 * r17b: "c 20" took the phone down again with not one of its lines in the log, the stages' 300 ms apart
 * notwithstanding: a poke runs on the USB driver's command thread (sweet_usb.c), the thread that also sends the
 * log, so nothing a command prints goes out before it returns. The capture runs on a thread of its own now; the
 * command only starts it.
 */
static struct { volatile bool busy; unsigned what; uint64_t a, b, c; } job;

static void
cam_job(void *arg __unused, wait_result_t w __unused)
{
	lck_mtx_lock(&sweet_cam_lock);
	int err = EINVAL;
	if (job.what == 20) {
		err = capture_tpg();
	} else if (job.what == 21) {
		err = capture_sensor((unsigned)job.a, (unsigned)job.b, job.c);
	}
	printf("SWEET CAM: job %u done: %d\n", job.what, err);
	job.busy = false;
	lck_mtx_unlock(&sweet_cam_lock);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* "c N [A B C]" (sweet_poke.c): 20 the CSID's test pattern, 21 SENSOR MODE OPTIONS a sensor's frames, on a thread */
int
sweet_cam_cmd_args(unsigned what, uint64_t a, uint64_t b, uint64_t c)
{
	if (cm.mode < 2 || !cm.tlmm) {
		printf("SWEET CAM: not ready (sweetcam=%u)\n", cm.mode);
		return ENXIO;
	}
	if (what == 22) {
		cam_step_ms = a > 30000 ? 30000 : (unsigned)a;
		printf("SWEET CAM: %u ms before each stage\n", cam_step_ms);
		return 0;
	}
	if (ios_streaming && what != 22) {
		printf("SWEET CAM: /dev/redwood-camera is streaming: not now\n");
		return EBUSY;
	}
	if (what == 20 || what == 21) {
		if (job.busy) {
			return EBUSY;
		}
		job.busy = true;
		job.what = what;
		job.a = a;
		job.b = b;
		job.c = c;
		thread_t t;
		if (kernel_thread_start(cam_job, NULL, &t) != KERN_SUCCESS) {
			job.busy = false;
			return ENOMEM;
		}
		thread_deallocate(t);
		printf("SWEET CAM: job %u started on its own thread\n", what);
		return 0;
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
	} else if (what < sizeof(sensors) / sizeof(sensors[0])) {
		err = probe(what);
	} else {
		err = EINVAL;
	}
	lck_mtx_unlock(&sweet_cam_lock);
	return err;
}

int
sweet_cam_cmd(unsigned what)
{
	return sweet_cam_cmd_args(what, 0, 0, 0);
}

static uint32_t camdev;                 /* boot-arg sweetcamdev=1 (0923): /dev/redwood-camera */

static void
cam_thread(void *arg __unused, wait_result_t wr __unused)
{
	if (camdev && cm.mode >= 2) {
		camdev_make();  /* (at once: mediaserverd looks for it when it starts; reads wait for the server thread) */
	}
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
	if (camdev && cm.mode >= 2) {
		thread_t t;
		if (kernel_thread_start(ios_thread, NULL, &t) == KERN_SUCCESS) {
			thread_deallocate(t);
		}
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
	PE_parse_boot_argn("sweetcamdev", &camdev, sizeof(camdev));
	if (!cm.mode) {
		return;
	}
	thread_t t;
	if (kernel_thread_start(cam_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
