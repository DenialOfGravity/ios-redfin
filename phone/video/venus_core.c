/*
 * venus_core: the Venus/IRIS1 video engine through /dev/pmem, as a library (video 4a): everything venusctl did, moved
 * here unchanged from venusctl.c (whose header comment documents the commands, switches and safety rules), shared by
 * venusctl (main only) and the daemon venusd. 4a additions, venusctl's output unchanged by them:
 *   - the stage 3 decoder split into session steps (vdec_prepare/start/etb/poll/close, venus_core.h); venusctl decode
 *     runs its old loop over the same steps
 *   - vdec_sink: decoded frames to a callback (venusd) instead of venusctl's crop + CRC + OUT file
 *   - vdec_place (vdcredit): the callback may name where a frame goes (a credited IOSurface's planes) before its read
 *   - vc_quiet: no per-buffer lines (ETB/FTB commands, EBD/FBD packets) for the daemon's log; venusctl leaves it 0
 * The bitstream helpers (access units, SPS, CRC32) are in venus_h264.c, HEVC's in venus_hevc.c.
 * 4e: HEVC 8-bit Main decode (vdec_prepare's hevc, decode's --hevc): SESSION_INIT codec HFI_VIDEO_CODEC_HEVC 0x2000,
 * everything else as for H.264, as downstream does (msm_vdec.c:507-526 the same input/output min counts 4/8,
 * msm_vidc_clocks.c:1393-1405 the same work mode rule, DPB NV12_UBWC for 8-bit msm_vdec.c:1147-1150, input size from
 * the firmware's requirement for both). Main10 / 4:2:2 are refused before the session.
 *
 * redfin (Pixel 5, SM7250 "lito", track V, ws-n-video): ported from Yofuness/ios-flame (sunfish, SM7150 sdmmagpie,
 * phone/wifi/venus_core.c at 598ba5b). Same core: IRIS1 (downstream 4.19 msm_vidc lito_data .vpu_ver
 * VPU_VERSION_IRIS1, hfi_iris1.c; the CPU_CS/WRAPPER register files and HFI gen1 packets are identical), core
 * 0x0aa00000 + 0x200000, IRQ SPI 174, GDSCs mvsc/mvs0/mvs1 at videocc + 0x814/0x874/0x8b4, iris RCG 0x7f0, the
 * branches (minus iris_ahb), PAS ID 9, the same venus_ns IOVA pool and CP (mem protect) sizes, BCMs MM3 + MM2.
 * Changed for lito (live DT of this phone, videocc-lito.c, gcc-lito.c, lito-vidc.dtsi):
 *   - video_pll0 is a Lucid PLL (Fabia on sdmmagpie): Lucid register layout and video_pll0_config (L 0x19, CAL_L 0x44,
 *     CONFIG 0x20485699/0x00002261/0x329a699c, USER 0x1/0x805, TEST_U1 0x01800000); the enable sequence as
 *     xnu 0921's camcc PLL0 (the same Lucid steps, proven on this phone)
 *   - no video_cc_iris_ahb_clk (0x8f4); video_cc_xo at 0x980 (was 0x984)
 *   - gcc_video_axi 0xb080 (was 0xb01c), gcc_video_xo 0xb038 (was 0xb028); gcc_video_ahb 0xb004 unchanged
 *   - iris efuse 0x786008 (qfprom iris@6008): bit 21 multipipe disabled -> L 0x14 (200 MHz), bit 27 VP fmax off
 *     (videocc-lito.c video_multipipe_fixup); was 0x786018 bit 10
 *   - mnoc_sf_0_tbu (SIDs 0x1000-0x13ff, status 0x15182220 unchanged): hlos1 vote GDSC 0x17d054 (was 0x17d044)
 *   - SIDs: venus_ns iommus <0x1380 0x60>, <0x1360 0x0> (was 0x1080 mask 0x60): 0x1380/0x13a0/0x13c0/0x13e0 + 0x1360
 *   - firmware carve-out pil_video 0x86a00000 + 0x500000 (was 0x94800000)
 *   - cmd-db at lito's fixed 0x80860000 (xnu 0901/0904), not through the AOP locator at 0x0c3f000c
 *   - the free RAM below 4 GB for queues and buffers: redfin's XNU window is at 0x100000000 (1033), PMEM_ALLOC gives
 *     memory above 4 GB (useless to the 32-bit firmware) and the kernel has no PMEM_ALLOC32: queues and buffers come
 *     from the low bank's free 0xa2400000-0xb7e00000 (xnu 1054's xram=1 range: must stay out of the VM while Venus
 *     is used), FIXED_LO/HI below; venusd's default: queues 0xa2600000, buffer window 0xa2a00000 + 160 MB (xnu 1057's
 *     sweetvenus mapping)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "pmem.h"
#include "smmu.h"
#include "venus_core.h"
#include "venus_hevc.h"

#define GCC		0x00100000u
#define GCC_VIDEO_AHB	(GCC + 0xb004)	/* gcc_video_ahb_clk, GDSC parent (notes table "GDSC parent clock") */
#define GCC_VIDEO_AXI	(GCC + 0xb080)	/* gcc-lito.c gcc_video_axi_clk (sdmmagpie 0xb01c) */
#define GCC_VIDEO_XO	(GCC + 0xb038)	/* gcc-lito.c, CLK_IS_CRITICAL: read only */
#define GCC_HW_CTL	(1u << 1)	/* CBCR hwcg bit, as gcc-sm7150.c hwcg_bit = 1 */
#define GCC_TBU_SF_VOTE	0x0017d054u	/* lito: hlos1_vote_mmnoc_mmu_tbu_sf0_gdsc (live DT gdsc@17d054; SIDs 0x1000-0x13ff): read only */
#define TBU_SF_STATUS	0x15182220u	/* 2 words (live DT mnoc_sf_0_tbu@15195000 status-reg, same as sdmmagpie) */
#define EFUSE_VIDEO	0x00786008u	/* lito: qfprom iris@6008 "iris-bin" (videocc-lito.c video_multipipe_fixup) */
#define EFUSE_NO_MPIPE	(1u << 21)	/* IRIS_DISABLE_MULTIPIPE: PLL L 0x14, 200 MHz only */
#define EFUSE_NO_FMAX	(1u << 27)	/* IRIS_DISABLE_VP_FMAX: L 0x14, at most 365 MHz (we run 240 or 200) */

#define VCC		0x0ab00000u
/* video_pll0, Lucid on lito, .offset 0x42c (videocc-lito.c); Lucid layout (clk-alpha-pll.c CLK_ALPHA_PLL_TYPE_LUCID,
 * as xnu 0921's camcc PLL0) */
#define PLL0		(VCC + 0x42c)
#define PLL_MODE	(PLL0 + 0x00)
#define PLL_L		(PLL0 + 0x04)
#define PLL_CAL_L	(PLL0 + 0x08)
#define PLL_USER	(PLL0 + 0x0c)
#define PLL_USER_U	(PLL0 + 0x10)
#define PLL_USER_U1	(PLL0 + 0x14)
#define PLL_CONFIG	(PLL0 + 0x18)
#define PLL_CONFIG_U	(PLL0 + 0x1c)
#define PLL_CONFIG_U1	(PLL0 + 0x20)
#define PLL_TEST	(PLL0 + 0x24)
#define PLL_TEST_U	(PLL0 + 0x28)
#define PLL_TEST_U1	(PLL0 + 0x2c)
#define PLL_STATUS	(PLL0 + 0x30)
#define PLL_OPMODE	(PLL0 + 0x38)
#define PLL_FRAC	(PLL0 + 0x40)	/* ALPHA_VAL (the last register allowed() lets through) */
#define OUTCTRL		(1u << 0)
#define RESET_N		(1u << 2)
#define VOTE_FSM_ENA	(1u << 20)
#define UPDATE_BYPASS	(1u << 23)
#define LOCK_DET	(1u << 31)
#define PCAL_DONE	(1u << 27)	/* Lucid: calibration done (LUCID_PCAL_DONE); stays set after alpha_pll_lucid_disable */
#define PLL_RUN		1u
#define PLL_OUT_MASK	7u
/* video_cc_iris_clk_src: cmd_rcgr 0x7f0, hid_width 5, no MND, PLL0 main = sel 1 (videocc-lito.c, as sdmmagpie) */
#define IRIS_RCG	(VCC + 0x7f0)
#define IRIS_CFG	(IRIS_RCG + 4)
/* GDSCRs (live.dts 6995-7023); SW_COLLAPSE bit 0, HW_CONTROL bit 1, PWR_ON bit 31 (Linux gdsc.c) */
#define GDSC_MVSC	(VCC + 0x814)
#define GDSC_MVS0	(VCC + 0x874)
#define GDSC_MVS1	(VCC + 0x8b4)
#define SW_COLLAPSE	(1u << 0)
#define HW_CONTROL	(1u << 1)
#define PWR_ON		(1u << 31)
#define CBCR_EN		(1u << 0)
#define CLK_OFF		(1u << 31)
#define VCC_END		(VCC + 0xb94)	/* regmap max_register (videocc-lito.c, as sdmmagpie) */

#define WRAP_HW_VERSION	0x0aae0000u	/* WRAPPER 0xE0000 + 0 (vidc_hfi_io.h) */

/* video stage 2 steps 4-6: core registers (vidc_hfi_io.h, absolute = 0x0aa00000 + offset; notes section 1) */
#define CORE		0x0aa00000u
#define CORE_END	0x0aae4000u	/* up to the end of the wrapper (PIL reg 0x0aae0000 + 0x4000); videocc lies above */
#define CS		(CORE + 0xd2000)	/* VIDC_CPU_CS_BASE_OFFS */
#define CS_VMIMSG	(CS + 0x34)	/* HFI_DSP_QTBL_ADDR (setup_dsp_uc_memmap_vpu5, venus_hfi.c:4688) */
#define CS_VMIMSGAG0	(CS + 0x38)	/* HFI_DSP_UC_REGION_ADDR (venus_hfi.c:4690) */
#define CS_VMIMSGAG1	(CS + 0x3c)	/* HFI_DSP_UC_REGION_SIZE (venus_hfi.c:4692) */
#define CTRL_INIT	(CS + 0x48)	/* SCIACMD (__boot_firmware, venus_hfi.c:1274) */
#define CTRL_STATUS	(CS + 0x4c)	/* SCIACMDARG0: read only */
#define QTBL_INFO	(CS + 0x50)	/* SCIACMDARG1 (__setup_ucregion_memory_map, venus_hfi.c:1888) */
#define QTBL_ADDR	(CS + 0x54)	/* SCIACMDARG2 (venus_hfi.c:1886) */
#define VERSION_INFO	(CS + 0x58)	/* SCIACMDARG3: read only */
#define SFR_ADDR	(CS + 0x5c)	/* SCIBCMD (venus_hfi.c:1890) */
#define UC_ADDR		(CS + 0x64)	/* SCIBARG1 (venus_hfi.c:1883) */
#define UC_SIZE		(CS + 0x68)	/* SCIBARG2 (venus_hfi.c:1885) */
#define IC_SOFTINT	(CORE + 0xdf018)	/* VIDC_CPU_IC_SOFTINT, bit 15 = H2A (__iface_cmdq_write doorbell) */
#define SOFTINT_H2A	(1u << 15)
#define WRAP_INTR_STATUS (CORE + 0xe000c)	/* read only */
#define WRAP_CPU_CLKCFG	(CORE + 0xe2000)	/* clock_config_on_enable_vpu5 (venus_hfi.c:4706) */
#define WRAP_CPU_CGCDIS	(CORE + 0xe2010)	/* clock_config_on_enable_vpu5 (venus_hfi.c:4705) */
#define WRAP_CPU_STATUS	(CORE + 0xe2014)	/* read only */
#define ST_INIT		1u		/* CTRL_STATUS bit 0 */
#define ST_ERROR	0xfeu		/* CTRL_STATUS 7:1, 4 = bad UC region (venus_hfi.c:1277) */

#define FW_PA		0x86a00000ull	/* redfin: pil_video_region@86a00000 (live DT, no-map, 0x500000) */
#define FW_SIZE		0x00500000ull
#define SCM_MP_VIDEO_VAR 0x42000c08ull	/* SCM_SIP_FNID(SCM_SVC_MP 0xc, TZBSP_MEM_PROTECT_VIDEO_VAR 8) */
/* __protect_cp_mem (venus_hfi.c:4347-4390): cp_size = venus_ns start, non-pixel = venus_sec_non_pixel range */
#define CP_SIZE		0x25800000u
#define CP_NP_START	0x01000000u
#define CP_NP_SIZE	0x24800000u
/* redfin: venus_ns iommus <&apps_smmu 0x1380 0x60>, <&apps_smmu 0x1360 0x0> (live DT qcom,vidc0/non_secure_cb) */
static const uint16_t venus_sids[] = { 0x1380, 0x13a0, 0x13c0, 0x13e0, 0x1360 };
#define NSID		(sizeof(venus_sids) / sizeof(venus_sids[0]))
#define SID_LO		0x1360u	/* probe's scan range */
#define SID_HI		0x13e0u

/* HFI shared memory (venus_hfi.h:36-113): 3 MB block = table 0x1c8 + 3 queues of 0xc8000; queues get
 * SHARED_QSIZE - SFR - QDSS = 0x2fe000 (venus_hfi.c:1916), the SFR page follows them, inside the UC region */
#define QSHARED		0x300000u
#define QTBL_SIZE	0x1c8u
#define QHDR0		0x120u		/* sizeof(struct hfi_queue_table_header) */
#define QHDR_SIZE	0x38u
#define QSIZE		0xc8000u	/* 1024 x 50 x 16 */
#define QWORDS		(QSIZE / 4)
#define SFR_OFF		0x2fe000u
#define SFR_SIZE	0x1000u
#define FIXED_LO	0xa2600000ull	/* redfin: free low-bank RAM 0xa2400000-0xb7e00000 (xnu 1054 notes; memtest first) */
#define FIXED_HI	0xaca00000ull	/* redfin: end of xnu 1057's Venus window (xram=3/4 keep 0xa2600000-0xaca00000 out of
				 * the VM; the RAM above may be the VM's) */
/* redfin (rn4x): the Venus streams go through ONE translating bank mapping only FIXED_LO-FIXED_HI, IOVA = PA
 * (smmu_window), as Linux's venus_ns bank; its two table pages sit in the unused gap between the queue block
 * (0xa2600000 + 0x301000) and the buffer window (0xa2a00000). VENUSCTL_SMMU=identity: the old translation-off banks. */
#define SMMU_TBL	0xa29fe000ull
static uint64_t win_cbp;	/* the window bank (from boot or STATE), for fault reports */
#define POOL_END	0xe0000000ull	/* venus_ns IOVA pool 0x25800000-0xe0000000 (notes 8.4) */

#define PAS_VENUS	9
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))
#define MDT_RELOC	(1u << 27)

/* videocc branches (videocc-sdmmagpie.c:170-300); voted = BRANCH_VOTED there: no halt poll on disable (10 us) */
struct br { const char *name; uint32_t pa; int voted, cvp; };
static const struct br branches[] = {	/* enable order (spec); disable runs it backwards */
	{ "mvsc_ctl_axi", VCC + 0x9cc, 0, 0 },
	{ "mvs0_axi", VCC + 0x9ec, 0, 0 },
	{ "mvs1_axi", VCC + 0xa0c, 0, 1 },
	{ "mvsc_core", VCC + 0x850, 0, 0 },
	{ "mvs0_core", VCC + 0x890, 1, 0 },
	{ "mvs1_core", VCC + 0x8d0, 1, 1 },
	{ "venus_ahb", VCC + 0xa6c, 0, 0 },	/* lito has no video_cc_iris_ahb_clk (sdmmagpie 0x8f4) */
};
#define NBR (sizeof(branches) / sizeof(branches[0]))
#define VIDEO_CC_XO	(VCC + 0x980)	/* videocc-lito.c video_cc_xo_clk (sdmmagpie 0x984): read only */

int verbose, cvp, force;
int vc_quiet;	/* 4a: venusd sets it: no per-buffer lines (chatty() below) */

/* ---- boot session id (pre-run review 8.5.1): state files from an earlier boot are dropped unread ---- */

static char bootid[64];

const char *boot_id(void)
{
	if (bootid[0])
		return bootid;
	size_t n = sizeof(bootid) - 1;
	if (sysctlbyname("kern.bootsessionuuid", bootid, &n, NULL, 0) || !bootid[0]) {
		struct timeval bt = { 0, 0 };
		n = sizeof(bt);
		sysctlbyname("kern.boottime", &bt, &n, NULL, 0);
		snprintf(bootid, sizeof(bootid), "boottime-%ld.%06d", (long)bt.tv_sec, (int)bt.tv_usec);
	}
	return bootid;
}

/* a state file without our "boot ID" line is from an earlier boot (or older venusctl): delete it, act on nothing */
void stale_drop(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[256], id[128] = "";
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "boot %127s", id) == 1)
			break;
	fclose(f);
	if (!strcmp(id, boot_id()))
		return;
	if (getenv("VENUSCTL_NO_BOOTID")) {
		printf("venus: %s is from boot %s, not %s: kept (VENUSCTL_NO_BOOTID)\n", path, id[0] ? id : "?", boot_id());
		return;
	}
	printf("venus: %s: stale state from an earlier boot: dropped (boot %s, now %s)\n", path, id[0] ? id : "?",
	       boot_id());
	unlink(path);
}

static int ahb_on(void)
{
	return !(rd32(GCC_VIDEO_AHB) & CLK_OFF);
}

/* vdstop (research/notes/venus-wpt-2026-11-03.md section 4): a STOP below calls vc_stop_hook instead of exiting at
 * once (venusd unwinds and brings Venus down; it must not return); NULL or a hook that returns: exit(1) as before */
void (*vc_stop_hook)(const char *what);
int vc_fake_off;	/* vdstop test: the next N core_ok() reads report off (venusd VENUSD_TEST_CLKOFF) */
static int fake_hit;	/* the last core_ok() was such a fake */

static void vc_stop(const char *what)
{
	if (vc_stop_hook)
		vc_stop_hook(what);
	exit(1);
}

/* vdstop: a gate's reads before a STOP, 1 ms apart (a clock reading off once was transient in vdcold);
 * VENUS_CLOCK_READS=1: one read, as before */
static unsigned gate_reads(void)
{
	static int n = -1;
	if (n < 0)
		n = getenv("VENUS_CLOCK_READS") && atoi(getenv("VENUS_CLOCK_READS")) > 0 ? atoi(getenv("VENUS_CLOCK_READS")) : 3;
	return (unsigned)n;
}

/* every videocc access: gcc_video_ahb must be on, else stop here (an access with it off hangs the bus) */
static void vcc_gate(uint32_t pa, const char *what)
{
	if (pa < VCC || pa > VCC_END)
		return;
	unsigned n = gate_reads();
	for (unsigned i = 1; i <= n; i++) {
		if (ahb_on()) {
			if (i > 1)
				printf("venus: clock check: gcc_video_ahb on again at read %u/%u (%s %#x): going on\n", i, n,
				       what, pa);
			return;
		}
		printf("venus: clock check: gcc_video_ahb %#010x off (%u/%u)\n", rd32(GCC_VIDEO_AHB), i, n);
		if (i < n)
			delay_ms(1);
	}
	printf("venus: STOP at %s (%#x): gcc_video_ahb %#010x is off, videocc not touched\n", what, pa,
	       rd32(GCC_VIDEO_AHB));
	vc_stop(what);
}

static uint32_t vrd(uint32_t pa)
{
	vcc_gate(pa, "read");
	return rd32(pa);
}

/* every core access (0x0aa00000-0x0aae4000): AHB, gcc_video_axi, GDSCs mvsc + mvs0 and the branches all on */
static int core_ok(void)
{
	fake_hit = 0;
	if (vc_fake_off > 0) {
		vc_fake_off--;
		fake_hit = 1;
		return 0;
	}
	int ok = ahb_on() && !(rd32(GCC_VIDEO_AXI) & CLK_OFF) && (vrd(GDSC_MVSC) & PWR_ON) && (vrd(GDSC_MVS0) & PWR_ON);
	for (unsigned i = 0; i < NBR && ok; i++)
		if (!branches[i].cvp || cvp)
			ok = !(vrd(branches[i].pa) & CLK_OFF);
	return ok;
}

int vc_core_ok(void)
{
	return core_ok();
}

/* vdstop: which of core_ok's registers read off, one line each (videocc only read while the AHB reads on) */
static void clock_report(unsigned i, unsigned n)
{
	if (fake_hit) {
		printf("venus: clock check: fake off (VENUSD_TEST_CLKOFF) (%u/%u)\n", i, n);
		return;
	}
	int found = 0;
	uint32_t v = rd32(GCC_VIDEO_AHB);
	if (v & CLK_OFF) {
		printf("venus: clock check: gcc_video_ahb %#010x off (%u/%u), videocc not read\n", v, i, n);
		return;
	}
	if ((v = rd32(GCC_VIDEO_AXI)) & CLK_OFF)
		found++, printf("venus: clock check: gcc_video_axi %#010x off (%u/%u)\n", v, i, n);
	if (!((v = rd32(GDSC_MVSC)) & PWR_ON))
		found++, printf("venus: clock check: gdsc mvsc %#010x off (%u/%u)\n", v, i, n);
	if (!((v = rd32(GDSC_MVS0)) & PWR_ON))
		found++, printf("venus: clock check: gdsc mvs0 %#010x off (%u/%u)\n", v, i, n);
	for (unsigned k = 0; k < NBR; k++)
		if ((!branches[k].cvp || cvp) && ((v = rd32(branches[k].pa)) & CLK_OFF))
			found++, printf("venus: clock check: %s %#010x off (%u/%u)\n", branches[k].name, v, i, n);
	if (!found)
		printf("venus: clock check: none reads off on a second look (%u/%u)\n", i, n);
}

static void core_gate(uint32_t pa, const char *what)
{
	if (pa < CORE || pa >= CORE_END)
		return;
	unsigned n = gate_reads();
	for (unsigned i = 1; i <= n; i++) {
		if (core_ok()) {
			if (i > 1)
				printf("venus: clock check: on again at read %u/%u (%s %#x): going on\n", i, n, what, pa);
			return;
		}
		clock_report(i, n);
		if (i < n)
			delay_ms(1);
	}
	printf("venus: STOP at %s (%#x): a video clock or GDSC is off, core not touched"
	       " (venusctl shutdown, then power off)\n", what, pa);
	vc_stop(what);
}

static uint32_t crd(uint32_t pa)
{
	core_gate(pa, "read");
	return rd32(pa);
}

static int allowed(uint32_t pa)
{
	if (pa == GCC_VIDEO_AHB || pa == GCC_VIDEO_AXI)
		return 1;
	/* stage 2 steps 4-6, each from its downstream line (defines above) */
	if (pa == WRAP_CPU_CGCDIS || pa == WRAP_CPU_CLKCFG)	/* boot: clock_config_on_enable_vpu5 */
		return 1;
	if (pa == UC_ADDR || pa == UC_SIZE || pa == QTBL_ADDR || pa == QTBL_INFO || pa == SFR_ADDR)	/* init */
		return 1;
	if (pa == CS_VMIMSG || pa == CS_VMIMSGAG0 || pa == CS_VMIMSGAG1 || pa == CTRL_INIT)	/* init */
		return 1;
	if (pa == IC_SOFTINT)	/* ping: doorbell */
		return 1;
	if ((pa >= PLL0 && pa <= PLL_FRAC) || pa == IRIS_RCG || pa == IRIS_CFG)
		return 1;
	if (pa == VIDEO_CC_XO)	/* redfin: power on enables it if off (Linux keeps it on, CLK_IS_CRITICAL) */
		return 1;
	if (pa == GDSC_MVSC || pa == GDSC_MVS0 || pa == GDSC_MVS1)
		return 1;
	for (unsigned i = 0; i < NBR; i++)
		if (pa == branches[i].pa)
			return 1;
	return 0;
}

static void wr(uint32_t pa, uint32_t v, const char *why)
{
	if (!allowed(pa)) {
		printf("venus: BLOCKED wr %#x = %#x (%s)\n", pa, v, why);
		vc_stop(why);
	}
	vcc_gate(pa, why);
	core_gate(pa, why);
	uint32_t old = rd32(pa);
	wr32(pa, v);
	if (verbose)
		printf("venus: wr %#x %#010x -> %#010x, reads %#010x  [%s]\n", pa, old, v, rd32(pa), why);
}

static void upd(uint32_t pa, uint32_t mask, uint32_t val, const char *why)
{
	wr(pa, (vrd(pa) & ~mask) | (val & mask), why);
}

static unsigned polled;	/* lat2: real ms the last poll()/gcc_on() waited (for the timeout lines) */

/* (reg & mask) == want within ms (1 ms steps, lat2: a CLOCK_MONOTONIC deadline, pmem.h wait_over); 0 = ok */
static int poll(uint32_t pa, uint32_t mask, uint32_t want, unsigned ms)
{
	double t0 = mono_ms();
	for (unsigned i = 0;; i++) {
		if ((vrd(pa) & mask) == want)
			return 0;
		if (wait_over(t0, i, ms)) {
			polled = wait_ms(t0, i);
			return -1;
		}
		delay_ms(1);
	}
}

static const char *onoff(uint32_t cbcr)
{
	return (cbcr & CLK_OFF) ? "off" : "on";
}

/* GCC CBCRs: CLK_OFF with HW_CTL set means hardware-gated (idle), not off */
static const char *gcc_state(uint32_t cbcr)
{
	return (cbcr & CLK_OFF) && (cbcr & GCC_HW_CTL) ? "hw-gated (HW_CTL)" : onoff(cbcr);
}

/* ---- RPMh BCMs (cmd-db as adspboot.c cmddb_aux; command = commit | valid | x << 14 | y, sweet_gpu.c gpu_bus_vote) ---- */

#define CMDDB_LITO	0x80860000ull	/* lito.dtsi qcom,cmd-db: a fixed reserved-memory region (xnu 0901, 0904) */

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = CMDDB_LITO;	/* redfin: no AOP locator at 0x0c3f000c; never read through a random address */
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

/* Minimal nonzero vote x = y = 1 (BCM units): the notes give KBps only (vidc 1000 KBps), not the BCM unit. */
#define BCM_ON		(1u << 30 | 1u << 29 | 1u << 14 | 1u)
#define BCM_OFF		(1u << 30)	/* commit, not valid: x = y = 0 (Linux tcs_cmd_gen) */
static const char *const bcms[] = { "MM3", "MM2" };	/* vote order (notes table "RPMh BCMs") */

/* vdbcm (the 15-min Safari soak, video.md 2026-11-03: "bcm MM3 vote failed" -> 16, venusd power on failed): the
 * kernel's PMEM_RPMH answers EBUSY while another /dev/pmem user's vote holds its lock or an ACTIVE TCS (up to its
 * 100 ms timeout; bsd/dev/sweet_rpmh.c): retried, 5 ms doubling, VENUS_BCM_TRIES (6, ~155 ms in all; 1: one try as
 * before). EIO is retried once only: it is what a quarantined driver answers (an earlier vote of this boot timed
 * out, kernel log "SWEET RPMH: timeout": no vote works until a reboot). ETIMEDOUT: never (the vote may be half done,
 * the kernel quarantines its TCS). Only the MM2/MM3 BCMs pass here (no MX/CX). */
static int bcm(const char *res, uint32_t v)
{
	static int tries = -1;
	if (tries < 0)
		tries = getenv("VENUS_BCM_TRIES") && atoi(getenv("VENUS_BCM_TRIES")) > 0 ? atoi(getenv("VENUS_BCM_TRIES")) : 6;
	uint32_t off[1] = { 0 }, val[1] = { v };
	int rc = pm_rpmh(res, 1, off, val);
	printf("venus: bcm %s = %#010x -> %d\n", res, v, rc);
	for (int i = 1, wait = 5, eio = 0; rc && i < tries && (rc == EBUSY || (rc == EIO && !eio++)); i++, wait *= 2) {
		delay_ms((unsigned)wait);
		rc = pm_rpmh(res, 1, off, val);
		printf("venus: bcm %s = %#010x -> %d (try %d of %d, after %d ms)\n", res, v, rc, i + 1, tries, wait);
	}
	if (rc == EIO)
		printf("venus: bcm %s: EIO: the kernel's RPMh driver refuses votes (quarantined after a timeout this boot?"
		       " kernel log \"SWEET RPMH: timeout\"): a reboot clears it\n", res);
	return rc;
}

/* ---- probe (read-only) ---- */

static int sid_match(uint32_t smr, uint32_t sid)
{
	uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
	return ((id ^ sid) & ~m & 0x7fff) == 0;
}

static void probe_smmu(void)
{
	uint64_t s = 0x15000000ull;	/* apps SMMU (smmu.c), register reads only */
	uint32_t id0 = rd32(s + 0x20), nsmr = id0 & 0xff, valid = 0, hits = 0;
	printf("smmu: sCR0 %08x (USFCFG %u: a stream with no SMR is %s) ID0 %08x: %u SMRs\n", rd32(s), (rd32(s) >> 10) & 1,
	       (rd32(s) >> 10) & 1 ? "FAULTED (global fault)" : "bypassed", id0, nsmr);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i);
		if (!(smr >> 31))
			continue;
		valid++;
		uint32_t first = 0;
		for (uint32_t sid = SID_LO; sid <= SID_HI; sid++)
			if (sid_match(smr, sid)) {
				first = sid;
				break;
			}
		if (!first)
			continue;
		uint32_t s2cr = rd32(s + 0xc00 + 4 * i);
		printf("smmu: SMR%u %08x (id %#x mask %#x, first venus SID %#x) S2CR %08x type %u cb %u\n", i, smr,
		       smr & 0x7fff, (smr >> 16) & 0x7fff, first, s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		hits++;
	}
	printf("smmu: %u valid SMRs, %u match SIDs %#x-%#x\n", valid, hits, SID_LO, SID_HI);
}

static void probe_bcm(void)
{
	for (unsigned i = 0; i < 2; i++) {
		uint64_t aux;
		uint32_t len;
		if (cmddb_aux(bcms[i], &aux, &len)) {
			printf("cmd-db: %s not found\n", bcms[i]);
			continue;
		}
		printf("cmd-db: %s aux %u bytes:", bcms[i], len);
		for (uint32_t o = 0; o < len && o < 16; o += 4)
			printf(" %08x", rd32(aux + o));
		printf("\n");
	}
}

static void probe_vcc(void)
{
	uint32_t ef = rd32(EFUSE_VIDEO);
	printf("efuse %#x = %#010x: multipipe %s, VP fmax %s\n", EFUSE_VIDEO, ef,
	       (ef & EFUSE_NO_MPIPE) ? "disabled (power on uses L 0x14 = 192 MHz)" : "ok",
	       (ef & EFUSE_NO_FMAX) ? "disabled (365 MHz cap; we run 240)" : "ok");
	static const struct { const char *n; uint32_t off; } r[] = {
		{ "MODE", 0x00 }, { "L", 0x04 }, { "CAL_L", 0x08 }, { "USER_CTL", 0x0c }, { "USER_CTL_U", 0x10 },
		{ "USER_CTL_U1", 0x14 }, { "CONFIG_CTL", 0x18 }, { "CONFIG_CTL_U", 0x1c }, { "CONFIG_CTL_U1", 0x20 },
		{ "TEST_CTL", 0x24 }, { "TEST_CTL_U", 0x28 }, { "TEST_CTL_U1", 0x2c }, { "STATUS", 0x30 },
		{ "OPMODE", 0x38 }, { "ALPHA", 0x40 },
	};
	for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
		printf("pll0 %-12s %#x = %#010x\n", r[i].n, PLL0 + r[i].off, vrd(PLL0 + r[i].off));
	uint32_t m = vrd(PLL_MODE);
	printf("pll0: lock %u outctrl %u reset_n %u fsm %u opmode %u, VCO %.1f MHz\n", m >> 31, m & 1, (m >> 2) & 1,
	       (m >> 20) & 1, vrd(PLL_OPMODE), 19.2 * ((vrd(PLL_L) & 0xffff) + (vrd(PLL_FRAC) & 0xffff) / 65536.0));
	uint32_t cmd = vrd(IRIS_RCG), cfg = vrd(IRIS_CFG);
	printf("iris rcg %#x CMD %#010x (root %s) CFG %#010x (src %u, div field %u)\n", IRIS_RCG, cmd,
	       (cmd >> 31) ? "off" : "on", cfg, (cfg >> 8) & 7, cfg & 0x1f);
	static const struct { const char *n; uint32_t pa; } g[] = {
		{ "mvsc", GDSC_MVSC }, { "mvs0", GDSC_MVS0 }, { "mvs1", GDSC_MVS1 } };
	for (unsigned i = 0; i < 3; i++) {
		uint32_t v = vrd(g[i].pa);
		printf("gdsc %-4s %#x = %#010x: %s, sw_collapse %u, hw_control %u\n", g[i].n, g[i].pa, v,
		       (v & PWR_ON) ? "PWR_ON" : "off", v & 1, (v >> 1) & 1);
	}
	for (unsigned i = 0; i < NBR; i++) {
		uint32_t v = vrd(branches[i].pa);
		printf("cbcr %-12s %#x = %#010x: %s, en %u\n", branches[i].name, branches[i].pa, v, onoff(v), v & 1);
	}
	uint32_t xo = vrd(VIDEO_CC_XO);
	printf("cbcr %-12s %#x = %#010x: %s\n", "xo", VIDEO_CC_XO, xo, onoff(xo));
}

int probe(void)
{
	static const struct { const char *n; uint32_t pa; } g[] = {
		{ "gcc_video_ahb", GCC_VIDEO_AHB }, { "gcc_video_axi", GCC_VIDEO_AXI }, { "gcc_video_xo", GCC_VIDEO_XO } };
	for (unsigned i = 0; i < 3; i++) {
		uint32_t v = rd32(g[i].pa);
		printf("%-14s %#x = %#010x: %s, en %u\n", g[i].n, g[i].pa, v, gcc_state(v), v & 1);
	}
	uint32_t vote = rd32(GCC_TBU_SF_VOTE);
	printf("tbu sf vote %#x = %#010x (bit0 sw_collapse %u, PWR_ON %u); status %#x = %#010x %#010x\n",
	       GCC_TBU_SF_VOTE, vote, vote & 1, vote >> 31, TBU_SF_STATUS, rd32(TBU_SF_STATUS), rd32(TBU_SF_STATUS + 4));
	probe_smmu();
	probe_bcm();
	if (!ahb_on()) {
		printf("videocc skipped: AHB %s\n", gcc_state(rd32(GCC_VIDEO_AHB)));
		return 0;
	}
	probe_vcc();
	return 0;
}

/* ---- power ---- */

static int branch_on(uint32_t pa, const char *name)
{
	if (!(vrd(pa) & CBCR_EN))
		upd(pa, CBCR_EN, CBCR_EN, name);
	if (poll(pa, CLK_OFF, 0, 100)) {
		printf("venus: STOP: %s %#x still CLK_OFF after %u ms (%#010x)\n", name, pa, polled, vrd(pa));
		return -1;
	}
	printf("venus: %s on (%#010x)\n", name, vrd(pa));
	return 0;
}

static void branch_off(uint32_t pa, const char *name, int voted)
{
	if (!(vrd(pa) & CBCR_EN)) {
		printf("venus: %s already disabled (%#010x)\n", name, vrd(pa));
		return;
	}
	upd(pa, CBCR_EN, 0, name);
	if (voted)
		usleep(10);	/* clk-branch.c: voted branches only delay on disable */
	else if (poll(pa, CLK_OFF, CLK_OFF, 100))
		printf("venus: %s %#x CLK_OFF not set after %u ms (%#010x), going on\n", name, pa, polled, vrd(pa));
	printf("venus: %s off (%#010x)\n", name, vrd(pa));
}

static int gdsc_on(uint32_t pa, const char *name)
{
	uint32_t v = vrd(pa);
	if (v & HW_CONTROL) {	/* review 8.5.5: in HW mode the domain can collapse under us; downstream SW mode first */
		upd(pa, HW_CONTROL, 0, "gdsc SW mode");
		uint32_t n = vrd(pa);
		printf("venus: gdsc %s HW_CONTROL was set (%#010x): cleared (%#010x)\n", name, v, n);
		if (n & HW_CONTROL) {
			printf("venus: STOP: gdsc %s HW_CONTROL won't clear (%#010x)\n", name, n);
			return -1;
		}
		v = n;
	}
	if (!(v & PWR_ON) || (v & SW_COLLAPSE))
		upd(pa, SW_COLLAPSE, 0, name);
	if (poll(pa, PWR_ON, PWR_ON, 100)) {
		printf("venus: STOP: gdsc %s %#x no PWR_ON after %u ms (%#010x)\n", name, pa, polled, vrd(pa));
		return -1;
	}
	printf("venus: gdsc %s on (%#010x)\n", name, vrd(pa));
	return 0;
}

static void gdsc_off(uint32_t pa, const char *name)
{
	if (vrd(pa) & SW_COLLAPSE && !(vrd(pa) & PWR_ON)) {
		printf("venus: gdsc %s already off (%#010x)\n", name, vrd(pa));
		return;
	}
	upd(pa, SW_COLLAPSE, SW_COLLAPSE, name);
	if (poll(pa, PWR_ON, 0, 100))
		printf("venus: gdsc %s %#x still PWR_ON after %u ms (%#010x), going on\n", name, pa, polled, vrd(pa));
	else
		printf("venus: gdsc %s off (%#010x)\n", name, vrd(pa));
}

/* clk_lucid_pll_configure + alpha_pll_lucid_enable (clk-alpha-pll.c) with videocc-lito.c video_pll0_config; the same
 * steps as xnu 0921's camcc PLL0 pll0_up() on this phone */
static int pll_on(uint32_t l)
{
	uint32_t m = vrd(PLL_MODE), op = vrd(PLL_OPMODE);
	if (m & VOTE_FSM_ENA) {
		printf("venus: STOP: pll0 MODE %#x in FSM vote mode\n", m);
		return -1;
	}
	if ((op & PLL_RUN) && (m & OUTCTRL)) {
		if ((vrd(PLL_L) & 0xffff) == l) {
			printf("venus: pll0 already running, L %#x (MODE %#x)\n", l, m);
			return 0;
		}
		printf("venus: STOP: pll0 running at L %#x, not %#x (MODE %#x)\n", vrd(PLL_L), l, m);
		return -1;
	}
	/* reset state 0 or our own 'off' state: redfin rn4x, a second power on after power off read MODE 0x08800004
	 * (PCAL_DONE | UPDATE_BYPASS | RESET_N) with OPMODE STANDBY, i.e. Lucid's disabled state */
	if ((m & ~(RESET_N | UPDATE_BYPASS | LOCK_DET | PCAL_DONE)) || (vrd(PLL_OPMODE) & PLL_RUN)) {
		printf("venus: STOP: pll0 MODE %#x not reset/standby\n", m);
		return -1;
	}
	/* configure (clk_lucid_pll_configure): zero fields (alpha, user_u1, test_ctl, test_u) skipped as there */
	wr(PLL_L, l, "pll0 L");
	wr(PLL_CAL_L, 0x44, "pll0 CAL_L");	/* LUCID_PLL_CAL_VAL */
	wr(PLL_CONFIG, 0x20485699, "pll0 CONFIG_CTL");
	wr(PLL_CONFIG_U, 0x00002261, "pll0 CONFIG_CTL_U");
	wr(PLL_CONFIG_U1, 0x329a699c, "pll0 CONFIG_CTL_U1");
	wr(PLL_USER, 0x00000001, "pll0 USER_CTL");
	wr(PLL_USER_U, 0x00000805, "pll0 USER_CTL_U");
	wr(PLL_TEST_U1, 0x01800000, "pll0 TEST_CTL_U1");
	upd(PLL_MODE, UPDATE_BYPASS, UPDATE_BYPASS, "pll0 UPDATE_BYPASS");
	upd(PLL_MODE, OUTCTRL, 0, "pll0 OUTCTRL off");
	wr(PLL_OPMODE, 0, "pll0 OPMODE STANDBY");
	upd(PLL_MODE, RESET_N, RESET_N, "pll0 RESET_N");
	/* enable (alpha_pll_lucid_enable) */
	wr(PLL_OPMODE, PLL_RUN, "pll0 OPMODE RUN");
	if (poll(PLL_MODE, LOCK_DET, LOCK_DET, 100)) {
		printf("venus: STOP: pll0 no lock after %u ms (MODE %#x), back to STANDBY\n", polled, vrd(PLL_MODE));
		wr(PLL_OPMODE, 0, "pll0 OPMODE STANDBY");
		return -1;
	}
	upd(PLL_USER, PLL_OUT_MASK, PLL_OUT_MASK, "pll0 outputs");
	upd(PLL_MODE, OUTCTRL, OUTCTRL, "pll0 OUTCTRL");
	printf("venus: pll0 locked at %.1f MHz (MODE %#010x)\n", 19.2 * l, vrd(PLL_MODE));
	return 0;
}

static void pll_off(void)	/* alpha_pll_lucid_disable (outputs off, STANDBY; as 0921's pll0_down) */
{
	uint32_t m = vrd(PLL_MODE);
	if (m & VOTE_FSM_ENA) {
		printf("venus: pll0 in FSM vote mode, left alone\n");
		return;
	}
	if (!(m & OUTCTRL) && !(vrd(PLL_OPMODE) & PLL_RUN)) {
		printf("venus: pll0 already off (MODE %#x)\n", m);
		return;
	}
	upd(PLL_MODE, OUTCTRL, 0, "pll0 OUTCTRL off");
	upd(PLL_USER, PLL_OUT_MASK, 0, "pll0 outputs off");
	wr(PLL_OPMODE, 0, "pll0 OPMODE STANDBY");
	printf("venus: pll0 off (MODE %#010x)\n", vrd(PLL_MODE));
}

/* clk_rcg2_configure (camclk.c mclk_24): src SEL, div field 2h-1, mode 0 (no MND), then CMD UPDATE */
static int rcg_set(uint32_t src, uint32_t divf, const char *why)
{
	uint32_t cfg = vrd(IRIS_CFG);
	cfg &= ~((0x7u << 8) | 0x1fu | (3u << 12) | (1u << 20));
	cfg |= src << 8 | divf;
	wr(IRIS_CFG, cfg, why);
	upd(IRIS_RCG, 1, 1, "iris rcg UPDATE");
	if (poll(IRIS_RCG, 1, 0, 100)) {
		printf("venus: STOP: iris rcg didn't update after %u ms (CMD %#x, CFG %#x)\n", polled, vrd(IRIS_RCG), vrd(IRIS_CFG));
		return -1;
	}
	printf("venus: iris rcg src %u div field %u (CMD %#010x CFG %#010x)\n", src, divf, vrd(IRIS_RCG), vrd(IRIS_CFG));
	return 0;
}

static int gcc_on(uint32_t pa, const char *name, int must)
{
	if (!(rd32(pa) & CBCR_EN))
		upd(pa, CBCR_EN, CBCR_EN, name);
	double t0 = mono_ms();	/* lat2: 100 ms on the clock, not 101 loop passes */
	unsigned i;
	for (i = 0;; i++) {
		if (!(rd32(pa) & CLK_OFF)) {
			printf("venus: %s on (%#010x)\n", name, rd32(pa));
			return 0;
		}
		if (wait_over(t0, i, 100)) {
			i = wait_ms(t0, i);
			break;
		}
		delay_ms(1);
	}
	printf("venus: %s %#x still CLK_OFF after %u ms (%#010x)%s\n", name, pa, i, rd32(pa),
	       must ? "" : ", checked again after the GDSCs");
	return -1;
}

/* gcc_video_ahb: clear HW_CTL (keep CLK_ENABLE) so CLK_OFF reflects the clock; the old value goes to AHB_STATE.
 * gcc_video_xo (0x10b028, en 0) stays untouched: no video node in sdmmagpie(-vidc).dtsi uses GCC_VIDEO_XO_CLK
 * (videocc runs from RPMh bi_tcxo, videocc-sdmmagpie.c:60,100; the venus PIL node takes VIDEO_CC_XO_CLK, dtsi:3408) */
static void ahb_hwctl_clear(void)
{
	uint32_t v = rd32(GCC_VIDEO_AHB);
	if (!(v & GCC_HW_CTL))
		return;
	if (getenv("VENUSCTL_KEEP_HWCTL")) {
		printf("venus: gcc_video_ahb HW_CTL set (%#010x), kept (VENUSCTL_KEEP_HWCTL)\n", v);
		return;
	}
	FILE *f = fopen(AHB_STATE, "w");
	if (f) {
		fprintf(f, "%#x\nboot %s\n", v, boot_id());
		fclose(f);
	} else {
		printf("venus: can't write %s, power off won't restore HW_CTL\n", AHB_STATE);
	}
	upd(GCC_VIDEO_AHB, GCC_HW_CTL | CBCR_EN, CBCR_EN, "gcc_video_ahb clear HW_CTL");
	printf("venus: gcc_video_ahb HW_CTL cleared (%#010x -> %#010x)\n", v, rd32(GCC_VIDEO_AHB));
}

/* power off: put HW_CTL back if power on cleared it */
static void ahb_hwctl_restore(void)
{
	unsigned v = 0;
	FILE *f = fopen(AHB_STATE, "r");
	if (!f)
		return;
	int n = fscanf(f, "%x", &v);
	fclose(f);
	unlink(AHB_STATE);
	if (n == 1 && (v & GCC_HW_CTL)) {
		upd(GCC_VIDEO_AHB, GCC_HW_CTL, GCC_HW_CTL, "gcc_video_ahb restore HW_CTL");
		printf("venus: gcc_video_ahb HW_CTL restored (%#010x)\n", rd32(GCC_VIDEO_AHB));
	}
}

int power_on(void)
{
	for (unsigned i = 0; i < 2; i++)
		if (bcm(bcms[i], BCM_ON)) {
			printf("venus: STOP: bcm %s vote failed\n", bcms[i]);
			return power_off(), 1;
		}
	ahb_hwctl_clear();
	if (gcc_on(GCC_VIDEO_AHB, "gcc_video_ahb", 1))	/* BRANCH_HALT-style poll, as gcc_video_axi */
		return power_off(), 1;
	int axi_late = gcc_on(GCC_VIDEO_AXI, "gcc_video_axi", 0);	/* may need the video GDSCs to report on */

	/* videocc-lito.c video_multipipe_fixup writes L 0x14 into the config for either efuse bit, but the RCG then sets
	 * the PLL's rate itself (CLK_SET_RATE_PARENT): with VP fmax off the table still starts at 240 MHz = PLL 480 / 2,
	 * only multipipe-disabled parts are capped at 200 MHz. So: L 0x19 (480 -> 240 MHz) unless multipipe is disabled,
	 * then L 0x14 (384 -> 192 MHz, under the 200 cap). rn4x (L 0x14 for either bit) locked at 384 MHz. */
	uint32_t l = (rd32(EFUSE_VIDEO) & EFUSE_NO_MPIPE) ? 0x14 : 0x19;
	if (getenv("VENUSCTL_PLL_L"))
		l = (uint32_t)strtoul(getenv("VENUSCTL_PLL_L"), NULL, 0) == 0x14 ? 0x14 : 0x19;	/* 0x14 or 0x19 only */
	if (l != 0x19)
		printf("venus: efuse %#010x: multipipe or VP fmax disabled, PLL L %#x\n", rd32(EFUSE_VIDEO), l);
	if (pll_on(l) || rcg_set(1, 3, "iris rcg PLL0 main /2"))	/* F(240 MHz, PLL0_OUT_MAIN, 2): field 2*2-1 */
		return power_off(), 1;
	printf("venus: pll0 L %#x wanted, reads L %#x MODE %#010x OPMODE %#x: %.1f MHz, iris %.1f MHz\n", l,
	       vrd(PLL_L) & 0xffff, vrd(PLL_MODE), vrd(PLL_OPMODE), 19.2 * (vrd(PLL_L) & 0xffff),
	       19.2 * (vrd(PLL_L) & 0xffff) / 2);
	/* redfin: video_cc_xo_clk is CLK_IS_CRITICAL in videocc-lito.c (always on under Linux) and the venus PIL node's
	 * proxy clock "xo": the firmware's timers run from it. Nothing in XNU turns it on; enable it if it reads off
	 * (left on afterwards, as Linux does). VENUSCTL_XO=0: only report it. */
	{
		uint32_t xo = vrd(VIDEO_CC_XO);
		printf("venus: video_cc_xo %#x = %#010x (%s)\n", VIDEO_CC_XO, xo, onoff(xo));
		if ((xo & CLK_OFF) && (!getenv("VENUSCTL_XO") || atoi(getenv("VENUSCTL_XO"))) &&
		    branch_on(VIDEO_CC_XO, "video_cc_xo"))
			return power_off(), 1;
	}
	if (gdsc_on(GDSC_MVSC, "mvsc") || gdsc_on(GDSC_MVS0, "mvs0") || (cvp && gdsc_on(GDSC_MVS1, "mvs1")))
		return power_off(), 1;
	if (axi_late && gcc_on(GCC_VIDEO_AXI, "gcc_video_axi", 1))
		return power_off(), 1;
	for (unsigned i = 0; i < NBR; i++)
		if ((!branches[i].cvp || cvp) && branch_on(branches[i].pa, branches[i].name))
			return power_off(), 1;
	/* the core read: only with everything above on, checked once more */
	if (!core_ok()) {
		printf("venus: STOP: a clock or GDSC dropped before the HW version read\n");
		return power_off(), 1;
	}
	printf("venus: WRAPPER_HW_VERSION %#x = %#010x\n", WRAP_HW_VERSION, rd32(WRAP_HW_VERSION));
	return 0;
}

/* STATE's pas line: 1 = TZ holds an image (pas-check --no-shutdown, boot --keep), the firmware may run */
static int state_pas(void)
{
	int p = 0;
	char line[256];
	FILE *f = fopen(STATE, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "pas %d", &p) == 1)
			break;
	fclose(f);
	return p;
}

int power_off(void)
{
	/* review 8.5.2: collapsing GDSCs/clocks/BCMs under a live ARM9 hangs the bus */
	if (state_pas()) {
		if (!force) {
			printf("venus: refused: %s says pas 1 (TZ image loaded, firmware may run): venusctl shutdown first,"
			       " or power off --force\n", STATE);
			return 1;
		}
		printf("venus: --force: powering off with %s pas 1\n", STATE);
	}
	if (ahb_on()) {
		for (unsigned i = NBR; i-- > 0;)
			if (!branches[i].cvp || cvp || (vrd(branches[i].pa) & CBCR_EN))
				branch_off(branches[i].pa, branches[i].name, branches[i].voted);
		if (cvp || (vrd(GDSC_MVS1) & PWR_ON))
			gdsc_off(GDSC_MVS1, "mvs1");
		gdsc_off(GDSC_MVS0, "mvs0");
		gdsc_off(GDSC_MVSC, "mvsc");
		if (!(vrd(PLL_MODE) & VOTE_FSM_ENA) && ((vrd(IRIS_CFG) >> 8) & 7) == 1)
			rcg_set(0, 1, "iris rcg park on XO");	/* downstream enable_safe_config: off the PLL before it stops */
		pll_off();
	} else {
		printf("venus: videocc skipped: AHB off\n");
	}
	if (rd32(GCC_VIDEO_AXI) & CBCR_EN) {
		upd(GCC_VIDEO_AXI, CBCR_EN, 0, "gcc_video_axi");
		printf("venus: gcc_video_axi off (%#010x)\n", rd32(GCC_VIDEO_AXI));
	}
	/* gcc_video_ahb stays enabled: critical in mainline gcc-sm7150, and the GDSC parent; only HW_CTL goes back */
	ahb_hwctl_restore();
	int rc = 0;
	for (unsigned i = 2; i-- > 0;)
		rc |= bcm(bcms[i], BCM_OFF);
	return rc ? 1 : 0;
}

/* ---- PAS check (adspboot.c boot(): metadata = ELF header + phdrs + hash segment) ---- */

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = calloc(1, *len + 4);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

/* venus.mdt and its metadata (ELF header + phdrs + hash segment), shared by pas-check and boot */
struct fwimg {
	uint8_t *mdt, *meta;
	size_t mlen, meta_len;
	struct phdr *ph;
	uint32_t phnum;
	int hash;
};

static int fw_meta(const char *fw, struct fwimg *f)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/venus.mdt", fw);
	uint8_t *mdt = slurp(path, &f->mlen);
	if (!mdt) {
		fprintf(stderr, "venus: no %s\n", path);
		return 1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	if (memcmp(mdt, "\177ELF", 4) || phoff + 32ull * phnum > f->mlen) {
		fprintf(stderr, "venus: %s is not an ELF32 image\n", path);
		return 1;
	}
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2) {
			hash = (int)i;
			break;
		}
	if (hash < 0) {
		fprintf(stderr, "venus: no hash segment in %s\n", path);
		return 1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = calloc(1, meta_len + 4);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= f->mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		size_t hl;
		snprintf(path, sizeof(path), "%s/venus.b%02d", fw, hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "venus: hash segment %s missing\n", path);
			return 1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	printf("venus: %s/venus.mdt: %u phdrs, hash seg %d (%u bytes), metadata %zu bytes\n", fw, phnum, hash,
	       ph[hash].filesz, meta_len);
	*f = (struct fwimg){ .mdt = mdt, .meta = meta, .mlen = f->mlen, .meta_len = meta_len, .ph = ph,
		.phnum = phnum, .hash = hash };
	return 0;
}

int pas_check(const char *fw, int no_shutdown)
{
	if (!ahb_on() || !(vrd(GDSC_MVSC) & PWR_ON) || !(vrd(GDSC_MVS0) & PWR_ON)) {
		printf("venus: refused: power is off (gcc_video_ahb %#010x%s): run 'venusctl power on' first\n",
		       rd32(GCC_VIDEO_AHB), ahb_on() ? ", or gdsc mvsc/mvs0 off" : "");
		return 1;
	}
	struct fwimg f;
	if (fw_meta(fw, &f))
		return 1;
	size_t meta_len = f.meta_len;
	uint8_t *meta = f.meta;

	uint64_t r1 = 0;
	int64_t rc = scm(7, 1, PAS_VENUS, 0, 0, &r1);
	printf("venus: pas supported(%d) -> %lld/%llx\n", PAS_VENUS, (long long)rc, (unsigned long long)r1);

	uint64_t meta_pa = pm_alloc(meta_len, 0x1000);
	if (!meta_pa)
		return 1;
	pm_wr(meta_pa, meta, (meta_len + 3) & ~(size_t)3);
	printf("venus: pas init_image(%d, %llx)...\n", PAS_VENUS, (unsigned long long)meta_pa);
	rc = scm(1, 2 | (2u << 6), PAS_VENUS, meta_pa, 0, &r1);
	printf("venus: pas init_image -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	pm_free(meta_pa);
	int init_ok = !rc && !r1;
	if (no_shutdown && init_ok) {	/* review 8.5.8: record it, so shutdown can undo it */
		FILE *sf = fopen(STATE, "w");
		if (sf) {
			fprintf(sf, "boot %s\npas 1\nq 0 0\ninit 0\nsysinit 0\n", boot_id());
			fclose(sf);
			printf("venus: %s: pas 1 (venusctl shutdown undoes it)\n", STATE);
		} else {
			printf("venus: can't write %s: a PAS shutdown(9) is owed, nothing records it\n", STATE);
		}
	}
	/* adspboot adsp3: a shutdown with nothing in TZ blocked forever, so only after an accepted init */
	if (no_shutdown || !init_ok) {
		printf("venus: pas shutdown skipped (%s)\n", no_shutdown ? "--no-shutdown" : "init failed");
		return init_ok ? 0 : 1;
	}
	printf("venus: pas shutdown(%d)...\n", PAS_VENUS);
	rc = scm(6, 1, PAS_VENUS, 0, 0, &r1);
	printf("venus: pas shutdown -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	return rc || r1 ? 1 : 0;
}

/* ---- stage 2 steps 4-6: state shared by boot --keep, init, ping and shutdown (separate runs) ---- */

struct vstate {
	int pas;		/* init_image accepted: PAS shutdown(9) owed */
	int nsmr;
	struct smmu_saved smr[8];	/* what smmu_identity wrote, for smmu_release (redfin: 5 SIDs) */
	uint32_t sid[8];		/* the SID each smr[] entry was written for */
	uint64_t q;		/* HFI block (QSHARED) */
	int qkind;		/* 1 PMEM_ALLOC32 (pm_free), 2 --phys region */
	int init, sysinit;
};

static void state_save(const struct vstate *v)
{
	FILE *f = fopen(STATE ".new", "w");
	if (!f) {
		printf("venus: can't write %s: shutdown won't know what to undo\n", STATE);
		return;
	}
	fprintf(f, "boot %s\npas %d\nq %#llx %d\ninit %d\nsysinit %d\n", boot_id(), v->pas, (unsigned long long)v->q, v->qkind, v->init,
		v->sysinit);
	for (int i = 0; i < v->nsmr; i++) {
		const struct smmu_saved *m = &v->smr[i];
		fprintf(f, "smr %d %d %#llx %#llx %#x %#x %#x %#x %#x %#x\n", m->slot, m->cb, (unsigned long long)m->cbp,
			(unsigned long long)m->gr1, m->smr, m->s2cr, m->cbar, m->cba2r, m->sctlr, v->sid[i]);
	}
	fclose(f);
	rename(STATE ".new", STATE);
}

static int state_load(struct vstate *v)
{
	memset(v, 0, sizeof(*v));
	FILE *f = fopen(STATE, "r");
	if (!f)
		return -1;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		unsigned long long a, b;
		struct smmu_saved m;
		if (sscanf(line, "pas %d", &v->pas) == 1 || sscanf(line, "init %d", &v->init) == 1 ||
		    sscanf(line, "sysinit %d", &v->sysinit) == 1)
			continue;
		if (sscanf(line, "q %llx %d", &a, &v->qkind) == 2) {
			v->q = a;
			continue;
		}
		unsigned sid;
		if (v->nsmr < 8 && !strncmp(line, "smr ", 4) && sscanf(line, "smr %d %d %llx %llx %x %x %x %x %x %x", &m.slot, &m.cb, &a, &b, &m.smr,
					  &m.s2cr, &m.cbar, &m.cba2r, &m.sctlr, &sid) == 10) {
			m.cbp = a;
			win_cbp = a;
			m.gr1 = b;
			v->sid[v->nsmr] = sid;
			v->smr[v->nsmr++] = m;
		}
	}
	fclose(f);
	return 0;
}

static int64_t pas(uint32_t cmd, const char *what)
{
	uint64_t r1 = 0;
	printf("venus: pas %s(%d)...\n", what, PAS_VENUS);
	int64_t rc = scm(cmd, 1, PAS_VENUS, 0, 0, &r1);
	printf("venus: pas %s -> %lld/%llx\n", what, (long long)rc, (unsigned long long)r1);
	return rc ? rc : (int64_t)r1;
}

static void print_sfr(uint64_t q)
{
	char s[SFR_SIZE + 1];
	uint32_t sz = rd32(q + SFR_OFF);
	memset(s, 0, sizeof(s));
	pm_rd(q + SFR_OFF + 4, s, SFR_SIZE - 4);
	for (unsigned i = 0; i < SFR_SIZE - 4 && s[i]; i++)
		if (s[i] < 0x20 || s[i] > 0x7e)
			s[i] = '.';
	printf("venus: SFR %#llx bufSize %u: \"%s\"\n", (unsigned long long)(q + SFR_OFF), sz, s);
}

/* the undo for boot/init, in reverse: PAS shutdown (only powered: TZ touches the core), queue memory, SMRs */
static int teardown(struct vstate *v)
{
	if (v->pas) {
		if (!core_ok()) {
			printf("venus: STOP: PAS shutdown needs the power on (a clock or GDSC is off): nothing undone,"
			       " state kept in %s\n", STATE);
			return 1;
		}
		if (pas(6, "shutdown")) {
			printf("venus: STOP: PAS shutdown failed: queue memory and SMRs kept (the firmware may still run),"
			       " state kept in %s\n", STATE);
			return 1;
		}
		v->pas = 0;
		v->init = v->sysinit = 0;
		state_save(v);
	}
	if (v->q) {
		if (v->qkind == 1 && pm_free(v->q))
			printf("venus: pm_free %#llx failed\n", (unsigned long long)v->q);
		else
			printf("venus: queue block %#llx %s\n", (unsigned long long)v->q, v->qkind == 1 ? "freed" :
			       "(--phys region) given up");
		v->q = 0;
		state_save(v);
	}
	int rc = 0;
	while (v->nsmr > 0) {
		/* review 8.5.1: only a slot that still holds exactly what we wrote (WLAN/ADSP may own it now) */
		const struct smmu_saved *m = &v->smr[--v->nsmr];
		uint32_t sid = v->sid[v->nsmr], smr = rd32(0x15000800ull + 4 * m->slot), s2cr = rd32(0x15000c00ull + 4 * m->slot);
		if (smr != ((1u << 31) | sid) || ((s2cr >> 16) & 3) || (s2cr & 0xff) != (uint32_t)m->cb) {
			printf("venus: SMR%d left alone: reads SMR %08x S2CR %08x, we wrote SMR %08x S2CR -> CB%d (not ours now)\n",
			       m->slot, smr, s2cr, (1u << 31) | sid, m->cb);
		} else {
			smmu_last = *m;
			int r = smmu_release("venus");
			/* v2c: SMR invalid (r 1) = released, the other differences only warned (after PAS shutdown
			 * the bank registers read changed every run); a still-valid SMR stays a failure.
			 * VENUSCTL_STRICT_RELEASE=1: any difference fails (old behaviour). */
			if (r > 0 && !getenv("VENUSCTL_STRICT_RELEASE")) {
				printf("venus: warning: SMR%d released (reads invalid), other registers differ (above)\n",
				       m->slot);
				r = 0;
			}
			rc |= r != 0;
		}
		state_save(v);
	}
	unlink(STATE);
	return rc ? 1 : 0;
}

/* review 8.5.4: SIDs 0x1000-0x13ff go through mnoc_sf_0_tbu, powered only by someone else's vote (ours = collapse).
 * Re-read right before the firmware starts DMA; no vote of our own. The status words are printed only (their bits
 * are not known). VENUSCTL_NO_TBUCHECK=1: skip the check. 0 = on. */
static int tbu_ok(const char *when)
{
	uint32_t vote = rd32(GCC_TBU_SF_VOTE);
	if (vote & PWR_ON)
		return 0;
	printf("venus: %s mnoc_sf_0_tbu vote %#x = %#010x has no PWR_ON (status %#010x %#010x) before %s\n",
	       getenv("VENUSCTL_NO_TBUCHECK") ? "note (VENUSCTL_NO_TBUCHECK):" : "STOP:", GCC_TBU_SF_VOTE, vote,
	       rd32(TBU_SF_STATUS), rd32(TBU_SF_STATUS + 4), when);
	return getenv("VENUSCTL_NO_TBUCHECK") ? 0 : -1;
}

/* review 8.5.6: a block the firmware may DMA into: 1 MB aligned, in the free gap FIXED_LO-FIXED_HI (redfin) and inside
 * the venus_ns pool [0x25800000, 0xe0000000). 0 = ok. */
static int range_ok(uint64_t a, uint64_t len, const char *what)
{
	if (a < SMMU_TBL + 0x2000 && a + len > SMMU_TBL) {
		printf("venus: refused: %s %#llx+%#llx overlaps the SMMU tables at %#llx\n", what, (unsigned long long)a,
		       (unsigned long long)len, SMMU_TBL);
		return -1;
	}
	if (!(a & 0xfffff) && len && a >= CP_SIZE && a + len <= POOL_END && a >= FIXED_LO && a + len <= FIXED_HI)
		return 0;
	printf("venus: refused: %s %#llx+%#llx: needs 1 MB alignment, inside %#llx-%#llx (and %#x-%#llx)\n", what,
	       (unsigned long long)a, (unsigned long long)len, FIXED_LO, FIXED_HI, CP_SIZE, POOL_END);
	return -1;
}

static void win_faults(const char *when)	/* redfin: the window bank's fault record (reads only) */
{
	if (!win_cbp)
		return;
	/* FSR bits 10:9 are FORMAT (0x400 = AArch64 record), not a fault; only 0xc00001fe are fault bits. FAR/FSYNR0
	 * mean something only while a fault bit is set (otherwise: reset leftovers, never cleared). FAR = 0x60 low,
	 * 0x64 high; FSYNR0 = 0x68 (bits 7:0 PLVL/WNR/etc., 23:16 ... see smmu_faults); CBFRSYNRA (GR1 0x400 + 4 n)
	 * holds the faulting stream ID. */
	uint32_t fsr = rd32(win_cbp + 0x58);
	if (!(fsr & 0xc00001feu)) {
		printf("venus: %s: window bank: no fault (FSR %#010x)\n", when, fsr);
		pm_step("window bank: no fault (FSR %#010x) (%s)", fsr, when);
		return;
	}
	uint64_t gr1 = 0x15000000ull + ((rd32(0x15000024ull) >> 31) ? 0x10000 : 0x1000);
	uint32_t cb = (uint32_t)((win_cbp - 0x15000000ull) / ((rd32(0x15000024ull) >> 31) ? 0x10000 : 0x1000)) -
		      (1u << (((rd32(0x15000024ull) >> 28) & 7) + 1));
	uint32_t frsynra = rd32(gr1 + 0x400 + 4 * cb);
	printf("venus: %s: window bank FAULT FSR %#010x FAR %#010x%08x FSYNR0 %#010x CBFRSYNRA %#010x (stream %#x)\n",
	       when, fsr, rd32(win_cbp + 0x64), rd32(win_cbp + 0x60), rd32(win_cbp + 0x68), frsynra, frsynra & 0xffff);
	pm_step("window bank FAULT FSR %#010x FAR %#010x%08x FSYNR0 %#010x CBFRSYNRA %#010x (%s)", fsr, rd32(win_cbp + 0x64),
		rd32(win_cbp + 0x60), rd32(win_cbp + 0x68), frsynra, when);
}

static void core_status(const char *when)
{
	win_faults(when);
	uint32_t st = crd(CTRL_STATUS);
	printf("venus: %s: CTRL_STATUS %#010x (init %u error %#x pc_ready %u) VERSION_INFO %#010x HW_VERSION %#010x"
	       " CPU_STATUS %#010x INTR_STATUS %#010x\n", when, st, st & ST_INIT, st & ST_ERROR, (st >> 8) & 1,
	       crd(VERSION_INFO), crd(WRAP_HW_VERSION), crd(WRAP_CPU_STATUS), crd(WRAP_INTR_STATUS));
}

static int boot_fail(struct vstate *v)
{
	printf("venus: boot failed, undoing\n");
	teardown(v);
	return 1;
}

/* redfin rn4x: read-only SMMU picture for the Venus range: every valid SMR that matches any SID 0x1300-0x13ff (with its
 * S2CR, bank SCTLR/CBAR and FSR fault bits), the number of valid SMRs, and the global fault registers. Called around
 * auth_and_reset and memory protect to see whether TZ/the hypervisor adds or changes SMRs for the video streams. */
void smmu_snapshot(const char *when)
{
	uint64_t s = 0x15000000ull;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24), nsmr = id0 & 0xff, valid = 0;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i);
		if (!(smr >> 31))
			continue;
		valid++;
		uint32_t first = 0, cnt = 0;
		for (uint32_t sid = 0x1300; sid <= 0x13ff; sid++)
			if (sid_match(smr, sid))
				first = cnt++ ? first : sid;
		if (!cnt)
			continue;
		uint32_t s2cr = rd32(s + 0xc00 + 4 * i), cb = s2cr & 0xff;
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
		printf("venus: smmu %s: SMR%u %08x (%u SIDs from %#x) S2CR %08x type %u CB%u SCTLR %08x CBAR %08x FSR %08x\n",
		       when, i, smr, cnt, first, s2cr, (s2cr >> 16) & 3, cb, rd32(cbp), rd32(s + psize + 4 * cb), rd32(cbp + 0x58));
		pm_step("smmu %s: SMR%u %08x (%u SIDs from %#x) S2CR %08x CB%u SCTLR %08x CBAR %08x FSR %08x", when, i, smr, cnt,
			first, s2cr, cb, rd32(cbp), rd32(s + psize + 4 * cb), rd32(cbp + 0x58));
	}
	printf("venus: smmu %s: %u valid SMRs, sCR0 %08x sGFSR %08x sGFSYNR0 %08x sGFSYNR1 %08x sGFSYNR2 %08x\n", when, valid,
	       rd32(s), rd32(s + 0x48), rd32(s + 0x50), rd32(s + 0x54), rd32(s + 0x58));
	pm_step("smmu %s: %u valid SMRs, sCR0 %08x sGFSR %08x sGFSYNR0 %08x sGFSYNR1 %08x sGFSYNR2 %08x", when, valid,
		rd32(s), rd32(s + 0x48), rd32(s + 0x50), rd32(s + 0x54), rd32(s + 0x58));
}

/*
 * redfin rn4x: SMMU setup for the Venus SIDs, by VENUSCTL_SMMU:
 *   window   (default) all of them through one translating bank mapping only FIXED_LO-FIXED_HI (smmu_window)
 *   identity translation-off banks, one per SID (sunfish's way; 4 SIDs passed boot/init/session on rn4x)
 *   hybrid   identity for the 0x1380 group, the window bank for 0x1360 only
 * Appends to v (STATE saved). 0 = ok.
 */
static int map_sids(struct vstate *v, const uint16_t *sids, unsigned n, const char *mode)
{
	if (!strcmp(mode, "identity") || !strcmp(mode, "hybrid")) {
		for (unsigned k = 0; k < n; k++) {
			if (!strcmp(mode, "hybrid") && sids[k] == 0x1360)
				continue;
			smmu_last.slot = -1;
			int bad = smmu_identity("venus", sids[k], 0);
			if (smmu_last.slot >= 0) {
				v->sid[v->nsmr] = sids[k];
				v->smr[v->nsmr++] = smmu_last;
				state_save(v);
				printf("venus: SID %#x: SMR%d + CB%d added (identity)\n", sids[k], smmu_last.slot, smmu_last.cb);
			}
			if (bad)
				return -1;
		}
		if (!strcmp(mode, "identity"))
			return 0;
	}
	uint16_t w[8];
	unsigned nw = 0;
	for (unsigned k = 0; k < n && nw < 8; k++)
		if (strcmp(mode, "hybrid") || sids[k] == 0x1360)
			w[nw++] = sids[k];
	if (!nw)
		return 0;
	if (strcmp(mode, "window") && strcmp(mode, "hybrid")) {
		printf("venus: refused: VENUSCTL_SMMU=%s (window, identity or hybrid)\n", mode);
		return -1;
	}
	struct smmu_saved sv[8];
	int wcb = smmu_window("venus", w, nw, FIXED_LO, FIXED_HI - FIXED_LO, SMMU_TBL, sv);
	if (wcb < 0)
		return -1;
	for (unsigned k = 0; k < nw; k++) {
		v->sid[v->nsmr] = w[k];
		v->smr[v->nsmr++] = sv[k];
	}
	win_cbp = sv[0].cbp;
	state_save(v);
	printf("venus: %u SID(s) through window bank CB%d (%#llx-%#llx, IOVA = PA)\n", nw, wcb,
	       (unsigned long long)FIXED_LO, (unsigned long long)FIXED_HI);
	return 0;
}

/* redfin rn4x: `venusctl map-sid SID` after boot --keep (and init): one more SID now, recorded in STATE (shutdown
 * releases it); mode as VENUSCTL_SMMU (window default). Tests whether 0x1360's SMR kills the phone only when it is
 * there at auth_and_reset. */
int map_sid(uint32_t sid)
{
	struct vstate v;
	if (state_load(&v)) {
		printf("venus: refused: no %s (boot --keep first)\n", STATE);
		return 1;
	}
	for (int i = 0; i < v.nsmr; i++)
		if (v.sid[i] == sid) {
			printf("venus: refused: SID %#x already mapped (SMR%d)\n", sid, v.smr[i].slot);
			return 1;
		}
	if (v.nsmr >= 8) {
		printf("venus: refused: 8 SMRs recorded already\n");
		return 1;
	}
	smmu_snapshot("before map-sid");
	uint16_t one = (uint16_t)sid;
	const char *mode = getenv("VENUSCTL_SMMU") ? getenv("VENUSCTL_SMMU") : "window";
	int rc = map_sids(&v, &one, 1, !strcmp(mode, "hybrid") ? "window" : mode);
	smmu_snapshot("after map-sid");
	core_status("after map-sid");
	return rc ? 1 : 0;
}

/* redfin: `venusctl smmu`: the snapshot, the window bank's fault record (from STATE) and, with the power on, the
 * core status: reads only, safe to run in a loop while the firmware runs */
int smmu_cmd(void)
{
	struct vstate v;
	state_load(&v);	/* sets win_cbp when a window bank is recorded */
	smmu_snapshot("now");
	win_faults("now");
	if (core_ok())
		printf("venus: now: CTRL_STATUS %#010x CPU_STATUS %#010x INTR_STATUS %#010x\n", rd32(CTRL_STATUS),
		       rd32(WRAP_CPU_STATUS), rd32(WRAP_INTR_STATUS));
	return 0;
}

int boot(const char *fw, int keep)
{
	struct vstate v;
	if (state_load(&v) == 0) {
		printf("venus: refused: %s exists (a boot --keep is active): venusctl shutdown first\n", STATE);
		return 1;
	}
	if (!core_ok()) {
		printf("venus: refused: power is off (a video clock or GDSC is off): run 'venusctl power on' first\n");
		return 1;
	}
	uint32_t vote = rd32(GCC_TBU_SF_VOTE);
	if (!(vote & PWR_ON)) {	/* SIDs 0x1000-0x13ff go through mnoc_sf_0_tbu (camera-sid-cc0.md) */
		printf("venus: refused: mnoc_sf_0_tbu vote %#x = %#010x has no PWR_ON\n", GCC_TBU_SF_VOTE, vote);
		return 1;
	}
	struct fwimg f;
	if (fw_meta(fw, &f))
		return 1;
	/* load segments: span and relocation as adspboot boot() */
	uint64_t lo = ~0ull, hi = 0;
	int reloc = 0;
	for (uint32_t i = 0; i < f.phnum; i++) {
		if ((int)i == f.hash || f.ph[i].type != 1 || !f.ph[i].memsz)
			continue;
		reloc |= !!(f.ph[i].flags & MDT_RELOC);
		if (f.ph[i].paddr < lo)
			lo = f.ph[i].paddr;
		if (f.ph[i].paddr + f.ph[i].memsz > hi)
			hi = f.ph[i].paddr + f.ph[i].memsz;
	}
	uint64_t base = reloc ? FW_PA : lo;
	if (lo > hi || base < FW_PA || base + (hi - lo) > FW_PA + FW_SIZE) {
		printf("venus: refused: image %llx-%llx (reloc %d) does not fit %llx+%llx\n", (unsigned long long)lo,
		       (unsigned long long)hi, reloc, FW_PA, FW_SIZE);
		return 1;
	}
	printf("venus: image linked %llx-%llx, %s to %llx\n", (unsigned long long)lo, (unsigned long long)hi,
	       reloc ? "relocated" : "fixed", (unsigned long long)base);

	/* exact SMRs only; refuse if any SMR already matches one of the SIDs (not ours to change) */
	/* redfin: 5 SIDs (0x1360 last); VENUSCTL_SID1=1: the first only; VENUSCTL_NSID=N: the first N (4 = venus_ns's
	 * 0x1380 mask 0x60 without 0x1360) */
	unsigned nsid = getenv("VENUSCTL_SID1") ? 1 : (unsigned)NSID;
	if (getenv("VENUSCTL_NSID") && atoi(getenv("VENUSCTL_NSID")) > 0 && (unsigned)atoi(getenv("VENUSCTL_NSID")) < NSID)
		nsid = (unsigned)atoi(getenv("VENUSCTL_NSID"));
	uint64_t sm = 0x15000000ull;
	uint32_t nsmr = rd32(sm + 0x20) & 0xff;
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(sm + 0x800 + 4 * i);
		for (unsigned k = 0; k < nsid && (smr >> 31); k++)
			if (sid_match(smr, venus_sids[k])) {
				printf("venus: refused: SMR%u %08x already matches SID %#x\n", i, smr, venus_sids[k]);
				return 1;
			}
	}
	const char *smode = getenv("VENUSCTL_SMMU") ? getenv("VENUSCTL_SMMU") : "window";
	smmu_snapshot("before boot");
	if (map_sids(&v, venus_sids, nsid, smode))
		return boot_fail(&v);
	smmu_snapshot("SIDs mapped");
	/* clock_config_on_enable_vpu5 (venus_hfi.c:4703-4707), part of downstream __venus_power_on before the PIL load */
	if (!getenv("VENUSCTL_NO_CLKCFG")) {
		wr(WRAP_CPU_CGCDIS, 0, "CPU_CGC_DIS");
		wr(WRAP_CPU_CLKCFG, 0, "CPU_CLOCK_CONFIG");
		printf("venus: CPU_CGC_DIS %#010x CPU_CLOCK_CONFIG %#010x\n", crd(WRAP_CPU_CGCDIS), crd(WRAP_CPU_CLKCFG));
	}

	uint64_t r1 = 0;
	int64_t rc = scm(7, 1, PAS_VENUS, 0, 0, &r1);
	printf("venus: pas supported(%d) -> %lld/%llx\n", PAS_VENUS, (long long)rc, (unsigned long long)r1);
	uint64_t meta_pa = pm_alloc(f.meta_len, 0x1000);
	if (!meta_pa)
		return boot_fail(&v);
	pm_wr(meta_pa, f.meta, (f.meta_len + 3) & ~(size_t)3);
	printf("venus: pas init_image(%d, %llx)...\n", PAS_VENUS, (unsigned long long)meta_pa);
	rc = scm(1, 2 | (2u << 6), PAS_VENUS, meta_pa, 0, &r1);
	printf("venus: pas init_image -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
	pm_free(meta_pa);
	if (rc || r1)
		return boot_fail(&v);	/* nothing in TZ: no shutdown (adspboot adsp3) */
	v.pas = 1;
	state_save(&v);
	if (reloc) {
		rc = scm(2, 3, PAS_VENUS, FW_PA, FW_SIZE, &r1);
		printf("venus: pas mem_setup %llx+%llx -> %lld/%llx\n", FW_PA, FW_SIZE, (long long)rc,
		       (unsigned long long)r1);
		if (rc || r1)
			return boot_fail(&v);
	}
	static uint8_t zeros[0x10000];
	uint64_t total = 0;
	for (uint32_t i = 0; i < f.phnum; i++) {
		if ((int)i == f.hash || f.ph[i].type != 1 || !f.ph[i].memsz)
			continue;
		uint64_t pa = base + (f.ph[i].paddr - lo);
		if (f.ph[i].filesz) {
			char path[256];
			size_t sl;
			snprintf(path, sizeof(path), "%s/venus.b%02u", fw, i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl < f.ph[i].filesz) {
				printf("venus: %s missing or short\n", path);
				return boot_fail(&v);
			}
			if (pm_wr(pa, seg, (f.ph[i].filesz + 3) & ~3u))
				return boot_fail(&v);
			free(seg);
			total += f.ph[i].filesz;
		}
		for (uint64_t o = (f.ph[i].filesz + 3) & ~3ull; o < f.ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = f.ph[i].memsz - o < sizeof(zeros) ? f.ph[i].memsz - o : sizeof(zeros);
			if (pm_wr(pa + o, zeros, (n + 3) & ~3ull))
				return boot_fail(&v);
		}
		printf("venus: seg %u -> %llx (%u file, %u mem)\n", i, (unsigned long long)pa, f.ph[i].filesz,
		       f.ph[i].memsz);
	}
	printf("venus: loaded %llu bytes\n", (unsigned long long)total);
	if (!core_ok()) {	/* auth_and_reset unpowered resets the phone (notes section 5) */
		printf("venus: STOP: a clock or GDSC dropped before auth_and_reset\n");
		return boot_fail(&v);
	}
	if (pas(5, "auth_and_reset"))
		return boot_fail(&v);
	smmu_snapshot("after auth");
	if (!getenv("VENUSCTL_NO_MEMPROT")) {
		uint64_t x[8] = { SCM_MP_VIDEO_VAR, 4, 0, CP_SIZE, CP_NP_START, CP_NP_SIZE, 0, 0 };
		printf("venus: mem protect video var (0, %#x, %#x, %#x)...\n", CP_SIZE, CP_NP_START, CP_NP_SIZE);
		pm_smc(x);
		printf("venus: mem protect -> %lld/%llx\n", (long long)x[0], (unsigned long long)x[1]);
		if (x[0])
			return boot_fail(&v);
	}
	smmu_snapshot("after mem protect");
	core_status("after auth");
	if (keep) {
		printf("venus: boot ok, kept (state %s): next venusctl init, or venusctl shutdown\n", STATE);
		return 0;
	}
	int r = teardown(&v);
	printf("venus: boot ok, undone -> %s\n", r ? "teardown FAILED" : "ok");
	return r;
}

/* ---- HFI queues (downstream __write_queue / __read_queue on the uncached block) ---- */

static uint64_t qb;	/* the block, from STATE */

static uint64_t qhdr(unsigned i)
{
	return qb + QHDR0 + QHDR_SIZE * i;
}

static uint64_t qstart(unsigned i)
{
	return qb + QTBL_SIZE + (uint64_t)QSIZE * i;
}

/* qhdr fields (struct hfi_queue_header) */
#define QH_RX_REQ	0x20
#define QH_TX_REQ	0x24
#define QH_READ		0x30
#define QH_WRITE	0x34

/* 4a vc_quiet: the per-buffer packets (ETB 0x211004, FTB 0x211005, EBD 0x221007, FBD 0x221008) are not logged */
static int chatty(uint32_t type)
{
	return !vc_quiet || (type != 0x211004u && type != 0x211005u && type != 0x221007u && type != 0x221008u);
}

static int q_write(const uint32_t *w)
{
	uint32_t n = w[0] / 4, rd = rd32(qhdr(0) + QH_READ), wi = rd32(qhdr(0) + QH_WRITE);
	if (!n || rd >= QWORDS || wi >= QWORDS) {
		printf("venus: STOP: cmdq bad (size %u, read %u, write %u)\n", w[0], rd, wi);
		return -1;
	}
	uint32_t empty = wi >= rd ? QWORDS - (wi - rd) : rd - wi;
	if (empty <= n) {
		wr32(qhdr(0) + QH_TX_REQ, 1);
		printf("venus: STOP: cmdq full (%u words free, %u needed)\n", empty, n);
		return -1;
	}
	wr32(qhdr(0) + QH_TX_REQ, 0);
	if (pm_stepping())
		pm_step("HFI cmd %#x size %u: %08x %08x %08x %08x %08x %08x", w[1], w[0], n > 2 ? w[2] : 0, n > 3 ? w[3] : 0,
			n > 4 ? w[4] : 0, n > 5 ? w[5] : 0, n > 6 ? w[6] : 0, n > 7 ? w[7] : 0);
	uint32_t first = n < QWORDS - wi ? n : QWORDS - wi;
	pm_wr(qstart(0) + 4ull * wi, w, 4 * first);
	if (first < n)
		pm_wr(qstart(0), w + first, 4 * (n - first));
	wr32(qhdr(0) + QH_WRITE, (wi + n) % QWORDS);
	uint32_t rx = rd32(qhdr(0) + QH_RX_REQ);
	if (chatty(w[1]))
		printf("venus: cmd %#x size %u at %u, rx_req %u%s\n", w[1], w[0], wi, rx, rx == 1 ? ", doorbell" : "");
	if (rx == 1) {	/* __iface_cmdq_write: interrupt only if the firmware asked for one */
		if (tbu_ok("doorbell"))
			return -1;
		wr(IC_SOFTINT, SOFTINT_H2A, "doorbell");
	}
	return 0;
}

static uint32_t pkt[3072];	/* VIDC_IFACEQ_VAR_HUGE_PKT_SIZE */

/* one packet from queue i (1 msg, 2 debug) into pkt: words, 0 = empty, -1 = bad */
static int q_read(unsigned i)
{
	uint32_t rd = rd32(qhdr(i) + QH_READ), wi = rd32(qhdr(i) + QH_WRITE), want_rx = i == 1;
	if (rd == wi) {
		wr32(qhdr(i) + QH_RX_REQ, want_rx);
		return 0;
	}
	if (rd >= QWORDS || wi >= QWORDS) {
		printf("venus: STOP: queue %u bad indexes (read %u, write %u)\n", i, rd, wi);
		return -1;
	}
	uint32_t size = rd32(qstart(i) + 4ull * rd), n = size / 4, nr;
	int got = (int)n;
	if (!n) {
		printf("venus: queue %u: zero packet size at %u\n", i, rd);
		return -1;
	}
	if (size <= sizeof(pkt)) {
		uint32_t first = n < QWORDS - rd ? n : QWORDS - rd;
		pm_rd(qstart(i) + 4ull * rd, pkt, 4 * first);
		if (first < n)
			pm_rd(qstart(i), pkt + first, 4 * (n - first));
		nr = (rd + n) % QWORDS;
	} else {
		printf("venus: queue %u: bad packet size %u at %u, dropped to the write index\n", i, size, rd);
		nr = wi;
		got = -1;
	}
	wr32(qhdr(i) + QH_RX_REQ, nr != wi ? 0 : want_rx);
	wr32(qhdr(i) + QH_READ, nr);
	if (pm_stepping() && got > 0)
		pm_step("HFI %s pkt %#x size %u: %08x %08x %08x %08x %08x %08x", i == 1 ? "msg" : "dbg", pkt[1], pkt[0],
			got > 2 ? pkt[2] : 0, got > 3 ? pkt[3] : 0, got > 4 ? pkt[4] : 0, got > 5 ? pkt[5] : 0,
			got > 6 ? pkt[6] : 0, got > 7 ? pkt[7] : 0);
	return got;
}

int vc_frame_tag;	/* vdstop: venusd sets it: firmware debug lines get the session's frame count */
static int sess_frames(void);	/* -1: no session started */

static void print_pkt(unsigned q, int n)
{
	printf("venus: %s pkt type %#x size %u:", q == 1 ? "msgq" : "dbgq", pkt[1], pkt[0]);
	for (int k = 0; k < n && k < 8; k++)
		printf(" %08x", pkt[k]);
	printf("\n");
	if (pkt[1] == 0x20004 && n > 6) {	/* HFI_MSG_SYS_DEBUG: msg_type, msg_size, timestamp, text */
		char s[sizeof(pkt)];
		size_t len = 4u * (unsigned)n - 24;
		if (pkt[3] < len)
			len = pkt[3];
		memcpy(s, pkt + 6, len);
		s[len] = 0;
		for (size_t k = 0; k < len && s[k]; k++)
			if (s[k] < 0x20 || s[k] > 0x7e)
				s[k] = ' ';
		int fr = vc_frame_tag ? sess_frames() : -1;
		if (fr >= 0)
			printf("venus: [frame %d] fw [%#x]: %s\n", fr, pkt[2], s);
		else
			printf("venus: fw [%#x]: %s\n", pkt[2], s);
	}
}

static int wait_soft;	/* v2c: a timeout is reported as a note, not a STOP (ping) */
static uint32_t lastmsg[8];	/* v2c: first words of the last msgq packet, for a non-fatal ping */
static int lastmsg_n;
static int (*on_pkt)(int n);	/* stage 3 decode: sees every msgq packet before the want check */
static int fw_dead;		/* stage 3: a sys error came: no more session commands */

/* redfin --step: every 500 ms while waiting, the core's status words and the SMMU's global fault registers (reads
 * only: CTRL_STATUS, CPU_STATUS, INTR_STATUS through core_ok's gate; sGFSR/sGFSYNR0-2/sGFAR) to the step log */
static void step_status(int force_now)
{
	static double last;
	double t = mono_ms();
	if (!force_now && t - last < 500)
		return;
	last = t;
	if (!core_ok()) {
		pm_step("status: a video clock or GDSC reads off");
		return;
	}
	uint64_t s = 0x15000000ull;
	pm_step("status: CTRL_STATUS %#010x CPU_STATUS %#010x INTR_STATUS %#010x | smmu sGFSR %#010x sGFSYNR0 %#010x"
		" sGFSYNR1 %#010x sGFSYNR2 %#010x sGFAR %#010x%08x", rd32(CTRL_STATUS), rd32(WRAP_CPU_STATUS),
		rd32(WRAP_INTR_STATUS), rd32(s + 0x48), rd32(s + 0x50), rd32(s + 0x54), rd32(s + 0x58), rd32(s + 0x44),
		rd32(s + 0x40));
	if (win_cbp)
		pm_step("status: window bank FSR %#010x FAR %#010x%08x FSYNR0 %#010x", rd32(win_cbp + 0x58),
			rd32(win_cbp + 0x64), rd32(win_cbp + 0x60), rd32(win_cbp + 0x68));
}

/* drain the debug queue, then the message queue until a packet of type want (0 = just drain); 0 = got it */
static int wait_msg(uint32_t want, unsigned ms)
{
	double t0 = mono_ms();	/* lat2: ms on the clock (pmem.h wait_over), not loop passes */
	unsigned t;
	for (t = 0;; t++) {
		int n;
		while ((n = q_read(2)) > 0)
			print_pkt(2, n);
		while ((n = q_read(1)) > 0) {
			if (chatty(pkt[1]))
				print_pkt(1, n);
			lastmsg_n = n < 8 ? n : 8;
			memcpy(lastmsg, pkt, 4u * (unsigned)lastmsg_n);
			if (pkt[1] == 0x21001 && n > 3 && pkt[3] == 1) {	/* EVENT_NOTIFY, HFI_EVENT_SYS_ERROR */
				printf("venus: STOP: firmware sys error (data %#x %#x)\n", pkt[4], pkt[5]);
				fw_dead = 1;
				print_sfr(qb);
				return -1;
			}
			if (on_pkt)
				on_pkt(n);
			if (want && pkt[1] == want)
				return 0;
		}
		if (n < 0)
			return -1;
		if (pm_stepping())
			step_status(0);
		if (wait_over(t0, t, ms))
			break;
		delay_ms(1);
	}
	if (!want)
		return 0;
	printf("venus: %s no %#x after %u ms\n", wait_soft ? "note:" : "STOP:", want, wait_ms(t0, t));
	core_status("timeout");
	print_sfr(qb);
	return -1;
}

int init(uint64_t phys)
{
	struct vstate v;
	if (state_load(&v) || !v.pas) {
		printf("venus: refused: no running boot (venusctl boot --keep first)\n");
		return 1;
	}
	if (v.q) {
		printf("venus: refused: queues already set up at %#llx (venusctl shutdown first)\n",
		       (unsigned long long)v.q);
		return 1;
	}
	if (!core_ok()) {
		printf("venus: refused: a video clock or GDSC is off\n");
		return 1;
	}
	core_status("before init");
	uint64_t q;
	if (phys) {
		if (range_ok(phys, QSHARED + SFR_SIZE, "--phys"))	/* 3 MB + the SFR page, as the review asks */
			return 1;
		q = phys;
		v.qkind = 2;
	} else {
		q = pm_alloc32(QSHARED, 0x100000);	/* UC region address is bits 31:20 (vidc_hfi_io.h HFI_DSP_UC_REGION_ADDR) */
		if (!q) {
			printf("venus: STOP: PMEM_ALLOC32 failed: rerun with --phys ADDR (1 MB aligned, %#llx-%#llx)\n",
			       FIXED_LO, FIXED_HI);
			return 1;
		}
		v.qkind = 1;
	}
	v.q = q;
	state_save(&v);
	/* device addresses are physical (identity bank) and 32-bit; below CP_SIZE is the content-protected range */
	if (q < CP_SIZE || q + QSHARED > POOL_END) {
		printf("venus: STOP: queue block %#llx outside %#x-%#llx\n", (unsigned long long)q, CP_SIZE, POOL_END);
		return 1;
	}
	qb = q;
	static uint8_t zeros[0x10000];
	for (uint32_t o = 0; o < QSHARED; o += sizeof(zeros))
		if (pm_wr(q + o, zeros, sizeof(zeros)))
			return 1;
	/* __interface_queues_init (venus_hfi.c:1898-2052) */
	uint32_t t[QTBL_SIZE / 4];
	memset(t, 0, sizeof(t));
	t[0] = 0;		/* qtbl_version */
	t[1] = QTBL_SIZE;
	t[2] = QHDR0;
	t[3] = QHDR_SIZE;
	t[4] = 3;		/* num_q */
	t[5] = 3;		/* num_active_q */
	/* t[6..7] device_addr: a host pointer downstream, 0 here */
	memcpy(&t[8], "msm_v4l2_vidc", 14);
	for (unsigned i = 0; i < 3; i++) {
		uint32_t *h = &t[(QHDR0 + QHDR_SIZE * i) / 4];	/* __set_queue_hdr_defaults */
		h[0] = 1;				/* status */
		h[1] = (uint32_t)qstart(i);		/* start_addr = device address */
		h[2] = 0x01010000u | i;			/* VIDC_IFACEQ_DFLT_QHDR | queue id */
		h[3] = QSIZE / 4;
		h[6] = 1;				/* rx_wm */
		h[7] = 1;				/* tx_wm */
		h[8] = i == 2 ? 0 : 1;			/* rx_req: 0 on the debug queue */
	}
	pm_wr(q, t, sizeof(t));
	wr32(q + SFR_OFF, SFR_SIZE);	/* hfi_sfr_struct bufSize */
	printf("venus: queue block %#llx (%s): table %#x, cmdq %#llx msgq %#llx dbgq %#llx, SFR %#llx\n",
	       (unsigned long long)q, v.qkind == 1 ? "PMEM_ALLOC32" : "--phys", QTBL_SIZE, (unsigned long long)qstart(0),
	       (unsigned long long)qstart(1), (unsigned long long)qstart(2), (unsigned long long)(q + SFR_OFF));
	/* __setup_ucregion_memory_map (venus_hfi.c:1881-1896) + setup_dsp_uc_memmap_vpu5 (4685-4692); no QDSS */
	wr(UC_ADDR, (uint32_t)q, "UC_REGION_ADDR");
	wr(UC_SIZE, QSHARED, "UC_REGION_SIZE");
	wr(QTBL_ADDR, (uint32_t)q, "QTBL_ADDR");
	wr(QTBL_INFO, 1, "QTBL_INFO");
	wr(SFR_ADDR, (uint32_t)(q + SFR_OFF), "SFR_ADDR");
	wr(CS_VMIMSG, (uint32_t)q, "DSP_QTBL_ADDR");
	wr(CS_VMIMSGAG0, (uint32_t)q, "DSP_UC_REGION_ADDR");
	wr(CS_VMIMSGAG1, QSHARED, "DSP_UC_REGION_SIZE");
	/* __boot_firmware (venus_hfi.c:1265-1290): CTRL_INIT = 1 (no CVP domain in the DT), wait for a status */
	if (tbu_ok("CTRL_INIT"))
		return 1;
	wr(CTRL_INIT, 1, "CTRL_INIT");
	uint32_t st = 0;
	unsigned ms;
	double t0 = mono_ms();	/* lat2: 1 s on the clock; the line prints the real time (lat1: passes, "after 1 ms") */
	for (unsigned i = 0;; i++) {
		st = crd(CTRL_STATUS);
		if (st & (ST_INIT | ST_ERROR) || wait_over(t0, i, 1000)) {
			ms = wait_ms(t0, i);
			break;
		}
		delay_ms(1);
	}
	v.init = (st & ST_INIT) && !(st & ST_ERROR);
	state_save(&v);
	printf("venus: CTRL_STATUS %#010x after %u ms: init %u, error %#x%s\n", st, ms, st & ST_INIT, st & ST_ERROR,
	       (st & ST_ERROR) == 4 ? " (invalid UC_REGION)" : "");
	core_status("after init");
	if (!v.init) {
		printf("venus: STOP: %s\n", st & ST_ERROR ? "firmware reports an error" : "no init status after 1 s");
		print_sfr(q);
		return 1;
	}
	printf("venus: init ok: next venusctl ping, or venusctl shutdown\n");
	return 0;
}

/* ping and session: state check, then SYS_INIT (once per boot), debug config and IMAGE_VERSION. 0 = ok. */
static int sys_up(struct vstate *vp, uint32_t debug, const char *what)
{
	struct vstate v;
	if (state_load(&v) || !v.init || !v.q) {
		printf("venus: refused: no initialised firmware (venusctl boot --keep, then init)\n");
		return 1;
	}
	if (!core_ok()) {
		printf("venus: refused: a video clock or GDSC is off\n");
		return 1;
	}
	qb = v.q;
	core_status(what);
	if (wait_msg(0, 0))
		return 1;
	if (v.sysinit) {
		printf("venus: SYS_INIT already sent in an earlier run, skipped\n");
	} else {
		uint32_t sys_init[3] = { 12, 0x10001, 1 };	/* HFI_CMD_SYS_INIT, HFI_VIDEO_ARCH_OX */
		if (q_write(sys_init) || wait_msg(0x20001, 1000))
			return 1;
		printf("venus: SYS_INIT_DONE error %#x, %u properties\n", pkt[2], pkt[3]);
		if (pkt[2]) {
			printf("venus: STOP: SYS_INIT_DONE with an error\n");
			return 1;
		}
		v.sysinit = 1;
		state_save(&v);
	}
	/* create_pkt_cmd_sys_debug_config: SYS_SET_PROPERTY, SYS_DEBUG_CONFIG {mask, HFI_DEBUG_MODE_QUEUE} + pad */
	uint32_t dbg[7] = { 28, 0x10005, 1, 0x1, debug, 1, 0 };
	if (q_write(dbg) || wait_msg(0, 100))
		return 1;
	/* create_pkt_cmd_sys_image_version: SYS_GET_PROPERTY IMAGE_VERSION -> SYS_PROPERTY_INFO 0x2000a */
	uint32_t ver[4] = { 16, 0x10006, 1, 0x6 };
	if (q_write(ver) || wait_msg(0x2000a, 1000))
		return 1;
	if (pkt[0] >= 16 + 128 && pkt[3] == 0x6) {	/* hfi_process_sys_get_prop_image_version: 128 bytes at word 4 */
		char s[129];
		memcpy(s, pkt + 4, 128);
		for (int k = 0; k < 128; k++)
			if (s[k] < 0x20 || s[k] > 0x7e)
				s[k] = ' ';
		s[128] = 0;
		printf("venus: firmware image version: %s\n", s);
	} else {
		printf("venus: property info without an image version (size %u, property %#x)\n", pkt[0], pkt[3]);
	}
	*vp = v;
	return 0;
}

/*
 * redfin rn4x: `venusctl watch SECONDS [--sysinit] [--debug MASK]`: after init, read-only watch of the firmware until
 * it dies or SECONDS pass, every 200 ms: CTRL_STATUS (init/idle/pc_ready/error), WRAPPER_CPU_STATUS (WFI),
 * WRAPPER_INTR_STATUS (0x10 = watchdog A2HWD, 0x4 = A2H), the video NoC error logger (ERRVLD + ERRLOG0-3), the
 * SMMU's global fault registers (USF stream in sGFSYNR1), GDSC/clock/PLL state, and the firmware's debug/msg queues
 * (drained and printed). A line when anything changes, else every 2 s; with --step each line is also on /dev/console
 * and fsynced, so the last state before a reset survives. --sysinit first sends SYS_INIT + debug config + image
 * version (Linux's core_init does this right after the boot handshake; our init leaves the firmware before
 * SYS_INIT). Never writes a core register except the doorbell for --sysinit's commands.
 */
#define NOC_ERR		(CORE + 0x4000 + 0x500)	/* VCODEC_CORE0_VIDEO_NOC_BASE_OFFS + ERR_SWID_LOW (hfi_io_common.h) */
int watch(unsigned secs, int sysinit, uint32_t debug)
{
	struct vstate v;
	if (state_load(&v) || !v.init || !v.q) {
		printf("venus: refused: no initialised firmware (venusctl boot --keep, then init)\n");
		return 1;
	}
	qb = v.q;
	if (sysinit && sys_up(&v, debug, "watch"))
		return 1;
	uint64_t s = 0x15000000ull;
	uint32_t last[16] = { 0 };
	double t0 = mono_ms(), tl = -1e9;
	unsigned n = 0;
	for (;;) {
		double t = mono_ms();
		if (t - t0 > secs * 1e3)
			break;
		uint32_t cur[16] = { 0 };
		int on = core_ok();
		cur[0] = on;
		cur[1] = vrd(GDSC_MVSC), cur[2] = vrd(GDSC_MVS0), cur[3] = vrd(PLL_MODE), cur[4] = rd32(GCC_VIDEO_AXI);
		cur[5] = rd32(s + 0x48), cur[6] = rd32(s + 0x50), cur[7] = rd32(s + 0x54), cur[8] = rd32(s + 0x58);
		if (on) {
			cur[9] = rd32(CTRL_STATUS), cur[10] = rd32(WRAP_CPU_STATUS), cur[11] = rd32(WRAP_INTR_STATUS);
			cur[12] = rd32(NOC_ERR + 0x10);	/* ERRVLD */
			if (cur[12] & 1) {
				cur[13] = rd32(NOC_ERR + 0x20), cur[14] = rd32(NOC_ERR + 0x28), cur[15] = rd32(NOC_ERR + 0x30);
			}
		}
		int changed = memcmp(cur, last, sizeof(cur)) != 0;
		if (changed || t - tl >= 2000) {
			tl = t;
			char line[400];
			snprintf(line, sizeof(line), "watch %.1f s: %s CTRL_STATUS %#010x (init %u idle %u pc_ready %u err %#x)"
				 " CPU_STATUS %#x INTR_STATUS %#x%s | NoC ERRVLD %#x LOG0 %#x LOG1 %#x LOG2 %#x | sGFSR %#x"
				 " sGFSYNR0 %#x SYNR1 %#x SYNR2 %#x | gdsc mvsc %#x mvs0 %#x pll %#x axi %#x%s",
				 (t - t0) / 1e3, on ? "on" : "CLOCK/GDSC OFF", cur[9], cur[9] & 1, (cur[9] >> 30) & 1,
				 (cur[9] >> 8) & 1, cur[9] & 0xfe, cur[10], cur[11], (cur[11] & 0x10) ? " (WATCHDOG)" : "",
				 cur[12], cur[13], cur[14], cur[15], cur[5], cur[6], cur[7], cur[8], cur[1], cur[2], cur[3], cur[4],
				 changed ? "" : " (same)");
			printf("venus: %s\n", line);
			pm_step("%s", line);
			memcpy(last, cur, sizeof(cur));
		}
		if (on && wait_msg(0, 0))	/* drain + print firmware debug/messages (q_read only touches queue memory) */
			printf("venus: watch: queue read failed\n");
		if (on && n++ % 25 == 0)
			win_faults("watch");
		delay_ms(200);
	}
	printf("venus: watch: %u s done, phone alive\n", secs);
	return 0;
}

int ping(uint32_t debug)
{
	struct vstate v;
	if (sys_up(&v, debug, "before ping"))
		return 1;
	uint32_t pg[3] = { 12, 0x210002, 0x76656e75 };	/* HFI_CMD_SYS_PING; client_data: our own cookie */
	if (q_write(pg))
		return 1;
	/* v2c: VIDEO.IR.1.2-00053 reads word 2 as a session ID (HostDriver_GetSessionIdx fail) and answers EVENT_NOTIFY
	 * SESSION_ERROR 2, data 0x1004 (HFI_ERR_SESSION_INVALID_SESSION_ID): no PING_ACK. Not fatal: the firmware is
	 * alive, session is the real round trip. VENUSCTL_PING_FATAL=1: a missing PING_ACK fails (old behaviour). */
	int fatal = getenv("VENUSCTL_PING_FATAL") != NULL;
	lastmsg_n = 0;
	wait_soft = !fatal;
	int r = wait_msg(0x220002, 1000);
	wait_soft = 0;
	if (r && fatal)
		return 1;
	if (r) {
		printf("venus: ping not acked (non-fatal); last msgq packet:");
		for (int k = 0; k < lastmsg_n; k++)
			printf(" %08x", lastmsg[k]);
		printf("%s\n", lastmsg_n ? "" : " none");
	} else {
		printf("venus: PING_ACK client_data %#x%s\n", pkt[2], pkt[2] == pg[2] ? "" : " (not our cookie)");
	}
	wait_msg(0, 50);
	core_status("after ping");
	printf("venus: ping %s: venusctl session, or venusctl shutdown, then power off\n",
	       r ? "done without PING_ACK" : "ok (stage 2 go)");
	return 0;
}

/*
 * v2c: the first real HFI round trip, an H.264 decoder session opened and closed again (no buffers, no LOAD_RESOURCES).
 * Constants from the downstream driver (research/ext/msm-vidc-sunfish/, vidc_hfi_helper.h; same values in
 * research/notes/venus-stage1-2026-11-02.md "Packet IDs"):
 *   HFI_CMD_SYS_SESSION_INIT 0x10007 = HFI_CMD_START_OFFSET 0x10000 + 0x7, struct hfi_cmd_sys_session_init_packet
 *     {size 20, packet_type, session_id, session_domain, session_codec}; downstream session_id = hash32_ptr(session),
 *     any 32-bit cookie: ours 0x76656e75 ("venu")
 *   HFI_VIDEO_DOMAIN_DECODER 2 (HFI_COMMON_BASE + 0x2), HFI_VIDEO_CODEC_H264 2
 *   HFI_MSG_SYS_SESSION_INIT_DONE 0x20006 = HFI_MSG_START_OFFSET 0x20000 + 0x6 {size, type, session_id, error_type,
 *     num_properties, ...}; HFI_CMD_SYS_SESSION_END 0x10008 {size 12, type, session_id};
 *   HFI_MSG_SYS_SESSION_END_DONE 0x20007 {size 16, type, session_id, error_type}
 * A missing or failed reply is a STOP (exit 1); the caller's shutdown still undoes everything (PAS shutdown first).
 */
#define SESSION_ID	0x76656e75u
int session(uint32_t debug)
{
	struct vstate v;
	if (sys_up(&v, debug, "before session"))
		return 1;
	uint32_t si[5] = { 20, 0x10007, SESSION_ID, 2, 2 };	/* SYS_SESSION_INIT, decoder, H.264 */
	if (q_write(si) || wait_msg(0x20006, 1000))
		return 1;
	printf("venus: SESSION_INIT_DONE session %#x%s error %#x, %u properties\n", pkt[2],
	       pkt[2] == SESSION_ID ? "" : " (not ours)", pkt[3], pkt[0] >= 20 ? pkt[4] : 0);
	if (pkt[3]) {
		printf("venus: STOP: SESSION_INIT_DONE with an error (no session to end)\n");
		wait_msg(0, 50);
		return 1;
	}
	uint32_t se[3] = { 12, 0x10008, SESSION_ID };	/* SYS_SESSION_END */
	if (q_write(se) || wait_msg(0x20007, 1000))
		return 1;
	printf("venus: SESSION_END_DONE session %#x error %#x\n", pkt[2], pkt[0] >= 16 ? pkt[3] : 0);
	int bad = pkt[0] >= 16 && pkt[3];
	wait_msg(0, 50);
	core_status("after session");
	if (bad) {
		printf("venus: STOP: SESSION_END_DONE with an error\n");
		return 1;
	}
	printf("venus: session ok (H.264 decoder opened and closed): venusctl shutdown, then power off\n");
	return 0;
}

/*
 * Stage 3 (docs/features/video.md "Stage 3 spec"): decode an Annex B H.264 file. The sequence is downstream's for a
 * VPU5 decoder in split mode (v4l2 client order: s_fmt OUTPUT, STREAM_OUTPUT_MODE secondary, s_fmt CAPTURE NV12,
 * start_streaming), all from research/ext/msm-vidc-sunfish/ (file names without the drivers_media_platform_msm_vidc_
 * prefix). Linear NV12 only exists on OUTPUT2: OUTPUT is the UBWC DPB (msm_vdec.c:1145-1220; mainline vdec.c
 * "For Venus v4/v6 UBWC format is mandatory"). VENUSCTL_PRIMARY=1: linear NV12 straight on OUTPUT, no multi-stream.
 * Buffers: a bump allocator in a fixed window after the queue block (--bufphys, default the next 1 MB boundary after
 * --phys + 3 MB + SFR page; VENUSCTL_BUF_MB caps it, 64), device address = physical (identity banks). Capture buffers
 * are dynamic (msm_vdec.c:812): no SET_BUFFERS, FTB only (VENUSCTL_DPB_SETBUF=1: SET_BUFFERS the DPBs as the static
 * path, msm_vidc_common.c:3745-3765). Only the doorbell register is written (q_write), everything else is queue and
 * buffer memory.
 */
#define S_SET_PROP	0x11001u	/* HFI_CMD_SESSION_SET_PROPERTY (vidc_hfi_helper.h:873) */
#define S_SET_BUFS	0x11002u	/* HFI_CMD_SESSION_SET_BUFFERS (vidc_hfi_helper.h:875) */
#define S_LOAD_RES	0x211001u	/* HFI_CMD_SESSION_OX_START + 1 .. (vidc_hfi.h:318-333) */
#define S_START		0x211002u
#define S_STOP		0x211003u
#define S_ETB		0x211004u
#define S_FTB		0x211005u
#define S_FLUSH		0x211008u
#define S_GET_PROP	0x211009u
#define S_REL_BUFS	0x21100bu
#define S_REL_RES	0x21100cu
#define S_CONTINUE	0x21100du
#define M_EVENT		0x21001u	/* HFI_MSG_EVENT_NOTIFY (vidc_hfi_helper.h:896) */
#define M_LOAD_DONE	0x221001u	/* HFI_MSG_SESSION_OX_START + 1 .. (vidc_hfi.h:351-362) */
#define M_START_DONE	0x221002u
#define M_STOP_DONE	0x221003u
#define M_FLUSH_DONE	0x221006u
#define M_EBD		0x221007u
#define M_FBD		0x221008u
#define M_PROP_INFO	0x221009u
#define M_RELRES_DONE	0x22100au
#define M_RELBUF_DONE	0x22100cu
#define P_FRAME_SIZE	0x1001u		/* HFI_PROPERTY_PARAM_FRAME_SIZE {buffer_type, width, height} (helper.h:212, 636) */
#define P_FMT_SEL	0x1003u		/* UNCOMPRESSED_FORMAT_SELECT {buffer_type, format} (helper.h:216, 723) */
#define P_WORK_MODE	0x1015u		/* HFI_PROPERTY_PARAM_WORK_MODE {mode} (helper.h:242, 646) */
#define P_WORK_ROUTE	0x1017u		/* HFI_PROPERTY_PARAM_WORK_ROUTE {route} (helper.h:246) */
#define P_MULTI_STREAM	0x1003001u	/* HFI_PROPERTY_PARAM_VDEC_MULTI_STREAM {buffer_type, enable} (helper.h:260, 546) */
#define P_BUF_COUNT	0x201001u	/* BUFFER_COUNT_ACTUAL {buffer_type, actual, min_host} (vidc_hfi.h:122, 239) */
#define P_BUF_SIZE_MIN	0x20100cu	/* BUFFER_SIZE_MINIMUM {buffer_type, size} (vidc_hfi.h:130, 245) */
#define P_BUF_REQ	0x202001u	/* CONFIG_BUFFER_REQUIREMENTS: 8 words per type (vidc_hfi.h:137, 250) */
#define P_PROFILE	0x1005u		/* in SEQUENCE_CHANGED: PROFILE_LEVEL_CURRENT {profile, level} */
#define P_BITDEPTH	0x1003007u	/* {buffer_type, bit_depth} */
#define P_PICSTRUCT	0x1003009u	/* {progressive_only} */
#define P_COLOUR	0x100300au	/* {colour_space} */
#define P_DPB_COUNTS	0x100300bu	/* {max_dpb, max_ref, max_dec_buffering} */
#define P_ENTROPY	0x1204004u	/* HFI_PROPERTY_CONFIG_VDEC_ENTROPY {mode} (vidc_hfi.h:204) */
#define P_CROP		0x0700000eu	/* HFI_INDEX_EXTRADATA_INPUT_CROP {size, version, port, left, top, w, h} */
#define EV_SESSION_ERR	2u		/* HFI_EVENT_SESSION_ERROR (helper.h:77) */
#define EV_SEQ_CHANGED	0x1000003u	/* vidc_hfi.h:20-28: data1 0x1000001 sufficient, 0x1000002 insufficient */
#define EV_RELEASE_REF	0x1000006u	/* {packet_buffer, extra_data_buffer, output_tag} (hfi_response_handler.c:359) */
#define EV_INSUFFICIENT	0x1000002u
#define B_INPUT		1u		/* HFI_BUFFER_* (helper.h:163-170) */
#define B_OUTPUT	2u
#define B_OUTPUT2	3u
#define F_EOS		0x1u		/* HFI_BUFFERFLAG_* (vidc_hfi.h:30-47) */
#define F_EOF		0x10u		/* ENDOFFRAME */
#define F_READONLY	0x200u		/* still a reference: the firmware gives it back with EV_RELEASE_REF */
#define F_DROP		0x20000000u
#define FLUSH_OUTPUT	0x1000002u	/* vidc_hfi.h:62-64 */
#define FLUSH_ALL	0x1000004u
#define CF_NV12		2u		/* HFI_COLOR_FORMAT_NV12, _UBWC 0x8002 (helper.h:685, 705) */
#define CF_NV12_UBWC	0x8002u
#define ERR_SCALE	0x1012u		/* the two non-fatal session errors (hfi_response_handler.c:426-431) */
#define ERR_UPSCALE	0x1013u

struct vbuf { uint32_t pa, size; int fw; };	/* fw: queued to the firmware (ETB/FTB not yet done) */
#define NAU	4096
static struct {
	uint64_t base, top, lim;	/* bump allocator window */
	uint32_t req[16][8];		/* last BUFFER_REQUIREMENTS: type, size, region, min, min_host, actual, contig, align */
	int nreq;
	struct vbuf in[8], dpb[32], opb[32], intl[16];
	uint32_t intl_type[16];
	int nin, ndpb, nopb, nintl;
	int split, w, h, cx, cy, cw, ch;	/* coded size; visible crop */
	uint32_t ystride, yscan;
	int frames, want, eos, err, hold, stopping, reconfig, rw, rh, rcount;
	double cp_sum, cp_max, t_first, t_last;	/* v4a0: frame copy-out time (pread of the window), first/last FBD */
	double cp_core[8];	/* v4a0: copy time per CPU (0-5 A55, 6-7 A76), counted only when the copy stayed on one core */
	int n_core[8], n_moved;
	unsigned acts;		/* msgq packets handled: progress for the timeout */
	int started;		/* 4a: START sent (vdec_close flushes and stops) */
	int hevc;		/* 4e: an HEVC session (SESSION_INIT codec) */
	uint32_t isz;		/* 4a: input buffer size */
	FILE *out;
	uint8_t *fbuf;
	size_t fbuf_len;
} dc;

static int sess_frames(void)
{
	return dc.started && !dc.stopping ? dc.frames : -1;
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
	return (v + a - 1) / a * a;
}

/* /dev/pmem on the fixed region: 32-bit aligned, 64 KB per call as init's zeroing */
static int big_io(uint64_t pa, void *buf, size_t len, int write)
{
	for (size_t o = 0; o < len; o += 0x10000) {
		size_t n = len - o < 0x10000 ? len - o : 0x10000;
		if (write ? pm_wr(pa + o, (uint8_t *)buf + o, n) : pm_rd(pa + o, (uint8_t *)buf + o, n))
			return -1;
	}
	return 0;
}

/* a zeroed buffer from the window (downstream msm_comm_smem_alloc: zeroed, uncached); 0 = refused */
static uint32_t balloc(uint32_t size, const char *what)
{
	static uint8_t zeros[0x10000];
	uint64_t a = (dc.top + 0xfff) & ~0xfffull;
	size = align_up(size, 0x1000);
	if (!size || a + size > dc.lim) {
		printf("venus: STOP: %s: %#x bytes don't fit the buffer window (%#llx used of %#llx-%#llx)\n", what, size,
		       (unsigned long long)(dc.top - dc.base), (unsigned long long)dc.base, (unsigned long long)dc.lim);
		return 0;
	}
	for (uint32_t o = 0; o < size; o += sizeof(zeros))
		if (pm_wr(a + o, zeros, size - o < sizeof(zeros) ? size - o : sizeof(zeros)))
			return 0;
	dc.top = a + size;
	pm_step("balloc %s %#llx size %#x", what, (unsigned long long)a, size);
	return (uint32_t)a;
}

static int sess_cmd(uint32_t type)	/* create_pkt_cmd_session_cmd {size 12, type, session_id} (hfi_packetization.c:437) */
{
	uint32_t c[3] = { 12, type, SESSION_ID };
	return q_write(c);
}

/* create_pkt_cmd_session_set_property (hfi_packetization.c:983): {size, type, sid, 1, prop, body...}; no reply on
 * success, a bad one comes back as EVENT_NOTIFY SESSION_ERROR: drain 20 ms after each */
static int set_prop(uint32_t prop, uint32_t a, uint32_t b, uint32_t c, unsigned nbody, const char *what)
{
	uint32_t p[8] = { 20 + 4 * nbody, S_SET_PROP, SESSION_ID, 1, prop, a, b, c };
	printf("venus: set %s (%#x):", what, prop);
	for (unsigned k = 0; k < nbody; k++)
		printf(" %#x", p[5 + k]);
	printf("\n");
	if (q_write(p) || wait_msg(0, 20))
		return -1;
	return dc.err ? -1 : 0;
}

static const char *bname(uint32_t t)
{
	static const char *const n[] = { "?", "input", "output", "output2", "persist", "persist_1", "scratch",
		"scratch_1", "scratch_2", "recon", "extra_out", "extra_out2", "extra_in" };
	return t < 13 ? n[t] : "?";
}

/* create_pkt_cmd_session_get_buf_req (hfi_packetization.c:927) -> PROPERTY_INFO, N x hfi_buffer_requirements
 * (hfi_process_sess_get_prop_buf_req, hfi_response_handler.c:1309-1340) */
static int get_bufreq(void)
{
	uint32_t g[5] = { 20, S_GET_PROP, SESSION_ID, 1, P_BUF_REQ };
	if (q_write(g) || wait_msg(M_PROP_INFO, 1000))
		return -1;
	if (pkt[0] < 20 + 32 || pkt[4] != P_BUF_REQ) {
		printf("venus: STOP: property info without buffer requirements (size %u, property %#x)\n", pkt[0], pkt[4]);
		return -1;
	}
	dc.nreq = (int)((pkt[0] - 20) / 32);
	if (dc.nreq > 16)
		dc.nreq = 16;
	printf("venus: buffer requirements (%d):  type        size     region  min  min_host actual contig align\n",
	       dc.nreq);
	for (int i = 0; i < dc.nreq; i++) {
		memcpy(dc.req[i], pkt + 5 + 8 * i, 32);
		uint32_t *r = dc.req[i];
		printf("venus:   %-10s %2u %9u %9u %4u %8u %6u %6u %5u\n", bname(r[0]), r[0], r[1], r[2], r[3], r[4], r[5],
		       r[6], r[7]);
	}
	return 0;
}

static uint32_t *req_of(uint32_t type)
{
	for (int i = 0; i < dc.nreq; i++)
		if (dc.req[i][0] == type)
			return dc.req[i];
	return NULL;
}

/* create_pkt_cmd_session_set_buffers / _release_buffers (hfi_packetization.c:658, 708), one buffer per packet as
 * set_internal_buf_on_fw (msm_vidc_common.c:3810-3830): OUTPUT/OUTPUT2 carry {addr, extradata addr} pairs */
static int buf_cmd(uint32_t type, uint32_t pa, uint32_t size, int release)
{
	int out = type == B_OUTPUT || type == B_OUTPUT2;
	uint32_t p[10] = { out ? 40u : 36u, release ? S_REL_BUFS : S_SET_BUFS, SESSION_ID, type, size, 0,
		release ? 1u : size, 1, pa, 0 };	/* word 6: min_buffer_size (set) / response_req (release) */
	return q_write(p);
}

static int ftb(uint32_t stream, struct vbuf *b)	/* create_pkt_cmd_session_ftb (hfi_packetization.c:890) */
{
	if (dc.stopping || dc.hold)
		return 0;
	uint32_t p[11] = { 44, S_FTB, SESSION_ID, stream, 0, b->size, 0, b->pa, b->pa, 0, 0 };	/* output_tag = addr */
	b->fw = 1;
	return q_write(p);
}

static void ftb_all(void)	/* msm_comm_queue_output_buffers (msm_vidc_common.c:2015-2060) + the client's buffers */
{
	for (int i = 0; dc.split && i < dc.ndpb; i++)
		if (!dc.dpb[i].fw)
			ftb(0, &dc.dpb[i]);
	for (int i = 0; i < dc.nopb; i++)
		if (!dc.opb[i].fw)
			ftb(dc.split ? 1 : 0, &dc.opb[i]);
}

static struct vbuf *find_buf(struct vbuf *v, int n, uint32_t pa)
{
	for (int i = 0; i < n; i++)
		if (v[i].pa == pa)
			return &v[i];
	return NULL;
}

static double now_s(void);

/* the CPU this thread runs on: xnu keeps it in TPIDR_EL0's low 12 bits (osfmk/arm64/machine_machdep.h:34-42) */
static unsigned cpu_now(void)
{
	uint64_t t;
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(t));
	return (unsigned)(t & 0xfff);
}

void (*vdec_sink)(const struct vdec_frame *f);
int (*vdec_place)(const struct vdec_frame *f, struct vdec_dest *d);

/* vdcredit: the visible rect of the picture at pa into d's planes (vdec_place's rule: whole planes when the rows line
 * up, else row by row) */
static int place_io(uint64_t pa, const struct vdec_dest *d)
{
	size_t st = dc.ystride, ylen = st * dc.yscan, w = ((size_t)dc.cw + 3) & ~(size_t)3, crows = ((size_t)dc.ch + 1) / 2;
	uint64_t y0 = pa + (size_t)dc.cy * st + (size_t)dc.cx;
	uint64_t c0 = pa + ylen + (size_t)(dc.cy / 2) * st + (size_t)(dc.cx & ~1);
	int whole = !dc.cx && d->ybpr == st && d->cbpr == st;
	if (whole)
		return big_io(y0, d->y, (size_t)dc.ch * st, 0) || big_io(c0, d->c, crows * st, 0) ? -1 : 0;
	for (size_t r = 0; r < (size_t)dc.ch; r++)
		if (pm_rd(y0 + r * st, d->y + r * d->ybpr, w))
			return -1;
	for (size_t r = 0; r < crows; r++)
		if (pm_rd(c0 + r * st, d->c + r * d->cbpr, w))
			return -1;
	return 0;
}

/* one decoded frame: crop to the visible rect, append NV12 (Y w*h, then CbCr w*h/2) to OUT, CRC32 of that luma;
 * 4a: with vdec_sink set, the whole buffer as read goes there instead */
static void frame_out(uint32_t pa, const uint32_t *f)
{
	size_t ylen = (size_t)dc.ystride * dc.yscan, need = ylen + (size_t)dc.ystride * (dc.yscan / 2);
	if (dc.fbuf_len < need) {
		free(dc.fbuf);
		dc.fbuf = malloc(need);
		dc.fbuf_len = dc.fbuf ? need : 0;
	}
	double c0 = now_s();
	static int nocopy = -1;	/* v4a0: VENUSCTL_NOCOPY=1 skips the copy-out (pure decode speed; OUT gets nothing) */
	if (nocopy < 0)
		nocopy = getenv("VENUSCTL_NOCOPY") && atoi(getenv("VENUSCTL_NOCOPY"));
	if (nocopy && dc.fbuf) {
		if (!dc.frames)
			dc.t_first = c0;
		dc.t_last = c0;
		dc.frames++;
		return;
	}
	struct vdec_frame vf = { .stride = dc.ystride, .scanlines = dc.yscan,
		.cx = dc.cx, .cy = dc.cy, .cw = dc.cw, .ch = dc.ch, .ts = ((uint64_t)f[6] << 32) | f[7],
		.flags = f[8], .filled = f[13], .fbd_w = f[15], .fbd_h = f[16], .fbd_x = f[17], .fbd_y = f[18],
		.pic_type = f[22], .index = dc.frames };
	struct vdec_dest d;
	int placed = vdec_sink && vdec_place && vdec_place(&vf, &d) == 1;	/* vdcredit */
	unsigned k0 = cpu_now();
	int rd = placed ? place_io(pa, &d) : !dc.fbuf || big_io(pa, dc.fbuf, need, 0);
	double c1 = now_s();
	unsigned k1 = cpu_now();
	if (k0 == k1 && k0 < 8) {
		dc.cp_core[k0] += c1 - c0;
		dc.n_core[k0]++;
	} else
		dc.n_moved++;
	dc.cp_sum += c1 - c0;
	if (c1 - c0 > dc.cp_max)
		dc.cp_max = c1 - c0;
	if (!dc.frames)
		dc.t_first = c0;
	dc.t_last = c1;
	if (rd) {
		printf("venus: frame %d: can't read %#x%s\n", dc.frames, pa, placed ? " (into a credited surface)" : "");
		if (placed)
			vdec_place(NULL, NULL);
		return;
	}
	if (vdec_sink) {
		vf.buf = placed ? NULL : dc.fbuf, vf.len = placed ? 0 : need, vf.placed = placed;
		vf.copy_ms = 1e3 * (c1 - c0);
		vdec_sink(&vf);
		dc.frames++;
		return;
	}
	uint8_t *vis = malloc((size_t)dc.cw * dc.ch * 3 / 2);
	if (!vis)
		return;
	for (int y = 0; y < dc.ch; y++)
		memcpy(vis + (size_t)y * dc.cw, dc.fbuf + (size_t)(y + dc.cy) * dc.ystride + dc.cx, dc.cw);
	for (int y = 0; y < dc.ch / 2; y++)
		memcpy(vis + (size_t)dc.cw * dc.ch + (size_t)y * dc.cw,
		       dc.fbuf + ylen + (size_t)(y + dc.cy / 2) * dc.ystride + (dc.cx & ~1), dc.cw);
	uint32_t crc = crc32_buf(vis, (size_t)dc.cw * dc.ch);
	if (dc.out)
		fwrite(vis, 1, (size_t)dc.cw * dc.ch * 3 / 2, dc.out);
	free(vis);
	printf("venus: frame %d: FBD flags %#x filled %u ts %llu us, %ux%u at %u,%u, pic type %#x, luma crc32 %08x\n",
	       dc.frames, f[8], f[13], ((unsigned long long)f[6] << 32) | f[7], f[15], f[16], f[17], f[18], f[22], crc);
	dc.frames++;
}

/* SEQUENCE_CHANGED payload, as hfi_process_sess_evt_seq_changed (hfi_response_handler.c:117-356) */
static void seq_changed(const uint32_t *p, int n)
{
	unsigned nprop = p[5];
	int k = 6;
	dc.rw = dc.w;
	dc.rh = dc.h;
	dc.rcount = 0;
	while (nprop-- && k < n) {
		uint32_t id = p[k++];
		static const struct { uint32_t id, words; } sz[] = { { P_FRAME_SIZE, 3 }, { P_PROFILE, 2 },
			{ P_BITDEPTH, 2 }, { P_PICSTRUCT, 1 }, { P_DPB_COUNTS, 3 }, { P_COLOUR, 1 }, { P_ENTROPY, 1 },
			{ P_BUF_REQ, 8 }, { P_CROP, 7 } };
		unsigned w = 0;
		for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; i++)
			if (sz[i].id == id)
				w = sz[i].words;
		if (!w || k + (int)w > n) {
			printf("venus: seq changed: property %#x unknown or short, rest skipped\n", id);
			break;
		}
		if (id == P_FRAME_SIZE) {	/* hfi_frame_size {buffer_type, width, height} (vidc_hfi_helper.h:636) */
			dc.rw = (int)p[k + 1];
			dc.rh = (int)p[k + 2];
			printf("venus: seq changed: frame %ux%u (buffer type %u)\n", p[k + 1], p[k + 2], p[k]);
		} else if (id == P_BUF_REQ) {
			dc.rcount = (int)p[k + 3];
			printf("venus: seq changed: capture buffers min %u (type %u size %u)\n", p[k + 3], p[k], p[k + 1]);
		} else if (id == P_CROP) {
			printf("venus: seq changed: crop %u,%u %ux%u\n", p[k + 3], p[k + 4], p[k + 5], p[k + 6]);
			if (p[k + 5] && p[k + 6]) {
				dc.cx = (int)p[k + 3];
				dc.cy = (int)p[k + 4];
				dc.cw = (int)p[k + 5];
				dc.ch = (int)p[k + 6];
			}
		} else {
			printf("venus: seq changed: property %#x:", id);
			for (unsigned i = 0; i < w; i++)
				printf(" %u", p[k + i]);
			printf("\n");
		}
		k += (int)w;
	}
}

/* every msgq packet during decode (wait_msg on_pkt): EBD, FBD, EVENT_NOTIFY */
static int dec_pkt(int n)
{
	uint32_t p[64];	/* the SEQUENCE_CHANGED event is 44 words (crop extradata 0x700000e at word 31) */
	memcpy(p, pkt, 4u * (unsigned)(n < 64 ? n : 64));
	dc.acts++;
	if (p[1] == M_EBD && n >= 10) {	/* hfi_msg_session_empty_buffer_done_packet (vidc_hfi.h:604) */
		struct vbuf *b = find_buf(dc.in, dc.nin, p[7]);
		if (b)
			b->fw = 0;
		if (!vc_quiet || p[3] || !b)
			printf("venus: EBD tag %u buf %#x error %#x filled %u flags %#x%s\n", p[6], p[7], p[3], p[5], p[9],
			       b ? "" : " (not ours)");
	} else if (p[1] == M_FBD && n >= 25) {	/* hfi_msg_session_fbd_uncompressed_plane0_packet (vidc_hfi.h:641) */
		int frame = dc.split ? p[3] == 1 : p[3] == 0;
		struct vbuf *b = frame ? find_buf(dc.opb, dc.nopb, p[23]) : find_buf(dc.dpb, dc.ndpb, p[23]);
		if (b)
			b->fw = 0;
		if (p[5] || !b)
			printf("venus: FBD stream %u buf %#x error %#x%s\n", p[3], p[23], p[5], b ? "" : " (not ours)");
		if (frame && p[13] && !(p[8] & F_DROP) && dc.frames < dc.want && !dc.hold)
			frame_out(p[23], p);
		else if (!vc_quiet || frame)
			printf("venus: FBD stream %u buf %#x flags %#x filled %u (%s)\n", p[3], p[23], p[8], p[13],
			       frame ? "no frame" : "dpb back");
		if (p[8] & F_EOS)
			dc.eos = 1;
		/* back to the firmware unless it still references it (READONLY: EV_RELEASE_REF later) */
		if (b && !(p[8] & F_READONLY) && !dc.eos && dc.frames < dc.want)
			ftb(dc.split && frame ? 1 : 0, b);
	} else if (p[1] == M_EVENT && n >= 6) {	/* hfi_msg_event_notify_packet (vidc_hfi_helper.h:1044) */
		if (p[3] == EV_SESSION_ERR) {
			printf("venus: session error %#x data2 %#x%s\n", p[4], p[5],
			       p[4] == ERR_SCALE || p[4] == ERR_UPSCALE ? " (non-fatal)" : "");
			if (p[4] != ERR_SCALE && p[4] != ERR_UPSCALE)
				dc.err = (int)p[4];
		} else if (p[3] == EV_SEQ_CHANGED) {
			printf("venus: SEQUENCE_CHANGED %s, %u properties\n", p[4] == EV_INSUFFICIENT ? "insufficient" :
			       "sufficient", p[5]);
			seq_changed(p, n < 64 ? n : 64);
			dc.reconfig = p[4] == EV_INSUFFICIENT ? 2 : 1;
		} else if (p[3] == EV_RELEASE_REF && n >= 9) {
			struct vbuf *b = find_buf(dc.opb, dc.nopb, p[6]);
			if (!vc_quiet || !b)
				printf("venus: release buffer reference %#x%s\n", p[6], b ? "" : " (not ours)");
			if (b && !b->fw && !dc.eos && dc.frames < dc.want)
				ftb(dc.split ? 1 : 0, b);
		} else {
			printf("venus: event %#x data %#x %#x\n", p[3], p[4], p[5]);
		}
	}
	return 0;
}

/* the client's output buffers: OUTPUT2 (split) or OUTPUT (primary); DPBs only in split. count from the requirement,
 * at least the H.264 output_min_count 8 (msm_vdec.c:515); DPB count = FW count (msm_vidc_common.c:3691) */
static int alloc_outputs(int count)
{
	uint32_t *ro = req_of(B_OUTPUT), *r2 = req_of(B_OUTPUT2), *rq = dc.split ? r2 : ro;
	if (!rq || !rq[1] || (dc.split && (!ro || !ro[1]))) {
		printf("venus: STOP: no buffer requirement for %s\n", dc.split ? "output/output2" : "output");
		return -1;
	}
	/* v3: after SEQUENCE_CHANGED min drops to 5 but min_host stays 18 and the firmware wants all of them
	 * ("h264Dec_Continue(224): Not enough output2 buffers!"): take the largest of ours, min and min_host */
	int n = count > (int)rq[3] ? count : (int)rq[3];
	if (n < (int)rq[4])
		n = (int)rq[4];
	if (getenv("VENUSCTL_NOUT"))
		n = atoi(getenv("VENUSCTL_NOUT"));
	if (n < 1 || n > 32)
		n = n < 1 ? 1 : 32;
	size_t need = (size_t)dc.ystride * dc.yscan * 3 / 2;
	if (rq[1] < need)
		printf("venus: note: %s size %u is below our NV12 layout's %zu (stride %u, scanlines %u): check it\n",
		       dc.split ? "output2" : "output", rq[1], need, dc.ystride, dc.yscan);
	if (dc.split) {
		int nd = count > (int)ro[3] ? count : (int)ro[3];
		if (nd < (int)ro[4])
			nd = (int)ro[4];
		/* set_output_buffers (msm_vidc_common.c:3693-3703): BUFFER_SIZE_MINIMUM, then one buffer each */
		if (set_prop(P_BUF_SIZE_MIN, B_OUTPUT, ro[1], 0, 2, "dpb size minimum"))
			return -1;
		if (set_prop(P_BUF_COUNT, B_OUTPUT, (uint32_t)nd, (uint32_t)nd, 3, "dpb count"))
			return -1;
		dc.ndpb = 0;
		for (int i = 0; i < nd && i < 32; i++) {
			uint32_t pa = balloc(ro[1], "dpb");
			if (!pa)
				return -1;
			dc.dpb[dc.ndpb++] = (struct vbuf){ pa, ro[1], 0 };
			if (getenv("VENUSCTL_DPB_SETBUF") && buf_cmd(B_OUTPUT, pa, ro[1], 0))
				return -1;
		}
		printf("venus: %d DPBs of %u bytes (UBWC)\n", dc.ndpb, ro[1]);
	}
	uint32_t sz = rq[1] > need ? rq[1] : (uint32_t)need;
	/* v3-v3d: without this the firmware keeps the host's output2 size at 0 (dec[0x14c], set only by this property
	 * for type 3: venus.b02 0xdf2c4) and its CONTINUE check fails as "Not enough output2 buffers!" (0xe0bd0-0xe0bf4);
	 * downstream sends it in start_streaming before the DPBs and CONTINUE (msm_vidc.c:1183-1196) */
	if (dc.split && set_prop(P_BUF_SIZE_MIN, B_OUTPUT2, sz, 0, 2, "output2 size minimum"))
		return -1;
	if (set_prop(P_BUF_COUNT, dc.split ? B_OUTPUT2 : B_OUTPUT, (uint32_t)n, (uint32_t)n, 3, "output count"))
		return -1;
	dc.nopb = 0;
	for (int i = 0; i < n; i++) {
		uint32_t pa = balloc(sz, "output");
		if (!pa)
			return -1;
		dc.opb[dc.nopb++] = (struct vbuf){ pa, sz, 0 };
	}
	printf("venus: %d output buffers of %u bytes (linear NV12, %s)\n", dc.nopb, sz, dc.split ? "OUTPUT2" : "OUTPUT");
	return 0;
}

static void nv12_layout(void)	/* Venus NV12: Y stride 128, scanlines 32 (msm_media_info.h COLOR_FMT_NV12) */
{
	dc.ystride = align_up((uint32_t)dc.w, 128);
	dc.yscan = align_up((uint32_t)dc.h, 32);
	if (getenv("VENUSCTL_YSTRIDE"))
		dc.ystride = (uint32_t)strtoul(getenv("VENUSCTL_YSTRIDE"), NULL, 0);
	if (getenv("VENUSCTL_YSCAN"))
		dc.yscan = (uint32_t)strtoul(getenv("VENUSCTL_YSCAN"), NULL, 0);
}

/* reconfig after SEQUENCE_CHANGED insufficient: the client's streamoff/s_fmt/reqbufs/streamon on CAPTURE (flush
 * output, new OUTPUT2 size, requirements, new buffers), then SESSION_CONTINUE (msm_vidc_common.c:6129-6170) */
static int reconfig(void)
{
	int kind = dc.reconfig;
	dc.reconfig = 0;
	if (kind == 1) {	/* sufficient: continue, then the DPBs go back (msm_vidc_common.c:6149-6165) */
		if (sess_cmd(S_CONTINUE))
			return -1;
		ftb_all();
		return 0;
	}
	printf("venus: reconfig to %dx%d (capture min %d)\n", dc.rw, dc.rh, dc.rcount);
	dc.hold = 1;
	uint32_t fl[4] = { 16, S_FLUSH, SESSION_ID, FLUSH_OUTPUT };	/* create_pkt_cmd_session_flush (packetization.c:945) */
	if (q_write(fl) || wait_msg(M_FLUSH_DONE, 1000))
		return -1;
	if (dc.rw != dc.w || dc.rh != dc.h) {
		dc.w = dc.rw;
		dc.h = dc.rh;
		if (dc.cw > dc.w || dc.ch > dc.h) {
			dc.cx = dc.cy = 0;
			dc.cw = dc.w;
			dc.ch = dc.h;
		}
		nv12_layout();
	}
	if (set_prop(P_FRAME_SIZE, dc.split ? B_OUTPUT2 : B_OUTPUT, (uint32_t)dc.w, (uint32_t)dc.h, 3, "output frame size"))
		return -1;
	if (get_bufreq() || alloc_outputs(dc.rcount > 8 ? dc.rcount : 8))
		return -1;
	/* v3b: the firmware counts the output2 buffers it holds when CONTINUE arrives (after the flush it holds none:
	 * "h264Dec_Continue(224): Not enough output2 buffers!" with 18 set): queue them first, as the client's
	 * streamon/qbuf comes before its SESSION_CONTINUE decoder command (msm_vidc_common.c:4150) */
	dc.hold = 0;
	ftb_all();
	if (sess_cmd(S_CONTINUE))
		return -1;
	return 0;
}

static double now_s(void)	/* lat2: monotonic (was gettimeofday: a clock step moved the 3 s/15 s decode limits) */
{
	return mono_ms() / 1e3;
}

/* the downstream close: flush all, stop, release resources, release internal buffers, session end
 * (msm_comm_try_state STOP_DONE -> CLOSE_DONE, msm_vidc_common.c:3990-4060; release 4900-4960). Notes, not STOPs:
 * every step is tried; 1 = something didn't answer. */
static int dec_close(int started)
{
	int bad = 0;
	dc.stopping = 1;
	if (fw_dead) {
		printf("venus: firmware sys error: no close sequence (shutdown stops it)\n");
		on_pkt = NULL;
		return 1;
	}
	wait_soft = 1;
	if (started) {
		uint32_t fl[4] = { 16, S_FLUSH, SESSION_ID, FLUSH_ALL };	/* msm_comm_flush (msm_vidc_common.c:5379) */
		bad |= q_write(fl) || wait_msg(M_FLUSH_DONE, 1000);
		bad |= sess_cmd(S_STOP) || wait_msg(M_STOP_DONE, 1000);	/* msm_vidc_stop (3485) */
		bad |= sess_cmd(S_REL_RES) || wait_msg(M_RELRES_DONE, 1000);	/* msm_vidc_release_res (3518) */
	}
	for (int i = dc.nintl; i-- > 0;)	/* msm_comm_release_scratch/persist_buffers: response_req 1 */
		bad |= buf_cmd(dc.intl_type[i], dc.intl[i].pa, dc.intl[i].size, 1) || wait_msg(M_RELBUF_DONE, 1000);
	for (int i = 0; getenv("VENUSCTL_DPB_SETBUF") && i < dc.ndpb; i++)
		bad |= buf_cmd(B_OUTPUT, dc.dpb[i].pa, dc.dpb[i].size, 1) || wait_msg(M_RELBUF_DONE, 1000);
	dc.nintl = 0;
	bad |= sess_cmd(0x10008) || wait_msg(0x20007, 1000);	/* SYS_SESSION_END (msm_comm_session_close, 3547) */
	if (!bad)
		printf("venus: SESSION_END_DONE error %#x\n", pkt[0] >= 16 ? pkt[3] : 0);
	wait_soft = 0;
	on_pkt = NULL;
	wait_msg(0, 50);
	return bad;
}

/* 4a: decode's session in steps. prepare: SYS_INIT/debug config (sys_up), buffer window, layout from the SPS */
int vdec_prepare(uint32_t debug, uint64_t bufphys, uint64_t mb, int want, const struct h264_sps *sps, int hevc)
{
	uint8_t *fbuf = dc.fbuf;	/* the copy-out buffer outlives a session (venusd runs many) */
	size_t fbuf_len = dc.fbuf_len;
	memset(&dc, 0, sizeof(dc));
	dc.fbuf = fbuf;
	dc.fbuf_len = fbuf_len;
	dc.w = sps->w;
	dc.h = sps->h;
	dc.cx = sps->cx;
	dc.cy = sps->cy;
	dc.cw = sps->cw;
	dc.ch = sps->ch;
	dc.hevc = hevc;
	struct vstate v;
	if (sys_up(&v, debug, "before decode"))
		return 1;
	/* the buffer window: right after the queue block, 1 MB aligned, inside the free gap (range_ok) */
	uint64_t base = bufphys ? bufphys : (v.qkind == 2 ? (v.q + QSHARED + SFR_SIZE + 0xfffff) & ~0xfffffull : 0);
	if (!base) {
		printf("venus: refused: queues came from PMEM_ALLOC32: give --bufphys ADDR\n");
		return 1;
	}
	if (range_ok(base, mb << 20, "buffer window"))
		return 1;
	if (base < v.q + QSHARED + SFR_SIZE && base + (mb << 20) > v.q) {
		printf("venus: refused: buffer window %#llx+%lluM overlaps the queue block %#llx\n", (unsigned long long)base,
		       (unsigned long long)mb, (unsigned long long)v.q);
		return 1;
	}
	dc.base = dc.top = base;
	dc.lim = base + (mb << 20);
	dc.want = want;
	dc.split = !getenv("VENUSCTL_PRIMARY");
	nv12_layout();
	return 0;
}

/* redfin --stop-at STAGE (venusctl decode): vdec_start returns 3 right after STAGE, decode() then only watches the
 * queues for 2 s and closes. Stages in order: sessinit props workmode intl alloc load start ftb (--dry = ftb). */
const char *vc_stop_at;

static int stop_here(const char *stage)
{
	pm_step("stage %s done", stage);
	printf("venus: stage %s done\n", stage);
	if (vc_stop_at && !strcmp(vc_stop_at, stage)) {
		printf("venus: --stop-at %s: no further steps\n", stage);
		return 1;
	}
	return 0;
}

/* start: session init, the downstream decoder sequence, buffers, START, FTBs (see vdec_start in venus_core.h) */
int vdec_start(uint32_t min_in)
{
	printf("venus: buffer window %#llx-%#llx, %s mode, NV12 stride %u scanlines %u\n", (unsigned long long)dc.base,
	       (unsigned long long)dc.lim, dc.split ? "split (OUTPUT UBWC DPB + OUTPUT2 linear)" : "primary (OUTPUT linear)",
	       dc.ystride, dc.yscan);

	/* SYS_SESSION_INIT, decoder, H.264 (as session) or 4e HEVC (HFI_VIDEO_CODEC_HEVC, vidc_hfi_helper.h:83) */
	uint32_t si[5] = { 20, 0x10007, SESSION_ID, 2, dc.hevc ? VD_HFI_CODEC_HEVC : 2 };
	if (dc.hevc)
		printf("venus: HEVC decoder session (codec %#x)\n", VD_HFI_CODEC_HEVC);
	if (q_write(si) || wait_msg(0x20006, 1000))
		return 1;
	printf("venus: SESSION_INIT_DONE error %#x\n", pkt[3]);
	if (pkt[3])
		return 1;
	on_pkt = dec_pkt;
	if (stop_here("sessinit"))
		return 3;
	uint32_t W = (uint32_t)dc.w, H = (uint32_t)dc.h, ob = dc.split ? B_OUTPUT2 : B_OUTPUT;
	/* properties in the client's order (msm_vdec.c line of each) */
	if (set_prop(P_FRAME_SIZE, B_INPUT, W, H, 3, "input frame size") ||	/* s_fmt OUTPUT, msm_vdec.c:735-742 */
	    (dc.split && (set_prop(P_FMT_SEL, B_OUTPUT, CF_NV12_UBWC, 0, 2, "dpb format NV12_UBWC") ||	/* 1159 */
			  set_prop(P_MULTI_STREAM, B_OUTPUT2, 1, 0, 2, "output2 on") ||		/* 1171-1180 */
			  set_prop(P_MULTI_STREAM, B_OUTPUT, 0, 0, 2, "output off"))) ||		/* 1184-1193 */
	    set_prop(P_FRAME_SIZE, ob, W, H, 3, "output frame size") ||		/* 1197-1210 (msm_vdec.c:657-667) */
	    set_prop(P_FMT_SEL, ob, CF_NV12, 0, 2, "output format NV12") ||	/* s_fmt CAPTURE, msm_vdec.c:631 */
	    get_bufreq())
		return 2;
	if (stop_here("props"))
		return 3;
	uint32_t *ri = req_of(B_INPUT);
	int nin = ri && (int)ri[5] > 4 ? (int)ri[5] : 4;	/* input_min_count 4 (msm_vdec.c:514) */
	if (nin > 8)
		nin = 8;
	/* msm_vidc_update_host_buff_counts (msm_vidc_common.c:3218, 3252); work mode/route for VPU5 H.264 progressive
	 * (msm_vidc_clocks.c:1366-1420: mode 1 up to 1280x720, else 2; 1215-1290: route 2) */
	if (set_prop(P_BUF_COUNT, B_INPUT, (uint32_t)nin, (uint32_t)nin, 3, "input count") ||
	    set_prop(P_WORK_MODE, W * H <= 1280 * 720 ? 1 : 2, 0, 0, 1, "work mode") ||
	    set_prop(P_WORK_ROUTE, getenv("VENUSCTL_ROUTE") ? (uint32_t)atoi(getenv("VENUSCTL_ROUTE")) : 2, 0, 0, 1,
		     "work route") ||
	    get_bufreq())	/* start_streaming: msm_comm_try_get_bufreqs again (scratch depends on the work mode) */
		return 2;
	if (stop_here("workmode"))
		return 3;
	/* internal buffers: scratch, scratch_1, scratch_2, persist, persist_1, count_actual each
	 * (msm_comm_set_scratch_buffers 5150, msm_comm_set_persist_buffers 5230, allocate_and_set_internal_bufs 3883) */
	static const uint32_t itypes[] = { 6, 7, 8, 4, 5 };
	for (unsigned t = 0; t < 5; t++) {
		uint32_t type = itypes[t], *r = req_of(type), cnt = r ? r[5] : 0;
		if (r && r[1] && !cnt && r[3]) {	/* guess: downstream would set none; the minimum is safer */
			printf("venus: note: %s count_actual 0, using count_min %u\n", bname(type), r[3]);
			cnt = r[3];
		}
		for (uint32_t i = 0; r && r[1] && i < cnt && dc.nintl < 16; i++) {
			uint32_t pa = balloc(r[1], bname(type));
			if (!pa || buf_cmd(type, pa, r[1], 0))
				return 2;
			dc.intl[dc.nintl] = (struct vbuf){ pa, r[1], 1 };
			dc.intl_type[dc.nintl++] = type;
			printf("venus: %s buffer %#x (%u bytes) set\n", bname(type), pa, r[1]);
		}
	}
	if (wait_msg(0, 20) || dc.err)
		return 2;
	if (stop_here("intl"))
		return 3;
	ri = req_of(B_INPUT);
	uint32_t isz = ri && ri[1] ? ri[1] : 0x100000;
	if (min_in > isz)	/* venusctl: its longest access unit; venusd: VENUSD_IN_KB */
		isz = min_in;
	for (dc.nin = 0; dc.nin < nin; dc.nin++) {
		uint32_t pa = balloc(isz, "input");
		if (!pa)
			return 2;
		dc.in[dc.nin] = (struct vbuf){ pa, align_up(isz, 0x1000), 0 };
	}
	dc.isz = align_up(isz, 0x1000);
	if (alloc_outputs(8))
		return 2;
	printf("venus: buffers %#llx-%#llx (%llu KB of the window)\n", (unsigned long long)dc.base,
	       (unsigned long long)dc.top, (unsigned long long)((dc.top - dc.base) >> 10));
	if (stop_here("alloc"))
		return 3;
	/* msm_vidc_load_resources (msm_vidc_common.c:3419), msm_vidc_start (3452) -> START_DONE */
	if (sess_cmd(S_LOAD_RES) || wait_msg(M_LOAD_DONE, 1000))
		return 2;
	if (stop_here("load"))
		return 3;
	if (sess_cmd(S_START))
		return 2;
	dc.started = 1;
	if (wait_msg(M_START_DONE, 1000))
		return 2;
	printf("venus: START_DONE error %#x\n", pkt[0] >= 16 ? pkt[3] : 0);
	if (stop_here("start"))
		return 3;
	ftb_all();
	if (stop_here("ftb"))
		return 3;
	return 0;
}

static struct vbuf *free_in(void)
{
	for (int i = 0; i < dc.nin; i++)
		if (!dc.in[i].fw)
			return &dc.in[i];
	return NULL;
}

int vdec_can_etb(void)
{
	return free_in() && !dc.hold;
}

/* ETB one AU into a free input buffer (create_pkt_cmd_session_etb_decoder, packetization.c:820), or the empty EOS
 * buffer (msm_vidc_send_pending_eos_buffers, msm_vidc_common.c:4083); input_tag n + 1 */
int vdec_etb(const uint8_t *au, uint32_t len, uint64_t ts, int eos, int n)
{
	struct vbuf *b = free_in();
	if (!b)
		return -1;
	uint32_t fl = F_EOF, fill = 0;
	if (!eos) {
		if (len > b->size)
			return -2;
		static uint8_t tmp[0x10000];
		for (uint32_t o = 0; o < len; o += sizeof(tmp)) {	/* 4-byte padded copy */
			uint32_t k = len - o < sizeof(tmp) ? len - o : sizeof(tmp);
			memset(tmp, 0, sizeof(tmp));
			memcpy(tmp, au + o, k);
			if (pm_wr(b->pa + o, tmp, align_up(k, 4)))
				return -1;
		}
		fill = len;
	} else {
		fl = F_EOS;
	}
	uint32_t e[15] = { 60, S_ETB, SESSION_ID, (uint32_t)(ts >> 32), (uint32_t)ts, fl, 0, 0, 0, b->size, fill,
		(uint32_t)n + 1, b->pa, 0, 0 };
	b->fw = 1;
	if (!vc_quiet)
		printf("venus: ETB %d: %u bytes at %#x flags %#x\n", n, fill, b->pa, fl);
	return q_write(e) ? -1 : 0;
}

int vdec_poll(unsigned ms)
{
	if (dc.reconfig && reconfig())
		return -1;
	return wait_msg(0, ms);
}

int vdec_close(void)
{
	return dec_close(dc.started);
}

void vdec_status(struct vdec_status *st)
{
	*st = (struct vdec_status){ .frames = dc.frames, .eos = dc.eos, .err = dc.err, .fw_dead = fw_dead,
		.started = dc.started, .acts = dc.acts, .cp_sum = dc.cp_sum, .cp_max = dc.cp_max, .in_size = dc.isz };
}

void vdec_fw_reset(void)
{
	fw_dead = 0;
}

void vdec_abort(void)
{
	uint8_t *fbuf = dc.fbuf;
	size_t fbuf_len = dc.fbuf_len;
	on_pkt = NULL;	/* before any queue drain: no FTB re-queue (doorbell) from a dead session's FBDs */
	wait_soft = 0;
	memset(&dc, 0, sizeof(dc));
	dc.fbuf = fbuf;
	dc.fbuf_len = fbuf_len;
}

void vdec_sfr(void)
{
	struct vstate v;
	if (state_load(&v) || !v.q)
		printf("venus: SFR: no queue block in %s\n", STATE);
	else
		print_sfr(v.q);
}

int decode(uint32_t debug, const char *in, const char *outp, int want, uint64_t bufphys, int hevc)
{
	size_t len = 0;
	uint8_t *s = slurp(in, &len);
	if (!s || !len) {
		printf("venus: refused: can't read %s\n", in);
		return 1;
	}
	static uint32_t off[NAU], alen[NAU];
	memset(&dc, 0, sizeof(dc));
	struct h264_sps sps = { 0, 0, 0, 0, 0, 0 };
	if (hevc < 0)	/* 4e: from the stream's first NAL header (venusctl --h264/--hevc decide) */
		hevc = hevc_detect(s, len);
	struct hevc_sps hs = { 0 };
	int nau = hevc ? split_aus_hevc(s, len, off, alen, NAU, &sps, &hs) : split_aus(s, len, off, alen, NAU, &sps);
	if (hevc && nau >= 0 && !hevc_main8(&hs)) {
		printf("venus: refused: HEVC profile %d, chroma format %d, %d/%d bit: 8-bit 4:2:0 Main only\n", hs.profile,
		       hs.chroma, hs.depth_y, hs.depth_c);
		return 1;
	}
	if (nau < 0) {
		nau = -nau - 1;
		printf("venus: no SPS parsed: default 1920x1088 (msm_vidc_internal.h:43-44), the firmware's"
		       " SEQUENCE_CHANGED decides\n");
		sps = (struct h264_sps){ .w = 1920, .h = 1088, .cw = 1920, .ch = 1080 };
	}
	printf("venus: %s: %zu bytes, %d access units%s\n", in, len, nau, hevc ? " (HEVC)" : "");
	if (!nau)
		return 1;
	uint64_t mb = getenv("VENUSCTL_BUF_MB") ? strtoull(getenv("VENUSCTL_BUF_MB"), NULL, 0) : 64;
	if (vdec_prepare(debug, bufphys, mb, want, &sps, hevc))
		return 1;
	dc.out = fopen(outp, "wb");
	if (!dc.out) {
		printf("venus: refused: can't write %s\n", outp);
		return 1;
	}
	uint32_t maxau = 0;
	for (int i = 0; i < nau; i++)
		if (alen[i] > maxau)
			maxau = alen[i];
	int rc = 1, st = vdec_start(maxau);
	if (st == 1)
		return 1;
	if (st == 3) {	/* --stop-at: watch the queues (and with --step the status lines) for 2 s, then close */
		double td = now_s();
		while (now_s() - td < 2 && !dc.err)
			if (wait_msg(0, 5))
				break;
		step_status(1);
		rc = dc.err ? 1 : 0;
		goto close;
	}
	if (st)
		goto close;

	/* one AU per free input buffer, then an empty EOS buffer; stop at N frames, EOS, an error, 3 s without a
	 * message or 15 s in all */
	int next = 0, eos_sent = 0;
	double t0 = now_s(), tact = t0;
	unsigned acts = 0;
	while (dc.frames < want && !dc.eos && !dc.err) {
		if (dc.reconfig && reconfig())
			goto close;
		if (vdec_can_etb() && (next < nau || !eos_sent)) {
			int eos = next >= nau;
			if (vdec_etb(eos ? NULL : s + off[next], eos ? 0 : alen[next], (uint64_t)next * 33333, eos, next))
				goto close;
			eos_sent |= eos;
			next += next < nau;
			continue;
		}
		if (wait_msg(0, 5))
			goto close;
		double t = now_s();
		if (dc.acts != acts) {
			acts = dc.acts;
			tact = t;
		}
		if (t - tact > 3 || t - t0 > 15) {
			printf("venus: note: decode stopped after %.1f s (%.1f s without a message), %d frames\n", t - t0,
			       t - tact, dc.frames);
			core_status("decode timeout");
			break;
		}
	}
	step_status(1);
	printf("venus: decoded %d of %d frames%s%s in %.2f s\n", dc.frames, want, dc.eos ? ", EOS" : "",
	       dc.err ? ", session error" : "", now_s() - t0);
	if (dc.frames > 0)	/* v4a0: copy cost vs the decode itself (first to last frame) */
		printf("venus: copy-out %d frames: mean %.2f ms, max %.2f ms (%zu bytes each); frame 0 -> %d: %.2f s = %.1f fps\n",
		       dc.frames, 1e3 * dc.cp_sum / dc.frames, 1e3 * dc.cp_max,
		       (size_t)dc.ystride * dc.yscan * 3 / 2, dc.frames - 1, dc.t_last - dc.t_first,
		       dc.frames > 1 && dc.t_last > dc.t_first ? (dc.frames - 1) / (dc.t_last - dc.t_first) : 0.0);
	for (int k = 0; k < 8; k++)
		if (dc.n_core[k])
			printf("venus: copy-out on cpu%d (%s): %d frames, mean %.2f ms\n", k, k < 6 ? "A55" : "A76", dc.n_core[k],
			       1e3 * dc.cp_core[k] / dc.n_core[k]);
	if (dc.n_moved)
		printf("venus: copy-out: %d frames moved between CPUs during the copy\n", dc.n_moved);
	rc = dc.frames >= want || (dc.eos && dc.frames > 0) ? 0 : 1;
close:
	if (vdec_close())
		rc = 1;
	if (dc.out)
		fclose(dc.out);
	printf("venus: wrote %d frames %dx%d NV12 (Y, then interleaved CbCr) to %s\n", dc.frames, dc.cw, dc.ch, outp);
	core_status("after decode");
	printf("venus: decode %s: venusctl shutdown, then power off\n", rc ? "FAILED" : "ok (stage 3 go)");
	return rc;
}

/* review 8.1: is the --phys region really unused? write a pattern, let the load run, check it (/dev/pmem as init) */
int memtest(uint64_t a, uint64_t len, int write)
{
	if (range_ok(a, len, "memtest") || (len & 3))
		return 1;
	static uint32_t buf[0x4000];
	uint64_t bad = 0;
	unsigned shown = 0;
	for (uint64_t o = 0; o < len; o += sizeof(buf)) {
		uint64_t n = len - o < sizeof(buf) ? len - o : sizeof(buf);
		if (!write && pm_rd(a + o, buf, n))
			return 1;
		for (uint64_t k = 0; k < n / 4; k++) {
			uint64_t pa = a + o + 4 * k;
			uint32_t want = (uint32_t)pa ^ 0x5a5a5a5au;
			if (write) {
				buf[k] = want;
			} else if (buf[k] != want) {
				if (shown++ < 5)
					printf("venus: memtest mismatch at %#llx: %#010x, want %#010x\n", (unsigned long long)pa,
					       buf[k], want);
				bad++;
			}
		}
		if (write && pm_wr(a + o, buf, n))
			return 1;
	}
	if (write)
		printf("venus: memtest wrote %#llx+%#llx (addr ^ 0x5a5a5a5a): next memtest ... check\n",
		       (unsigned long long)a, (unsigned long long)len);
	else
		printf("venus: memtest %#llx+%#llx: %llu of %llu words changed -> %s\n", (unsigned long long)a,
		       (unsigned long long)len, (unsigned long long)bad, (unsigned long long)(len / 4), bad ? "IN USE" : "ok");
	return bad ? 1 : 0;
}

int shutdown_cmd(void)
{
	struct vstate v;
	if (state_load(&v)) {
		printf("venus: no %s: nothing to undo\n", STATE);
		return 0;
	}
	printf("venus: state: pas %d, %d SMRs, queues %#llx (kind %d), init %d, sysinit %d\n", v.pas, v.nsmr,
	       (unsigned long long)v.q, v.qkind, v.init, v.sysinit);
	if (v.q && v.pas && core_ok()) {
		qb = v.q;
		wait_msg(0, 0);	/* last firmware words */
		core_status("before shutdown");
	}
	int r = teardown(&v);
	printf("venus: shutdown %s: next venusctl power off\n", r ? "FAILED" : "ok");
	return r;
}
