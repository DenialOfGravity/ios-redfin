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

#define A6_FB_PA		0x9c400000ull
#define A6_FB_SIZE		0xa00000ull	/* mapped for the GPU: 5 x 2 MB */

#define A6_GPU_ID		640
#define A6_CHIP_ID		0x06040001ull

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

uint32_t a6_pkt7(uint32_t op, uint32_t cnt);
uint32_t a6_pkt4(uint32_t reg, uint32_t cnt);

/* Queue dwords into the ring, move WPTR, wait for rptr == wptr and !busy. what: log label or NULL. */
int a6_ring(struct a6_gpu *g, const uint32_t *dw, uint32_t n, const char *what, unsigned timeout_ms);
/* IB at iova, then WFI + fence write; waits for the fence. 0 = ok. */
int a6_submit_ib(struct a6_gpu *g, uint64_t iova, uint32_t dwords, unsigned timeout_ms);
/* The same without waiting: returns the fence value (0 = no ring space within timeout_ms) */
uint32_t a6_submit_ib_async(struct a6_gpu *g, uint64_t iova, uint32_t dwords, unsigned timeout_ms);
/* Has the fence (from a6_submit_ib_async) landed? (wraparound-safe) */
bool a6_fence_done(const struct a6_gpu *g, uint32_t fence);
uint32_t a6_fence_read(const struct a6_gpu *g);

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
#define A6_GPU_BASE		0x02c00000ull
#define A6_GREG(o)		(A6_GPU_BASE + 4ull * (o))
#define A6_GPUCC		0x02c90000ull
#define A6_SMMU			0x02ca0000ull
#define A6_SMMU_CB(n)		(A6_SMMU + 0x8000 + 0x1000ull * (n))
#define A6_FSR_FAULT		0xc00001feu
#define A6_GMEM_LOG_OFF		0x91000ull
#define A6_GMU_LOG_VA		0x60001000u
#define A6_GMU_LOG_SIZE		0x4000u

#endif
