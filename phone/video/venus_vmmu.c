/*
 * venus-iommu: userland side of /dev/vmmu (xnu 1270 bsd/dev/sweet_vmmu.c; same structs and numbers).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "venus_vmmu.h"

struct vmmu_attach_a {
	uint32_t cb, flags;
	uint64_t l1;
};
struct vmmu_map_a {
	uint64_t addr, len;
	uint32_t surface, flags;
	uint64_t iova, handle, plen;
	uint32_t info[4];
};
struct vmmu_sync_a {
	uint64_t iova, off, len;
	uint32_t op, pad;
};
struct vmmu_status_a {
	uint32_t cb, maps, tables, faults;
	uint32_t fsr, fsynr0, sid, enabled;
	uint64_t far, bytes, l1, pad;
};
#define VMMU_ATTACH	_IOWR('V', 1, struct vmmu_attach_a)
#define VMMU_MAP	_IOWR('V', 2, struct vmmu_map_a)
#define VMMU_UNMAP	_IOW('V', 3, uint64_t)
#define VMMU_SYNC	_IOW('V', 4, struct vmmu_sync_a)
#define VMMU_STATUS	_IOR('V', 5, struct vmmu_status_a)
#define VMMU_DETACH	_IO('V', 6)

#define NB 256
static struct {
	uint32_t iova, len;
	uint8_t *cpu;		/* our mapping (NULL for a surface mapped by id) */
	int owned;		/* mmap'd by vmmu_alloc: munmap at reset */
	void *ref;		/* an IOSurface we created (vmmu_new_surface): unlocked + released at unmap */
} vb[NB];
static void surf_release(void *ref);
static int nvb, fd = -1, active;
double vmmu_sync_ms;

static double ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int vmmu_want(void)
{
	const char *e = getenv("VENUSCTL_VMMU");
	return e && atoi(e) != 0;
}

int vmmu_active(void)
{
	return active;
}

int vmmu_attach(int cb, uint64_t l1)
{
	if (fd < 0)
		fd = open("/dev/vmmu", O_RDWR);
	if (fd < 0) {
		printf("vmmu: open /dev/vmmu: %s (kernel without 1270?)\n", strerror(errno));
		return -1;
	}
	struct vmmu_attach_a a = { .cb = (uint32_t)cb, .l1 = l1 };
	if (ioctl(fd, VMMU_ATTACH, &a)) {
		printf("vmmu: attach CB%d L1 %#llx: %s%s\n", cb, (unsigned long long)l1, strerror(errno),
		       errno == ENODEV ? " (sysctl kern.sweet_vmmu=1 first)" : "");
		return -1;
	}
	active = 1;
	printf("vmmu: on: CB%d, L1 %#llx; session buffers in ordinary memory (IOVA 0x40000000-0x80000000)\n", cb,
	       (unsigned long long)a.l1);
	return 0;
}

static int add(uint32_t iova, uint32_t len, uint8_t *cpu, int owned)
{
	if (nvb >= NB)
		return -1;
	vb[nvb].iova = iova;
	vb[nvb].len = len;
	vb[nvb].cpu = cpu;
	vb[nvb].owned = owned;
	vb[nvb].ref = NULL;
	nvb++;
	return 0;
}

static int find(uint64_t pa, size_t len)
{
	for (int i = 0; i < nvb; i++)
		if (pa >= vb[i].iova && pa + len <= (uint64_t)vb[i].iova + vb[i].len)
			return i;
	return -1;
}

static uint64_t fixed_iova;	/* vmmu_alloc_at: the next map goes there */
static uint32_t map(uint64_t addr, uint64_t len, uint32_t surface, uint32_t *plen, uint32_t info[4])
{
	struct vmmu_map_a m = { .addr = addr, .len = len, .surface = surface, .iova = fixed_iova };
	fixed_iova = 0;
	if (fd < 0 || ioctl(fd, VMMU_MAP, &m)) {
		printf("vmmu: map %s %#llx+%#llx: %s\n", surface ? "surface" : "memory", surface ? surface :
		       (unsigned long long)addr, (unsigned long long)len, strerror(errno));
		return 0;
	}
	if (plen)
		*plen = (uint32_t)m.plen;
	if (info)
		memcpy(info, m.info, sizeof(m.info));
	return (uint32_t)m.iova;
}

int vmmu_sync(uint64_t pa, size_t len, int op)
{
	double t0 = ms();
	struct vmmu_sync_a s = { .iova = pa, .len = len, .op = (uint32_t)op };
	int r = ioctl(fd, VMMU_SYNC, &s);
	vmmu_sync_ms += ms() - t0;
	if (r)
		printf("vmmu: sync %#llx+%#zx op %d: %s\n", (unsigned long long)pa, len, op, strerror(errno));
	return r ? -1 : 0;
}

uint32_t vmmu_alloc(uint32_t size, const char *what)
{
	size_t len = ((size_t)size + 0x3fff) & ~(size_t)0x3fff;
	uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED) {
		printf("vmmu: %s: mmap %#zx: %s\n", what, len, strerror(errno));
		return 0;
	}
	memset(p, 0, len);	/* fault every page in before the pin */
	uint32_t plen = 0, iova = map((uint64_t)(uintptr_t)p, len, 0, &plen, NULL);
	if (!iova || add(iova, plen, p, 1)) {
		munmap(p, len);
		return 0;
	}
	vmmu_sync(iova, plen, 2);	/* no dirty zero line may land on what Venus writes later */
	if (getenv("VENUSCTL_VMMU_LOG"))
		printf("vmmu: %s %#x bytes at IOVA %#x\n", what, size, iova);
	return iova;
}

/* the same at a fixed IOVA (e.g. the CP non-pixel range below 0x25800000, as stock's encoder persist buffer) */
uint32_t vmmu_alloc_at(uint32_t size, const char *what, uint32_t iova)
{
	fixed_iova = iova;
	uint32_t r = vmmu_alloc(size, what);
	fixed_iova = 0;
	if (r)
		printf("vmmu: %s %#x bytes at fixed IOVA %#x\n", what, size, r);
	return r;
}

uint32_t vmmu_map_user(void *p, size_t len, const char *what)
{
	uint32_t plen = 0, iova = map((uint64_t)(uintptr_t)p, len, 0, &plen, NULL);
	if (!iova || add(iova, plen, p, 0))
		return 0;
	if (getenv("VENUSCTL_VMMU_LOG"))
		printf("vmmu: %s %#zx bytes at IOVA %#x\n", what, len, iova);
	return iova;
}

uint32_t vmmu_map_surface(uint32_t id, uint32_t *len, uint32_t info[4])
{
	uint32_t plen = 0, iova = map(0, 0, id, &plen, info);
	if (!iova || add(iova, plen, NULL, 0))
		return 0;
	if (len)
		*len = plen;
	return iova;
}

int vmmu_unmap(uint32_t iova)
{
	int i = find(iova, 1);
	if (i < 0)
		return -1;
	uint64_t a = iova;
	int r = ioctl(fd, VMMU_UNMAP, &a);
	if (vb[i].owned)
		munmap(vb[i].cpu, vb[i].len);
	if (vb[i].ref)
		surf_release(vb[i].ref);
	vb[i] = vb[--nvb];
	return r;
}

int vmmu_io(uint64_t pa, void *buf, size_t len, int write)
{
	if (!active)
		return 0;
	int i = find(pa, len);
	if (i < 0)
		return 0;
	if (!vb[i].cpu)
		return -1;	/* a surface we did not map for the CPU */
	uint8_t *p = vb[i].cpu + (pa - vb[i].iova);
	if (write) {
		memcpy(p, buf, len);
		return vmmu_sync(pa, len, 1) ? -1 : 1;
	}
	if (vmmu_sync(pa, len, 2))
		return -1;
	memcpy(buf, p, len);
	return 1;
}

uint8_t *vmmu_cpu(uint64_t pa, size_t len)
{
	int i = active ? find(pa, len) : -1;
	return i < 0 || !vb[i].cpu ? NULL : vb[i].cpu + (pa - vb[i].iova);
}

void vmmu_reset(const char *why)
{
	int n = nvb;
	while (nvb > 0)
		vmmu_unmap(vb[nvb - 1].iova);
	if (n && getenv("VENUSCTL_VMMU_LOG"))
		printf("vmmu: %d buffers unmapped (%s)\n", n, why);
}

void vmmu_status(const char *tag)
{
	struct vmmu_status_a s;
	memset(&s, 0, sizeof(s));
	if (fd < 0 || ioctl(fd, VMMU_STATUS, &s)) {
		printf("vmmu: %s: no status\n", tag);
		return;
	}
	printf("vmmu: %s: CB%d maps %u (%llu KB) tables %u faults %u FSR %#x FAR %#llx SID %#x, sync %.1f ms\n", tag,
	       (int)s.cb, s.maps, (unsigned long long)(s.bytes >> 10), s.tables, s.faults, s.fsr, (unsigned long long)s.far,
	       s.sid, vmmu_sync_ms);
}

/* ---- venus-iommu zero-copy: NV12 IOSurfaces in Venus's own layout, mapped into the bank by id ---- */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

static void dnum(CFMutableDictionaryRef d, CFStringRef k, uint32_t v)
{
	int32_t x = (int32_t)v;
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &x);
	CFDictionarySetValue(d, k, n);
	CFRelease(n);
}

static void surf_release(void *ref)
{
	IOSurfaceUnlock((IOSurfaceRef)ref, 0, NULL);
	CFRelease((IOSurfaceRef)ref);
}

static void set_cpu(uint32_t iova, uint8_t *cpu, void *ref)
{
	int i = find(iova, 1);
	if (i >= 0) {
		vb[i].cpu = cpu;
		vb[i].ref = ref;
	}
}

/* a 420v surface W x H with Y stride STRIDE, CbCr plane at STRIDE * YSCAN, at least SIZE bytes; mapped into the bank
 * by id (as a camera / VT client surface would be), its CPU mapping registered for big_io (locked for good).
 * IOVA or 0; *id, *ref out */
uint32_t vmmu_new_surface(uint32_t w, uint32_t h, uint32_t stride, uint32_t yscan, uint32_t size, uint32_t *id,
			  void **ref)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef p0 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							      &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef p1 = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
							      &kCFTypeDictionaryValueCallBacks);
	uint32_t uvh = (h + 1) / 2, alloc = (size + 0x3fff) & ~0x3fffu;
	dnum(d, kIOSurfaceWidth, w);
	dnum(d, kIOSurfaceHeight, h);
	dnum(d, kIOSurfacePixelFormat, 0x34323076);	/* '420v' */
	dnum(d, kIOSurfaceAllocSize, alloc);
	dnum(p0, kIOSurfacePlaneWidth, w);
	dnum(p0, kIOSurfacePlaneHeight, h);
	dnum(p0, kIOSurfacePlaneBytesPerRow, stride);
	dnum(p0, kIOSurfacePlaneOffset, 0);
	dnum(p0, kIOSurfacePlaneSize, stride * yscan);
	dnum(p0, kIOSurfacePlaneBytesPerElement, 1);
	dnum(p1, kIOSurfacePlaneWidth, (w + 1) / 2);
	dnum(p1, kIOSurfacePlaneHeight, uvh);
	dnum(p1, kIOSurfacePlaneBytesPerRow, stride);
	dnum(p1, kIOSurfacePlaneOffset, stride * yscan);
	dnum(p1, kIOSurfacePlaneSize, alloc - stride * yscan);
	dnum(p1, kIOSurfacePlaneBytesPerElement, 2);
	const void *pl[2] = { p0, p1 };
	CFArrayRef a = CFArrayCreate(NULL, pl, 2, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(d, kIOSurfacePlaneInfo, a);
	IOSurfaceRef s = IOSurfaceCreate(d);
	CFRelease(a);
	CFRelease(p0);
	CFRelease(p1);
	CFRelease(d);
	if (!s) {
		printf("vmmu: IOSurfaceCreate %ux%u stride %u yscan %u failed\n", w, h, stride, yscan);
		return 0;
	}
	if (IOSurfaceGetBytesPerRowOfPlane(s, 0) != stride || IOSurfaceGetBytesPerRowOfPlane(s, 1) != stride ||
	    (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1) - (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 0) !=
		    (ptrdiff_t)stride * yscan) {
		printf("vmmu: surface %u layout differs: bpr %zu/%zu, plane 1 at %td (want %u, %u)\n", IOSurfaceGetID(s),
		       IOSurfaceGetBytesPerRowOfPlane(s, 0), IOSurfaceGetBytesPerRowOfPlane(s, 1),
		       (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1) - (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 0),
		       stride, stride * yscan);
		CFRelease(s);
		return 0;
	}
	uint32_t len = 0, info[4], iova = vmmu_map_surface(IOSurfaceGetID(s), &len, info);
	if (!iova || len < size) {
		printf("vmmu: surface %u: mapped %u of %u bytes\n", IOSurfaceGetID(s), len, size);
		if (iova)
			vmmu_unmap(iova);
		CFRelease(s);
		return 0;
	}
	/* the kernel zeroed the surface through a cached mapping at create time: those dirty lines must reach memory
	 * now, or an eviction later lands on what Venus wrote (seen: 720p decode into fresh surfaces, md5 off) */
	vmmu_sync(iova, len, 2);
	IOSurfaceLock(s, 0, NULL);
	set_cpu(iova, IOSurfaceGetBaseAddress(s), (void *)s);
	*id = IOSurfaceGetID(s);
	*ref = (void *)s;
	if (getenv("VENUSCTL_VMMU_LOG"))
		printf("vmmu: surface %u %ux%u stride %u yscan %u (%u bytes, info %ux%u bpr %u fmt %#x) at IOVA %#x\n", *id,
		       w, h, stride, yscan, len, info[0], info[1], info[2], info[3], iova);
	return iova;
}

void vmmu_free_surface(uint32_t iova, void *ref)
{
	(void)ref;
	if (iova)
		vmmu_unmap(iova);	/* releases the surface */
}
