/*
 * Adreno 640 core shared by the kernel (bsd/dev/sweet_gpu.c) and the
 * userland bring-up tool (phone/gpu/gpu.c, built from this same file): power
 * and GMU boot, HFI, SMMU banks, SQE + CP + zap shader, ringbuffer
 * submission, 4K page mappings, and the 2D blit stream.
 *
 * The environment supplies the a6_* hooks below (physical register and
 * memory access, delays, logging, SMC, firmware).
 *
 * One 16 MB contiguous pool (2 MB aligned) holds the GPU's own memory:
 *
 *   pool + 0x000000  GMU page tables, icache/dcache windows, log, HFI
 *   pool + 0x0a0000  zap shader metadata for TrustZone
 *   pool + 0x100000  GPU page tables: L1, L2 (2 MB blocks), then an arena of 4K tables
 *   pool + 0x200000  SQE microcode
 *   pool + 0x210000  ringbuffer (32 KB)
 *   pool + 0x218000  scratch: fence (+0), tests (+0x100..), border colours (+0x800), event timestamps (+0x900)
 *   pool + 0x400000  free for the environment (userland: named buffers)
 *
 * GPU VA (39 bits): the pool at 0x10000000, the panel framebuffer at
 * 0x20000000, everything else (surfaces, replayed buffers) mapped with 4K
 * pages from 4 GB up.
 */
#ifndef SWEET_A6XX_H
#define SWEET_A6XX_H
#ifdef KERNEL
#include <sys/types.h>
#include <stdarg.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#endif

#define A6_POOL_SIZE		0x1000000ull
#define A6_POOL_ALIGN		0x200000ull
#define A6_OFF_ZAP_META		0x0a0000ull
#define A6_OFF_GT_L1		0x100000ull
#define A6_OFF_GT_L2		0x101000ull
#define A6_OFF_PT_ARENA		0x102000ull
#define A6_OFF_SQE		0x200000ull
#define A6_OFF_RING		0x210000ull
#define A6_OFF_SCRATCH		0x218000ull
#define A6_OFF_FREE		0x400000ull

#define A6_GVA_POOL		0x10000000ull
#define A6_GVA_FB		0x20000000ull
#define A6_RING_SIZE		0x8000u
#define A6_RING_DW		(A6_RING_SIZE / 4)

/*
 * Which Adreno this kernel drives: 640 (flame, SM8150), 618 (sunfish, SM7150, legacy GMU) or
 * 620 (redfin, SM7250 lito: A650-family GMU and register spec, HFI v2, 512 KB GMEM).
 */
#ifndef A6_GPU_ID
#define A6_GPU_ID		620	/* redfin (sunfish: 618, flame: 640) */
#endif

#if A6_GPU_ID == 620
#define A6_FB_PA		0xa0000000ull	/* redfin cont_splash_region (0x2300000 long) */
/*
 * 0904: two panel buffers for the GPU present (sweetfbflip=1): the front one ABL scans out, the back
 * one A6_FB_BUF_OFF above it, both inside the splash region. Mapped for the GPU: 10 x 2 MB.
 */
#define A6_FB_SIZE		0x1400000ull
#define A6_FB_BUF_OFF		0xa00000ull
#define A6_CHIP_ID		0x06020000ull	/* redfin live DT qcom,chipid */
#elif A6_GPU_ID == 618
#define A6_FB_PA		0x9c000000ull
#define A6_FB_SIZE		0xa00000ull	/* mapped for the GPU: 5 x 2 MB */
#define A6_FB_BUF_OFF		0ull		/* one panel buffer */
#define A6_CHIP_ID		0x06010800ull	/* sunfish DT qcom,chipid */
#else
#define A6_FB_PA		0x9c400000ull
#define A6_FB_SIZE		0xa00000ull
#define A6_FB_BUF_OFF		0ull
#define A6_CHIP_ID		0x06040001ull
#endif

struct a6_gpu {
	uint64_t pool_pa;
	int32_t gmu_smr, gmu_cb, gpu_smr, gpu_cb;
	uint32_t wptr;			/* ring write pointer, dwords */
	uint32_t fence;			/* last fence value submitted */
	uint32_t hfi_seq;
	uint32_t pt_next;		/* next free table in the arena */
	uint32_t perf_level;		/* GMU perf vote: 1 (257 MHz) .. 5 (585 MHz); 0 = 1 */
};

/* ---- environment ---- */
uint32_t a6_rd(uint64_t pa);
void a6_wr(uint64_t pa, uint32_t v);
int a6_write(uint64_t pa, const void *buf, size_t len);	/* DRAM: any size; MMIO: words */
int a6_read(uint64_t pa, void *buf, size_t len);
void a6_delay_ms(unsigned ms);
void a6_delay_us(unsigned us);
void a6_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void a6_smc(uint64_t x[8]);				/* x0..x7 in, x0..x3 out */
const uint8_t *a6_firmware(const char *name, uint32_t *len);	/* NULL if missing */
uint64_t a6_pt_extra(uint32_t n);	/* PA of the nth 4K page table beyond the pool's arena; 0 = none */

/* ---- core ---- */
static inline uint64_t a6_pool_pa(const struct a6_gpu *g, uint64_t off) { return g->pool_pa + off; }
static inline uint64_t a6_pool_va(uint64_t off) { return A6_GVA_POOL + off; }

void a6_init_state(struct a6_gpu *g, uint64_t pool_pa);
int a6_power_up(struct a6_gpu *g);	/* everything up to a working CP with the zap shader; 0 = ok */
int a6_recover(struct a6_gpu *g);	/* after a hang/fault: soft reset, CP re-init (mappings kept); 0 = ok */
bool a6_cx_up(void);
bool a6_gx_up(void);
bool a6_cp_running(const struct a6_gpu *g);	/* GX on and the CP runs our SQE */

/* 1036: GPU DCVS helpers */
int a6_perf_vote_fast(struct a6_gpu *g, uint32_t lvl);	/* HFI perf vote, ack polled every 20 us; 0 = ok */
uint32_t a6_level_khz(uint32_t lvl);			/* 0 past the table */
uint32_t a6_nlevels(void);				/* highest level number */
void a6_busy_counter_enable(void);			/* GMU POWER_COUNTER XOCLK_0 = GPU busy (Linux a6xx) */
uint64_t a6_busy_counter(void);				/* 19.2 MHz ticks the GPU was busy */

uint32_t a6_pkt7(uint32_t op, uint32_t cnt);
uint32_t a6_pkt4(uint32_t reg, uint32_t cnt);

/* Queue dwords into the ring, move WPTR, wait for rptr == wptr and !busy. what: log label or NULL. */
int a6_ring(struct a6_gpu *g, const uint32_t *dw, uint32_t n, const char *what, unsigned timeout_ms);
/* IB at iova, then WFI + fence write; waits for the fence. 0 = ok. */
int a6_submit_ib(struct a6_gpu *g, uint64_t iova, uint32_t dwords, unsigned timeout_ms);
/* The same without waiting: returns the fence value (0 = no ring space within timeout_ms) */
uint32_t a6_submit_ib_async(struct a6_gpu *g, uint64_t iova, uint32_t dwords, unsigned timeout_ms);
/*
 * Up to A6_SUBMIT_MAX IBs as Linux's a6xx_submit wraps them: CCU depth/colour invalidate, the IBs, then
 * the fence written by CACHE_FLUSH_TS (so everything the IBs wrote is in memory when it lands). No wait:
 * returns the fence, or 0 when the ring has no room within timeout_ms (0 = look once).
 */
#define A6_SUBMIT_MAX	16
uint32_t a6_submit_ibs_async_wait(struct a6_gpu *g, const uint64_t *iova, const uint32_t *dwords, unsigned n,
	unsigned timeout_ms, uint64_t wait_va, uint32_t wait_ref);	/* 0989 */
#define A6_SCAN_WORD_OFF	0x3000ull	/* 0989: in the scratch page: the direct scanout's latched frame */
uint32_t a6_submit_ibs_async(struct a6_gpu *g, const uint64_t *iova, const uint32_t *dwords, unsigned n,
			     unsigned timeout_ms);
/*
 * 0999: the GPU interrupt (kgsl_3d0_irq). a6_irq_mask nonzero: RBBM_INT_0_MASK gets it at each CP init (and
 * a6_irq_enable now); a6_submit_irq: the fence's CACHE_FLUSH_TS raises CP_CACHE_FLUSH_TS (CP_EVENT_WRITE IRQ bit).
 * a6_irq_ack: RBBM_INT_0_STATUS, cleared (the line is level: before the handler returns).
 */
#define A6_INT_CP_RB_DONE_TS		0x00020000u	/* a6xx.xml A6XX_RBBM_INT_0_MASK */
#define A6_INT_CP_CACHE_FLUSH_TS	0x00100000u
extern uint32_t a6_irq_mask;
extern int a6_submit_irq;
void a6_irq_enable(uint32_t mask);
uint32_t a6_irq_ack(void);
/* Has the fence (from a6_submit_ib_async) landed? (wraparound-safe) */
bool a6_fence_done(const struct a6_gpu *g, uint32_t fence);
uint32_t a6_fence_read(const struct a6_gpu *g);
/* After a6_recover: the fence memory to the last fence submitted (lost work counts as done) */
void a6_fence_force(struct a6_gpu *g);
/* CP_ALWAYS_ON_COUNTER (19.2 MHz) */
uint64_t a6_timestamp(const struct a6_gpu *g);
/* GPU ticks (19.2 MHz) a landed fence's submit took (a6_submit_ibs_async stamps its start and idle end) */
#define A6_TS_OFF	0x1000ull	/* in the scratch page: 16-byte slots by fence */
#define A6_TS_SLOTS	256u
uint64_t a6_submit_time(const struct a6_gpu *g, uint32_t fence);

/* 4K pages (AP=01, NC) at iova -> pa; tables from the arena. Then a6_tlb_flush. */
int a6_map(struct a6_gpu *g, uint64_t iova, uint64_t pa, uint64_t size);
int a6_map_pages(struct a6_gpu *g, uint64_t iova, const uint64_t *pages, uint32_t npages, uint32_t page_size);
void a6_unmap(struct a6_gpu *g, uint64_t iova, uint64_t size);	/* clear 4K entries; then a6_tlb_flush */
void a6_unmap_high(struct a6_gpu *g);	/* drop all mappings above 1 GB, reset the arena */
void a6_tlb_flush(const struct a6_gpu *g);
int a6_clear_fault(const struct a6_gpu *g);	/* 1 if an SMMU fault was cleared */
uint32_t a6_fault_status(const struct a6_gpu *g, uint64_t *far, uint32_t *fsynr0);

/*
 * Scaled, filtered 2D blit (the one Mesa's turnip emits for vkCmdBlitImage
 * on the A640, work/gpu/cap-blit): BGRA8 linear -> BGRA8 linear, pitches
 * multiples of 64 bytes. Writes the IB into dw (A6_BLIT_MAX_DW) and returns
 * its length.
 */
struct a6_blit {
	uint64_t src_iova;
	uint32_t src_w, src_h, src_pitch;
	uint32_t sx, sy, sw, sh;		/* source rectangle */
	uint64_t dst_iova;
	uint32_t dst_pitch;
	uint32_t dx, dy, dw, dh;		/* destination rectangle */
	bool filter;				/* bilinear */
	bool blend;				/* source-over alpha blend */
	bool clip;				/* write only inside the scissor: */
	uint32_t cx0, cy0, cx1, cy1;		/* destination, inclusive */
};
#define A6_BLIT_MAX_DW	256
uint32_t a6_blit_ib(const struct a6_gpu *g, const struct a6_blit *b, uint32_t *dw);

#define A6_CP_NOP		0x10
#define A6_CP_WAIT_FOR_IDLE	0x26
#define A6_CP_BLIT		0x2c
#define A6_CP_MEM_WRITE		0x3d
#define A6_CP_INDIRECT_BUFFER	0x3f
#define A6_CP_EVENT_WRITE	0x46
#define A6_CP_ME_INIT		0x48
#define A6_CP_SET_SECURE_MODE	0x66
#define A6_CP_MEMCPY		0x75

/* Register/state access for status and dumps */
#if A6_GPU_ID == 620
#define A6_GPU_BASE		0x03d00000ull	/* redfin kgsl-3d0 */
#define A6_GPUCC		0x03d90000ull	/* gpucc-lito */
#define A6_SMMU			0x03da0000ull	/* redfin kgsl-smmu (CB5-7 belong to the hypervisor) */
#elif A6_GPU_ID == 618
#define A6_GPU_BASE		0x05000000ull	/* sunfish kgsl-3d0 (flame 0x02c00000) */
#define A6_GPUCC		0x05090000ull	/* gpucc-sdmmagpie */
#define A6_SMMU			0x05040000ull	/* sunfish kgsl-iommu */
#else
#define A6_GPU_BASE		0x02c00000ull
#define A6_GPUCC		0x02c90000ull
#define A6_SMMU			0x02ca0000ull
#endif
#define A6_GREG(o)		(A6_GPU_BASE + 4ull * (o))
#define A6_SMMU_CB(n)		(A6_SMMU + 0x8000 + 0x1000ull * (n))
#define A6_FSR_FAULT		0xc00001feu
#define A6_GMEM_LOG_OFF		0x91000ull
#if A6_GPU_ID == 618
#define A6_GMU_LOG_VA		0x60010000u	/* sunfish: kgsl order puts HFI at 0x60001000 */
#else
#define A6_GMU_LOG_VA		0x60001000u
#endif
#define A6_GMU_LOG_SIZE		0x4000u

#endif
