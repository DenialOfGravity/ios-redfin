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
#include <pexpert/pexpert.h>
#else
#include <string.h>
#endif
#include "sweet_a6xx.h"

int a6_submit_wfi;	/* 0157: CP_WAIT_FOR_IDLE after each client submit (boot-arg sweetwfi) */
/*
 * 0988: boot-arg sweetgputs=1: a submit's end stamp is written by CP_EVENT_WRITE RB_DONE_TS | TIMESTAMP (the
 * ALWAYS_ON counter once the rendering before it is done, as Turnip's A6XX timestamps) instead of CP_REG_TO_MEM,
 * and its start is max(its CP stamp, the previous submit's end). Without the 0157 WFI both CP stamps are taken when
 * the CP gets there, not when the GPU is done: the 5 s "GPU busy" sums measure the CP's progress, not the GPU's.
 */
int a6_submit_rbts;

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
#define GPU_SID		0
#if A6_GPU_ID == 618
/* A618 (0141): DT #iommu-cells = 1 → exact SIDs, mask 0 (the hypervisor drops SMRs with flame's masks) */
#define GMU_SID_MASK	0
#define GPU_SID_MASK	0
#else
/* lito/sm8150 DT: #iommu-cells = 2, gmu_kernel <5 0x400>, gfx3d_user <0 0x401> (smmu_bank falls back to 0) */
#define GMU_SID_MASK	0x400
#define GPU_SID_MASK	0x401
#endif

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
#if A6_GPU_ID == 618
#define VA_HFI		0x60001000u	/* kgsl: first NONCACHED_KERNEL alloc after the dummy page */
#else
#define VA_HFI		0x60005000u
#endif

#if A6_GPU_ID == 620
#define RSCC		0x03de0000ull	/* A650 family: the RSCC is its own block (redfin kgsl-3d0 reg "rscc") */
#else
#define RSCC		(A6_GPU_BASE + 0x6a000ull + 0x23000)	/* GMU + 0x23000; sunfish 0x0508d000 (was hardcoded flame 0x02c6a000) */
#endif
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
#define GMU_HFI_CTRL	0x1f980	/* HFI_CTRL_STATUS */
#define GMU_HFI_CTRL_INIT	0x1f986
#define GMU_BOOT_SLUMBER_OPTION	0x1cbf8
#define GMU_GX_VOTE_IDX	0x1cbf9
#define GMU_MX_VOTE_IDX	0x1cbfa
#define GMU_GX_SPTPRAC_POWER_CONTROL	0x1a881
#define GMU_SPTPRAC_PWR_CLK_STATUS	0x1f8d0
#define GMU_DCVS_ACK_OPTION	0x1cbfc
#define GMU_DCVS_PERF_SETTING	0x1cbfd
#define GMU_DCVS_BW_SETTING	0x1cbfe
#define GMU_DCVS_RETURN	0x1cbff
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
#if A6_GPU_ID == 618
	branch_on("gcc ddrss_gpu_axi", GCC + 0x4452c);	/* gcc-sdmmagpie (sm8150: 0x71154) */
#else
	branch_on("gcc ddrss_gpu_axi", GCC + 0x71154);	/* gcc-sm8150, gcc-lito */
#endif
	branch_on("gcc gpu_memnoc_gfx", GCC + 0x7100c);
	branch_on("gcc gpu_snoc_dvm_gfx", GCC + 0x71018);
#if A6_GPU_ID == 620
	/* 0904: lito's kgsl-smmu and GMU nodes also vote gpu_cc_hlos1_vote_gpu_smmu_clk (gpucc-lito 0x5000) */
	branch_on("gpucc hlos1_vote_gpu_smmu", GPUCC + 0x5000);
#endif

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
#if A6_GPU_ID == 618
	/* A618 (0140): DT attach-impl-defs of arm,smmu-kgsl@5040000, written by arm-smmu at attach */
	static const uint32_t impl[][2] = { {0x6000, 0x2378}, {0x6060, 0x1055}, {0x678c, 0x08}, {0x6794, 0x28},
	    {0x6800, 0x06}, {0x6900, 0x3ff}, {0x6924, 0x204}, {0x6928, 0x11000}, {0x6930, 0x800},
	    {0x6960, 0xffffffff}, {0x6b64, 0x1a5551}, {0x6b68, 0x9a82a382} };
	for (unsigned i = 0; i < sizeof(impl) / sizeof(impl[0]); i++)
		wr32(SMMU + impl[i][0], impl[i][1]);
#endif
	/* redfin's kgsl-smmu node has no attach-impl-defs (qsmmu-v500, qcom,skip-init): nothing to write */
	wr32(S2CR(smr), (uint32_t)cb);
	wr32(SMR(smr), (1u << 31) | (mask << 16) | sid);
	if (mask && rd32(SMR(smr)) != ((1u << 31) | (mask << 16) | sid)) {
		/* sunfish's hypervisor dropped masked SMRs (0141): take the exact SID instead */
		a6_log("smmu: smr%d with mask %x reads %x; exact sid %u instead\n", smr, mask, rd32(SMR(smr)), sid);
		wr32(SMR(smr), (1u << 31) | sid);
	}
#if A6_GPU_ID == 618
	uint32_t impl6000 = rd32(SMMU + 0x6000);
#else
	uint32_t impl6000 = 0;	/* 0904: no reads of implementation-defined SMMU space under lito's hypervisor */
#endif
	a6_log("smmu: scr0 %x sgfsr %x s2cr%d %x impl6000 %x; smr0-4 %x %x %x %x %x\n", rd32(SMMU), rd32(SMMU + 0x48),
	    smr, rd32(S2CR(smr)), impl6000, rd32(SMR(0)), rd32(SMR(1)), rd32(SMR(2)), rd32(SMR(3)), rd32(SMR(4)));
	a6_log("smmu: smr%d %x -> cb%d cbar %x sctlr %x tcr %x tcr2 %x\n", smr, rd32(SMR(smr)), cb,
	       rd32(CBAR(cb)), rd32(c + CB_SCTLR), rd32(c + CB_TCR), rd32(c + CB_TCR2));
}

static int smmu_pick(int32_t *smr_out, int32_t *cb_out, int cb_min)
{
	int smr = -1, cb = -1;
	uint32_t used_cb = 0;
	/* SMR count from IDR0.NUMSMRG: redfin's kgsl-smmu has 5 ("stream matching with 5 register groups") */
	int nsmr = 6;
#if A6_GPU_ID == 620
	nsmr = (int)(rd32(SMMU + 0x20) & 0xff);
	if (nsmr <= 0 || nsmr > 6)
		nsmr = 5;
#endif
	for (int i = 0; i < nsmr; i++) {
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

#if A6_GPU_ID == 620
/*
 * A650 family (A620): the GMU's address map is kgsl's gmu_vma, not gmu_vma_legacy: ITCM at 0, one cache
 * window from 16 KB to 16 MB, DTCM at 256 MB + 16 KB. a650_gmu.bin starts with zero-length blocks that
 * ask for the cache memory it needs (type 1 PREALLOC_REQ, value = size; redfin's: 0x4000 + 0x8000,
 * 0x144000 + 0x13000, 0x157000 + 0x9000), as kgsl's gmu_prealloc_req serves them. They are mapped here
 * from pool + 0x10000 up to the dummy page (0x80000 bytes), and fw_load copies cache blocks through this
 * table. Type 7 (PREALLOC_PERSIST) at 0x60000000 is the dummy page, mapped anyway.
 */
#define GMU_VA_DTCM	0x10004000u
#define GMU_CACHE_END	0x1000000u
#define GMU_CACHE_POOL	0x10000u
#define GMU_CACHE_POOL_END 0x90000u
#define GMU_PREALLOC_MAX 8
static struct { uint32_t va, size; uint64_t pa; } gmu_prealloc[GMU_PREALLOC_MAX];
static unsigned gmu_nprealloc;

static int gmu_prealloc_map(struct a6_gpu *g)
{
	uint32_t len;
	const uint8_t *fw = a6_firmware("a650_gmu.bin", &len);
	if (!fw)
		return -1;
	uint32_t next = GMU_CACHE_POOL;
	gmu_nprealloc = 0;
	for (uint32_t o = 0; o + 16 <= len; o += 16) {
		uint32_t blk[4];
		memcpy(blk, fw + o, 16);	/* addr, size, type, value */
		if (blk[1])
			break;			/* zero-length metadata blocks come first */
		if (blk[2] != 1 || blk[0] < VA_ICACHE || blk[0] >= GMU_CACHE_END)
			continue;		/* ITCM/DTCM ranges, versions, the persistent dummy page */
		uint32_t va = blk[0] & ~0xfffu, size = (blk[0] + blk[3] - va + 0xfff) & ~0xfffu;
		if (gmu_nprealloc == GMU_PREALLOC_MAX || va + size > 0x200000 || next + size > GMU_CACHE_POOL_END) {
			a6_log("gmu: prealloc %x + %x does not fit (pool next %x)\n", blk[0], blk[3], next);
			return -1;
		}
		gmu_prealloc[gmu_nprealloc].va = va;
		gmu_prealloc[gmu_nprealloc].size = size;
		gmu_prealloc[gmu_nprealloc].pa = a6_pool_pa(g, next);
		map_gmu_pages(GMEM_L3_LOW(g), va, a6_pool_pa(g, next), size, 1);
		a6_log("gmu: cache %x + %x -> pool + %x\n", va, size, next);
		gmu_nprealloc++;
		next += size;
	}
	return gmu_nprealloc ? 0 : -1;
}

/* Pool PA for a GMU cache-window address range, 0 if no prealloc covers it */
static uint64_t gmu_prealloc_pa(uint32_t addr, uint32_t size)
{
	for (unsigned i = 0; i < gmu_nprealloc; i++)
		if (addr >= gmu_prealloc[i].va && addr + size <= gmu_prealloc[i].va + gmu_prealloc[i].size)
			return gmu_prealloc[i].pa + (addr - gmu_prealloc[i].va);
	return 0;
}
#endif

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
#if A6_GPU_ID == 620
	if (gmu_prealloc_map(g))
		return -1;
#else
	map_gmu_pages(GMEM_L3_LOW(g), VA_ICACHE, GMEM_ICACHE(g), CACHE_SIZE, 1);
	map_gmu_pages(GMEM_L3_LOW(g), VA_DCACHE, GMEM_DCACHE(g), CACHE_SIZE, 1);
#endif
	map_gmu_pages(GMEM_L3_6(g), VA_DUMMY, GMEM_DUMMY(g), 0x1000, 0);
	map_gmu_pages(GMEM_L3_6(g), VA_LOG, GMEM_LOG(g), A6_GMU_LOG_SIZE, 0);
	map_gmu_pages(GMEM_L3_6(g), VA_HFI, GMEM_HFI(g), 0x5000, 0);	/* table + 4 queues */
	smmu_bank(g->gmu_smr, g->gmu_cb, GMEM_L1(g), GMU_SID, GMU_SID_MASK);
#if A6_GPU_ID == 618 || A6_GPU_ID == 620
	/* 0142: the A618 GMU also has SID 4 (DT gmu_user cb); route it to the same bank (redfin's DT has it too) */
	int32_t smr4, cb_unused;
	if (!smmu_pick(&smr4, &cb_unused, g->gmu_cb + 1)) {
		wr32(S2CR(smr4), (uint32_t)g->gmu_cb);
		wr32(SMR(smr4), (1u << 31) | 4);
		a6_log("smmu: smr%d %x (gmu sid 4) -> cb%d\n", smr4, rd32(SMR(smr4)), g->gmu_cb);
	}
#endif
	return 0;
}

static uint32_t cmddb_addr(const char *id);

/*
 * a6xx_gmu_rpmh_init: kgsl _load_gmu_rpmh_ucode. The TCS addresses are cmd-db's mx.lvl, cx.lvl and xo.lvl
 * (flame's were 0x30010, 0x30000, 0x30090; flame and sunfish wrote them as constants, kept for those).
 * TCS3 cmd1 data: 2 on the A618 and the A650 family (A620), 3 on the A640.
 * A650 family (0904): the RSCC is its own block (DT "rscc", RSCC above), its sequencer has other words, and
 * the A620 adds a fourth command per TCS for vrm.soc + 4 when cmd-db has it (enable banks stay 7, as kgsl).
 */
static void rpmh_init(void)
{
#if A6_GPU_ID == 620
	static const uint32_t rsc_seq[] = { 0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad };
#else
	static const uint32_t rsc_seq[] = { 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
#endif
	static const uint32_t rscc[][2] = {
		{ 0x0004, 1u << 24 }, { 0x000b, 1 }, { 0x000e, 0 }, { 0x000d, 0 }, { 0x0010, 0 }, { 0x000f, 0 },
		{ 0x0012, 0x80000000 }, { 0x0011, 0 }, { 0x0100, 0 }, { 0x0008, 0x4520 }, { 0x0009, 0x4510 },
		{ 0x000a, 0x4514 },
	};
	static const uint32_t seq[] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	uint32_t mx = 0x30010, cx = 0x30000, xo = 0x30090, vrm = 0;
#if A6_GPU_ID == 620
	uint32_t a;
	if ((a = cmddb_addr("mx.lvl")) != 0)
		mx = a;
	if ((a = cmddb_addr("cx.lvl")) != 0)
		cx = a;
	if ((a = cmddb_addr("xo.lvl")) != 0)
		xo = a;
	vrm = cmddb_addr("vrm.soc");
	a6_log("rpmh: cmd-db mx.lvl %x cx.lvl %x xo.lvl %x vrm.soc %x\n", mx, cx, xo, vrm);
#endif
#if A6_GPU_ID == 640
	const uint32_t tcs3_cx = 3;
#else
	const uint32_t tcs3_cx = 2;
#endif
	const uint32_t pdc[][2] = {
		{ 0x1573, 7 }, { 0x1574, 0 }, { 0x1572, 0 }, { 0x1575, 0x10108 }, { 0x1576, mx },
		{ 0x1577, 1 }, { 0x1579, 0x10108 }, { 0x157a, cx }, { 0x157b, 0 }, { 0x157d, 0x10108 },
		{ 0x157e, xo }, { 0x157f, 0 },
		{ 0x15d7, 7 }, { 0x15d8, 0 }, { 0x15d6, 0 }, { 0x15d9, 0x10108 }, { 0x15da, mx },
		{ 0x15db, 2 }, { 0x15dd, 0x10108 }, { 0x15de, cx }, { 0x15df, tcs3_cx }, { 0x15e1, 0x10108 },
		{ 0x15e2, xo }, { 0x15e3, 3 },
	};
	for (unsigned i = 0; i < sizeof(rscc) / sizeof(rscc[0]); i++)
		wr32(RSCC_REG(rscc[i][0]), rscc[i][1]);
	for (unsigned i = 0; i < sizeof(rsc_seq) / sizeof(rsc_seq[0]); i++)
		wr32(RSCC_REG(0x0180 + i), rsc_seq[i]);
	for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
		wr32(PDC_SEQ + 4 * i, seq[i]);
	for (unsigned i = 0; i < sizeof(pdc) / sizeof(pdc[0]); i++)
		wr32(PDC_REG(pdc[i][0]), pdc[i][1]);
	if (vrm) {
		/* TCS1/TCS3 cmd3 (PDC_CMD_OFFSET 4 words per command) */
		wr32(PDC_REG(0x1575 + 12), 0x10108);
		wr32(PDC_REG(0x1576 + 12), vrm + 4);
		wr32(PDC_REG(0x1577 + 12), 0);
		wr32(PDC_REG(0x15d9 + 12), 0x10108);
		wr32(PDC_REG(0x15da + 12), vrm + 4);
		wr32(PDC_REG(0x15db + 12), 1);
	}
	wr32(PDC_REG(0x1148), 0);		/* PDC_GPU_SEQ_START_ADDR */
	wr32(PDC_REG(0x1140), 0x80000001);	/* PDC_GPU_ENABLE_PDC */
}

static void hfi_table(struct a6_gpu *g)
{
	uint64_t t = GMEM_HFI(g);
#if A6_GPU_ID == 618 || A6_GPU_ID == 620
	/* kgsl hfi_init: HFI_QUEUE_MAX = 4 queues (the 4th, dispatch, disabled),
	 * sizes in dwords (flame wrote bytes for 3 queues; the A640 fw tolerated it). */
	const uint32_t nq = 4;
	wr32(t + 0, 0);
	wr32(t + 4, (6 * 4 + nq * 12 * 4) / 4);
#else
	const uint32_t nq = 3;
	wr32(t + 0, 0);
	wr32(t + 4, 6 * 4 + 3 * 12 * 4);
#endif
	wr32(t + 8, 6);
	wr32(t + 12, 12);
	wr32(t + 16, nq);
	wr32(t + 20, nq);
	for (uint32_t q = 0; q < nq; q++) {
		uint64_t h = t + 24 + 48 * q;
		wr32(h + 0, q < 3 ? 1 : 0);	/* the dispatch queue is disabled */
		wr32(h + 4, VA_HFI + 0x1000 * (q + 1));
#if A6_GPU_ID == 618
		static const uint32_t legacy_idx[4] = { 0, 4, 5, 1 };	/* cmd, msg, dbg, dsp0 (HFI_*_IDX_LEGACY) */
		wr32(h + 8, (10u << 8) | legacy_idx[q]);
#elif A6_GPU_ID == 620
		/* QUEUE_HDR_TYPE(idx, pri): cmd 0/10, msg 1/10, dbg 2/40, dsp0 3/20 (kgsl_hfi.h) */
		static const uint32_t pri[4] = { 10, 10, 40, 20 };
		wr32(h + 8, (pri[q] << 8) | q);
#else
		wr32(h + 8, (10u << 8) | q);
#endif
		wr32(h + 12, 0x1000 >> 2);
		wr32(h + 24, 1);
		wr32(h + 28, 1);
		wr32(h + 32, 1);
	}
}

static int fw_load(struct a6_gpu *g)
{
	uint32_t len;
#if A6_GPU_ID == 620
	const uint8_t *fw = a6_firmware("a650_gmu.bin", &len);	/* kgsl adreno_gpu_core_a620.gmufw_name */
#elif A6_GPU_ID == 618
	const uint8_t *fw = a6_firmware("a618_gmu.bin", &len);
#else
	const uint8_t *fw = a6_firmware("a640_gmu.bin", &len);
#endif
	if (!fw)
		return -1;
#if A6_GPU_ID == 618
	/* a615 family (A618): legacy GMU firmware, one raw image at ITCM start
	 * (kgsl _load_legacy_gmu_fw, max 32 KB). */
	if (len == 0 || len > 0x8000) {
		a6_log("gmu fw: legacy image size %u\n", len);
		return -1;
	}
	a6_write(GMU_REG(GMU_ITCM), fw, (len + 3) & ~3u);
	return 1;
#endif
	const uint8_t *p = fw, *end = fw + len;
	int n = 0;
	while (p + 16 <= end) {
		uint32_t addr, size;
		memcpy(&addr, p, 4);
		memcpy(&size, p + 4, 4);
		const uint8_t *data = p + 16;
		uint64_t dst = 0;
		if (size == 0) {		/* metadata (kgsl load_gmu_fw skips zero-size blocks) */
			p = data;
			continue;
		}
		if (addr < 0x4000)
			dst = GMU_REG(GMU_ITCM) + addr;
#if A6_GPU_ID == 620
		else if (addr >= GMU_VA_DTCM && addr + size <= GMU_VA_DTCM + 0x4000)
			dst = GMU_REG(GMU_DTCM) + (addr - GMU_VA_DTCM);
		else if (addr >= VA_ICACHE && addr + size <= GMU_CACHE_END)
			dst = gmu_prealloc_pa(addr, size);
#endif
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

#if A6_GPU_ID == 620
	/* kgsl a6xx_gmu_fw_start: veto_fal10 (adreno_gpu_core_a620) -> GPU_GMU_CX_GMU_CX_FAL_INTF = 1 */
	wr32(GMU_REG(0x1f8f0), 1);
#endif
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
	/* kgsl: (chipid & 0xffff0000) | minor << 12 | patch << 8 (A640 0x06040001 -> 0x06040100, A618 -> 0x06018000) */
	wr32(GMU_REG(GMU_SFR_ADDR), (uint32_t)((A6_CHIP_ID & 0xffff0000u) | (((A6_CHIP_ID >> 8) & 0xffu) << 12) | ((A6_CHIP_ID & 0xffu) << 8)));
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
	/* kgsl a6xx_gmu_start: CX_GMU_WFI_CONFIG 0 and GBIF out of halt before releasing the CM3 */
	wr32(GMU_REG(0x1f802), 0);
	wr32(GREG(0x3c45), 0);			/* GBIF_HALT (CX) */
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

/* A618 (sunfish DT pwrlevels), each at or below the A640 frequency of the same level */
#if A6_GPU_ID == 618
/* 0148: the A618's stock levels up to 825 MHz (gpucc-sdmmagpie rate_max: 650 NOM_L1, 800 TURBO, 825 TURBO_L1) */
#define LVL_NOM_L1	320
#define LVL_TURBO	384
#define LVL_TURBO_L1	416
static const uint32_t gpu_khz[] = { 0, 180000, 267000, 355000, 430000, 565000, 650000, 800000, 825000 };
static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_SVS_L2, LVL_NOM, LVL_NOM_L1, LVL_TURBO,
    LVL_TURBO_L1 };
#elif A6_GPU_ID == 620
/*
 * 0904: redfin live DT gpu-pwrlevels-0/-1 (speed bins 0 and 132; SM7250 "765G"): 275 LOW_SVS, 400 SVS,
 * 500 SVS_L1, 625 NOM (lito-gpu.dtsi opp table). Bin 115 tops out at 540, same NOM corner.
 */
static const uint32_t gpu_khz[] = { 0, 275000, 400000, 500000, 625000 };
static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_NOM };
#else
static const uint32_t gpu_khz[] = { 0, 180000, 267000, 355000, 430000, 565000 };
static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_SVS_L2, LVL_NOM };
#endif
#define NLVL	(int)(sizeof(gpu_khz) / sizeof(gpu_khz[0]))
static const uint32_t gmu_khz[] = { 0, 200000 };
static const uint32_t gmu_lvl[] = { 0, LVL_MIN_SVS };

#define CMDDB_MAGIC	0x0c0330db
#define CMDDB_LITO	0x80860000ull	/* lito.dtsi qcom,cmd-db: a fixed reserved-memory region (0901) */

static uint64_t cmddb_base(void)
{
#if A6_GPU_ID == 620
	if (rd32(CMDDB_LITO + 4) == CMDDB_MAGIC)
		return CMDDB_LITO;
#endif
	uint64_t db = rd32(0x0c3f000cull);
	if (db < 0x80000000ull || (db & 3))
		return 0;	/* no AOP locator (lito): do not read through a random address */
	return rd32(db + 4) == CMDDB_MAGIC ? db : 0;
}

/* cmd-db entry by name: its RPMh address and aux data (Linux cmd_db_read_addr / cmd_db_read_aux_data) */
static int cmddb_find(const char *id, uint32_t *addr, uint64_t *aux, uint32_t *len)
{
	uint64_t db = cmddb_base();
	if (!db)
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
			if (addr)
				*addr = rd32(ent + 16);
			if (len)
				*len = w5 & 0xffff;
			if (aux)
				*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t __attribute__((unused)) cmddb_addr(const char *id)
{
	uint32_t a = 0;
	return cmddb_find(id, &a, NULL, NULL) ? 0 : a;
}

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	return cmddb_find(id, NULL, aux, len);
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
#if A6_GPU_ID != 618
	/* HFI v2 only (kgsl hfi_queue_write): the A618's legacy v1 firmware reads messages back to back and
	 * took the 0xfafafafa padding for a 250-dword message header (0144) */
	for (; w % 4; w = (w + 1) % QSIZE)
		wr32(QDATA(g, HFI_Q_CMD) + 4 * w, 0xfafafafa);
#endif
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
#if A6_GPU_ID != 618
				r = ((r + 3) & ~3u) % QSIZE;
#endif
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

/* redfin (0964): runtime GPU level vote for the thermal cap (sweet_gpu_level_cap); HFI v2 GMUs only. */
int a6_perf_vote(struct a6_gpu *g, uint32_t lvl);
int a6_perf_vote(struct a6_gpu *g, uint32_t lvl)
{
#if A6_GPU_ID == 618
	(void)g;
	(void)lvl;
	return -1;
#else
	static uint32_t msg[8];
	if (lvl < 1 || (int)lvl >= NLVL)
		return -1;
	msg[1] = 1;
	msg[2] = lvl;
	msg[3] = 0;
	return hfi_send(g, 30, msg, 4);
#endif
}

static int gmu_hfi(struct a6_gpu *g)
{
	static uint32_t msg[256];
	uint32_t gx[NLVL], cx[2];
	if (arc_votes(gx, gpu_lvl, NLVL, "gfx.lvl", "mx.lvl") || arc_votes(cx, gmu_lvl, 2, "cx.lvl", "mx.lvl")) {
		a6_log("hfi: cmd-db levels not found\n");
		return -1;
	}
#if A6_GPU_ID == 618
	/*
	 * Legacy GMU (a615 family, HFI v1, qcom,gpu-quirk-hfi-use-reg), as kgsl:
	 * HFI_CTRL_INIT, H2F_MSG_INIT, v1 perf table, BW table, H2F_MSG_TEST; the
	 * GPU level then goes through the DCVS registers + OOB 7, not HFI.
	 */
	uint32_t lvl0 = g->perf_level >= 1 && (int)g->perf_level < NLVL ? g->perf_level : 1;
	/* kgsl a6xx_gmu_gfx_rail_on (hfi-use-reg): GX/MX vote, OOB 6 boot_slumber (legacy bits 22/30) */
	wr32(GMU_REG(GMU_BOOT_SLUMBER_OPTION), 0);	/* OOB_BOOT_OPTION */
	wr32(GMU_REG(GMU_GX_VOTE_IDX), gx[lvl0] & 0xff);
	wr32(GMU_REG(GMU_MX_VOTE_IDX), (gx[lvl0] >> 8) & 0xff);
	wr32(GMU_REG(GMU_H2G_SET), 1u << (6 + 16));
	int rack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << (6 + 24), 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << (6 + 24));
	/* kgsl a6xx_gmu_sptprac_enable (a630/a615 family) */
	wr32(GMU_REG(GMU_GX_SPTPRAC_POWER_CONTROL), 0x00778000);
	int sp = wait_bit(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS), 1u << 3, 1, 100);
	a6_log("legacy: gfx rail (gx idx %u mx idx %u) %s, sptprac %s (status %x), g2h %x\n",
	    gx[lvl0] & 0xff, (gx[lvl0] >> 8) & 0xff, rack ? "TIMEOUT" : "acked", sp ? "TIMEOUT" : "on",
	    rd32(GMU_REG(GMU_SPTPRAC_PWR_CLK_STATUS)), rd32(GMU_REG(GMU_G2H_INFO)));
	wr32(GMU_REG(GMU_HFI_CTRL_INIT), 1);
	int hs = wait_bit(GMU_REG(GMU_HFI_CTRL), 1, 1, 100);
	a6_log("hfi: ctrl init -> status %x %s\n", rd32(GMU_REG(GMU_HFI_CTRL)), hs ? "TIMEOUT" : "ok");
	/* diagnostic: GMU SMMU context bank fault state + CM3 registers after the fault */
	{
		uint64_t cb = SMMU_CB(g->gmu_cb);
		a6_log("diag: gmu cb%u fsr %x far %x%08x fsynr0 %x; g2h %x cm3 sysreset %x init %x\n", g->gmu_cb,
		    rd32(cb + CB_FSR), rd32(cb + CB_FAR + 4), rd32(cb + CB_FAR), rd32(cb + CB_FSYNR0),
		    rd32(GMU_REG(GMU_G2H_INFO)), rd32(GMU_REG(GMU_SYSRESET)), rd32(GMU_REG(GMU_INIT_RESULT)));
	}
	memset(msg, 0, sizeof(msg));
	msg[1] = 0;			/* seg_id */
	msg[2] = 0;			/* dbg_buffer_addr: none */
	msg[3] = 0;			/* dbg_buffer_size */
	msg[4] = 1;			/* boot_state: GMU_COLD_BOOT */
	int rc = hfi_send(g, 0, msg, 5);	/* H2F_MSG_INIT */
	memset(msg, 0, sizeof(msg));
	msg[1] = NLVL;
	msg[2] = 2;
	for (int i = 0; i < NLVL; i++) {	/* v1: struct opp_desc { vote, freq } x 16 */
		msg[3 + 2 * i] = gx[i];
		msg[4 + 2 * i] = gpu_khz[i];
	}
	for (int i = 0; i < 2; i++) {
		msg[3 + 32 + 2 * i] = cx[i];
		msg[4 + 32 + 2 * i] = gmu_khz[i];
	}
	rc |= hfi_send(g, 4, msg, 3 + 32 + 8);
	memset(msg, 0, sizeof(msg));
	/* hfi_bwtable_cmd: levels 1, cnoc 1 (CN0 0x500a8 on sunfish), ddr 3 (MC0/SH0/ACV,
	 * same addresses as flame), wait masks; cnoc data [2][6] at 12, ddr addrs 24, data 32 */
	static const uint32_t bw[][2] = {
		{ 1, 1 }, { 2, 1 }, { 3, 3 }, { 4, 1 }, { 5, 1 }, { 6, 0x500a8 },
		{ 12, 0x40000000 }, { 18, 0x60000001 },
		{ 24, 0x50000 }, { 25, 0x5003c }, { 26, 0x5000c }, { 32, 0x40000000 }, { 33, 0x40000000 },
		{ 34, 0x40000000 },
	};
	for (unsigned i = 0; i < sizeof(bw) / sizeof(bw[0]); i++)
		msg[bw[i][0]] = bw[i][1];
	rc |= hfi_send(g, 3, msg, 32 + 128);
	memset(msg, 0, sizeof(msg));
	rc |= hfi_send(g, 5, msg, 1);	/* H2F_MSG_TEST */
	uint32_t lvl = g->perf_level >= 1 && (int)g->perf_level < NLVL ? g->perf_level : 1;
	wr32(GMU_REG(GMU_DCVS_ACK_OPTION), 0);			/* DCVS_ACK_NONBLOCK */
	wr32(GMU_REG(GMU_DCVS_PERF_SETTING), (lvl & 0xff) | (3u << 28));	/* CLKSET_OPTION_ATLEAST */
	wr32(GMU_REG(GMU_DCVS_BW_SETTING), 0);
	wr32(GMU_REG(GMU_H2G_SET), 1u << (7 + 16));		/* legacy OOB 7 (dcvs) */
	int dack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << (7 + 24), 1, 1000);
	uint32_t dret = rd32(GMU_REG(GMU_DCVS_RETURN));
	wr32(GMU_REG(GMU_G2H_CLR), 1u << (7 + 24));
	wr32(GMU_REG(GMU_H2G_SET), 0);
	a6_log("hfi(v1): init, perf/bw tables, test -> %d; dcvs %u MHz %s ret %x\n", rc, gpu_khz[lvl] / 1000,
	    dack ? "TIMEOUT" : "acked", dret);
	a6_log("diag: smmu sgfsr %x sgfsynr0 %x sgfar %x%08x; gmu cb fsr %x far %x%08x\n", rd32(SMMU + 0x48),
	    rd32(SMMU + 0x50), rd32(SMMU + 0x44), rd32(SMMU + 0x40), rd32(SMMU_CB(g->gmu_cb) + CB_FSR),
	    rd32(SMMU_CB(g->gmu_cb) + CB_FAR + 4), rd32(SMMU_CB(g->gmu_cb) + CB_FAR));
#else
	memset(msg, 0, sizeof(msg));
	msg[1] = NLVL;
	msg[2] = 2;
	for (int i = 0; i < NLVL; i++) {	/* v2: struct opp_gx_desc { vote, acd, freq } x 16 */
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
#if A6_GPU_ID == 620
	/*
	 * 0904: lito's BCM addresses from cmd-db instead of flame's: one cnoc command (CN0, as sunfish) and
	 * the three DDR ones (MC0, SH0, ACV), level 0 only (the kernel votes MC0/SH0 itself, gpu_bus_vote).
	 * hfi_bwtable_cmd: cnoc addrs at 6, cnoc data [2][6] at 12, ddr addrs at 24, ddr data [16][8] at 32.
	 */
	{
		static const char *const ddr[] = { "MC0", "SH0", "ACV" };
		uint32_t cn0 = cmddb_addr("CN0"), da[3], nd = 0;
		for (unsigned i = 0; i < 3; i++)
			if ((da[nd] = cmddb_addr(ddr[i])) != 0)
				nd++;
		a6_log("hfi: bw cmd-db CN0 %x, ddr %u found (%x %x %x)\n", cn0, nd, nd > 0 ? da[0] : 0,
		    nd > 1 ? da[1] : 0, nd > 2 ? da[2] : 0);
		if (cn0 && nd) {
			memset(msg, 0, sizeof(msg));
			msg[1] = 1;
			msg[2] = 1;
			msg[3] = nd;
			msg[4] = 1;
			msg[5] = 1;
			msg[6] = cn0;
			msg[12] = 0x40000000;
			msg[18] = 0x60000001;
			for (uint32_t i = 0; i < nd; i++) {
				msg[24 + i] = da[i];
				msg[32 + i] = 0x40000000;
			}
		}
	}
#endif
	rc |= hfi_send(g, 3, msg, 32 + 128);
	msg[1] = 0;
	rc |= hfi_send(g, 14, msg, 2);
	rc |= hfi_send(g, 10, msg, 1);
	uint32_t lvl = g->perf_level >= 1 && (int)g->perf_level < NLVL ? g->perf_level : 1;
	msg[1] = 1;
	msg[2] = lvl;			/* index into gpu_khz */
	msg[3] = 0;
	rc |= hfi_send(g, 30, msg, 4);
	a6_log("hfi: perf/bw tables, core fw start, start, perf vote %u MHz -> %d\n", gpu_khz[lvl] / 1000, rc);
#endif

#if A6_GPU_ID == 618
	/* legacy GMU (a630/a615 family): set BIT(req+16), ack BIT(req+24), req 0 = GPU */
	wr32(GMU_REG(GMU_H2G_SET), 1u << 16);
	int ack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << 24, 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << 24);
#else
	wr32(GMU_REG(GMU_H2G_SET), 1u << 30);	/* OOB GPU_SET */
	int ack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << 31, 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << 31);
#endif
	a6_log("oob gpu_set %s, gx gdsc %x\n", ack ? "TIMEOUT" : "acked", rd32(GPUCC + GPUCC_GX_GDSCR));
	if (!a6_gx_up()) {
		a6_log("GX not powered; not touching GPU registers\n");
		return -1;
	}
	return 0;
}

/* ---- zap shader through TrustZone PAS ---- */

#define PAS_GPU		13
#if A6_GPU_ID == 620
/* redfin live DT pil_gpu_region@8bc15400 (0x2000); a620_zap's one loadable segment (b02, 0x68c) goes at its start */
#define ZAP_REGION	0x8bc15400ull
#define ZAP_MDT		"a620_zap.mdt"
#define ZAP_B02		"a620_zap.b02"
#elif A6_GPU_ID == 618
#define ZAP_REGION	0x98f95000ull	/* sunfish reserved-memory gpu_region (flame 0x9a915000) */
#define ZAP_MDT		"a615_zap.mdt"
#define ZAP_B02		"a615_zap.b02"
#else
#define ZAP_REGION	0x9a915000ull
#define ZAP_MDT		"a640_zap.mdt"
#define ZAP_B02		"a640_zap.b02"
#endif
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

/*
 * 0908 (redfin): r8's GPU log stopped at "zap: mem setup rc 4291821498" (0xffcfffba) for pil_gpu_region
 * 0x8bc15400. a620_zap.b02's program header asks for 1 MB alignment (p_align 0x100000; sunfish's a615
 * zap did not) and redfin's kgsl-hyp node has no memory-region, so Linux's PIL put it in memory it
 * allocated with that alignment. Tried in order: boot-arg sweetzappa=PA (if given), 0xc8100000 (1 MB
 * aligned HLOS RAM outside XNU and the GPU pool), then the DT region; between tries the PAS state is
 * reset (PAS_SHUTDOWN) and the image initialised again.
 */
static int zap_try(struct a6_gpu *g, uint64_t pa, const uint8_t *b02, uint32_t slen)
{
	uint64_t r1 = 0;
	int64_t rc = scm(1, 2 | (2u << 6), PAS_GPU, a6_pool_pa(g, A6_OFF_ZAP_META), 0, &r1);
	if (rc || r1) {
		a6_log("zap: init image rc %llx r1 %llx\n", (unsigned long long)rc, (unsigned long long)r1);
		return -1;
	}
	rc = scm(2, 3, PAS_GPU, pa, ZAP_SIZE, &r1);
	if (rc || r1) {
		a6_log("zap: mem setup at %llx rc %llx r1 %llx\n", (unsigned long long)pa, (unsigned long long)rc,
		    (unsigned long long)r1);
		return -2;
	}
	static uint8_t seg[ZAP_SIZE];
	memset(seg, 0, sizeof(seg));
	memcpy(seg, b02, slen);
	a6_write(pa, seg, ZAP_SIZE);
	rc = scm(5, 1, PAS_GPU, 0, 0, &r1);
	a6_log("zap: at %llx: auth and reset rc %llx r1 %llx\n", (unsigned long long)pa, (unsigned long long)rc,
	    (unsigned long long)r1);
	return rc || r1 ? -3 : 0;
}

static int zap_load(struct a6_gpu *g)
{
	uint32_t mlen, slen;
	const uint8_t *mdt = a6_firmware(ZAP_MDT, &mlen), *b02 = a6_firmware(ZAP_B02, &slen);
	if (!mdt || !b02 || slen > ZAP_SIZE)
		return -1;
	a6_write(a6_pool_pa(g, A6_OFF_ZAP_META), mdt, mlen);
#if A6_GPU_ID == 620
	uint64_t try_pa[3] = { 0, 0xc8100000ull, ZAP_REGION };
#ifdef KERNEL
	uint64_t over = 0;
	if (PE_parse_boot_argn("sweetzappa", &over, sizeof(over)) && over && !(over & 0xfff))
		try_pa[0] = over;
#endif
	for (unsigned i = 0; i < 3; i++) {
		if (!try_pa[i])
			continue;
		int rc = zap_try(g, try_pa[i], b02, slen);
		if (rc == 0)
			return 0;
		uint64_t r1 = 0;
		int64_t src = scm(6, 1, PAS_GPU, 0, 0, &r1);	/* PAS_SHUTDOWN before the next try */
		a6_log("zap: try %u failed (%d); pas shutdown rc %llx\n", i, rc, (unsigned long long)src);
	}
	return -1;
#else
	return zap_try(g, ZAP_REGION, b02, slen);
#endif
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
	return a6_submit_ibs_async_wait(g, iova, dwords, nib, timeout_ms, 0, 0);
}

/*
 * 0989: the same, with the CP first waiting until the word at GPU address wait_va is >= wait_ref (CP_WAIT_REG_MEM,
 * memory poll, WRITE_GE; wait_va 0: no wait). Whoever arms it must make sure the word gets there (sweet_gpu_scan_word).
 */
uint32_t a6_submit_ibs_async_wait(struct a6_gpu *g, const uint64_t *iova, const uint32_t *dwords, unsigned nib,
				  unsigned timeout_ms, uint64_t wait_va, uint32_t wait_ref)
{
	uint64_t fva = a6_pool_va(A6_OFF_SCRATCH);
	/* GPU time of this submit: CP_ALWAYS_ON_COUNTER before the IBs and once they are idle (a6_submit_time) */
	uint64_t ts = a6_pool_va(A6_OFF_SCRATCH + A6_TS_OFF) + 16ull * ((g->fence + 1) % A6_TS_SLOTS);
	uint32_t dw[7 + 4 + 4 + 4 * A6_SUBMIT_MAX + 5 + 5 + 2], n = 0;	/* 0988: + the longer end stamp; 0989: + the wait */
	if (nib == 0 || nib > A6_SUBMIT_MAX)
		return 0;
	if (wait_va != 0) {
		dw[n++] = a6_pkt7(0x3c, 6);		/* CP_WAIT_REG_MEM */
		dw[n++] = 5u | 1u << 4;			/* FUNCTION WRITE_GE, POLL memory */
		dw[n++] = (uint32_t)wait_va;
		dw[n++] = (uint32_t)(wait_va >> 32);
		dw[n++] = wait_ref;
		dw[n++] = 0xffffffffu;			/* MASK */
		dw[n++] = 16;				/* DELAY_LOOP_CYCLES */
	}
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
	/* 0157: no wait-for-idle between client submits (sweetwfi=1 brings it back): CACHE_FLUSH_TS below
	 * lands only once the work before it is done, and the CP may start the next submit meanwhile */
	if (a6_submit_wfi)
		dw[n++] = a6_pkt7(A6_CP_WAIT_FOR_IDLE, 0);
	if (a6_submit_rbts) {
		dw[n++] = a6_pkt7(A6_CP_EVENT_WRITE, 4);	/* 0988: RB_DONE_TS with the timestamp -> ts + 8 */
		dw[n++] = 0x16 | 1u << 30;
		dw[n++] = (uint32_t)(ts + 8);
		dw[n++] = (uint32_t)((ts + 8) >> 32);
		dw[n++] = 0;
	} else {
		dw[n++] = a6_pkt7(0x3e, 3);		/* ... -> ts + 8 */
		dw[n++] = 0x980 | 2u << 18;
		dw[n++] = (uint32_t)(ts + 8);
		dw[n++] = (uint32_t)((ts + 8) >> 32);
	}
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
	if (a6_submit_rbts) {
		/* 0988: the GPU was on the submit before until its end */
		uint64_t p[2] = { 0, 0 };
		a6_read(a6_pool_pa(g, A6_OFF_SCRATCH + A6_TS_OFF) + 16ull * ((fence - 1) % A6_TS_SLOTS), p, 16);
		if (p[1] > t[0] && p[1] < t[1]) {
			t[0] = p[1];
		}
	}
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
	if (rc == 0)
		return 0;
	/*
	 * 0152: kgsl a6xx_halt_gbif before a reset: stop new GBIF transactions (GBIF_HALT, ack in GBIF_HALT_ACK),
	 * then the RBBM side, so the reset does not land mid-transaction; then reset and CP init again (it
	 * clears both halts).
	 */
	wr32(GREG(0x3c45), 0x3);		/* GBIF_HALT: client + arbiter */
	(void)wait_bit(GREG(0x3c46), 0x3, 1, 100);
	wr32(GREG(0x16), 1);			/* RBBM_GBIF_HALT */
	(void)wait_bit(GREG(0x17), 1, 1, 100);	/* RBBM_GBIF_HALT_ACK */
	wr32(GREG(0x43), 1);
	(void)rd32(GREG(0x43));
	a6_delay_us(100);
	wr32(GREG(0x43), 0);
	a6_delay_ms(10);
	(void)a6_clear_fault(g);
	rc = gpu_cp_init(g, true);
	a6_log("recover: GBIF halt + soft reset + CP init -> %d\n", rc);
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
#if A6_GPU_ID == 618
		{ 0x0010, 3 },				/* RBBM_VBIF_CLIENT_QOS_CNTL (kgsl a615_gbif) */
#else
		{ 0x0011, 3 },				/* RBBM_GBIF_CLIENT_QOS_CNTL */
#endif
		{ 0x050b, 0xffffffff },			/* RBBM_PERFCTR_GPU_BUSY_MASKED */
		/* UCHE: write range max, trap base, write-through base (trap 0x1fffffffff000) */
		{ 0x0e05, 0xffffffc0 }, { 0x0e06, 0x1ffff }, { 0x0e09, 0xfffff000 }, { 0x0e0a, 0x1ffff },
		{ 0x0e07, 0xfffff000 }, { 0x0e08, 0x1ffff },
#if A6_GPU_ID == 618 || A6_GPU_ID == 620
		{ 0x0e0b, 0x100000 }, { 0x0e0c, 0 }, { 0x0e0d, 0x17ffff }, { 0x0e0e, 0 },	/* GMEM range: 512 KB */
#else
		{ 0x0e0b, 0x100000 }, { 0x0e0c, 0 }, { 0x0e0d, 0x1fffff }, { 0x0e0e, 0 },	/* GMEM range */
#endif
		{ 0x0e18, 0x804 }, { 0x0e17, 0x4 },	/* UCHE_FILTER_CNTL, CACHE_WAYS */
#if A6_GPU_ID == 618
		/* 0146: ROQ thresholds of the A618 (kgsl: the A640's are twice as big) */
		{ 0x08c2, 0x010000c0 }, { 0x08c1, 0x8040362c }, { 0x08c3, 128 },	/* ROQ, mem pool */
#else
		{ 0x08c2, 0x02000140 }, { 0x08c1, 0x8040362c }, { 0x08c3, 128 },	/* ROQ, mem pool */
#endif
		{ 0x098d, 1 }, { 0x0500, 1 },		/* CP_AHB_CNTL, RBBM_PERFCTR_CNTL */
#if A6_GPU_ID == 620
		{ 0x001f, (1u << 30) | 0x3ffff },	/* RBBM_INTERFACE_HANG_INT_CNTL: a620 hang_detect_cycles */
#else
		{ 0x001f, (1u << 30) | 0x1fffff },	/* RBBM_INTERFACE_HANG_INT_CNTL */
#endif
#if A6_GPU_ID == 618 || A6_GPU_ID == 620
		{ 0x0e19, 1 },				/* UCHE_CLIENT_PF (kgsl) */
#else
		{ 0x0e19, (1u << 7) | 1 },		/* UCHE_CLIENT_PF */
#endif
#if A6_GPU_ID == 620
		/* PC_DBG_ECO_CNTL: a620 prim_fifo_threshold 0x10000 | disable_tseskip (bit 9) */
		{ 0x9e00, 0x00010000 | (1u << 9) },
		/* a650 family: bicubic filter weights (TPL1_BICUBIC_WEIGHTS_TABLE_0..4) */
		{ 0xb608, 0 }, { 0xb609, 0x3fe05ff4 }, { 0xb60a, 0x3fa0ebee }, { 0xb60b, 0x3f5193ed },
		{ 0xb60c, 0x3f0243f0 },
#else
		{ 0x9e00, 0x00180000 },			/* PC_DBG_ECO_CNTL: prim_fifo_threshold */
#endif
		{ 0x08d0, 0 },				/* CP_PERFCTR_CP_SEL[0]: always count */
#if A6_GPU_ID == 618
		/* a6xx_set_ubwc_config for A618 (0145): highest bank bit 14 (DT, hbb_lo 1), no AMSBC */
		{ 0x8e08, 1u << 1 }, { 0xb604, 1u << 1 }, { 0xae02, 1u << 1 },
		{ 0x0e01, 1u << 21 }, { 0x0534, 0 },
#elif A6_GPU_ID == 620
		/*
		 * kgsl a6xx_start with redfin's DT: ubwc-mode 3 (mode 0, AMSBC), highest-bank-bit 14 (lower_bit 1),
		 * min-access-length 32, a650 family uavflagprd_inv 2 in SP_NC_MODE_CNTL.
		 */
		{ 0x8e08, (1u << 4) | (1u << 1) }, { 0xb604, 1u << 1 }, { 0xae02, (2u << 4) | (1u << 1) },
		{ 0x0e01, 1u << 21 }, { 0x0534, 0 },
#else
		/* a6xx_set_ubwc_config: highest bank bit 15 (hbb_lo 2), AMSBC, UBWC 3 */
		{ 0x8e08, (1u << 4) | (2u << 1) }, { 0xb604, 2u << 1 }, { 0xae02, 2u << 1 },
		{ 0x0e01, 2u << 21 }, { 0x0534, 0 },
#endif
	};
	for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++)
		wr32(GREG(init[i][0]), init[i][1]);

	uint32_t len;
#if A6_GPU_ID == 620
	const uint8_t *sqe = a6_firmware("a650_sqe.fw", &len);	/* kgsl adreno_gpu_core_a620.sqefw_name */
#else
	const uint8_t *sqe = a6_firmware("a630_sqe.fw", &len);
#endif
	if (!sqe || len < 8 || len > 0x10000)
		return -1;
	a6_write(a6_pool_pa(g, A6_OFF_SQE), sqe + 4, (len - 4 + 3) & ~3u);	/* minus the first dword */
	wr32(GREG(CP_SQE_INSTR_BASE), (uint32_t)a6_pool_va(A6_OFF_SQE));
	wr32(GREG(CP_SQE_INSTR_BASE + 1), 0);
	wr32(GREG(CP_RB_BASE), (uint32_t)a6_pool_va(A6_OFF_RING));
	wr32(GREG(CP_RB_BASE + 1), 0);
	wr32(GREG(CP_RB_CNTL), 0x0800020c);	/* 32K ring, 32-byte blocks, no rptr update */
#if A6_GPU_ID == 620
	/* ADRENO_APRIV (a620): CP_APRIV_CNTL = kgsl A6XX_APRIV_DEFAULT (CD write/read, RB rptr wb, priv level, fetch) */
	wr32(GREG(0x0844), (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1));
#endif
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
#if A6_GPU_ID == 620
	/* 0904: Mesa freedreno_devices.py FD620 raw_magic_regs (PC/VFD power 0, TPL1 0x01008000, SP chicken 0x400,
	 * SP_DBG_ECO 0x01000000, RB_RBP_CNTL 0) instead of the A640's */
	{ 0x9804, 0x1f }, { 0x9805, 0 }, { 0xa0f8, 0 }, { 0xb600, 0x01008000 }, { 0x8600, 0 },
	{ 0xae03, 0x400 }, { 0xae00, 0x01000000 }, { 0xbe04, 0 }, { 0x9600, 0x2000000 }, { 0x0e12, 1 },
	{ 0x8e01, 0 }, { 0x8e04, 0x4100000 }, { 0xae04, 0 }, { 0xae0f, 0x3f }, { 0xb605, 0x44 },
#else
	{ 0x9804, 0x1f }, { 0x9805, 1 }, { 0xa0f8, 1 }, { 0xb600, 0x8000 }, { 0x8600, 0 },
	{ 0xae03, 0x420 }, { 0xae00, 0 }, { 0xbe04, 0 }, { 0x9600, 0x2000000 }, { 0x0e12, 1 },
	{ 0x8e01, 1 }, { 0x8e04, 0x4100000 }, { 0xae04, 0 }, { 0xae0f, 0x3f }, { 0xb605, 0x44 },
#endif
	{ 0xbe00, 0x80 }, { 0xbe01, 0 }, { 0xab20, 0 }, { 0xb182, 0 }, { 0xbb11, 0 }, { 0xa9a8, 0 },
	{ 0xab00, 5 }, { 0xa009, 3 }, { 0x8811, 0x10 }, { 0x8110, 0 }, { 0x8818, 0 }, { 0x8819, 0 },
	{ 0x881a, 0 }, { 0x881b, 0 }, { 0x881c, 0 }, { 0x881d, 0 }, { 0x881e, 0 }, { 0x88f0, 0 },
	{ 0x9236, 0 }, { 0x9300, 0 }, { 0x9306, 1 }, { 0xb183, 0 }, { 0x80af, 0 }, { 0x8099, 0 },
	{ 0x980a, 0 }, { 0x9210, 0 }, { 0x9211, 0 }, { 0x9602, 0 }, { 0x9e72, 0 }, { 0xb309, 0xa2 },
	{ 0xb986, 0xfc }, { 0xa007, 0 }, { 0x8864, 0 }, { 0xb800, 0x102 },
#if A6_GPU_ID == 620
	{ 0x8e07, 0x08000000 },		/* RB_CCU_CNTL: sysmem, colour after 1 CCU x 64 KB of depth (A640: 2 CCUs) */
#else
	{ 0x8e07, 0x10000000 },		/* RB_CCU_CNTL: sysmem, full CCU */
#endif
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
