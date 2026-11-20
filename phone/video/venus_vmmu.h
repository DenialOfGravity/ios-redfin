#ifndef VENUS_VMMU_H
#define VENUS_VMMU_H
/*
 * venus-iommu: session buffers in ordinary memory mapped into the Venus bank by the kernel (/dev/vmmu, xnu 1270),
 * instead of the fixed /dev/pmem window. Switch: VENUSCTL_VMMU=1 (needs kern.sweet_vmmu=1); default off = window.
 * Device address = IOVA (0x40000000-0x80000000); the CPU reaches a buffer through its own cached mapping (memcpy +
 * cache maintenance through VMMU_SYNC: Venus is not IO-coherent).
 */
#include <stddef.h>
#include <stdint.h>

int vmmu_want(void);				/* VENUSCTL_VMMU set (non-zero) */
int vmmu_attach(int cb, uint64_t l1);		/* 0 = on: buffers come from vmmu_alloc from now on */
int vmmu_active(void);
uint32_t vmmu_alloc(uint32_t size, const char *what);	/* zeroed, cleaned; IOVA or 0 */
uint32_t vmmu_alloc_at(uint32_t size, const char *what, uint32_t iova);	/* the same at a fixed IOVA */
/* an IOSurface by id (no mapping in this process needed); IOVA or 0; info: width, height, bytesPerRow, format */
uint32_t vmmu_map_surface(uint32_t id, uint32_t *len, uint32_t info[4]);
uint32_t vmmu_map_user(void *p, size_t len, const char *what);	/* any page-aligned memory of ours */
int vmmu_unmap(uint32_t iova);
/* buffer I/O: 1 = done (pa is a vmmu buffer), 0 = not ours (caller uses /dev/pmem), -1 = error */
int vmmu_io(uint64_t pa, void *buf, size_t len, int write);
uint8_t *vmmu_cpu(uint64_t pa, size_t len);	/* our CPU pointer to [pa, pa+len) or NULL */
int vmmu_sync(uint64_t pa, size_t len, int op);	/* op 1 clean (before Venus reads), 2 clean+invalidate (before CPU reads) */
void vmmu_reset(const char *why);		/* unmap + free every buffer vmmu_alloc made */
void vmmu_status(const char *tag);		/* one log line: maps, bytes, FSR/FAR/SID */
extern double vmmu_sync_ms;
/* zero-copy: a 420v IOSurface in Venus's NV12 layout (stride, CbCr at stride * yscan), mapped by id; IOVA or 0 */
uint32_t vmmu_new_surface(uint32_t w, uint32_t h, uint32_t stride, uint32_t yscan, uint32_t size, uint32_t *id,
			  void **ref);
void vmmu_free_surface(uint32_t iova, void *ref);			/* time spent in VMMU_SYNC (cache maintenance) */

#endif
