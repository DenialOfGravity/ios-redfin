/*
 * ws-cam-icp: the ICP (camera A5) from userland: power, SMMU bank, firmware load, HFI queues. See icp.h.
 * Every register write that changes shared state is recorded (chg[]) and undone in reverse by icp_power_down;
 * nothing is written that was already in the wanted state (a running PLL, an on GDSC, an enabled branch are left as
 * found and not turned off later).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include "../video/pmem.h"
#include "icp.h"

int icp_verbose;
double icp_ms(void) { return mono_ms(); }

/* ---------------- change log ---------------- */
enum { C_BRANCH, C_RCG, C_GDSC, C_PLL, C_REG };
static struct chg { int kind; uint64_t pa; uint32_t old; const char *name; } chg[64];
static int nchg;
static void state_save(void);
static void rec(int kind, uint64_t pa, uint32_t old, const char *name)
{
	if (nchg < 64)
		chg[nchg++] = (struct chg){ kind, pa, old, name };
	state_save();
}

static int branch_on(uint64_t pa, const char *name, int delay_only)
{
	uint32_t v = rd32(pa);
	if (v & 1)
		return 0;
	wr32(pa, v | 1);
	rec(C_BRANCH, pa, v, name);
	if (delay_only) {
		delay_ms(1);
		return 0;
	}
	if (wait_bit(pa, 1u << 31, 0, 200)) {
		printf("icp: branch %s %#llx does not run (%08x)\n", name, (unsigned long long)pa, rd32(pa));
		return -1;
	}
	return 0;
}

static int rcg_set(uint32_t off, unsigned src, unsigned div, const char *name)
{
	uint64_t cmd = CAMCC + off, cfg = cmd + 4;
	uint32_t old = rd32(cfg), nv = (old & ~0x371fu) | src << 8 | div;
	rec(C_RCG, cmd, old, name);
	wr32(cfg, nv);
	wr32(cmd, rd32(cmd) | 1);
	if (wait_bit(cmd, 1, 0, 200)) {
		printf("icp: RCG %s %#x does not update (cmd %08x cfg %08x)\n", name, off, rd32(cmd), rd32(cfg));
		return -1;
	}
	if (icp_verbose)
		printf("icp: RCG %s cfg %08x -> %08x\n", name, old, rd32(cfg));
	return 0;
}

static int gdsc_on(uint64_t pa, const char *name)
{
	uint32_t v = rd32(pa);
	if (v >> 31) {
		printf("icp: GDSC %s already on (%08x)\n", name, v);
		return 0;
	}
	wr32(pa, v & ~1u);
	rec(C_GDSC, pa, v, name);
	if (wait_bit(pa, 1u << 31, 1, 500)) {
		printf("icp: GDSC %s does not come on (%08x)\n", name, rd32(pa));
		return -1;
	}
	printf("icp: GDSC %s on (%08x)\n", name, rd32(pa));
	return 0;
}

/* Lucid PLL: clk_lucid_pll_configure + alpha_pll_lucid_enable (as xnu 0921 pll0_up) */
#define PLL_MODE 0x00
#define PLL_L 0x04
#define PLL_CAL_L 0x08
#define PLL_USER 0x0c
#define PLL_USER_U 0x10
#define PLL_USER_U1 0x14
#define PLL_CONFIG 0x18
#define PLL_CONFIG_U 0x1c
#define PLL_CONFIG_U1 0x20
#define PLL_TEST_U1 0x2c
#define PLL_OPMODE 0x38
#define PLL_ALPHA 0x40
static int pll_up(uint32_t off, uint32_t l, uint32_t alpha, uint32_t user, const char *name)
{
	uint64_t p = CAMCC + off;
	uint32_t mode = rd32(p + PLL_MODE);
	if (mode & (1u | 1u << 20)) {
		printf("icp: PLL %s mode %08x: running or voted (somebody else's)\n", name, mode);
		return getenv("ICPCTL_SHARED") ? 0 : -1;
	}
	static const struct { uint32_t off, val; } fixed[] = {
		{ PLL_CAL_L, 0x44 }, { PLL_CONFIG, 0x20485699 }, { PLL_CONFIG_U, 0x00002261 },
		{ PLL_CONFIG_U1, 0x329a699c }, { PLL_USER_U, 0x00000805 }, { PLL_TEST_U1, 0x01800000 },
	};
	wr32(p + PLL_L, l);
	wr32(p + PLL_ALPHA, alpha);
	for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
		wr32(p + fixed[i].off, fixed[i].val);
	wr32(p + PLL_USER, user);
	rec(C_PLL, p, mode, name);
	wr32(p + PLL_MODE, rd32(p + PLL_MODE) | 1u << 23);
	wr32(p + PLL_MODE, rd32(p + PLL_MODE) & ~1u);
	wr32(p + PLL_OPMODE, 0);
	wr32(p + PLL_MODE, rd32(p + PLL_MODE) | 1u << 2);
	wr32(p + PLL_OPMODE, 1);
	if (wait_bit(p + PLL_MODE, 1u << 31, 1, 500)) {
		printf("icp: PLL %s does not lock (mode %08x)\n", name, rd32(p + PLL_MODE));
		return -1;
	}
	wr32(p + PLL_USER, rd32(p + PLL_USER) | 7u);
	wr32(p + PLL_MODE, rd32(p + PLL_MODE) | 1u);
	printf("icp: PLL %s locked (mode %08x user %08x L %#x)\n", name, rd32(p + PLL_MODE), rd32(p + PLL_USER), l);
	return 0;
}

static void pll_down(uint64_t p)
{
	wr32(p + PLL_MODE, rd32(p + PLL_MODE) & ~1u);
	wr32(p + PLL_USER, rd32(p + PLL_USER) & ~7u);
	wr32(p + PLL_OPMODE, 0);
	wr32(p + PLL_MODE, rd32(p + PLL_MODE) & ~(1u << 2));
}

int icp_camera_idle(void)
{
	return !(rd32(CAMCC + CC_PLL0 + PLL_MODE) & (1u | 1u << 20));
}

/* boot-arg xram: ones digit (1054/1142/1268) 1,3,5 = tier 1 (pil_npu, pil_cdsp kept out), 2,4,6 = tier 2 (theirs) */
int icp_mem_check(void)
{
	char ba[1024] = "";
	size_t l = sizeof(ba) - 1;
	if (sysctlbyname("kern.bootargs", ba, &l, NULL, 0)) {
		printf("icp: kern.bootargs: %s\n", strerror(errno));
		return -1;
	}
	const char *x = strstr(ba, "xram=");
	unsigned t = x ? (unsigned)strtoul(x + 5, NULL, 10) % 10 : 0;
	if (t == 2 || t == 4 || t == 6) {
		printf("icp: xram=%s: pil_npu/pil_cdsp belong to XNU at this tier: refused\n", x + 5);
		return -1;
	}
	printf("icp: memory: fw %#llx (pil_camera), hfi %#llx (pil_npu), io %#llx (pil_cdsp), tables %#llx; xram tier %u ok\n",
	       (unsigned long long)PA_FW, (unsigned long long)PA_HFI, (unsigned long long)PA_IO,
	       (unsigned long long)PA_TBL, t);
	return 0;
}

static int powered, ipe_powered;
int icp_ipe_power_up(void)
{
	if (ipe_powered)
		return 0;
	int rc = 0;
	rc |= pll_up(CC_PLL1, 0x1f, 0x4000, 0x00000101, "PLL1");	/* 600 MHz, even /2 = 300 */
	rc |= gdsc_on(CAMCC + GDSC_IPE0, "ipe_0");
	rc |= gdsc_on(CAMCC + 0x9004, "ipe_1");
	rc |= rcg_set(0x8010, 4, 1, "ipe_0 300");	/* PLL1 even / 1 */
	static const struct { uint32_t off; const char *n; } br[] = {
		{ 0x8040, "ipe_0_ahb" }, { 0x803c, "ipe_0_areg" }, { 0x8038, "ipe_0_axi" }, { 0x8028, "ipe_0" },
		{ 0x9028, "ipe_1_ahb" }, { 0x9024, "ipe_1_areg" }, { 0x9020, "ipe_1_axi" }, { 0x9010, "ipe_1" },
	};
	for (unsigned i = 0; !rc && i < sizeof(br) / sizeof(br[0]); i++)
		rc |= branch_on(CAMCC + br[i].off, br[i].n, 0);
	ipe_powered = !rc;
	printf("icp: IPE0/IPE1 power %s\n", rc ? "FAILED" : "on (300 MHz)");
	return rc ? -1 : 0;
}

/* Linux cam_icp_mgr_hw_open powers A5, BPS, IPE0, IPE1 together (device init): the firmware reads the cores'
 * versions at SYS_INIT. ICPCTL_MIN=1: A5 + CPAS only. */
int icp_power_up(int ipe)
{
	int rc = 0;
	uint32_t tbu = rd32(0x17d054);
	printf("icp: mnoc_sf_0_tbu vote %08x (PWR_ON %u)\n", tbu, tbu >> 31);
	if (!(tbu >> 31)) {
		if (!getenv("ICPCTL_TBUVOTE")) {
			printf("icp: the SF TBU (SIDs 0x1000-0x13ff) is off: ICPCTL_TBUVOTE=1 to vote it\n");
			return -1;
		}
		wr32(0x17d054, tbu & ~1u);
		rec(C_REG, 0x17d054, tbu, "tbu_sf0 vote");
		if (wait_bit(0x17d054, 1u << 31, 1, 200))
			return -1;
	}
	powered = 1;
	if (!(rd32(CAMCC + GDSC_CLK) & 1))
		rc |= branch_on(CAMCC + GDSC_CLK, "camcc gdsc_clk", 0);
	if (rc || gdsc_on(CAMCC + GDSC_TITAN, "titan_top"))
		return -1;
	/* the camera AXI branches only run once titan top is on (they read 80000001 before: ws-cam-icp run 1) */
	rc |= branch_on(GCC_CAM + 0x28, "gcc camera_hf_axi", 0);
	rc |= branch_on(GCC_CAM + 0x2c, "gcc camera_sf_axi", 0);
	/* gcc_camera_xo_clk is CLK_IS_CRITICAL in Linux (gcc-lito.c) but off here (80000000): without XO the A5's
	 * timer never ticks and the firmware's sleep-clock watchdog barks ("ICP SS WD Timeout" in the SFR, run 3/4) */
	rc |= branch_on(GCC_CAM + 0x3c, "gcc camera_xo", 0);
	rc |= branch_on(CAMCC + 0xc150, "camnoc_dcd_xo", 0);
	if (rc || pll_up(CC_PLL0, 0x3e, 0x8000, 0x00003101, "PLL0"))
		return -1;
	rc |= rcg_set(0x7058, 3, 9, "slow_ahb 80");		/* PLL0 odd 400 / 5 */
	rc |= rcg_set(0xc12c, 2, 7, "camnoc_axi 150");		/* PLL0 even 600 / 4 */
	rc |= rcg_set(0x703c, 2, 11, "fast_ahb 100");		/* PLL0 even 600 / 6 */
	rc |= rcg_set(0xc074, 3, 1, "icp 400");			/* PLL0 odd / 1 */
	if (rc)
		return -1;
	rc |= branch_on(CAMCC + 0xc124, "cpas_ahb", 0);
	rc |= branch_on(CAMCC + 0xc184, "core_ahb", 1);
	rc |= branch_on(CAMCC + 0xc148, "camnoc_axi", 0);
	rc |= branch_on(CAMCC + 0xc094, "icp_ahb", 0);
	rc |= branch_on(CAMCC + 0xc08c, "icp", 0);
	if (rc)
		return -1;
	if (!getenv("ICPCTL_MIN")) {
		rc |= gdsc_on(CAMCC + GDSC_BPS, "bps");
		rc |= rcg_set(0x7010, 3, 3, "bps 200");		/* PLL0 odd 400 / 2 */
		static const struct { uint32_t off; const char *n; } br[] = {
			{ 0x7070, "bps_ahb" }, { 0x7054, "bps_areg" }, { 0x7038, "bps_axi" }, { 0x7028, "bps" },
		};
		for (unsigned i = 0; !rc && i < 4; i++)
			rc |= branch_on(CAMCC + br[i].off, br[i].n, 0);
		if (rc)
			return -1;
		if (ipe || !getenv("ICPCTL_NOIPE"))
			if (icp_ipe_power_up())
				return -1;
	}
	wr32(A5_SIERRA + HFI_A5_CSR_ACCESS, 0xa0a);		/* qos-val */
	printf("icp: power on: titan top %08x, PLL0 %08x, icp cbcr %08x, A5 HW version %08x, status %08x\n",
	       rd32(CAMCC + GDSC_TITAN), rd32(CAMCC + CC_PLL0), rd32(CAMCC + 0xc08c), rd32(A5_SIERRA + HFI_A5_HW_VERSION),
	       rd32(A5_SIERRA + HFI_A5_STATUS));
	return 0;
}

void icp_power_down(void)
{
	for (int i = nchg - 1; i >= 0; i--) {
		struct chg *c = &chg[i];
		switch (c->kind) {
		case C_BRANCH:
			wr32(c->pa, rd32(c->pa) & ~1u);
			break;
		case C_RCG:
			wr32(c->pa + 4, c->old);
			wr32(c->pa, rd32(c->pa) | 1);
			(void)wait_bit(c->pa, 1, 0, 100);
			break;
		case C_GDSC:
			wr32(c->pa, rd32(c->pa) | 1);
			(void)wait_bit(c->pa, 1u << 31, 0, 500);
			break;
		case C_PLL:
			pll_down(c->pa);
			wr32(c->pa + PLL_MODE, c->old);	/* also bit 23 (update bypass) as found: run 8 left 00800000 */
			break;
		case C_REG:
			wr32(c->pa, c->old);
			break;
		}
		if (icp_verbose)
			printf("icp: undo %s\n", c->name);
	}
	if (nchg)
		printf("icp: power down: %d changes undone; titan top %08x PLL0 %08x PLL1 %08x ipe0 %08x bps %08x\n", nchg,
		       rd32(CAMCC + GDSC_TITAN), rd32(CAMCC + CC_PLL0), rd32(CAMCC + CC_PLL1), rd32(CAMCC + GDSC_IPE0),
		       rd32(CAMCC + GDSC_BPS));
	nchg = 0;
	powered = ipe_powered = 0;
	state_save();
}

/* ---------------- SMMU: one translating bank, 4 KB pages, tables in PA_TBL ---------------- */
#define SMMU		0x15000000ull
#define R_SCTLR		0x00001127u
#define R_TCR		(25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define R_TCR2		(1u | 1u << 4 | 7u << 15)
#define R_MAIR0		0xf404ff44u
#define R_CBAR		(1u << 16 | 3u << 8 | 0xfu << 12)
#define R_PAGE		(3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
static const struct { uint16_t sid, mask; } sids[] = {	/* DT msm_cam_smmu_icp; 0x1320/0x20 is inside 0x1300/0x20 */
	{ 0x1042, 0 }, { 0x11a0, 0 }, { 0x1220, 0 }, { 0x1300, 0x20 }, { 0x1180, 0 }, { 0x1200, 0 }, { 0x11e0, 0 },
	{ 0x1260, 0 },
};
#define NSID (sizeof(sids) / sizeof(sids[0]))
static struct { int on, cb, slot[NSID]; uint64_t cbp, gr1; uint32_t smr[NSID], s2cr[NSID], cbar, cba2r, sctlr; } bank;
static uint32_t tbl_next;	/* next free table page (offset in PA_TBL) */
static uint32_t l3_of[512];	/* L2 index -> L3 table offset (slot 0 only: IOVAs < 1 GB) */

/* PRIV: AP 00 (privileged only), as Linux maps the firmware (IOMMU_PRIV): an EL0-writable page (AP 01) is implicitly
 * PXN, and the A5's privileged instruction fetch at IOVA 0 took a permission fault (run 2: FSR 408, FSYNR0 63) */
static int map_pages(uint32_t iova, uint64_t pa, uint32_t len, int priv)
{
	if ((iova | pa | len) & 0xfff || (uint64_t)iova + len > 0x40000000ull)
		return -1;
	for (uint32_t o = 0; o < len; o += 0x1000) {
		uint32_t a = iova + o, i2 = a >> 21;
		if (!l3_of[i2]) {
			if (tbl_next + 0x1000 > TBL_LEN)
				return -1;
			l3_of[i2] = tbl_next;
			tbl_next += 0x1000;
			uint64_t d = (PA_TBL + l3_of[i2]) | 3;
			pm_wr(PA_TBL + 0x1000 + 8 * i2, &d, 8);
		}
		uint64_t d = (pa + o) | (priv ? R_PAGE & ~(1ull << 6) : R_PAGE);
		pm_wr(PA_TBL + l3_of[i2] + 8 * ((a >> 12) & 0x1ff), &d, 8);
	}
	if (icp_verbose)
		printf("icp: smmu map IOVA %#x -> PA %#llx + %#x\n", iova, (unsigned long long)pa, len);
	return 0;
}

static int sid_match(uint32_t smr, uint32_t sid, uint32_t mask)
{
	uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
	return ((id ^ sid) & ~(m | mask) & 0x7fff) == 0;
}

int icp_smmu_up(void)
{
	uint64_t s = SMMU;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24), nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint32_t psize = (id1 >> 31) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
	uint64_t used[4] = { 0 };
	int cb = -1;
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i), s2cr = rd32(s + 0xc00 + 4 * i);
		if (!(smr >> 31))
			continue;
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		for (unsigned k = 0; k < NSID; k++)
			if (sid_match(smr, sids[k].sid, sids[k].mask)) {
				printf("icp: smmu: SMR%u %08x already matches %#x: refused\n", i, smr, sids[k].sid);
				return -1;
			}
	}
	for (uint32_t c = 0; c < ncb && c < 256; c++) {
		uint64_t cbp = s + (uint64_t)npage * psize + (uint64_t)c * psize;
		uint32_t cbar = rd32(s + psize + 4 * c);
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbp) & 1) &&
		    (cbar == 0 || cbar == 0x0001f000u || cbar == R_CBAR)) {
			cb = (int)c;
			break;
		}
	}
	if (cb < 0 || (uint64_t)(npage + cb + 1) * psize > 0x100000) {
		printf("icp: smmu: no free bank\n");
		return -1;
	}
	for (unsigned k = 0; k < NSID; k++) {
		bank.slot[k] = -1;
		for (uint32_t i = 0; i < nsmr && i < 128 && bank.slot[k] < 0; i++) {
			int taken = 0;
			for (unsigned j = 0; j < k; j++)
				taken |= bank.slot[j] == (int)i;
			if (!(rd32(s + 0x800 + 4 * i) >> 31) && !taken)
				bank.slot[k] = (int)i;
		}
		if (bank.slot[k] < 0) {
			printf("icp: smmu: no free SMR\n");
			return -1;
		}
	}
	bank.cb = cb;
	bank.gr1 = s + psize;
	bank.cbp = s + (uint64_t)npage * psize + (uint64_t)cb * psize;
	bank.cbar = rd32(bank.gr1 + 4 * cb);
	bank.cba2r = rd32(bank.gr1 + 0x800 + 4 * cb);
	bank.sctlr = rd32(bank.cbp);
	for (unsigned k = 0; k < NSID; k++) {
		bank.smr[k] = rd32(s + 0x800 + 4 * bank.slot[k]);
		bank.s2cr[k] = rd32(s + 0xc00 + 4 * bank.slot[k]);
	}
	/* tables: zero, L1[0] -> L2, then the regions */
	static const uint8_t zero[0x10000];
	for (uint32_t o = 0; o < TBL_LEN; o += sizeof(zero))
		pm_wr(PA_TBL + o, zero, sizeof(zero));
	memset(l3_of, 0, sizeof(l3_of));
	tbl_next = 0x2000;
	uint64_t d = (PA_TBL + 0x1000) | 3;
	pm_wr(PA_TBL, &d, 8);
	if (map_pages(0, PA_FW, FW_LEN, 1) || map_pages(IOVA_SHARED, PA_HFI, 0x100000, 0) ||
	    map_pages(IOVA_SECHEAP, PA_SECHEAP, SECHEAP_LEN, 0) || map_pages(IOVA_QDSS, PA_QDSS, QDSS_LEN, 0) ||
	    map_pages(IOVA_IO, PA_IO, IO_LEN, 0)) {
		printf("icp: smmu: table space\n");
		return -1;
	}
	uint64_t cbp = bank.cbp, gr1 = bank.gr1;
	wr32(cbp, 0);
	wr32(cbp + 0x58, 0xffffffff);
	wr64(cbp + 0x20, PA_TBL | (uint64_t)cb << 48);
	wr64(cbp + 0x28, 0);
	wr32(cbp + 0x30, R_TCR);
	wr32(cbp + 0x10, R_TCR2);
	wr32(cbp + 0x38, R_MAIR0);
	wr32(cbp + 0x3c, 0);
	wr32(gr1 + 0x800 + 4 * cb, 1);			/* CBA2R: AArch64 */
	wr32(gr1 + 4 * cb, R_CBAR);
	wr32(cbp + 0x610, (uint32_t)cb);			/* TLBIASID */
	wr32(cbp + 0x618, 0);				/* TLBIALL */
	wr32(cbp + 0x7f0, 0);				/* TLBSYNC */
	(void)wait_bit(cbp + 0x7f4, 1, 0, 500);
	wr32(cbp, R_SCTLR);
	uint64_t ttbr = ((uint64_t)rd32(cbp + 0x24) << 32) | rd32(cbp + 0x20);
	if (ttbr != (PA_TBL | (uint64_t)cb << 48) || rd32(cbp + 0x30) != R_TCR || rd32(gr1 + 4 * cb) != R_CBAR) {
		printf("icp: smmu: CB%d not as written (TTBR0 %llx TCR %08x CBAR %08x)\n", cb, (unsigned long long)ttbr,
		       rd32(cbp + 0x30), rd32(gr1 + 4 * cb));
		wr32(cbp, bank.sctlr);
		return -1;
	}
	bank.on = 1;
	state_save();
	for (unsigned k = 0; k < NSID; k++) {
		wr32(s + 0xc00 + 4 * bank.slot[k], (uint32_t)cb);
		wr32(s + 0x800 + 4 * bank.slot[k], 0x80000000u | (uint32_t)sids[k].mask << 16 | sids[k].sid);
	}
	wr32(s + 0x70, 0);
	(void)wait_bit(s + 0x74, 1, 0, 500);
	printf("icp: smmu: CB%d, %u SMRs (first SMR%d), tables %#llx (%u pages): fw 0 -> %#llx, shared %#x, secheap %#x,"
	       " qdss %#x, io %#x+%#x -> %#llx\n", cb, (unsigned)NSID, bank.slot[0], (unsigned long long)PA_TBL,
	       tbl_next / 0x1000, (unsigned long long)PA_FW, IOVA_SHARED, IOVA_SECHEAP, IOVA_QDSS, IOVA_IO, IO_LEN,
	       (unsigned long long)PA_IO);
	return 0;
}

void icp_smmu_faults(const char *tag)
{
	if (!bank.on)
		return;
	uint32_t fsr = rd32(bank.cbp + 0x58), sy = rd32(bank.cbp + 0x68);
	printf("icp: smmu %s: CB%d FSR %08x%s FAR %08x%08x FSYNR0 %08x FSYNR1 %08x\n", tag, bank.cb, fsr,
	       (fsr & 0xc00001feu) ? " FAULT" : "", rd32(bank.cbp + 0x64), rd32(bank.cbp + 0x60), sy, rd32(bank.cbp + 0x6c));
}

void icp_smmu_down(void)
{
	if (!bank.on)
		return;
	icp_smmu_faults("release");
	uint64_t s = SMMU;
	for (unsigned k = 0; k < NSID; k++) {
		wr32(s + 0x800 + 4 * bank.slot[k], bank.smr[k]);
		wr32(s + 0xc00 + 4 * bank.slot[k], bank.s2cr[k]);
	}
	wr32(s + 0x70, 0);
	(void)wait_bit(s + 0x74, 1, 0, 500);
	wr32(bank.cbp, bank.sctlr);
	wr32(bank.gr1 + 4 * bank.cb, bank.cbar);
	wr32(bank.gr1 + 0x800 + 4 * bank.cb, bank.cba2r);
	bank.on = 0;
	state_save();
	printf("icp: smmu: CB%d released (SMR%d %08x, SCTLR %08x)\n", bank.cb, bank.slot[0],
	       rd32(s + 0x800 + 4 * bank.slot[0]), rd32(bank.cbp));
}

/* ---------------- firmware ---------------- */
static int zero_range(uint64_t pa, uint32_t len)
{
	static const uint8_t z[0x10000];
	for (uint32_t o = 0; o < len; o += sizeof(z))
		if (pm_wr(pa + o, z, len - o < sizeof(z) ? len - o : sizeof(z)))
			return -1;
	return 0;
}

int icp_load_fw(const char *path)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		printf("icp: %s: %s\n", path, strerror(errno));
		return -1;
	}
	uint8_t *f = malloc((size_t)st.st_size + 4);
	if (!f || read(fd, f, (size_t)st.st_size) != st.st_size) {
		printf("icp: read %s failed\n", path);
		close(fd);
		return -1;
	}
	close(fd);
	if (memcmp(f, "\177ELF\001\001", 6) || *(uint16_t *)(f + 18) != 40) {	/* ELF32 LE, EM_ARM */
		printf("icp: %s: not an ARM ELF32\n", path);
		return -1;
	}
	uint32_t phoff = *(uint32_t *)(f + 28), entry = *(uint32_t *)(f + 24);
	uint16_t phnum = *(uint16_t *)(f + 44), phes = *(uint16_t *)(f + 42);
	double t0 = mono_ms();
	if (zero_range(PA_FW, FW_LEN))
		return -1;
	uint32_t total = 0, sum = 0;
	for (unsigned i = 0; i < phnum; i++) {
		const uint32_t *ph = (const uint32_t *)(f + phoff + i * phes);
		uint32_t type = ph[0], off = ph[1], va = ph[2], fsz = ph[4], msz = ph[5], flags = ph[6];
		printf("icp: fw phdr %u: type %u off %#x va %#x filesz %#x memsz %#x flags %#x\n", i, type, off, va, fsz,
		       msz, flags);
		if (!flags || !fsz)
			continue;
		if ((uint64_t)va + msz > FW_LEN || (uint64_t)off + fsz > (uint64_t)st.st_size || (va & 3)) {
			printf("icp: fw segment %u outside the 5 MB region\n", i);
			return -1;
		}
		uint32_t n = (fsz + 3) & ~3u;
		memset(f + off + fsz, 0, n - fsz < 4 ? n - fsz : 0);
		if (pm_wr(PA_FW + va, f + off, n))
			return -1;
		uint8_t *chk = malloc(n);
		if (!chk || pm_rd(PA_FW + va, chk, n) || memcmp(chk, f + off, fsz)) {
			printf("icp: fw segment %u read back differs\n", i);
			free(chk);
			return -1;
		}
		for (uint32_t k = 0; k < fsz; k++)
			sum = sum * 31 + chk[k];
		free(chk);
		total += fsz;
	}
	free(f);
	printf("icp: firmware %s: %u bytes in %u segments, entry %#x, checksum %08x, %.0f ms (5 MB zeroed + copy + verify)\n",
	       path, total, phnum, entry, sum, mono_ms() - t0);
	return 0;
}

/* ---------------- HFI queues ---------------- */
#define QT(off)		(PA_HFI + HFI_QTBL + (off))
#define QH(q, f)	QT(24 + (q) * 0x3bc + (f))
enum { F_STATUS = 0x3c, F_START = 0x7c, F_TYPE = 0xbc, F_QSIZE = 0xfc, F_PKTSIZE = 0x13c, F_DROP = 0x17c,
       F_RXWM = 0x1bc, F_TXWM = 0x1fc, F_RXREQ = 0x23c, F_TXREQ = 0x27c, F_RXIRQ = 0x2bc, F_TXIRQ = 0x2fc,
       F_READ = 0x33c, F_WRITE = 0x37c };
static const uint32_t q_off[3] = { HFI_CMDQ, HFI_MSGQ, HFI_DBGQ }, q_words[3] = { CMDQ_BYTES / 4, MSGQ_BYTES / 4,
										 DBGQ_BYTES / 4 };
static int booted;

static void qtbl_init(void)
{
	zero_range(PA_HFI, 0x100000);
	uint32_t hdr[6] = { 0xffffffffu, 24 + 4 * 0x3bc, 24, 0x3bc, 3, 3 }	/* hfi_q_hdr: 14 fields x 64 + 60 = 956 */;
	pm_wr(QT(0), hdr, sizeof(hdr));
	for (int q = 0; q < 3; q++) {	/* INTR_MODE, as Linux cam_hfi_init(0, ...) */
		wr32(QH(q, F_STATUS), 1);
		wr32(QH(q, F_START), IOVA_SHARED + q_off[q]);
		wr32(QH(q, F_QSIZE), q_words[q]);
		wr32(QH(q, F_TYPE), (uint32_t)q);
		wr32(QH(q, F_RXWM), 1);
		wr32(QH(q, F_TXWM), q == 2 ? 1024 : 1);
		wr32(QH(q, F_RXREQ), q == 2 ? 0 : 1);
	}
	wr32(PA_HFI + HFI_SFR, SFR_BYTES);
}

static void sfr_print(void)
{
	char b[257];
	if (pm_rd(PA_HFI + HFI_SFR + 4, b, 256))
		return;
	b[256] = 0;
	if (b[0])
		printf("icp: SFR: %s\n", b);
}

static void a5_regs(const char *tag)
{
	uint64_t a = A5_SIERRA, c = 0x0ac40000ull;
	printf("icp: CPAS %s: hw %08x ipe0 ctl/st %08x/%08x ipe1 %08x/%08x bps %08x/%08x\n", tag, rd32(c), rd32(c + 0x4c),
	       rd32(c + 0x48), rd32(c + 0x54), rd32(c + 0x50), rd32(c + 0x5c), rd32(c + 0x58));
	printf("icp: A5 %s: ctl %08x nsec %08x status %08x init req %u resp %u fw %08x int st %08x en %08x\n", tag,
	       rd32(a + HFI_A5_CONTROL), rd32(a + HFI_A5_NSEC_RESET), rd32(a + HFI_A5_STATUS), rd32(a + HFI_HOST_INIT_REQ),
	       rd32(a + HFI_ICP_INIT_RESP), rd32(a + HFI_FW_VERSION), rd32(a + HFI_A5_A2HOSTINTSTATUS),
	       rd32(a + HFI_A5_A2HOSTINTEN));
}
void icp_regs(const char *tag) { a5_regs(tag); }

int icp_boot(void)
{
	uint64_t a = A5_SIERRA;
	double t0 = mono_ms();
	a5_regs("before");
	qtbl_init();
	/* cam_hfi_enable_cpu: A5_EN, NSEC_RESET 0x10 (Linux does it right before copying the firmware; ours is in) */
	wr32(a + HFI_A5_CONTROL, A5_CTL_EN);
	wr32(a + HFI_A5_NSEC_RESET, 0x10);
	booted = 1;
	state_save();
	/* cam_hfi_init */
	wr32(a + HFI_A5_CONTROL, A5_CTL_EN | A5_CTL_WAKE_UP_EN | A5_CTL_CLKGATE_WFI);
	wr32(a + HFI_QTBL_PTR, IOVA_SHARED + HFI_QTBL);
	wr32(a + HFI_SFR_PTR, IOVA_SHARED + HFI_SFR);
	wr32(a + HFI_SHARED_MEM_PTR, IOVA_SHARED);
	wr32(a + HFI_SHARED_MEM_SIZE, SHARED_LEN);
	wr32(a + HFI_UNCACHED_HEAP_PTR, IOVA_SECHEAP);
	wr32(a + HFI_UNCACHED_HEAP_SIZE, SECHEAP_LEN);
	wr32(a + HFI_QDSS_IOVA, IOVA_QDSS);
	wr32(a + HFI_QDSS_SIZE, QDSS_LEN);
	wr32(a + HFI_IO_REGION_IOVA, IOVA_IO);
	wr32(a + HFI_IO_REGION_SIZE, IO_REGION_LEN);
	wr32(a + HFI_IO2_REGION_IOVA, 0);
	wr32(a + HFI_IO2_REGION_SIZE, 0);
	wr32(a + HFI_HOST_INIT_REQ, 1);
	uint32_t st = 0;
	double t1 = mono_ms();
	for (unsigned i = 0; !wait_over(t1, i, 2000); i++) {
		st = rd32(a + HFI_ICP_INIT_RESP);
		if (st == 1 || st == 2)
			break;
		delay_ms(1);
	}
	printf("icp: ICP_HOST_INIT_RESPONSE %u (%s) after %.0f ms, FW_VERSION %08x, HW_VERSION %08x\n", st,
	       st == 1 ? "SUCCESS" : st == 2 ? "FAILED" : "no answer", mono_ms() - t1, rd32(a + HFI_FW_VERSION),
	       rd32(a + HFI_A5_HW_VERSION));
	if (st != 1) {
		a5_regs("no init");
		sfr_print();
		icp_smmu_faults("no init");
		icp_drain_dbg();
		return -1;
	}
	if (!getenv("ICPCTL_NOIRQ"))
		wr32(a + HFI_A5_A2HOSTINTEN, 1 | 2);	/* INTR_ENABLE | WD0, as Linux; status cleared by polling */
	printf("icp: boot handshake %.0f ms\n", mono_ms() - t0);
	if (getenv("ICPCTL_SFRWATCH")) {	/* diagnosis: when does the firmware write its SFR, what does the A5 do */
		unsigned w = (unsigned)atoi(getenv("ICPCTL_SFRWATCH"));
		double t2 = mono_ms();
		uint32_t last_st = 0, last_sfr = 0;
		for (unsigned i = 0; !wait_over(t2, i, w); i++) {
			uint32_t st2 = rd32(a + HFI_A5_STATUS), sf = rd32(PA_HFI + HFI_SFR + 4);
			uint32_t is = rd32(a + HFI_A5_A2HOSTINTSTATUS);
			if (st2 != last_st || sf != last_sfr || is) {
				printf("icp: +%.1f ms: A5 status %08x int %08x sfr %08x dbg w %u msg w %u\n", mono_ms() - t2, st2,
				       is, sf, rd32(QH(2, F_WRITE)), rd32(QH(1, F_WRITE)));
				last_st = st2, last_sfr = sf;
				if (is)
					wr32(a + HFI_A5_A2HOSTINTCLR, is);
			}
		}
	}
	return 0;
}

int icp_cmd(const void *pkt)
{
	uint32_t words = *(const uint32_t *)pkt / 4, qs = q_words[0];
	uint32_t r = rd32(QH(0, F_READ)), w = rd32(QH(0, F_WRITE));
	uint32_t space = w >= r ? qs - (w - r) : r - w;
	if (!words || space <= words || w >= qs) {
		printf("icp: cmd queue: no space (r %u w %u, %u words)\n", r, w, words);
		return -1;
	}
	uint64_t base = PA_HFI + HFI_CMDQ;
	uint32_t first = words, nw = w + words;
	if (nw >= qs) {
		nw -= qs;
		first = words - nw;
	}
	pm_wr(base + 4ull * w, pkt, first * 4);
	if (first < words)
		pm_wr(base, (const uint32_t *)pkt + first, (words - first) * 4);
	wr32(QH(0, F_WRITE), nw);
	wr32(A5_SIERRA + HFI_A5_HOST2ICPINT, 1);
	if (icp_verbose)
		printf("icp: cmd %08x (%u bytes) queued, w %u -> %u\n", ((const uint32_t *)pkt)[1], words * 4, w, nw);
	return 0;
}

/* read everything waiting in queue Q into BUF (words); returns words read */
static uint32_t q_read(int q, uint32_t *buf, uint32_t max)
{
	uint32_t qs = q_words[q], r = rd32(QH(q, F_READ)), w = rd32(QH(q, F_WRITE));
	if (r == w || r >= qs || w >= qs)
		return 0;
	uint32_t n = w > r ? w - r : qs - r + w;
	if (n > max) {
		printf("icp: queue %d: %u words waiting, more than %u: skipped\n", q, n, max);
		wr32(QH(q, F_READ), w);
		return 0;
	}
	uint64_t base = PA_HFI + q_off[q];
	if (w > r) {
		pm_rd(base + 4ull * r, buf, n * 4);
	} else {
		pm_rd(base + 4ull * r, buf, (qs - r) * 4);
		if (w)
			pm_rd(base, buf + (qs - r), w * 4);
	}
	wr32(QH(q, F_READ), w);
	return n;
}

static unsigned dbg_lines;
void icp_drain_dbg(void)
{
	static uint32_t buf[DBGQ_BYTES / 4];
	uint32_t n = q_read(2, buf, DBGQ_BYTES / 4);
	for (uint32_t i = 0; i + 6 <= n;) {
		uint32_t sz = buf[i] / 4;
		if (sz < 6 || i + sz > n)
			break;
		if (buf[i + 1] == HFI_MSG_SYS_DEBUG) {
			char t[512];
			uint32_t len = (sz - 6) * 4 < sizeof(t) - 1 ? (sz - 6) * 4 : sizeof(t) - 1;
			memcpy(t, &buf[i + 6], len);
			t[len] = 0;
			for (char *p = t + strlen(t); p > t && (p[-1] == '\n' || p[-1] == '\r'); p--)
				p[-1] = 0;
			printf("fw[%08x%08x t%u]: %s\n", buf[i + 4], buf[i + 5], buf[i + 2], t);
			dbg_lines++;
		} else {
			printf("icp: dbg queue packet %08x size %u\n", buf[i + 1], sz * 4);
		}
		i += sz;
	}
	uint32_t st = rd32(A5_SIERRA + HFI_A5_A2HOSTINTSTATUS);
	if (st)
		wr32(A5_SIERRA + HFI_A5_A2HOSTINTCLR, st);
}

static struct { uint32_t len; uint32_t w[256]; } stash[16];
static int nstash;

static void show_msg(const uint32_t *m)
{
	printf("icp: msg %08x size %u:", m[1], m[0]);
	for (uint32_t k = 2; k < m[0] / 4 && k < 14; k++)
		printf(" %08x", m[k]);
	printf("%s\n", m[0] / 4 > 14 ? " ..." : "");
}

int icp_wait(uint32_t type, void *out, size_t outlen, unsigned ms)
{
	static uint32_t buf[MSGQ_BYTES / 4];
	double t0 = mono_ms();
	for (unsigned pass = 0;; pass++) {
		for (int i = 0; i < nstash; i++)
			if (!type || stash[i].w[1] == type) {
				memcpy(out, stash[i].w, outlen < stash[i].len * 4 ? outlen : stash[i].len * 4);
				memmove(&stash[i], &stash[i + 1], (size_t)(nstash - i - 1) * sizeof(stash[0]));
				nstash--;
				return 0;
			}
		icp_drain_dbg();
		uint32_t n = q_read(1, buf, MSGQ_BYTES / 4);
		for (uint32_t i = 0; i + 2 <= n;) {
			uint32_t sz = buf[i] / 4;
			if (sz < 2 || i + sz > n) {
				printf("icp: msg queue: bad packet size %u at %u\n", sz * 4, i);
				break;
			}
			show_msg(&buf[i]);
			if (buf[i + 1] == HFI_MSG_EVENT_NOTIFY)
				sfr_print();
			if (nstash < 16 && sz <= 256) {
				stash[nstash].len = sz;
				memcpy(stash[nstash].w, &buf[i], sz * 4);
				nstash++;
			}
			i += sz;
		}
		if (n)
			continue;
		if (wait_over(t0, pass, ms)) {
			printf("icp: no message %08x within %u ms (cmd q r %u w %u, msg q r %u w %u, dbg q r %u w %u)\n", type, ms,
			       rd32(QH(0, F_READ)), rd32(QH(0, F_WRITE)), rd32(QH(1, F_READ)), rd32(QH(1, F_WRITE)),
			       rd32(QH(2, F_READ)), rd32(QH(2, F_WRITE)));
			a5_regs("timeout");
			sfr_print();
			icp_smmu_faults("timeout");
			return -1;
		}
		delay_ms(1);
	}
}

int icp_set_prop(uint32_t prop, const uint32_t *vals, unsigned n)
{
	uint32_t p[16] = { 16 + 4 * n, HFI_CMD_SYS_SET_PROPERTY, 1, prop };
	for (unsigned i = 0; i < n && i < 12; i++)
		p[4 + i] = vals[i];
	return icp_cmd(p);
}

void icp_halt(void)
{
	if (!booted)
		return;
	uint64_t a = A5_SIERRA;
	uint32_t pc[2] = { 8, HFI_CMD_SYS_PC_PREP }, m[16];
	if (rd32(a + HFI_ICP_INIT_RESP) == 1 && !icp_cmd(pc))
		(void)icp_wait(HFI_MSG_SYS_PC_PREP_DONE, m, sizeof(m), 500);
	icp_drain_dbg();
	double t0 = mono_ms();
	for (unsigned i = 0; !(rd32(a + HFI_A5_STATUS) & A5_STATUS_WFI) && !wait_over(t0, i, 200); i++)
		delay_ms(1);
	uint32_t st = rd32(a + HFI_A5_STATUS);
	wr32(a + HFI_A5_CONTROL, rd32(a + HFI_A5_CONTROL) & ~(A5_CTL_EN | A5_CTL_WAKE_UP_EN));
	wr32(a + HFI_A5_NSEC_RESET, rd32(a + HFI_A5_NSEC_RESET));
	wr32(a + HFI_HOST_INIT_REQ, 0);
	wr32(a + HFI_A5_A2HOSTINTEN, 0);
	booted = 0;
	state_save();
	printf("icp: A5 halted (status before %08x, WFI %u; %u firmware debug lines in all)\n", st,
	       !!(st & A5_STATUS_WFI), dbg_lines);
	a5_regs("halted");
}

/* ---------------- IO region ---------------- */
static uint32_t io_next;
uint32_t icp_io_alloc(uint32_t len, uint64_t *pa)
{
	len = (len + 0xfff) & ~0xfffu;
	if (io_next + len > IO_LEN)
		return 0;
	uint32_t o = io_next;
	io_next += len;
	if (pa)
		*pa = PA_IO + o;
	zero_range(PA_IO + o, len);
	return IOVA_IO + o;
}
uint64_t icp_io_pa(uint32_t iova)
{
	return PA_IO + (iova - IOVA_IO);
}

void icp_status(void)
{
	printf("icp: camcc PLL0 %08x PLL1 %08x | GDSC titan %08x bps %08x ipe0 %08x ipe1 %08x | icp cbcr %08x ahb %08x |"
	       " cpas_ahb %08x camnoc %08x | tbu_sf0 %08x\n", rd32(CAMCC + CC_PLL0), rd32(CAMCC + CC_PLL1),
	       rd32(CAMCC + GDSC_TITAN), rd32(CAMCC + GDSC_BPS), rd32(CAMCC + GDSC_IPE0), rd32(CAMCC + 0x9004),
	       rd32(CAMCC + 0xc08c), rd32(CAMCC + 0xc094), rd32(CAMCC + 0xc124), rd32(CAMCC + 0xc148), rd32(0x17d054));
	if ((rd32(CAMCC + 0xc08c) & 1) && (rd32(CAMCC + GDSC_TITAN) >> 31))
		a5_regs("now");
	else
		printf("icp: ICP clock or titan top off: A5 registers not read\n");
}

void icp_cleanup(void)
{
	static int done;
	if (done++)
		return;
	icp_halt();
	icp_smmu_down();
	icp_power_down();
}

/* ---------------- crash state: what to undo if this process dies (icpctl down) ---------------- */
#define STATE "/var/tmp/icpctl.state"
struct state_file { uint32_t magic, nchg; struct { int kind; uint64_t pa; uint32_t old; } c[64]; int bank_on;
	int cb, slot[NSID]; uint64_t cbp, gr1; uint32_t smr[NSID], s2cr[NSID], cbar, cba2r, sctlr; int booted; };
static void state_save(void)
{
	struct state_file f;
	memset(&f, 0, sizeof(f));
	f.magic = 0x49435031;
	f.nchg = (uint32_t)nchg;
	for (int i = 0; i < nchg; i++) {
		f.c[i].kind = chg[i].kind;
		f.c[i].pa = chg[i].pa;
		f.c[i].old = chg[i].old;
	}
	f.bank_on = bank.on;
	f.cb = bank.cb;
	memcpy(f.slot, bank.slot, sizeof(f.slot));
	f.cbp = bank.cbp;
	f.gr1 = bank.gr1;
	memcpy(f.smr, bank.smr, sizeof(f.smr));
	memcpy(f.s2cr, bank.s2cr, sizeof(f.s2cr));
	f.cbar = bank.cbar;
	f.cba2r = bank.cba2r;
	f.sctlr = bank.sctlr;
	f.booted = booted;
	if (!nchg && !bank.on && !booted) {
		unlink(STATE);
		return;
	}
	int fd = open(STATE ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		if (write(fd, &f, sizeof(f)) == (ssize_t)sizeof(f))
			rename(STATE ".new", STATE);
		close(fd);
	}
}

int icp_state_down(void)
{
	struct state_file f;
	int fd = open(STATE, O_RDONLY);
	if (fd < 0) {
		printf("icp: no %s: nothing recorded to undo\n", STATE);
		return 0;
	}
	ssize_t n = read(fd, &f, sizeof(f));
	close(fd);
	if (n != (ssize_t)sizeof(f) || f.magic != 0x49435031 || f.nchg > 64) {
		printf("icp: %s unreadable\n", STATE);
		return -1;
	}
	nchg = (int)f.nchg;
	for (int i = 0; i < nchg; i++)
		chg[i] = (struct chg){ f.c[i].kind, f.c[i].pa, f.c[i].old, "recorded" };
	bank.on = f.bank_on;
	bank.cb = f.cb;
	memcpy(bank.slot, f.slot, sizeof(f.slot));
	bank.cbp = f.cbp;
	bank.gr1 = f.gr1;
	memcpy(bank.smr, f.smr, sizeof(f.smr));
	memcpy(bank.s2cr, f.s2cr, sizeof(f.s2cr));
	bank.cbar = f.cbar;
	bank.cba2r = f.cba2r;
	bank.sctlr = f.sctlr;
	booted = f.booted && (rd32(CAMCC + 0xc08c) & 1);	/* only touch A5 registers while its clock runs */
	printf("icp: undoing a recorded run: %d changes, bank %s, A5 %s\n", nchg, bank.on ? "on" : "off",
	       booted ? "on" : "off");
	if (booted) {
		uint64_t a = A5_SIERRA;
		wr32(a + HFI_A5_CONTROL, rd32(a + HFI_A5_CONTROL) & ~(A5_CTL_EN | A5_CTL_WAKE_UP_EN));
		wr32(a + HFI_HOST_INIT_REQ, 0);
		wr32(a + HFI_A5_A2HOSTINTEN, 0);
		booted = 0;
	}
	icp_smmu_down();
	icp_power_down();
	unlink(STATE);
	return 0;
}
