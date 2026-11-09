/*
 * redfin (Pixel 5): the two Cirrus Logic CS35L41 speaker amplifiers. Patch 0953 (WS-A).
 *
 * Where they are (live DT i2c@98c000, lito-redbull-audio.dtsi): I2C 0x40 (cs35l41_l, "cirrus,left-channel-amp",
 * ASP RX slot 0; heard as the TOP/earpiece speaker, r29b) and 0x41 (cs35l41_r, prefix "R", slot 1; heard as the BOTTOM one) on
 * QUPv3 SE9, the DRV2624's bus: every transfer goes through bsd/dev/sweet_haptic.c's
 * sweet_se9_xfer(), under its lock (so sweethaptic=1 must have brought the bus up). Reset lines:
 * PM8150L GPIO11 (0x40, SID 4 peripheral 0xca) and PM7250B GPIO11 (0x41, SID 2 peripheral 0xca): the 0x41
 * node's reset-gpios still names PM8150L GPIO10, but on production redfin (lito-redfin-proto1_1-audio.dtsi,
 * lito-v2-redfin-evt-audio.dtsi, the live DT) that pin's pinctrl is disabled and its pinctrl-0 drives
 * reset_pin_bottom_default1_1 = PM7250B GPIO11 output-high instead -- that is the real reset line; boost 1.0 uH / 15 uF / 3700 mA peak, ASP SDOUT hi-Z 3, HW noise gate 0x3f.
 *
 * How they are set up: the redbull kernel's sound/soc/codecs/cs35l41.c without its DSP: probe (OTP boot
 * done, DEVID 0x35a40, REVID, the per-revision errata patch, the OTP trim unpack with
 * cs35l41-tables.c's map, Halo core held), cs35l41_set_pdata() (boost K1/K2/slope/LBST/IPK, hi-Z,
 * noise gate), the DAI as kona.c sets it for QUIN_TDM_RX (DSP_A, codec slave, NB/NF; 48 kHz, 32-bit
 * slots, 24-bit words), SCLK 6.144 MHz as the PLL reference (cs35l41_pll_sysclk 0x28) and the FS
 * monitor window for it, "PCM Source" ASP (0x08: the ASP's RX1 straight to the amplifier: bypass, no
 * speaker-protection firmware), "AMP PCM Gain" 17 as Android's mixer paths, the digital volume as
 * "how far under Android's level" (Digital PCM Volume 817 = 0 dB, 1/8 dB steps), DRE on; power-up
 * as cs35l41_main_amp_event() (the pup patch, GLOBAL_EN) and power-down (GLOBAL_EN off, PDN_DONE,
 * the pdn patch). Errors (amp short, boost short/OVP/UVP, over-temperature) power both down at once.
 *
 * The reset lines are PMIC GPIOs: a write to them is an SPMI write, so it happens only with boot-arg
 * sndgpio=1 (allow-list: PM8150L GPIO11, PM7250B GPIO11 and PM7250B GPIO6, the microphones' LDO, nothing else, and
 * only on channels this EE owns, as sweet_haptic.c does for PM7250B GPIO1). Without it the amplifiers
 * are probed as the boot chain left them, and if they do not answer the line states are only read.
 *
 * Log lines: "SWEET AMP: ...".
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include "sweet_spmi.h"
#include <sys/systm.h>
#include <sys/errno.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern int sweet_se9_xfer(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr);

bool sweet_amp_prepare(void);
bool sweet_amp_start(void);
bool sweet_amp_volume(uint32_t below);
bool sweet_amp_healthy(void);
void sweet_amp_stop(void);
bool sweet_amp_ready(void);
void sweet_amp_status(void);
void sweet_amp_reprobe(void);
int sweet_audio_pmic_gpio(unsigned sid, unsigned peripheral, int high);

#define SAY(...)                printf("SWEET AMP: " __VA_ARGS__)

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- PMIC GPIOs over SPMI (the APPS EE's channels, as sweet_haptic.c's hap_spmi()) -------------------------- */

#define SP_CORE_PA              0x0c440000u
#define SP_CHNLS_PA             0x0c600000u
#define SP_OBS_PA               0x0e600000u
#define SP_CNFG_PA              0x0c40a000u

static vm_offset_t sp_core, sp_cnfg, sp_obs;
static vm_offset_t sp_chnl[3];                  /* the three allowed peripherals' write channels, mapped once */
static uint32_t pmic_writes = 0;                /* boot-arg sndgpio */
static LCK_GRP_DECLARE(amp_lock_group, "sweet-amp");
static LCK_MTX_DECLARE(spmi_lock, &amp_lock_group);

static const struct {
	unsigned sid, peripheral;
	const char *name;
} allowed[3] = {
	{ 4, 0xca, "PM8150L GPIO11 (CS35L41 0x40 reset)" },
	{ 2, 0xca, "PM7250B GPIO11 (CS35L41 0x41 reset)" },
	{ 2, 0xc5, "PM7250B GPIO6 (audio LDO, mic 1)" },
};

static uint32_t
mrd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
mwr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static int
spmi(unsigned which, unsigned reg, bool write, uint8_t *val)
{
	unsigned ppid = (allowed[which].sid << 8) | allowed[which].peripheral, n, apid = 0xffff, owner = 7;
	vm_offset_t ch;

	if (sp_core == 0) {
		sp_core = ml_io_map(SP_CORE_PA, 0x2000);
		sp_cnfg = ml_io_map(SP_CNFG_PA, 0x1000);
		sp_obs = ml_io_map(SP_OBS_PA, 0x10000);
	}
	n = mrd(sp_core, 4) & 0x7ffu;
	for (unsigned i = 0; i < n && i < 512; i++) {
		uint32_t map = mrd(sp_core, 0x900u + 4u * i);
		unsigned o;

		if (!map || ((map >> 8) & 0xfffu) != ppid) {
			continue;
		}
		o = mrd(sp_cnfg, 0x700u + 4u * i) & 7u;
		if (apid == 0xffff || o == 0) {
			apid = i;
			owner = o;
		}
	}
	if (apid == 0xffff || (write && owner != 0)) {
		SAY("spmi %s %s reg %#x refused (apid %#x owner %u)\n", write ? "write" : "read", allowed[which].name, reg, apid, owner);
		return -1;
	}
	int bus = sweet_spmi_bus_lock();        /* 1062 */
	if (write) {
		if (sp_chnl[which] == 0) {
			sp_chnl[which] = ml_io_map(SP_CHNLS_PA + 0x10000u * apid, 0x1000);
		}
		ch = sp_chnl[which];
		mwr(ch, 0x10, *val);
	} else {
		ch = sp_obs + 0x80u * apid;
	}
	mwr(ch, 0x00, ((write ? 0u : 1u) << 27) | ((reg & 0xffu) << 4));
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t st = mrd(ch, 0x08);

		if (st & 1u) {
			if (st & 0x0eu) {
				sweet_spmi_bus_unlock(bus);
				SAY("spmi %s reg %#x status %#x\n", allowed[which].name, reg, st);
				return -1;
			}
			if (!write) {
				*val = (uint8_t)mrd(ch, 0x18);
			}
			sweet_spmi_bus_unlock(bus);
			return 0;
		}
		delay(1);
	}
	sweet_spmi_bus_unlock(bus);
	SAY("spmi %s reg %#x timeout\n", allowed[which].name, reg);
	return -1;
}

static void
gpio_look(unsigned which)
{
	uint8_t type = 0, sub = 0, st = 0, mode = 0, vin = 0, src = 0, en = 0;

	lck_mtx_lock(&spmi_lock);
	(void)spmi(which, 0x04, false, &type);
	(void)spmi(which, 0x05, false, &sub);
	(void)spmi(which, 0x08, false, &st);
	(void)spmi(which, 0x40, false, &mode);
	(void)spmi(which, 0x41, false, &vin);
	(void)spmi(which, 0x44, false, &src);
	(void)spmi(which, 0x46, false, &en);
	lck_mtx_unlock(&spmi_lock);
	SAY("%s: type %#x sub %#x status %#x mode %#x vin %#x out-src %#x en %#x\n", allowed[which].name, type, sub, st, mode, vin,
	    src, en);
}

/*
 * One of the allowed PMIC GPIOs as an output, high or low (high < 0: only looked at). Only with sndgpio=1.
 * DIG_VIN_CTL 0x41 = 1 (power-source 1), DIG_OUT_SOURCE_CTL 0x44 = 0x80 high / 0 low, MODE_CTL 0x40 = 1, EN_CTL 0x46 = 0x80.
 */
int
sweet_audio_pmic_gpio(unsigned sid, unsigned peripheral, int high)
{
	unsigned which;
	uint8_t type = 0, v;
	int err = 0;

	PE_parse_boot_argn("sndgpio", &pmic_writes, sizeof(pmic_writes));
	for (which = 0; which < 3 && (allowed[which].sid != sid || allowed[which].peripheral != peripheral); which++) {
	}
	if (which == 3) {
		SAY("PMIC GPIO %u:%#x is not on the allow-list\n", sid, peripheral);
		return EPERM;
	}
	if (high < 0 || !pmic_writes) {
		gpio_look(which);
		if (high >= 0) {
			SAY("%s NOT driven %s: SPMI writes need sndgpio=1\n", allowed[which].name, high ? "high" : "low");
			return EPERM;
		}
		return 0;
	}
	lck_mtx_lock(&spmi_lock);
	if (spmi(which, 0x04, false, &type) || type != 0x10) {
		SAY("%s: type %#x, not a GPIO: not written\n", allowed[which].name, type);
		err = ENXIO;
	} else {
		if (!(allowed[which].sid == 2 && allowed[which].peripheral == 0xca)) {
			v = 0x01;       /* power-source 1 (the DT gives none for PM7250B GPIO11: its VIN is left as found) */
			err |= spmi(which, 0x41, true, &v);
		}
		v = high ? 0x80 : 0x00;
		err |= spmi(which, 0x44, true, &v);
		v = 0x01;
		err |= spmi(which, 0x40, true, &v);
		v = 0x80;
		err |= spmi(which, 0x46, true, &v);
	}
	lck_mtx_unlock(&spmi_lock);
	SAY("%s driven %s -> %s\n", allowed[which].name, high ? "HIGH" : "low", err ? "FAILED" : "ok");
	return err ? EIO : 0;
}

/* ---- the amplifiers' registers (32-bit addresses and values, big-endian, on SE9) --------------------------- */

#define AMP_L                   0x40
#define AMP_R                   0x41

#define CS_DEVID                0x00000000u
#define CS_REVID                0x00000004u
#define CS_OTPID                0x00000010u
#define CS_TEST_KEY_CTL         0x00000040u
#define CS_OTP_MEM0             0x00000400u
#define CS_PWR_CTRL1            0x00002014u
#define CS_PWR_CTRL2            0x00002018u
#define CS_PWR_CTRL3            0x0000201cu
#define CS_AMP_OUT_MUTE         0x00002024u
#define CS_PLL_CLK_CTRL         0x00002c04u
#define CS_GLOBAL_CLK_CTRL      0x00002c0cu
#define CS_TST_FS_MON0          0x00002d10u
#define CS_BSTCVRT_PEAK_CUR     0x00003808u
#define CS_BSTCVRT_COEFF        0x00003810u
#define CS_BSTCVRT_SLOPE_LBST   0x00003814u
#define CS_SP_ENABLES           0x00004800u
#define CS_SP_RATE_CTRL         0x00004804u
#define CS_SP_FORMAT            0x00004808u
#define CS_SP_HIZ_CTRL          0x0000480cu
#define CS_SP_FRAME_RX_SLOT     0x00004820u
#define CS_SP_RX_WL             0x00004840u
#define CS_DAC_PCM1_SRC         0x00004c00u
#define CS_AMP_DIG_VOL_CTRL     0x00006000u
#define CS_NG_CFG               0x00006808u
#define CS_AMP_GAIN_CTRL        0x00006c04u
#define CS_IRQ1_STATUS1         0x00010010u
#define CS_IRQ1_STATUS3         0x00010018u
#define CS_IRQ1_STATUS4         0x0001001cu
#define CS_IRQ1_MASK1           0x00010110u
#define CS_DSP1_CCM_CORE_CTRL   0x02bc1000u

#define CS_CHIP_ID              0x35a40u
#define CS_CHIP_ID_R            0x35b40u
#define CS_OTP_BOOT_DONE        0x02u
#define CS_OTP_BOOT_ERR         0x80000000u
#define CS_PDN_DONE             0x00800000u
#define CS_INT1_MASK_DEFAULT    0x7ffcee3fu
#define CS_ERRORS               (0x80000000u | 0x00020000u | 0x0100u | 0x80u | 0x40u)  /* amp short, temp, bst short/UVP/OVP */
#define CS_TEMP_WARN            0x8000u
#define CS_VOL_MUTE             0x4cfu
#define CS_VOL_MAX_BELOW        816u            /* 1/8 dB: -102 dB is the quietest code */
#define CS_PCM_GAIN_ANDROID     17u             /* mixer "AMP PCM Gain" 17 */

static struct amp {
	uint8_t addr;
	const char *name;
	unsigned slot, reset_gpio;              /* reset_gpio: index into allowed[] */
	bool present, prepared, on;
	uint32_t revid, otpid, pwr2;
	unsigned reprobes;                      /* 0958: lazy re-probes made (limit CS_REPROBE_MAX per boot) */
	uint64_t last_try_us;                   /* 0958: when the last one began (0: none yet) */
} amps[2] = {
	/* (position labels as heard on the phone, r29b: 0x41 is the bottom speaker, 0x40 the top/earpiece one; routing unchanged) */
	{ AMP_L, "0x40 (top)", 0, 0, false, false, false, 0, 0, 0, 0, 0 },
	{ AMP_R, "0x41 (bottom)", 1, 1, false, false, false, 0, 0, 0, 0, 0 },
};
static unsigned se9_waited_ms;                  /* 0958: how long sweet_amp_prepare() waited for SE9 */
static LCK_MTX_DECLARE(reprobe_lock, &amp_lock_group);
#define CS_REPROBE_MAX          20u             /* per boot, per amplifier */
#define CS_REPROBE_GAP_US       10000000ull     /* at least 10 s between two tries of one amplifier */
#define CS_REPROBE_OTP_MS       30u             /* (boot probe: 100) so the ON path stays short */
static uint32_t below_now = CS_VOL_MAX_BELOW + 1;
static bool gave_up, warned_temp;

static int
cs_read(struct amp *a, uint32_t reg, uint32_t *val)
{
	uint8_t tx[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg }, rx[4] = { 0 };
	int err = sweet_se9_xfer(a->addr, tx, 4, rx, 4);

	if (err == 0) {
		*val = (uint32_t)rx[0] << 24 | (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
	}
	return err;
}

static int
cs_write(struct amp *a, uint32_t reg, uint32_t val)
{
	uint8_t tx[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		          (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val };
	int err = sweet_se9_xfer(a->addr, tx, 8, NULL, 0);

	if (err) {
		SAY("%s: write %#x <- %#x FAILED (%d)\n", a->name, reg, val, err);
	}
	return err;
}

static int
cs_bits(struct amp *a, uint32_t reg, uint32_t mask, uint32_t val)
{
	uint32_t old = 0;
	int err = cs_read(a, reg, &old);

	return err ? err : cs_write(a, reg, (old & ~mask) | (val & mask));
}

static int
cs_unlock(struct amp *a, bool unlock)
{
	int err = cs_write(a, CS_TEST_KEY_CTL, unlock ? 0x55 : 0xcc);

	return err ? err : cs_write(a, CS_TEST_KEY_CTL, unlock ? 0xaa : 0x33);
}

/* cs35l41.c's errata patches: B0 and B2 are the same; A0 has three more */
static int
cs_errata(struct amp *a)
{
	static const uint32_t common[][2] = {
		{ 0x00004100, 0 }, { 0x00004310, 0 }, { 0x00004400, 0 }, { 0x0000381c, 0x51 }, { 0x02bc20e0, 0 }, { 0x02bc2020, 0 },
	};
	int err = cs_write(a, CS_TEST_KEY_CTL, 0x5555) || cs_write(a, CS_TEST_KEY_CTL, 0xaaaa);

	if (a->revid == 0xa0) {
		err = err || cs_write(a, 0x00003854, 0x05180240) || cs_write(a, 0x00007418, 0x9091a1c8) ||
		    cs_write(a, 0x00003014, 0x0200ee0e) || cs_write(a, 0x00000054, 0x00000004) || cs_write(a, 0x00010318, 0) ||
		    cs_write(a, 0x00010b18, 0);
	}
	for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]) && !err; i++) {
		err = cs_write(a, common[i][0], common[i][1]);
	}
	return err || cs_write(a, CS_TEST_KEY_CTL, 0xcccc) || cs_write(a, CS_TEST_KEY_CTL, 0x3333);
}

/* cs35l41-tables.c otp_map_1 / otp_map_2 (they differ only in element 98: map 2 has VMON_POL 0x4000 bit 11) */
static const struct {
	uint32_t reg;
	uint8_t shift, size;
} otp_map[] = {
	{ 0x2030, 0, 4 }, { 0x2030, 7, 1 }, { 0x208c, 24, 6 }, { 0x2090, 14, 4 }, { 0x2090, 10, 4 }, { 0x300c, 11, 4 },
	{ 0x394c, 23, 2 }, { 0x3950, 0, 7 }, { 0x3950, 8, 7 }, { 0x3950, 16, 8 }, { 0x3950, 24, 8 }, { 0x3954, 0, 7 },
	{ 0x3954, 8, 7 }, { 0x3954, 16, 8 }, { 0x3954, 24, 8 }, { 0x3958, 0, 7 }, { 0x3958, 8, 7 }, { 0x3958, 16, 8 },
	{ 0x3958, 24, 8 }, { 0x395c, 0, 7 }, { 0x395c, 8, 7 }, { 0x395c, 16, 8 }, { 0x395c, 24, 8 }, { 0x416c, 0, 8 },
	{ 0x4160, 0, 7 }, { 0x416c, 8, 8 }, { 0x4160, 16, 10 }, { 0x416c, 16, 12 }, { 0x416c, 28, 1 }, { 0x4170, 0, 6 },
	{ 0x4170, 6, 1 }, { 0x4170, 8, 6 }, { 0x4170, 14, 1 }, { 0x4170, 16, 9 }, { 0x4360, 0, 5 }, { 0x4360, 6, 9 },
	{ 0x4448, 0, 8 }, { 0x4448, 8, 8 }, { 0x4448, 16, 8 }, { 0x4448, 24, 8 }, { 0x444c, 0, 3 }, { 0x6e30, 0, 5 },
	{ 0x6e30, 8, 5 }, { 0x6e30, 16, 5 }, { 0x6e30, 24, 5 }, { 0x6e34, 0, 5 }, { 0x6e34, 8, 5 }, { 0x6e34, 16, 5 },
	{ 0x6e34, 24, 5 }, { 0x6e38, 0, 5 }, { 0x6e38, 8, 5 }, { 0x6e38, 16, 5 }, { 0x6e38, 24, 5 }, { 0x6e3c, 0, 5 },
	{ 0x6e3c, 8, 5 }, { 0x6e3c, 16, 5 }, { 0x6e3c, 24, 5 }, { 0x6e40, 0, 5 }, { 0x6e40, 8, 5 }, { 0x6e40, 16, 5 },
	{ 0x6e40, 24, 5 }, { 0x6e44, 0, 5 }, { 0x6e48, 0, 10 }, { 0x6e48, 10, 10 }, { 0x6e48, 20, 10 }, { 0x6e4c, 0, 10 },
	{ 0x6e4c, 10, 10 }, { 0x6e4c, 20, 10 }, { 0x6e50, 0, 10 }, { 0x6e50, 10, 10 }, { 0x6e50, 20, 10 }, { 0x6e54, 0, 10 },
	{ 0x6e54, 10, 10 }, { 0x6e54, 20, 10 }, { 0x6e58, 0, 10 }, { 0x6e58, 10, 10 }, { 0x6e58, 20, 10 }, { 0x6e5c, 0, 10 },
	{ 0x6e5c, 10, 10 }, { 0x6e5c, 20, 10 }, { 0x6e60, 0, 10 }, { 0x6e60, 10, 10 }, { 0x6e60, 20, 10 }, { 0x6e64, 0, 10 },
	{ 0x7418, 7, 5 }, { 0x741c, 0, 5 }, { 0x741c, 11, 4 }, { 0x741c, 19, 4 }, { 0x7434, 17, 1 }, { 0x7434, 18, 7 },
	{ 0x7068, 0, 9 }, { 0x410c, 7, 1 }, { 0x400c, 0, 7 }, { 0x0000, 0, 1 }, { 0x17040, 0, 8 }, { 0x17040, 8, 8 },
	{ 0x17040, 16, 8 }, { 0x17040, 24, 8 }, { 0x17044, 0, 24 },
};
#define OTP_ELEMENT_98          98

static uint32_t
genmask(unsigned high, unsigned low)
{
	return (high >= 31 ? 0xffffffffu : ((1u << (high + 1)) - 1)) & ~((1u << low) - 1);
}

/* cs35l41_otp_unpack(): the trims from OTP into their registers */
static int
cs_otp(struct amp *a)
{
	uint32_t mem[32], id = 0;
	unsigned bit = 16, word = 2, written = 0;
	int err;

	if ((err = cs_read(a, CS_OTPID, &id)) != 0) {
		return err;
	}
	if (id != 1 && id != 2 && id != 6 && id != 8) {
		SAY("%s: OTP map %u unknown: no trims\n", a->name, id);
		return ENODEV;
	}
	a->otpid = id;
	for (unsigned i = 0; i < 32; i++) {
		if ((err = cs_read(a, CS_OTP_MEM0 + 4 * i, &mem[i])) != 0) {
			return err;
		}
	}
	if ((err = cs_unlock(a, true)) != 0) {
		return err;
	}
	for (unsigned i = 0; i < sizeof(otp_map) / sizeof(otp_map[0]) && !err; i++) {
		uint32_t reg = otp_map[i].reg, val, size = otp_map[i].size, shift = otp_map[i].shift;

		if (i == OTP_ELEMENT_98 && (id == 2 || id == 6)) {
			reg = 0x4000;
			shift = 11;
		}
		if (bit + size - 1 >= 32) {
			val = (mem[word] & genmask(31, bit)) >> bit;
			word++;
			val |= (mem[word] & genmask(bit + size - 33, 0)) << (32 - bit);
			bit += size - 32;
		} else {
			val = (mem[word] & genmask(bit + size - 1, bit)) >> bit;
			bit += size;
		}
		if (bit == 32) {
			bit = 0;
			word++;
		}
		if (reg != 0) {
			err = cs_bits(a, reg, genmask(shift + size - 1, shift), val << shift);
			written++;
		}
	}
	err = err || cs_unlock(a, false);
	SAY("%s: OTP map %u: %u trims written%s\n", a->name, id, written, err ? ", WITH ERRORS" : "");
	return err;
}

static bool
probe(struct amp *a, unsigned otp_ms, const char *tag)
{
	uint32_t devid = 0, st = 0;
	int err;
	unsigned ms;

	err = cs_read(a, CS_DEVID, &devid);
	if (err) {
		SAY("%s: %sNO ANSWER on SE9 (%d%s)\n", a->name, tag, err, err == ENXIO ? ": NACK, or the SE9 bus is not up (sweethaptic=1)" : "");
		return false;
	}
	(void)cs_read(a, CS_REVID, &a->revid);
	for (ms = 0; ms < otp_ms; ms++) {
		if (cs_read(a, CS_IRQ1_STATUS4, &st) == 0 && (st & CS_OTP_BOOT_DONE)) {
			break;
		}
		sleep_ms(1);
	}
	(void)cs_read(a, CS_IRQ1_STATUS3, &st);
	SAY("%s: %sDEVID %#x REVID %#x, OTP boot %s%s\n", a->name, tag, devid, a->revid, ms < otp_ms ? "done" : "NOT DONE",
	    (st & CS_OTP_BOOT_ERR) ? ", OTP BOOT ERROR" : "");
	if ((devid != CS_CHIP_ID && devid != CS_CHIP_ID_R) || ms >= otp_ms || (st & CS_OTP_BOOT_ERR)) {
		return false;
	}
	a->present = true;
	return true;
}

/* Everything but power: the amplifier configured, muted, its output stage off. */
static bool
configure(struct amp *a)
{
	uint32_t pwr2 = 0;
	int err;

	err = cs_write(a, CS_IRQ1_MASK1, CS_INT1_MASK_DEFAULT) || cs_errata(a) || cs_otp(a) ||
	    cs_write(a, CS_DSP1_CCM_CORE_CTRL, 0);
	/* boost: 1.0 uH (LBST 0), 15 uF (range 0): K1 0x24, K2 0x24, slope 0x75; peak (3700 - 1600) / 50 + 0x10 */
	err = err || cs_bits(a, CS_BSTCVRT_COEFF, 0xffff, 0x24 | 0x24 << 8) || cs_bits(a, CS_BSTCVRT_SLOPE_LBST, 0xff03, 0x75 << 8 | 0) ||
	    cs_bits(a, CS_BSTCVRT_PEAK_CUR, 0x7f, (3700 - 1600) / 50 + 0x10);
	err = err || cs_bits(a, CS_SP_HIZ_CTRL, 0x3, 3) || cs_bits(a, CS_NG_CFG, 0x3f00, 0x3f << 8);
	/* DAI: DSP_A (ASP_FMT 0), SCLK and LRCLK slave (MSTR 0), no inversion; RX width 32, word length 24; 48 kHz */
	err = err || cs_bits(a, CS_SP_FORMAT, 0x0700 | 0x10 | 0x01 | 0x40 | 0x04 | 0xff000000u, 32u << 24) ||
	    cs_bits(a, CS_SP_RX_WL, 0x3f, 24) || cs_bits(a, CS_GLOBAL_CLK_CTRL, 0x1f, 0x03);
	/* the slots (mixer: ASPRX1/ASPRX2 Slot Position 0 for L, 1 for R), PCM source ASP RX1 (bypass) */
	err = err || cs_bits(a, CS_SP_FRAME_RX_SLOT, 0x3f3f, a->slot | a->slot << 8) || cs_bits(a, CS_DAC_PCM1_SRC, 0x7f, 0x08);
	/* SCLK 6.144 MHz as the PLL reference (cs35l41_component_set_sysclk with cfg 0x28) */
	err = err || cs_bits(a, CS_SP_RATE_CTRL, 0x3f, 0x28) || cs_bits(a, CS_PLL_CLK_CTRL, 0x0800, 0x0800) ||
	    cs_bits(a, CS_PLL_CLK_CTRL, 0x07e0, 0x28 << 5) || cs_bits(a, CS_PLL_CLK_CTRL, 0x10, 0) ||
	    cs_bits(a, CS_PLL_CLK_CTRL, 0x07, 0) || cs_bits(a, CS_PLL_CLK_CTRL, 0x0800, 0) || cs_bits(a, CS_PLL_CLK_CTRL, 0x10, 0x10);
	/* the FS monitor for SCLK > 6 MHz (cs35l41_dai_set_sysclk): fs1 16, fs2 36 */
	err = err || cs_unlock(a, true) || cs_write(a, CS_TST_FS_MON0, 16 | 36u << 12) || cs_unlock(a, false);
	/* gain 17 as Android, digital volume muted, output muted, DRE on, the ASP's RX1 on */
	err = err || cs_bits(a, CS_AMP_GAIN_CTRL, 0x1f << 5, CS_PCM_GAIN_ANDROID << 5) ||
	    cs_bits(a, CS_AMP_DIG_VOL_CTRL, 0x7ff << 3, CS_VOL_MUTE << 3) || cs_bits(a, CS_AMP_OUT_MUTE, 0x10, 0x10) ||
	    cs_bits(a, CS_PWR_CTRL3, 1u << 20 | 1u << 4, 1u << 20 | 1u << 4) || cs_bits(a, CS_SP_ENABLES, 1u << 16, 1u << 16);
	(void)cs_read(a, CS_PWR_CTRL2, &pwr2);
	a->pwr2 = pwr2;
	SAY("%s: %s (PWR_CTRL2 %#x)\n", a->name, err ? "CONFIGURATION FAILED" : "configured: ASP bypass, DSP_A slot, 24/32, PLL from "
	    "SCLK 6.144 MHz, gain 17, muted", pwr2);
	a->prepared = !err;
	return !err;
}

/* 0958: what "the SE9 bus is up" rests on: a DEVID read of each amplifier (sweet_se9_xfer: 0 = answered, ENXIO = bus down or NACK). */
static void
se9_look(const char *why)
{
	uint32_t id = 0;
	int e0 = cs_read(&amps[0], CS_DEVID, &id), e1 = cs_read(&amps[1], CS_DEVID, &id);

	SAY("SE9 state (%s): DEVID read of %s -> %d, of %s -> %d, waited %u ms at boot: %s\n", why, amps[0].name, e0, amps[1].name, e1,
	    se9_waited_ms, e0 && e1 ? "neither answers (bus not up, or both NACK)" : e0 || e1 ?
	    "the bus is up; the other one NACKs (reset, power or address)" : "both answer");
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/*
 * 0958: an amplifier that did not answer at boot is probed again: its own reset pulse (as sweet_amp_prepare(): low, 3 ms, high,
 * 3 ms, only with sndgpio=1; the other amplifier is left alone), the DEVID read, and if it answers the same configure() as at
 * boot, so it joins the next start. Lazy: from sweet_amp_start() (the watcher thread, never the audio feeder), at most once per
 * 10 s per amplifier, 20 times per boot; `force' (poke a probe) skips the 10 s. One log line per attempt. Cost of a failed try
 * ~7 ms; a successful one adds the OTP wait (<= 30 ms) and configure().
 */
static void
reprobe_one(struct amp *a, bool force)
{
	char tag[24];
	uint64_t now = now_us();

	if (a->prepared || a->on || (!force && (a->reprobes >= CS_REPROBE_MAX ||
	    (a->last_try_us != 0 && now - a->last_try_us < CS_REPROBE_GAP_US)))) {
		return;
	}
	a->last_try_us = now;
	a->reprobes++;
	snprintf(tag, sizeof(tag), "re-probe %u: ", a->reprobes);
	if (pmic_writes) {
		(void)sweet_audio_pmic_gpio(allowed[a->reset_gpio].sid, allowed[a->reset_gpio].peripheral, 0);
		sleep_ms(3);
		(void)sweet_audio_pmic_gpio(allowed[a->reset_gpio].sid, allowed[a->reset_gpio].peripheral, 1);
		sleep_ms(3);
	}
	a->present = false;
	if (probe(a, CS_REPROBE_OTP_MS, tag)) {
		(void)configure(a);
	}
}

void
sweet_amp_reprobe(void)
{
	/* (poke a probe) */
	if (!lck_mtx_try_lock(&reprobe_lock)) {
		SAY("re-probe: one is running already\n");
		return;
	}
	for (unsigned i = 0; i < 2; i++) {
		if (!amps[i].prepared) {
			reprobe_one(&amps[i], true);
		}
	}
	lck_mtx_unlock(&reprobe_lock);
}

bool
sweet_amp_ready(void)
{
	return amps[0].prepared || amps[1].prepared;
}

/* Found, reset (sndgpio=1), configured: powered down and muted. Once. */
bool
sweet_amp_prepare(void)
{
	static bool tried;
	bool any = false;

	if (tried) {
		return sweet_amp_ready();
	}
	tried = true;
	PE_parse_boot_argn("sndgpio", &pmic_writes, sizeof(pmic_writes));
	if (pmic_writes) {
		/* the reset pulse cs35l41_probe() makes: low >= 2 ms, then high, 2 ms before the first access */
		for (unsigned i = 0; i < 2; i++) {
			(void)sweet_audio_pmic_gpio(allowed[amps[i].reset_gpio].sid, allowed[amps[i].reset_gpio].peripheral, 0);
		}
		sleep_ms(3);
		for (unsigned i = 0; i < 2; i++) {
			(void)sweet_audio_pmic_gpio(allowed[amps[i].reset_gpio].sid, allowed[amps[i].reset_gpio].peripheral, 1);
		}
		sleep_ms(3);
	}
	/*
	 * The SE9 bus is sweet_haptic.c's, brought up by its own thread (8 s after start, then the DRV2624):
	 * the DSP may be up before it (r26a: amps probed at 67.47 s, SE9 up at 67.67 s), and sweet_se9_xfer()
	 * says ENXIO both for "bus not up" and for a NACK. So wait, up to 15 s, until either amplifier answers.
	 */
	{
		uint32_t id = 0;
		unsigned waited;

		for (waited = 0; waited < 15000; waited += 100) {
			if (cs_read(&amps[0], CS_DEVID, &id) == 0 || cs_read(&amps[1], CS_DEVID, &id) == 0) {
				break;
			}
			sleep_ms(100);
		}
		se9_waited_ms = waited;
		SAY("SE9: %s after %u ms\n", waited < 15000 ? "an amplifier answers" : "NO AMPLIFIER ANSWERS (or the bus never came up)",
		    waited);
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];

		if (!probe(a, 100, "")) {
			se9_look("boot probe failed");
			gpio_look(a->reset_gpio);
			if (!pmic_writes) {
				SAY("%s: if its reset line above is low or an input, sndgpio=1 lets this kernel drive it high\n", a->name);
			}
			continue;
		}
		any |= configure(a);
	}
	SAY("%s\n", any ? "AMPLIFIERS READY (powered down, muted)" : "NO AMPLIFIER READY");
	return any;
}

/* The digital volume: `below' eighths of a dB under Android's level (0 = Android's own); over 816: mute. */
bool
sweet_amp_volume(uint32_t below)
{
	uint32_t code = below > CS_VOL_MAX_BELOW ? CS_VOL_MUTE : ((0x800u - below) & 0x7ffu);
	int err = 0;

	for (unsigned i = 0; i < 2; i++) {
		if (amps[i].prepared) {
			err |= cs_bits(&amps[i], CS_AMP_DIG_VOL_CTRL, 0x7ff << 3, code << 3);
		}
	}
	below_now = below;
	return err == 0;
}

/* Powered up and unmuted at the current volume: only while the TDM clock runs (the PLL locks to SCLK). */
bool
sweet_amp_start(void)
{
	bool any = false;

	if (gave_up) {
		return false;
	}
	/* 0958: an amplifier missing since boot gets another try now (rate-limited inside; skipped if poke a probe is at it) */
	if (lck_mtx_try_lock(&reprobe_lock)) {
		for (unsigned i = 0; i < 2; i++) {
			reprobe_one(&amps[i], false);
		}
		lck_mtx_unlock(&reprobe_lock);
	}
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t s1 = 0, pwr2 = 0;
		int err;

		if (!a->prepared || a->on) {
			any |= a->on;
			continue;
		}
		/* "Main AMP" (PWR_CTRL2 AMP_EN), the boost converter (BST_EN 2), then cs35l41_main_amp_event POST_PMU */
		err = cs_bits(a, CS_PWR_CTRL2, 0x31, 0x21) || cs_unlock(a, true) || cs_write(a, 0x2084, 0x002f1aa0) ||
		    cs_unlock(a, false) || cs_bits(a, CS_PWR_CTRL1, 1, 1);
		sleep_ms(2);
		err = err || cs_bits(a, CS_AMP_OUT_MUTE, 0x10, 0);
		(void)cs_read(a, CS_IRQ1_STATUS1, &s1);
		(void)cs_read(a, CS_PWR_CTRL2, &pwr2);
		SAY("%s: %s (IRQ1_STATUS1 %#x, PWR_CTRL2 %#x)\n", a->name, err ? "START FAILED" : "ON", s1, pwr2);
		a->on = !err;
		any |= a->on;
	}
	return any;
}

/* Muted, GLOBAL_EN off, PDN_DONE waited for, the pdn patch: powered down (cs35l41_main_amp_event POST_PMD). */
void
sweet_amp_stop(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t s1 = 0;
		unsigned ms;

		if (!a->on) {
			continue;
		}
		(void)cs_bits(a, CS_AMP_OUT_MUTE, 0x10, 0x10);
		(void)cs_bits(a, CS_PWR_CTRL1, 1, 0);
		for (ms = 0; ms < 100; ms++) {
			if (cs_read(a, CS_IRQ1_STATUS1, &s1) == 0 && (s1 & CS_PDN_DONE)) {
				break;
			}
			sleep_ms(1);
		}
		(void)cs_write(a, CS_IRQ1_STATUS1, CS_PDN_DONE);
		(void)(cs_unlock(a, true) || cs_write(a, 0x2084, 0x002f1aa3) || cs_unlock(a, false));
		(void)cs_bits(a, CS_PWR_CTRL2, 0x01, 0);
		a->on = false;
		SAY("%s: off (%s)\n", a->name, ms < 100 ? "PDN_DONE" : "PDN NOT SEEN");
	}
}

/* No error reported: true. On an error both are powered down and not started again this boot. */
bool
sweet_amp_healthy(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t s1 = 0;

		if (!a->on) {
			continue;
		}
		if (cs_read(a, CS_IRQ1_STATUS1, &s1) != 0) {
			SAY("%s: status not readable: both powered down\n", a->name);
			goto bad;
		}
		if (s1 & CS_ERRORS) {
			SAY("%s: ERROR (IRQ1_STATUS1 %#x: %s%s%s%s%s): both powered down for this boot\n", a->name, s1,
			    (s1 & 0x80000000u) ? "amp short " : "", (s1 & 0x00020000u) ? "over-temperature " : "",
			    (s1 & 0x0100u) ? "boost short " : "", (s1 & 0x80u) ? "boost UVP " : "", (s1 & 0x40u) ? "boost OVP" : "");
			goto bad;
		}
		if ((s1 & CS_TEMP_WARN) && !warned_temp) {
			warned_temp = true;
			SAY("%s: temperature warning (IRQ1_STATUS1 %#x)\n", a->name, s1);
		}
	}
	return true;
bad:
	gave_up = true;
	sweet_amp_stop();
	return false;
}

void
sweet_amp_status(void)
{
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t s1 = 0, p1 = 0, p2 = 0, vol = 0;

		if (!a->present) {
			SAY("%s: not present (re-probes %u of %u)\n", a->name, a->reprobes, CS_REPROBE_MAX);
			continue;
		}
		(void)cs_read(a, CS_IRQ1_STATUS1, &s1);
		(void)cs_read(a, CS_PWR_CTRL1, &p1);
		(void)cs_read(a, CS_PWR_CTRL2, &p2);
		(void)cs_read(a, CS_AMP_DIG_VOL_CTRL, &vol);
		SAY("%s: %s, REVID %#x OTP %u, IRQ1_STATUS1 %#x PWR_CTRL1 %#x PWR_CTRL2 %#x DIG_VOL %#x; level %u.%u dB under "
		    "Android's%s\n", a->name, a->on ? "ON" : "off", a->revid, a->otpid, s1, p1, p2, vol, below_now / 8,
		    below_now % 8 * 125 / 100, gave_up ? "; GIVEN UP after an error" : "");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
