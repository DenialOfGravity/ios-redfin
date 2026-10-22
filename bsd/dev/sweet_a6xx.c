/*
 * Adreno 640 core (see sweet_a6xx.h). The bring-up follows shim/gpuprobe.c,
 * which first worked on the phone; that file has the reasoning behind each
 * register value (offsets from mainline a6xx_gmu.c/a6xx_gpu.c, a6xx.xml,
 * gpucc-sm8150.c, sm8150.dtsi).
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#ifdef KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
#include "sweet_a6xx.h"

#define rd32		a6_rd
#define wr32		a6_wr

static void wr64(uint64_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

static int wait_bit(uint64_t pa, uint32_t mask, int set, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if (!!(rd32(pa) & mask) == set)
			return 0;
		if (i < ms)
			a6_delay_ms(1);
	}
	return -1;
}

static void zero_pa(uint64_t pa, uint64_t size)
{
	static const uint8_t z[0x4000];
	for (uint64_t o = 0; o < size; o += sizeof(z))
		a6_write(pa + o, z, (size_t)(size - o < sizeof(z) ? size - o : sizeof(z)));
}

/* ---- registers ---- */

#define GCC		0x00100000ull
#define GPUCC		A6_GPUCC
#define GPU		A6_GPU_BASE
#define GREG(o)		A6_GREG(o)
#define GMU_REG(o)	A6_GREG(o)
#define SMMU		A6_SMMU

#define GPUCC_CX_GDSCR	0x106c
#define GPUCC_GX_GDSCR	0x100c
#define GPUCC_GMU_RCG	0x1120
#define GPUCC_AHB	0x1078
#define GPUCC_CXO	0x109c
#define GPUCC_CX_GMU	0x1098

#define GMU_ITCM	0x1b400
#define GMU_DTCM	0x1c400
#define GMU_FW_VERSION	0x1d3f8
#define GMU_INIT_RESULT	0x1f81c
#define GMU_RPMH_STATE	0x1f8ec
#define GMU_GENERAL_7	0x1f9cc
#define GMU_CX_BUSY	0x23b0c

#define SMMU_GR1	(SMMU + 0x1000)
#define SMMU_CB(n)	A6_SMMU_CB(n)
#define SMR(n)		(SMMU + 0x800 + 4 * (n))
#define S2CR(n)		(SMMU + 0xc00 + 4 * (n))
#define CBAR(n)		(SMMU_GR1 + 4 * (n))
#define CBA2R(n)	(SMMU_GR1 + 0x800 + 4 * (n))
#define CB_SCTLR	0x00
#define CB_TCR2		0x10
#define CB_TTBR0	0x20
#define CB_TCR		0x30
#define CB_MAIR0	0x38
#define CB_FSR		0x58
#define CB_FAR		0x60
#define CB_FSYNR0	0x68
#define CB_TLBIALL	0x618
#define CB_TLBSYNC	0x7f0
#define CB_TLBSTATUS	0x7f4
/*
 * TCR2: 40-bit PA, SEP = 7 (upstream bus size). SEP 0 (the reset value)
 * makes VA bit 31 select TTBR1, which is off: every GPU VA >= 2 GB faulted
 * at level 0. Linux's arm-smmu sets SEP_UPSTREAM too.
 */
#define TCR2_VAL	((7u << 15) | 2u)

#define GMU_SID		5
#define GMU_SID_MASK	0x400
#define GPU_SID		0
#define GPU_SID_MASK	0x401

#define GMEM_L1(g)	a6_pool_pa(g, 0x0000)
#define GMEM_L2_0(g)	a6_pool_pa(g, 0x1000)
#define GMEM_L2_1(g)	a6_pool_pa(g, 0x2000)
#define GMEM_L3_LOW(g)	a6_pool_pa(g, 0x3000)
#define GMEM_L3_6(g)	a6_pool_pa(g, 0x4000)
#define GMEM_ICACHE(g)	a6_pool_pa(g, 0x10000)
#define GMEM_DCACHE(g)	a6_pool_pa(g, 0x50000)
#define GMEM_DUMMY(g)	a6_pool_pa(g, 0x90000)
#define GMEM_LOG(g)	a6_pool_pa(g, A6_GMEM_LOG_OFF)
#define GMEM_HFI(g)	a6_pool_pa(g, 0x95000)

#define VA_ICACHE	0x4000u
#define VA_DCACHE	0x44000u
#define CACHE_SIZE	0x3c000u
#define VA_DUMMY	0x60000000u
#define VA_LOG		A6_GMU_LOG_VA
#define VA_HFI		0x60005000u

#define RSCC		(0x02c6a000ull + 0x23000)
#define RSCC_REG(o)	(RSCC + 4ull * (o))
#define PDC		0x0b290000ull
#define PDC_REG(o)	(PDC + 4ull * (o))
#define PDC_SEQ		0x0b490000ull

#define GMU_SYSRESET	0x1f800
#define GMU_BOOT_CFG	0x1f801
#define GMU_CM3_CFG	0x1f82d
#define GMU_PWRCOL_CTRL	0x1f8c0
#define GMU_RPMH_CTRL	0x1f8e8
#define GMU_CP_MSG	0x1f900
#define GMU_CP_RESP	0x1f901
#define GMU_HFI_CTRL	0x1f980
#define GMU_SFR_ADDR	0x1f982
#define GMU_QTBL_INFO	0x1f984
#define GMU_QTBL_ADDR	0x1f985
#define GMU_SYS_BUS	0x1f40f
#define GMU_ICACHE_CFG	0x1f400
#define GMU_DCACHE_CFG	0x1f401
#define GMU_RSCC_REQ	0x23b07
#define GMU_RSCC_ACK	0x23b08
#define GMU_FENCE0	0x23b11
#define GMU_G2H_CLR	0x1f991
#define GMU_G2H_INFO	0x1f992
#define GMU_H2G_SET	0x1f994

#define RBBM_STATUS		0x210
#define RBBM_INT_STATUS		0x201
#define CP_RB_BASE		0x800
#define CP_RB_CNTL		0x802
#define CP_RB_RPTR		0x806
#define CP_RB_WPTR		0x807
#define CP_SQE_CNTL		0x808
#define CP_HW_FAULT		0x821
#define CP_STATUS_1		0x825
#define CP_SQE_INSTR_BASE	0x830

static bool is_on(uint64_t gdscr)
{
	return (rd32(gdscr) >> 31) & 1;
}

static bool clk_on(uint64_t cbcr)
{
	return !(rd32(cbcr) >> 31);	/* CLK_OFF */
}

bool a6_cx_up(void)
{
	return is_on(GPUCC + GPUCC_CX_GDSCR) && clk_on(GPUCC + GPUCC_AHB) && clk_on(GPUCC + GPUCC_CX_GMU);
}

bool a6_gx_up(void)
{
	return a6_cx_up() && is_on(GPUCC + GPUCC_GX_GDSCR);
}

bool a6_cp_running(const struct a6_gpu *g)
{
	return a6_gx_up() && rd32(GREG(CP_SQE_INSTR_BASE)) == (uint32_t)a6_pool_va(A6_OFF_SQE) && g->gpu_cb >= 0;
}

void a6_init_state(struct a6_gpu *g, uint64_t pool_pa)
{
	memset(g, 0, sizeof(*g));
	g->pool_pa = pool_pa;
	g->gmu_smr = g->gmu_cb = g->gpu_smr = g->gpu_cb = -1;
}

/* ---- stage 1: clocks and CX power ---- */

static void branch_on(const char *name, uint64_t reg)
{
	wr32(reg, rd32(reg) | 1);
	int rc = wait_bit(reg, 1u << 31, 0, 10);
	a6_log("%s = %x%s\n", name, rd32(reg), rc ? " (still halted)" : "");
}

static int power_cx(void)
{
	a6_log("before: cx_gdsc %x gx_gdsc %x gmu_rcg %x/%x ahb %x cxo %x cx_gmu %x\n",
	       rd32(GPUCC + GPUCC_CX_GDSCR), rd32(GPUCC + GPUCC_GX_GDSCR), rd32(GPUCC + GPUCC_GMU_RCG),
	       rd32(GPUCC + GPUCC_GMU_RCG + 4), rd32(GPUCC + GPUCC_AHB), rd32(GPUCC + GPUCC_CXO),
	       rd32(GPUCC + GPUCC_CX_GMU));
	branch_on("gcc gpu_cfg_ahb", GCC + 0x71004);
	branch_on("gcc ddrss_gpu_axi", GCC + 0x71154);
	branch_on("gcc gpu_memnoc_gfx", GCC + 0x7100c);
	branch_on("gcc gpu_snoc_dvm_gfx", GCC + 0x71018);

	uint64_t gdsc = GPUCC + GPUCC_CX_GDSCR;
	wr32(gdsc, rd32(gdsc) & ~1u);
	int rc = wait_bit(gdsc, 1u << 31, 1, 100);
	a6_log("cx gdsc -> %x %s\n", rd32(gdsc), rc ? "TIMEOUT" : "on");
	if (rc)
		return -1;

	uint64_t rcg = GPUCC + GPUCC_GMU_RCG;
	wr32(rcg + 4, (rd32(rcg + 4) & ~0x71fu) | 1u);
	wr32(rcg, rd32(rcg) | 1u);
	rc = wait_bit(rcg, 1, 0, 10);
	a6_log("gmu rcg %x cfg %x %s\n", rd32(rcg), rd32(rcg + 4), rc ? "UPDATE TIMEOUT" : "");

	branch_on("gpucc ahb", GPUCC + GPUCC_AHB);
	branch_on("gpucc cxo", GPUCC + GPUCC_CXO);
	branch_on("gpucc cx_gmu", GPUCC + GPUCC_CX_GMU);
	if (!a6_cx_up())
		return -1;
	a6_log("gmu fw_version %x init_result %x rpmh_state %x; smmu idr0 %x\n",
	       rd32(GMU_REG(GMU_FW_VERSION)), rd32(GMU_REG(GMU_INIT_RESULT)), rd32(GMU_REG(GMU_RPMH_STATE)),
	       rd32(SMMU + 0x20));
	return 0;
}

/* ---- SMMU ---- */

/* 4K pages for the GMU windows; priv: AP=00 (GMU code), else AP=01. */
static void map_gmu_pages(uint64_t l3, uint32_t va, uint64_t pa, uint32_t size, int priv)
{
	for (uint32_t o = 0; o < size; o += 0x1000) {
		uint32_t idx = ((va + o) >> 12) & 0x1ff;
		wr64(l3 + 8 * idx, (pa + o) | (1u << 10) | (3u << 8) | (priv ? 0 : (1u << 6)) | 3u);
	}
}

/* T0SZ: 32 (4 GB) for the GMU, 25 (39-bit) for the GPU; walks start at level 1 either way. */
static void smmu_bank(int smr, int cb, uint64_t ttbr0, uint32_t sid, uint32_t mask)
{
	uint32_t t0sz = sid == GPU_SID ? 25 : 32;
	uint64_t c = SMMU_CB(cb);
	wr32(c + CB_SCTLR, 0);
	wr32(CBA2R(cb), 1);					/* AArch64 descriptors */
	wr32(CBAR(cb), (1u << 16) | (0xfu << 12) | (3u << 8) | (uint32_t)(cb + 1));
	wr32(c + CB_TCR2, TCR2_VAL);
	wr64(c + CB_TTBR0, ttbr0);
	wr32(c + CB_TCR, t0sz | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23));
	wr32(c + CB_MAIR0, 0x0000ff44);
	wr32(c + CB_FSR, 0xffffffff);
	wr32(c + CB_SCTLR, 1 | 2 | 4 | 0x20 | 0x40);		/* M TRE AFE CFRE CFIE */
	wr32(SMMU + 0x68, 0);					/* TLBIALLNSNH */
	wr32(SMMU + 0x70, 0);					/* TLBGSYNC */
	wait_bit(SMMU + 0x74, 1, 0, 10);
	wr32(S2CR(smr), (uint32_t)cb);
	wr32(SMR(smr), (1u << 31) | (mask << 16) | sid);
	a6_log("smmu: smr%d %x -> cb%d cbar %x sctlr %x tcr %x tcr2 %x\n", smr, rd32(SMR(smr)), cb,
	       rd32(CBAR(cb)), rd32(c + CB_SCTLR), rd32(c + CB_TCR), rd32(c + CB_TCR2));
}

static int smmu_pick(int32_t *smr_out, int32_t *cb_out, int cb_min)
{
	int smr = -1, cb = -1;
	uint32_t used_cb = 0;
	for (int i = 0; i < 6; i++) {
		uint32_t m = rd32(SMR(i)), c = rd32(S2CR(i));
		if ((m >> 31) && ((c >> 16) & 3) == 0)
			used_cb |= 1u << (c & 0xff);
		if (smr < 0 && !(m >> 31))
			smr = i;
	}
	for (int i = cb_min; i < 5; i++)
		if (cb < 0 && !(used_cb & (1u << i)) && !(rd32(SMMU_CB(i) + CB_SCTLR) & 1))
			cb = i;
	*smr_out = smr;
	*cb_out = cb;
	return smr < 0 || cb < 0 ? -1 : 0;
}

void a6_tlb_flush(const struct a6_gpu *g)
{
	uint64_t c = SMMU_CB(g->gpu_cb);
	wr32(c + CB_TLBIALL, 0);
	wr32(c + CB_TLBSYNC, 0);
	wait_bit(c + CB_TLBSTATUS, 1, 0, 10);
}

uint32_t a6_fault_status(const struct a6_gpu *g, uint64_t *far, uint32_t *fsynr0)
{
	if (g->gpu_cb < 0)
		return 0;
	uint64_t c = SMMU_CB(g->gpu_cb);
	if (far)
		*far = rd32(c + CB_FAR) | ((uint64_t)rd32(c + CB_FAR + 4) << 32);
	if (fsynr0)
		*fsynr0 = rd32(c + CB_FSYNR0);
	return rd32(c + CB_FSR) & A6_FSR_FAULT;
}

/*
 * A translation fault blocks the GPU's later writes (the fence never lands)
 * until the context bank's FSR is cleared; clearing it is enough to go on.
 */
int a6_clear_fault(const struct a6_gpu *g)
{
	uint64_t far;
	uint32_t syn, fsr = a6_fault_status(g, &far, &syn);
	if (!fsr)
		return 0;
	a6_log("clearing smmu cb%d fault: fsr %x far %llx fsynr0 %x\n", g->gpu_cb, fsr,
	       (unsigned long long)far, syn);
	wr32(SMMU_CB(g->gpu_cb) + CB_FSR, rd32(SMMU_CB(g->gpu_cb) + CB_FSR));
	return 1;
}

/* ---- stage 2: GMU boot ---- */

static int gmu_smmu(struct a6_gpu *g)
{
	if (smmu_pick(&g->gmu_smr, &g->gmu_cb, 0)) {
		a6_log("smmu: no free smr/cb\n");
		return -1;
	}
	zero_pa(g->pool_pa, 0x100000);
	wr64(GMEM_L1(g) + 0, GMEM_L2_0(g) | 3);
	wr64(GMEM_L1(g) + 8, GMEM_L2_1(g) | 3);
	wr64(GMEM_L2_0(g) + 0, GMEM_L3_LOW(g) | 3);
	wr64(GMEM_L2_1(g) + 8 * ((VA_DUMMY >> 21) & 0x1ff), GMEM_L3_6(g) | 3);
	map_gmu_pages(GMEM_L3_LOW(g), VA_ICACHE, GMEM_ICACHE(g), CACHE_SIZE, 1);
	map_gmu_pages(GMEM_L3_LOW(g), VA_DCACHE, GMEM_DCACHE(g), CACHE_SIZE, 1);
	map_gmu_pages(GMEM_L3_6(g), VA_DUMMY, GMEM_DUMMY(g), 0x1000, 0);
	map_gmu_pages(GMEM_L3_6(g), VA_LOG, GMEM_LOG(g), A6_GMU_LOG_SIZE, 0);
	map_gmu_pages(GMEM_L3_6(g), VA_HFI, GMEM_HFI(g), 0x4000, 0);
	smmu_bank(g->gmu_smr, g->gmu_cb, GMEM_L1(g), GMU_SID, GMU_SID_MASK);
	return 0;
}

/* a6xx_gmu_rpmh_init for the A640 (pdc_address_offset 0x30090). */
static void rpmh_init(void)
{
	static const uint32_t rscc[][2] = {
		{ 0x0004, 1u << 24 }, { 0x000b, 1 }, { 0x000e, 0 }, { 0x000d, 0 }, { 0x0010, 0 }, { 0x000f, 0 },
		{ 0x0012, 0x80000000 }, { 0x0011, 0 }, { 0x0100, 0 }, { 0x0008, 0x4520 }, { 0x0009, 0x4510 },
		{ 0x000a, 0x4514 }, { 0x0180, 0xa7a506a0 }, { 0x0181, 0xa1e6a6e7 }, { 0x0182, 0xa2e081e1 },
		{ 0x0183, 0xe9a982e2 }, { 0x0184, 0x0020e8a8 },
	};
	static const uint32_t seq[] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	static const uint32_t pdc[][2] = {
		{ 0x1573, 7 }, { 0x1574, 0 }, { 0x1572, 0 }, { 0x1575, 0x10108 }, { 0x1576, 0x30010 },
		{ 0x1577, 1 }, { 0x1579, 0x10108 }, { 0x157a, 0x30000 }, { 0x157b, 0 }, { 0x157d, 0x10108 },
		{ 0x157e, 0x30090 }, { 0x157f, 0 },
		{ 0x15d7, 7 }, { 0x15d8, 0 }, { 0x15d6, 0 }, { 0x15d9, 0x10108 }, { 0x15da, 0x30010 },
		{ 0x15db, 2 }, { 0x15dd, 0x10108 }, { 0x15de, 0x30000 }, { 0x15df, 3 }, { 0x15e1, 0x10108 },
		{ 0x15e2, 0x30090 }, { 0x15e3, 3 },
		{ 0x1148, 0 }, { 0x1140, 0x80000001 },
	};
	for (unsigned i = 0; i < sizeof(rscc) / sizeof(rscc[0]); i++)
		wr32(RSCC_REG(rscc[i][0]), rscc[i][1]);
	for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
		wr32(PDC_SEQ + 4 * i, seq[i]);
	for (unsigned i = 0; i < sizeof(pdc) / sizeof(pdc[0]); i++)
		wr32(PDC_REG(pdc[i][0]), pdc[i][1]);
}

static void hfi_table(struct a6_gpu *g)
{
	uint64_t t = GMEM_HFI(g);
	wr32(t + 0, 0);
	wr32(t + 4, 6 * 4 + 3 * 12 * 4);
	wr32(t + 8, 6);
	wr32(t + 12, 12);
	wr32(t + 16, 3);
	wr32(t + 20, 3);
	for (uint32_t q = 0; q < 3; q++) {
		uint64_t h = t + 24 + 48 * q;
		wr32(h + 0, 1);
		wr32(h + 4, VA_HFI + 0x1000 * (q + 1));
		wr32(h + 8, (10u << 8) | q);
		wr32(h + 12, 0x1000 >> 2);
		wr32(h + 24, 1);
		wr32(h + 28, 1);
		wr32(h + 32, 1);
	}
}

static int fw_load(struct a6_gpu *g)
{
	uint32_t len;
	const uint8_t *fw = a6_firmware("a640_gmu.bin", &len);
	if (!fw)
		return -1;
	const uint8_t *p = fw, *end = fw + len;
	int n = 0;
	while (p + 16 <= end) {
		uint32_t addr, size;
		memcpy(&addr, p, 4);
		memcpy(&size, p + 4, 4);
		const uint8_t *data = p + 16;
		uint64_t dst = 0;
		if (addr < 0x4000)
			dst = GMU_REG(GMU_ITCM) + addr;
		else if (addr >= 0x40000 && addr < 0x44000)
			dst = GMU_REG(GMU_DTCM) + (addr - 0x40000);
		else if (addr >= VA_ICACHE && addr + size <= VA_ICACHE + CACHE_SIZE)
			dst = GMEM_ICACHE(g) + (addr - VA_ICACHE);
		else if (addr >= VA_DCACHE && addr + size <= VA_DCACHE + CACHE_SIZE)
			dst = GMEM_DCACHE(g) + (addr - VA_DCACHE);
		if (!dst || data + size > end) {
			a6_log("gmu fw: bad block %x size %x\n", addr, size);
			return -1;
		}
		a6_write(dst, data, (size + 3) & ~3u);
		p = data + size;
		n++;
	}
	return n;
}

static int gmu_boot(struct a6_gpu *g)
{
	if (gmu_smmu(g))
		return -1;
	rpmh_init();
	hfi_table(g);

	wr32(GMU_REG(GMU_GENERAL_7), 1);
	wr32(GMU_REG(GMU_RSCC_REQ), 1u << 1);
	(void)wait_bit(GMU_REG(GMU_RSCC_ACK), 1u << 1, 1, 200);	/* never acks; harmless */
	(void)wait_bit(RSCC_REG(0x0101), 0xffffffff, 0, 10);
	wr32(GMU_REG(GMU_RSCC_REQ), 0);

	int nblk = fw_load(g);
	a6_log("gmu: %d firmware blocks loaded\n", nblk);
	if (nblk <= 0)
		return -1;

	wr32(GMU_REG(GMU_INIT_RESULT), 0);
	wr32(GMU_REG(GMU_BOOT_CFG), 2);
	wr32(GMU_REG(GMU_QTBL_ADDR), VA_HFI);
	wr32(GMU_REG(GMU_QTBL_INFO), 1);
	wr32(GMU_REG(GMU_FENCE0), (1u << 31) | (0xau << 18) | 0xa0);
	wr32(GMU_REG(GMU_CM3_CFG), 0x4052);
	wr32(GMU_REG(GMU_SFR_ADDR), 0x06040100);
	for (uint32_t i = 0; i < 4; i++)
		wr32(GPU + 4 * (0x3c03 + i), 0x00071620);
	wr32(GMU_REG(GMU_CP_MSG), VA_LOG | (A6_GMU_LOG_SIZE / 0x1000 - 1));
	wr32(GMU_REG(GMU_SYS_BUS), 1);
	wr32(GMU_REG(GMU_ICACHE_CFG), 1);
	wr32(GMU_REG(GMU_DCACHE_CFG), 1);
	wr32(GMU_REG(GMU_PWRCOL_CTRL), 0x9c40400);
	wr32(GMU_REG(GMU_RPMH_CTRL), rd32(GMU_REG(GMU_RPMH_CTRL)) |
	     1 | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));

	uint32_t v = rd32(GMU_REG(GMU_DTCM) + 0xff8 * 4);
	uint32_t mask = v <= 0x20010004 ? 0xffffffff : 0x1ff;
	uint32_t want = v <= 0x20010004 ? 0xbabeface : 0x100;
	wr32(GMU_REG(GMU_SYSRESET), 1);
	wr32(GMU_REG(GMU_CP_RESP), 0);
	wr32(GMU_REG(GMU_SYSRESET), 0);
	uint32_t r = 0;
	bool ok = false;
	for (int i = 0; i < 2000 && !ok; i++) {
		r = rd32(GMU_REG(GMU_INIT_RESULT));
		ok = (r & mask) == want;
		if (!ok)
			a6_delay_ms(1);
	}
	a6_log("gmu: init result %x %s, hfi_ctrl %x\n", r, ok ? "BOOTED" : "timeout", rd32(GMU_REG(GMU_HFI_CTRL)));
	return ok ? 0 : -1;
}

/* ---- stage 3: HFI, GX power ---- */

#define HFI_Q_CMD	0
#define HFI_Q_RESP	1
#define QHDR(g, q)	(GMEM_HFI(g) + 24 + 48 * (q))
#define QDATA(g, q)	(GMEM_HFI(g) + 0x1000 * ((q) + 1))
#define QSIZE		1024

#define LVL_MIN_SVS	48
#define LVL_LOW_SVS	64
#define LVL_SVS		128
#define LVL_SVS_L1	192
#define LVL_SVS_L2	224
#define LVL_NOM		256

static const uint32_t gpu_khz[] = { 0, 257000, 345000, 427000, 499200, 585000 };
static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_SVS_L2, LVL_NOM };
static const uint32_t gmu_khz[] = { 0, 200000 };
static const uint32_t gmu_lvl[] = { 0, LVL_MIN_SVS };

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
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

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

static int arc_votes(uint32_t *votes, const uint32_t *lvl, int n, const char *pri_id, const char *sec_id)
{
	uint64_t pri, sec;
	uint32_t plen, slen;
	if (cmddb_aux(pri_id, &pri, &plen) || cmddb_aux(sec_id, &sec, &slen))
		return -1;
	plen /= 2;
	slen /= 2;
	for (int i = 0; i < n; i++) {
		uint32_t pindex = 0, sindex = 0, j;
		for (j = 0; j < plen; j++)
			if (aux16(pri, j) >= lvl[i]) {
				pindex = j;
				break;
			}
		if (j == plen)
			return -1;
		for (j = 0; j < slen; j++) {
			if (aux16(sec, j) >= lvl[i]) {
				sindex = j;
				break;
			} else if (aux16(sec, j)) {
				sindex = j;
			}
		}
		votes[i] = (aux16(pri, pindex) << 16) | (sindex << 8) | pindex;
	}
	return 0;
}

static int hfi_send(struct a6_gpu *g, uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t seq = ++g->hfi_seq;
	msg[0] = (seq << 20) | (dwords << 8) | id;
	uint64_t h = QHDR(g, HFI_Q_CMD);
	uint32_t w = rd32(h + 44);
	for (uint32_t i = 0; i < dwords; i++) {
		wr32(QDATA(g, HFI_Q_CMD) + 4 * w, msg[i]);
		w = (w + 1) % QSIZE;
	}
	for (; w % 4; w = (w + 1) % QSIZE)
		wr32(QDATA(g, HFI_Q_CMD) + 4 * w, 0xfafafafa);
	wr32(h + 44, w);
	wr32(GMU_REG(GMU_H2G_SET), 1);

	for (int t = 0; t < 1000; t++) {
		if (rd32(GMU_REG(GMU_G2H_INFO)) & 1) {
			wr32(GMU_REG(GMU_G2H_CLR), 1);
			uint64_t rh = QHDR(g, HFI_Q_RESP);
			while (rd32(rh + 40) != rd32(rh + 44)) {
				uint32_t r = rd32(rh + 40), hdr = rd32(QDATA(g, HFI_Q_RESP) + 4 * r);
				uint32_t n = (hdr >> 8) & 0xff, buf[24] = { 0 };
				for (uint32_t i = 0; i < n && i < 24; i++) {
					buf[i] = rd32(QDATA(g, HFI_Q_RESP) + 4 * r);
					r = (r + 1) % QSIZE;
				}
				r = ((r + 3) & ~3u) % QSIZE;
				wr32(rh + 40, r);
				if ((hdr & 0xff) != 100 && (buf[1] >> 20) == seq)
					return (int)buf[2];
			}
		}
		a6_delay_ms(1);
	}
	a6_log("hfi: msg %u seq %u: no ack (info %x)\n", id, seq, rd32(GMU_REG(GMU_G2H_INFO)));
	return -1;
}

static int gmu_hfi(struct a6_gpu *g)
{
	static uint32_t msg[256];
	uint32_t gx[6], cx[2];
	if (arc_votes(gx, gpu_lvl, 6, "gfx.lvl", "mx.lvl") || arc_votes(cx, gmu_lvl, 2, "cx.lvl", "mx.lvl")) {
		a6_log("hfi: cmd-db levels not found\n");
		return -1;
	}
	memset(msg, 0, sizeof(msg));
	msg[1] = 6;
	msg[2] = 2;
	for (int i = 0; i < 6; i++) {
		msg[3 + 3 * i] = gx[i];
		msg[4 + 3 * i] = 0xffffffff;
		msg[5 + 3 * i] = gpu_khz[i];
	}
	for (int i = 0; i < 2; i++) {
		msg[3 + 48 + 2 * i] = cx[i];
		msg[4 + 48 + 2 * i] = gmu_khz[i];
	}
	int rc = hfi_send(g, 4, msg, 3 + 48 + 8);

	memset(msg, 0, sizeof(msg));
	static const uint32_t bw[][2] = {
		{ 1, 1 }, { 2, 3 }, { 3, 3 }, { 4, 1 }, { 5, 1 }, { 6, 0x50034 }, { 7, 0x5007c }, { 8, 0x5004c },
		{ 12, 0x40000000 }, { 14, 0x40000000 }, { 18, 0x60000001 }, { 19, 0x20000001 }, { 20, 0x60000001 },
		{ 24, 0x50000 }, { 25, 0x5003c }, { 26, 0x5000c }, { 32, 0x40000000 }, { 33, 0x40000000 },
		{ 34, 0x40000000 },
	};
	for (unsigned i = 0; i < sizeof(bw) / sizeof(bw[0]); i++)
		msg[bw[i][0]] = bw[i][1];
	rc |= hfi_send(g, 3, msg, 32 + 128);
	msg[1] = 0;
	rc |= hfi_send(g, 14, msg, 2);
	rc |= hfi_send(g, 10, msg, 1);
	uint32_t lvl = g->perf_level >= 1 && g->perf_level <= 5 ? g->perf_level : 1;
	msg[1] = 1;
	msg[2] = lvl;			/* index into gpu_khz */
	msg[3] = 0;
	rc |= hfi_send(g, 30, msg, 4);
	a6_log("hfi: perf/bw tables, core fw start, start, perf vote %u MHz -> %d\n", gpu_khz[lvl] / 1000, rc);

	wr32(GMU_REG(GMU_H2G_SET), 1u << 30);	/* OOB GPU_SET */
	int ack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << 31, 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << 31);
	a6_log("oob gpu_set %s, gx gdsc %x\n", ack ? "TIMEOUT" : "acked", rd32(GPUCC + GPUCC_GX_GDSCR));
	if (!a6_gx_up()) {
		a6_log("GX not powered; not touching GPU registers\n");
		return -1;
	}
	return 0;
}

/* ---- zap shader through TrustZone PAS ---- */

#define PAS_GPU		13
#define ZAP_REGION	0x9a915000ull	/* reserved-memory pil_gpu_region */
#define ZAP_SIZE	0x2000u
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	a6_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static int zap_load(struct a6_gpu *g)
{
	uint64_t r1 = 0;
	uint32_t mlen, slen;
	const uint8_t *mdt = a6_firmware("a640_zap.mdt", &mlen), *b02 = a6_firmware("a640_zap.b02", &slen);
	if (!mdt || !b02 || slen > ZAP_SIZE)
		return -1;
	a6_write(a6_pool_pa(g, A6_OFF_ZAP_META), mdt, mlen);
	int64_t rc = scm(1, 2 | (2u << 6), PAS_GPU, a6_pool_pa(g, A6_OFF_ZAP_META), 0, &r1);
	if (rc || r1) {
		a6_log("zap: init image rc %lld r1 %llx\n", (long long)rc, (unsigned long long)r1);
		return -1;
	}
	rc = scm(2, 3, PAS_GPU, ZAP_REGION, ZAP_SIZE, &r1);
	if (rc || r1) {
		a6_log("zap: mem setup rc %lld r1 %llx\n", (long long)rc, (unsigned long long)r1);
		return -1;
	}
	static uint8_t seg[ZAP_SIZE];
	memset(seg, 0, sizeof(seg));
	memcpy(seg, b02, slen);
	a6_write(ZAP_REGION, seg, ZAP_SIZE);
	rc = scm(5, 1, PAS_GPU, 0, 0, &r1);
	a6_log("zap: auth and reset rc %lld r1 %llx\n", (long long)rc, (unsigned long long)r1);
	return rc || r1 ? -1 : 0;
}

/* ---- ring ---- */

static uint32_t pm4_parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^
				  (v >> 24) ^ (v >> 28)))) & 1;
}

uint32_t a6_pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000 | cnt | (pm4_parity(cnt) << 15) | ((op & 0x7f) << 16) | (pm4_parity(op) << 23);
}

uint32_t a6_pkt4(uint32_t reg, uint32_t cnt)
{
	return 0x40000000 | cnt | (pm4_parity(cnt) << 7) | ((reg & 0x3ffff) << 8) | (pm4_parity(reg) << 27);
}

int a6_ring(struct a6_gpu *g, const uint32_t *dw, uint32_t n, const char *what, unsigned timeout_ms)
{
	if (n >= A6_RING_DW)
		return -1;
	uint32_t w = g->wptr, first = n < A6_RING_DW - w ? n : A6_RING_DW - w;
	a6_write(a6_pool_pa(g, A6_OFF_RING) + 4ull * w, dw, 4ull * first);
	if (first < n)
		a6_write(a6_pool_pa(g, A6_OFF_RING), dw + first, 4ull * (n - first));
	g->wptr = (w + n) % A6_RING_DW;
	wr32(GREG(CP_RB_WPTR), g->wptr);
	/* poll: a few reads, then 20 us sleeps (1 ms ones made every submit take whole milliseconds) */
	unsigned i;
	for (i = 0; i < timeout_ms * 50 + 8; i++) {
		if (rd32(GREG(CP_RB_RPTR)) == g->wptr && !(rd32(GREG(RBBM_STATUS)) & (1u << 23)))
			break;
		if (i >= 8)
			a6_delay_us(20);
	}
	bool ok = i < timeout_ms * 50 + 8;
	if (what || !ok)
		a6_log("ring %s: rptr %x wptr %x status %x int %x hwfault %x cp_status1 %x %s\n",
		       what ? what : "", rd32(GREG(CP_RB_RPTR)), g->wptr, rd32(GREG(RBBM_STATUS)),
		       rd32(GREG(RBBM_INT_STATUS)), rd32(GREG(CP_HW_FAULT)), rd32(GREG(CP_STATUS_1)),
		       ok ? "idle" : "TIMEOUT");
	return ok ? 0 : -1;
}

uint32_t a6_fence_read(const struct a6_gpu *g)
{
	uint32_t v = 0;
	a6_read(a6_pool_pa(g, A6_OFF_SCRATCH), &v, 4);
	return v;
}

uint32_t a6_submit_ibs_async(struct a6_gpu *g, const uint64_t *iova, const uint32_t *dwords, unsigned nib,
			     unsigned timeout_ms)
{
	uint64_t fva = a6_pool_va(A6_OFF_SCRATCH);
	/* GPU time of this submit: CP_ALWAYS_ON_COUNTER before the IBs and once they are idle (a6_submit_time) */
	uint64_t ts = a6_pool_va(A6_OFF_SCRATCH + A6_TS_OFF) + 16ull * ((g->fence + 1) % A6_TS_SLOTS);
	uint32_t dw[4 + 4 + 4 * A6_SUBMIT_MAX + 5 + 5], n = 0;
	if (nib == 0 || nib > A6_SUBMIT_MAX)
		return 0;
	dw[n++] = a6_pkt7(A6_CP_EVENT_WRITE, 1);
	dw[n++] = 0x18;				/* PC_CCU_INVALIDATE_DEPTH */
	dw[n++] = a6_pkt7(A6_CP_EVENT_WRITE, 1);
	dw[n++] = 0x19;				/* PC_CCU_INVALIDATE_COLOR */
	dw[n++] = a6_pkt7(0x3e, 3);		/* CP_REG_TO_MEM CP_ALWAYS_ON_COUNTER, 2 dwords -> ts */
	dw[n++] = 0x980 | 2u << 18;
	dw[n++] = (uint32_t)ts;
	dw[n++] = (uint32_t)(ts >> 32);
	for (unsigned i = 0; i < nib; i++) {
		dw[n++] = a6_pkt7(A6_CP_INDIRECT_BUFFER, 3);
		dw[n++] = (uint32_t)iova[i];
		dw[n++] = (uint32_t)(iova[i] >> 32);
		dw[n++] = dwords[i] & 0xfffff;
	}
	dw[n++] = a6_pkt7(A6_CP_WAIT_FOR_IDLE, 0);
	dw[n++] = a6_pkt7(0x3e, 3);		/* ... -> ts + 8 */
	dw[n++] = 0x980 | 2u << 18;
	dw[n++] = (uint32_t)(ts + 8);
	dw[n++] = (uint32_t)((ts + 8) >> 32);
	dw[n++] = a6_pkt7(A6_CP_EVENT_WRITE, 4);
	dw[n++] = 0x04;				/* CACHE_FLUSH_TS: the fence lands once the caches are clean */
	dw[n++] = (uint32_t)fva;
	dw[n++] = (uint32_t)(fva >> 32);
	dw[n++] = g->fence + 1;
	/* room in the ring: what the CP has not read yet stays intact */
	for (unsigned i = 0; ; i++) {
		uint32_t r = rd32(GREG(CP_RB_RPTR)), used = (g->wptr + A6_RING_DW - r) % A6_RING_DW;
		if (used + n + 16 < A6_RING_DW)
			break;
		if (i >= timeout_ms * 50)
			return 0;
		a6_delay_us(20);
	}
	uint32_t w = g->wptr, first = n < A6_RING_DW - w ? n : A6_RING_DW - w;
	a6_write(a6_pool_pa(g, A6_OFF_RING) + 4ull * w, dw, 4ull * first);
	if (first < n)
		a6_write(a6_pool_pa(g, A6_OFF_RING), dw + first, 4ull * (n - first));
	g->wptr = (w + n) % A6_RING_DW;
	wr32(GREG(CP_RB_WPTR), g->wptr);
	return ++g->fence;
}

uint32_t a6_submit_ib_async(struct a6_gpu *g, uint64_t iova, uint32_t dwords, unsigned timeout_ms)
{
	return a6_submit_ibs_async(g, &iova, &dwords, 1, timeout_ms);
}

void a6_fence_force(struct a6_gpu *g)
{
	uint32_t v = g->fence;
	a6_write(a6_pool_pa(g, A6_OFF_SCRATCH), &v, 4);
}

uint64_t a6_submit_time(const struct a6_gpu *g, uint32_t fence)
{
	uint64_t t[2] = { 0, 0 };
	a6_read(a6_pool_pa(g, A6_OFF_SCRATCH + A6_TS_OFF) + 16ull * (fence % A6_TS_SLOTS), t, 16);
	return t[1] > t[0] ? t[1] - t[0] : 0;
}

uint64_t a6_timestamp(const struct a6_gpu *g)
{
	(void)g;
	uint32_t hi = rd32(GREG(0x0981)), lo = rd32(GREG(0x0980)), hi2 = rd32(GREG(0x0981));
	if (hi2 != hi)
		lo = rd32(GREG(0x0980));
	return (uint64_t)hi2 << 32 | lo;
}

bool a6_fence_done(const struct a6_gpu *g, uint32_t fence)
{
	return (int32_t)(a6_fence_read(g) - fence) >= 0;
}

int a6_submit_ib(struct a6_gpu *g, uint64_t iova, uint32_t dwords, unsigned timeout_ms)
{
	uint64_t fva = a6_pool_va(A6_OFF_SCRATCH);
	uint32_t fence = ++g->fence;
	uint32_t dw[] = {
		a6_pkt7(A6_CP_INDIRECT_BUFFER, 3), (uint32_t)iova, (uint32_t)(iova >> 32), dwords,
		a6_pkt7(A6_CP_WAIT_FOR_IDLE, 0),
		a6_pkt7(A6_CP_MEM_WRITE, 3), (uint32_t)fva, (uint32_t)(fva >> 32), fence,
	};
	int rc = a6_ring(g, dw, sizeof(dw) / 4, NULL, timeout_ms);
	return rc || a6_fence_read(g) != fence ? -1 : 0;
}

/* ---- GPU page tables ---- */

static void map_gpu_low(struct a6_gpu *g)
{
	uint64_t l1 = a6_pool_pa(g, A6_OFF_GT_L1), l2 = a6_pool_pa(g, A6_OFF_GT_L2);
	zero_pa(l1, 0x2000);
	wr64(l1 + 0, l2 | 3);		/* VA 0-1 GB */
	for (uint64_t o = 0; o < A6_POOL_SIZE; o += 0x200000)	/* AP=01, NC, 2 MB blocks */
		wr64(l2 + 8 * (((A6_GVA_POOL + o) >> 21) & 0x1ff),
		     (g->pool_pa + o) | (1u << 10) | (3u << 8) | (1u << 6) | 1u);
	for (uint64_t o = 0; o < A6_FB_SIZE; o += 0x200000)
		wr64(l2 + 8 * (((A6_GVA_FB + o) >> 21) & 0x1ff),
		     (A6_FB_PA + o) | (1u << 10) | (3u << 8) | (1u << 6) | 1u);
}

static uint64_t pt_alloc(struct a6_gpu *g)
{
	const uint32_t arena = (uint32_t)((A6_OFF_SQE - A6_OFF_PT_ARENA) / 0x1000);
	uint64_t pa = g->pt_next < arena ? a6_pool_pa(g, A6_OFF_PT_ARENA + 0x1000ull * g->pt_next) :
	    a6_pt_extra(g->pt_next - arena);
	if (!pa)
		return 0;
	g->pt_next++;
	zero_pa(pa, 0x1000);
	return pa;
}

static uint64_t pt_walk(struct a6_gpu *g, uint64_t table, unsigned idx)
{
	uint64_t e = 0;
	a6_read(table + 8ull * idx, &e, 8);
	if (e & 1)
		return (e & 3) == 3 ? e & 0xfffffff000ull : 0;	/* 0: a block */
	uint64_t t = pt_alloc(g);
	if (t)
		wr64(table + 8ull * idx, t | 3);
	return t;
}

/* 4K entries for [iova, iova + n * 4K) taking the physical address of each 4K page from pa_of. */
static int map_4k(struct a6_gpu *g, uint64_t iova, uint32_t n4k, uint64_t (*pa_of)(const void *, uint32_t),
		  const void *arg)
{
	static uint64_t pte[512];
	uint64_t l1 = a6_pool_pa(g, A6_OFF_GT_L1);
	for (uint32_t i = 0; i < n4k;) {
		uint64_t va = iova + 0x1000ull * i;
		uint64_t l2 = pt_walk(g, l1, (va >> 30) & 0x1ff);
		uint64_t l3 = l2 ? pt_walk(g, l2, (va >> 21) & 0x1ff) : 0;
		if (!l3) {
			a6_log("map: no page table for %llx (arena full, or a block there)\n", (unsigned long long)va);
			return -1;
		}
		unsigned first = (va >> 12) & 0x1ff, k = 0;
		while (first + k < 512 && i < n4k)
			pte[k++] = pa_of(arg, i++) | (1u << 10) | (3u << 8) | (1u << 6) | 3u;
		a6_write(l3 + 8ull * first, pte, 8ull * k);
	}
	return 0;
}

static uint64_t linear_pa(const void *arg, uint32_t i)
{
	return *(const uint64_t *)arg + 0x1000ull * i;
}

int a6_map(struct a6_gpu *g, uint64_t iova, uint64_t pa, uint64_t size)
{
	return map_4k(g, iova, (uint32_t)((size + 0xfff) >> 12), linear_pa, &pa);
}

struct page_list {
	const uint64_t *pages;
	uint32_t shift;		/* log2(page_size / 4K) */
};

static uint64_t list_pa(const void *arg, uint32_t i)
{
	const struct page_list *l = arg;
	return l->pages[i >> l->shift] + 0x1000ull * (i & ((1u << l->shift) - 1));
}

int a6_map_pages(struct a6_gpu *g, uint64_t iova, const uint64_t *pages, uint32_t npages, uint32_t page_size)
{
	struct page_list l = { pages, 0 };
	while ((0x1000u << l.shift) < page_size)
		l.shift++;
	return map_4k(g, iova, npages << l.shift, list_pa, &l);
}

/* Clear the 4K entries for [iova, iova + size) (tables stay, for the next mapping there). Then a6_tlb_flush. */
void a6_unmap(struct a6_gpu *g, uint64_t iova, uint64_t size)
{
	static const uint64_t zeros[512];
	uint64_t l1 = a6_pool_pa(g, A6_OFF_GT_L1);
	uint64_t end = iova + ((size + 0xfff) & ~0xfffull);
	for (uint64_t va = iova & ~0xfffull; va < end;) {
		uint64_t e = 0, l2, l3;
		a6_read(l1 + 8 * ((va >> 30) & 0x1ff), &e, 8);
		l2 = (e & 3) == 3 ? e & 0xfffffff000ull : 0;
		e = 0;
		if (l2)
			a6_read(l2 + 8 * ((va >> 21) & 0x1ff), &e, 8);
		l3 = (e & 3) == 3 ? e & 0xfffffff000ull : 0;
		uint64_t next = (va | 0x1fffffull) + 1;
		if (next > end)
			next = end;
		if (l3)
			a6_write(l3 + 8 * ((va >> 12) & 0x1ff), zeros, (size_t)((next - va) >> 12) * 8);
		va = next;
	}
}

void a6_unmap_high(struct a6_gpu *g)
{
	static const uint64_t zeros[511];
	a6_write(a6_pool_pa(g, A6_OFF_GT_L1) + 8, zeros, sizeof(zeros));
	g->pt_next = 0;
	/* Banks set up before 39-bit iovas were needed had T0SZ 32 and SEP 0. */
	uint64_t c = SMMU_CB(g->gpu_cb);
	wr32(c + CB_TCR, 25 | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23));
	wr32(c + CB_TCR2, TCR2_VAL);
	a6_tlb_flush(g);
}

/* ---- stage 4: GPU SMMU bank, hw_init, SQE, CP ---- */

/* The zap shader is already in TrustZone after a GPU reset: tell TZ to resume it (Linux: SCM_GPU_ZAP_SHADER_RESUME) */
static int zap_resume(void)
{
	uint64_t x[8] = { 0x42000000ull | (1u << 8) | 0x0a, 2, 0 /* resume */, PAS_GPU, 0, 0, 0, 0 };
	a6_smc(x);
	a6_log("zap: resume rc %lld\n", (long long)x[0]);
	return x[0] ? -1 : 0;
}

static int gpu_cp_init(struct a6_gpu *g, bool resume);

static int gpu_cp(struct a6_gpu *g)
{
	if (smmu_pick(&g->gpu_smr, &g->gpu_cb, g->gmu_cb + 1)) {
		a6_log("smmu: no free smr/cb for the GPU\n");
		return -1;
	}
	map_gpu_low(g);
	smmu_bank(g->gpu_smr, g->gpu_cb, a6_pool_pa(g, A6_OFF_GT_L1), GPU_SID, GPU_SID_MASK);
	return gpu_cp_init(g, false);
}

/* After a hang or fault: soft reset of the GPU core, then the CP up again (page tables and SMMU banks kept). */
int a6_recover(struct a6_gpu *g)
{
	(void)a6_clear_fault(g);
	wr32(GREG(0x43), 1);			/* RBBM_SW_RESET_CMD */
	(void)rd32(GREG(0x43));
	a6_delay_us(100);
	wr32(GREG(0x43), 0);
	a6_delay_ms(1);
	(void)a6_clear_fault(g);
	int rc = gpu_cp_init(g, true);
	a6_log("recover: soft reset + CP init -> %d\n", rc);
	return rc;
}

static int gpu_cp_init(struct a6_gpu *g, bool resume)
{
	/* a6xx hw_init for the A640 (a6xx_gpu.c), minus clock gating, CP protection and interrupts */
	wr32(GREG(0x3c45), 0);			/* GBIF_HALT */
	(void)rd32(GREG(0x3c45));
	wr32(GREG(0x16), 0);			/* RBBM_GBIF_HALT */
	(void)rd32(GREG(0x16));
	static const uint32_t init[][2] = {
		{ 0xf803, 0 }, { 0xf800, 0 }, { 0xf801, 0 }, { 0xf802, 0 },	/* SECVID TSB */
		/* 64-bit addressing: CP VSC GRAS RB PC HLSQ VFD VPC UCHE SP TPL1 SECVID */
		{ 0x0842, 1 }, { 0x0c01, 1 }, { 0x8601, 1 }, { 0x8e05, 1 }, { 0x9e01, 1 }, { 0xbe05, 1 },
		{ 0xa601, 1 }, { 0x9601, 1 }, { 0x0e00, 1 }, { 0xae01, 1 }, { 0xb601, 1 }, { 0xf810, 1 },
		{ 0x0011, 3 },				/* RBBM_GBIF_CLIENT_QOS_CNTL */
		{ 0x050b, 0xffffffff },			/* RBBM_PERFCTR_GPU_BUSY_MASKED */
		/* UCHE: write range max, trap base, write-through base (trap 0x1fffffffff000) */
		{ 0x0e05, 0xffffffc0 }, { 0x0e06, 0x1ffff }, { 0x0e09, 0xfffff000 }, { 0x0e0a, 0x1ffff },
		{ 0x0e07, 0xfffff000 }, { 0x0e08, 0x1ffff },
		{ 0x0e0b, 0x100000 }, { 0x0e0c, 0 }, { 0x0e0d, 0x1fffff }, { 0x0e0e, 0 },	/* GMEM range */
		{ 0x0e18, 0x804 }, { 0x0e17, 0x4 },	/* UCHE_FILTER_CNTL, CACHE_WAYS */
		{ 0x08c2, 0x02000140 }, { 0x08c1, 0x8040362c }, { 0x08c3, 128 },	/* ROQ, mem pool */
		{ 0x098d, 1 }, { 0x0500, 1 },		/* CP_AHB_CNTL, RBBM_PERFCTR_CNTL */
		{ 0x001f, (1u << 30) | 0x1fffff },	/* RBBM_INTERFACE_HANG_INT_CNTL */
		{ 0x0e19, (1u << 7) | 1 },		/* UCHE_CLIENT_PF */
		{ 0x9e00, 0x00180000 },			/* PC_DBG_ECO_CNTL: prim_fifo_threshold */
		{ 0x08d0, 0 },				/* CP_PERFCTR_CP_SEL[0]: always count */
		/* a6xx_set_ubwc_config: highest bank bit 15 (hbb_lo 2), AMSBC, UBWC 3 */
		{ 0x8e08, (1u << 4) | (2u << 1) }, { 0xb604, 2u << 1 }, { 0xae02, 2u << 1 },
		{ 0x0e01, 2u << 21 }, { 0x0534, 0 },
	};
	for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++)
		wr32(GREG(init[i][0]), init[i][1]);

	uint32_t len;
	const uint8_t *sqe = a6_firmware("a630_sqe.fw", &len);
	if (!sqe || len < 8 || len > 0x10000)
		return -1;
	a6_write(a6_pool_pa(g, A6_OFF_SQE), sqe + 4, (len - 4 + 3) & ~3u);	/* minus the first dword */
	wr32(GREG(CP_SQE_INSTR_BASE), (uint32_t)a6_pool_va(A6_OFF_SQE));
	wr32(GREG(CP_SQE_INSTR_BASE + 1), 0);
	wr32(GREG(CP_RB_BASE), (uint32_t)a6_pool_va(A6_OFF_RING));
	wr32(GREG(CP_RB_BASE + 1), 0);
	wr32(GREG(CP_RB_CNTL), 0x0800020c);	/* 32K ring, 32-byte blocks, no rptr update */
	g->wptr = 0;
	wr32(GREG(CP_SQE_CNTL), 1);

	uint32_t me_init[] = { a6_pkt7(A6_CP_ME_INIT, 8), 0x2f, 3, 0x20000000, 0, 0, 0, 0, 0 };
	if (a6_ring(g, me_init, 9, "me_init", 1000))
		return -1;
	if (resume ? zap_resume() : zap_load(g))
		return -1;
	uint32_t secure[] = { a6_pkt7(A6_CP_SET_SECURE_MODE, 1), 0 };
	return a6_ring(g, secure, 2, "secure mode off", 1000);
}

int a6_power_up(struct a6_gpu *g)
{
	if (power_cx() || gmu_boot(g) || gmu_hfi(g) || gpu_cp(g))
		return -1;
	return 0;
}

/* ---- 2D blit (turnip's r2d path for vkCmdBlitImage, captured on the A640) ---- */

/* The A640 state turnip writes before any work (tu6_init_hw + the device's magic registers). */
static const uint32_t blit_init[][2] = {
	{ 0x9804, 0x1f }, { 0x9805, 1 }, { 0xa0f8, 1 }, { 0xb600, 0x8000 }, { 0x8600, 0 },
	{ 0xae03, 0x420 }, { 0xae00, 0 }, { 0xbe04, 0 }, { 0x9600, 0x2000000 }, { 0x0e12, 1 },
	{ 0x8e01, 1 }, { 0x8e04, 0x4100000 }, { 0xae04, 0 }, { 0xae0f, 0x3f }, { 0xb605, 0x44 },
	{ 0xbe00, 0x80 }, { 0xbe01, 0 }, { 0xab20, 0 }, { 0xb182, 0 }, { 0xbb11, 0 }, { 0xa9a8, 0 },
	{ 0xab00, 5 }, { 0xa009, 3 }, { 0x8811, 0x10 }, { 0x8110, 0 }, { 0x8818, 0 }, { 0x8819, 0 },
	{ 0x881a, 0 }, { 0x881b, 0 }, { 0x881c, 0 }, { 0x881d, 0 }, { 0x881e, 0 }, { 0x88f0, 0 },
	{ 0x9236, 0 }, { 0x9300, 0 }, { 0x9306, 1 }, { 0xb183, 0 }, { 0x80af, 0 }, { 0x8099, 0 },
	{ 0x980a, 0 }, { 0x9210, 0 }, { 0x9211, 0 }, { 0x9602, 0 }, { 0x9e72, 0 }, { 0xb309, 0xa2 },
	{ 0xb986, 0xfc }, { 0xa007, 0 }, { 0x8864, 0 }, { 0xb800, 0x102 },
	{ 0x8e07, 0x10000000 },		/* RB_CCU_CNTL: sysmem, full CCU */
};

#define EV_CACHE_FLUSH_TS	0x04
#define EV_PC_CCU_INVALIDATE_COLOR 0x19
#define EV_PC_CCU_FLUSH_DEPTH_TS 0x1c
#define EV_PC_CCU_FLUSH_COLOR_TS 0x1d
#define EV_CACHE_INVALIDATE	0x31

uint32_t a6_blit_ib(const struct a6_gpu *g, const struct a6_blit *b, uint32_t *dw)
{
	(void)g;
	uint32_t n = 0;
	uint64_t border = a6_pool_va(A6_OFF_SCRATCH + 0x800), ts = a6_pool_va(A6_OFF_SCRATCH + 0x900);
#define EV(e)		do { dw[n++] = a6_pkt7(A6_CP_EVENT_WRITE, 1); dw[n++] = (e); } while (0)
#define EV_TS(e)	do { dw[n++] = a6_pkt7(A6_CP_EVENT_WRITE, 4); dw[n++] = (e); dw[n++] = (uint32_t)ts; \
			     dw[n++] = (uint32_t)(ts >> 32); dw[n++] = 0; } while (0)
#define REG(r, v)	do { dw[n++] = a6_pkt4((r), 1); dw[n++] = (v); } while (0)
	EV(EV_CACHE_INVALIDATE);
	REG(0xbb08, 0xfffff);		/* SP_UPDATE_CNTL */
	dw[n++] = a6_pkt7(A6_CP_WAIT_FOR_IDLE, 0);
	for (unsigned i = 0; i < sizeof(blit_init) / sizeof(blit_init[0]); i++)
		REG(blit_init[i][0], blit_init[i][1]);
	dw[n++] = a6_pkt4(0xb302, 2);	/* TPL1_GFX_BORDER_COLOR_BASE */
	dw[n++] = (uint32_t)border;
	dw[n++] = (uint32_t)(border >> 32);
	dw[n++] = a6_pkt4(0xb180, 2);	/* TPL1_CS_BORDER_COLOR_BASE */
	dw[n++] = (uint32_t)border;
	dw[n++] = (uint32_t)(border >> 32);
	EV_TS(EV_PC_CCU_FLUSH_COLOR_TS);
	EV(EV_PC_CCU_INVALIDATE_COLOR);
	EV(EV_CACHE_INVALIDATE);
	dw[n++] = a6_pkt7(A6_CP_WAIT_FOR_IDLE, 0);

	/* Blend: colour src * SRC_ALPHA + dst * ONE_MINUS_SRC_ALPHA, alpha src + dst * (1 - src alpha). */
	REG(0x8c01, b->blend ? 0x3809c181 : 0);	/* RB_A2D_PIXEL_CNTL */
	uint32_t cntl = 0x00f03000 | (b->clip ? 1u << 16 : 0);
	REG(0x8c00, cntl);		/* RB_A2D_BLT_CNTL: 8888 UNORM, mask f, SCISSOR */
	REG(0x8400, cntl);		/* GRAS_A2D_BLT_CNTL */
	if (b->clip) {
		dw[n++] = a6_pkt4(0x840a, 2);	/* GRAS_A2D_SCISSOR_TL, _BR (inclusive) */
		dw[n++] = b->cx0 | (b->cy0 << 16);
		dw[n++] = b->cx1 | (b->cy1 << 16);
	}
	REG(0xacc0, 0xf180);		/* SP_A2D_OUTPUT_INFO */
	dw[n++] = a6_pkt4(0x8405, 2);	/* GRAS_A2D_DEST_TL, _BR (inclusive) */
	dw[n++] = b->dx | (b->dy << 16);
	dw[n++] = (b->dx + b->dw - 1) | ((b->dy + b->dh - 1) << 16);
	dw[n++] = a6_pkt4(0x8401, 4);	/* GRAS_A2D_SRC_XMIN..YMAX, 24.8 fixed, inclusive */
	dw[n++] = b->sx << 8;
	dw[n++] = (b->sx + b->sw - 1) << 8;
	dw[n++] = b->sy << 8;
	dw[n++] = (b->sy + b->sh - 1) << 8;
	dw[n++] = a6_pkt4(0xb4c0, 5);	/* TPL1_A2D_SRC_TEXTURE_INFO, SIZE, BASE, PITCH */
	dw[n++] = 0x00500430 | (b->filter ? 0x10000 : 0);	/* 8888, linear, WXYZ, UNK20|UNK22 */
	dw[n++] = b->src_w | (b->src_h << 15);
	dw[n++] = (uint32_t)b->src_iova;
	dw[n++] = (uint32_t)(b->src_iova >> 32);
	dw[n++] = b->src_pitch << 3;
	dw[n++] = a6_pkt4(0xb4ca, 3);	/* TPL1_A2D_SRC_TEXTURE_FLAG_BASE, _PITCH */
	dw[n++] = (uint32_t)b->src_iova;
	dw[n++] = (uint32_t)(b->src_iova >> 32);
	dw[n++] = 0;
	dw[n++] = a6_pkt4(0x8c17, 4);	/* RB_A2D_DEST_BUFFER_INFO, BASE, PITCH (64 B units) */
	dw[n++] = 0x430;
	dw[n++] = (uint32_t)b->dst_iova;
	dw[n++] = (uint32_t)(b->dst_iova >> 32);
	dw[n++] = b->dst_pitch >> 6;
	dw[n++] = a6_pkt4(0x8c20, 3);	/* RB_A2D_DEST_FLAG_BUFFER_BASE, _PITCH */
	dw[n++] = (uint32_t)b->dst_iova;
	dw[n++] = (uint32_t)(b->dst_iova >> 32);
	dw[n++] = 0;
	dw[n++] = a6_pkt7(A6_CP_BLIT, 1);
	dw[n++] = 3;			/* BLIT_OP_SCALE */
	EV_TS(EV_PC_CCU_FLUSH_COLOR_TS);
	EV_TS(EV_PC_CCU_FLUSH_DEPTH_TS);
	EV_TS(EV_CACHE_FLUSH_TS);
#undef EV
#undef EV_TS
#undef REG
	return n;
}
#endif
