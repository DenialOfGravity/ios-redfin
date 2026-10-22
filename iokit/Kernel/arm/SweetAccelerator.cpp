/*
 * The GPU as Metal sees it. Metal.framework (MTLRegisterDevices) iterates
 * IOServiceMatching("IOAcceleratorES"), loads
 * /System/Library/Extensions/<MetalPluginName>.bundle, takes the class named
 * MetalPluginClassName (a subclass of Metal's _MTLDevice) and calls
 * -initWithAcceleratorPort: with the service. SweetAccelerator publishes
 * itself under that class name with the properties that point Metal at
 * SweetMetal.bundle (phone/metal), whose device drives the Adreno 640
 * through bsd/dev/sweet_gpu.c.
 *
 * Boot-arg sweetmtl=1 publishes it; without it Metal finds no device and
 * Core Animation keeps its software renderer.
 *
 * User client (IOServiceOpen type 0), scalar in/out:
 *   0 info            -> [gpu_id, chip_id, ready]
 *   1 buffer alloc    [size] -> [handle, gpu_va, size]  zeroed, wired, cleaned; map it into the task
 *                     with IOConnectMapMemory64(type = handle): cacheable, so userland does the cache
 *                     maintenance (the GPU does not snoop)
 *   2 buffer free     [handle]                          (GPU VA and handle are reused)
 *   3 surface import  [IOSurface id] -> [handle, gpu_va, size]  the surface's pages, wired
 *   4 submit          [ib_gpu_va, dwords, timeout_ms] -> [fence]  runs the IB and waits for it
 *   6 submit async    [ib_gpu_va, dwords] -> [fence]      queued, not waited for (fence 0: GPU not usable)
 *   7 wait fence      [fence, timeout_ms]               0, or timeout (the GPU is recovered)
 *   5 cache flush     [handle, offset, length]  clean+invalidate through the kernel mapping: before
 *                     a submit for what the CPU wrote, after it for what the CPU will read
 *                     (sys_dcache_flush does nothing on iOS: Apple GPUs are coherent)
 * Buffers belong to the client and go with it.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include "SweetSurface.h"

extern "C" {
bool sweet_gpu_ready(void);
int sweet_gpu_map_user(uint64_t *iova_out, const uint64_t *pages, uint32_t npages, uint32_t page_size);
void sweet_gpu_unmap_user(uint64_t iova, uint64_t len);
int sweet_gpu_submit_user(uint64_t ib_iova, uint32_t dwords, uint32_t timeout_ms, uint32_t *fence_out);
uint32_t sweet_gpu_submit_user_async(uint64_t ib_iova, uint32_t dwords);
int sweet_gpu_wait_fence(uint32_t fence, uint32_t timeout_ms);
void flush_dcache64(addr64_t addr, unsigned count, int phys);
}

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class SweetAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(SweetAccelerator);

public:
	IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAccelerator, IOAcceleratorES);

IOService *
SweetAccelerator::probe(IOService *provider, SInt32 *score)
{
	uint32_t on = 0;
	PE_parse_boot_argn("sweetmtl", &on, sizeof(on));
	return on ? IOService::probe(provider, score) : NULL;
}

bool
SweetAccelerator::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty("MetalPluginName", "SweetMetal");
	setProperty("MetalPluginClassName", "SweetMTLDevice");
	setProperty("IOGLBundleName", "SweetMetal");
	setProperty("model", "Adreno 640");
	setProperty(kIOUserClientClassKey, "SweetAcceleratorUserClient");
	registerService();
	IOLog("SWEET MTL: IOAcceleratorES published (MetalPluginName SweetMetal)\n");
	return true;
}

/* ---- user client ---- */

#define SWEET_MTL_MAX_BUFFERS   4096

class SweetAcceleratorUserClient : public IOUserClient {
	OSDeclareDefaultStructors(SweetAcceleratorUserClient);

	task_t   fTask;
	IOLock  *fLock;
	OSArray *fMem;          /* index + 1 = handle; IOMemoryDescriptor, or kOSBooleanFalse once freed */
	OSArray *fMaps;         /* same index: the kernel IOMemoryMap, for cache maintenance */
	uint64_t *fVA;          /* same index: GPU VA and length (0 once freed) */
	uint64_t *fLen;

	int addMemory(IOMemoryDescriptor *md, uint64_t *handle, uint64_t *va);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetAcceleratorUserClient, IOUserClient);

bool
SweetAcceleratorUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	fLock = IOLockAlloc();
	fMem = OSArray::withCapacity(64);
	fMaps = OSArray::withCapacity(64);
	fVA = IONew(uint64_t, SWEET_MTL_MAX_BUFFERS);
	fLen = IONew(uint64_t, SWEET_MTL_MAX_BUFFERS);
	return fLock != NULL && fMem != NULL && fMaps != NULL && fVA != NULL && fLen != NULL;
}

IOReturn
SweetAcceleratorUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

void
SweetAcceleratorUserClient::free(void)
{
	if (fMem != NULL) {
		for (unsigned i = 0; i < fMem->getCount(); i++) {
			IOMemoryDescriptor *md = OSDynamicCast(IOMemoryDescriptor, fMem->getObject(i));
			if (md != NULL) {
				sweet_gpu_unmap_user(fVA[i], fLen[i]);
				md->complete();
			}
		}
	}
	if (fVA != NULL) {
		IODelete(fVA, uint64_t, SWEET_MTL_MAX_BUFFERS);
		fVA = NULL;
	}
	if (fLen != NULL) {
		IODelete(fLen, uint64_t, SWEET_MTL_MAX_BUFFERS);
		fLen = NULL;
	}
	OSSafeReleaseNULL(fMem);
	OSSafeReleaseNULL(fMaps);
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

/* Wire md, clean it, map its pages into the GPU, keep it. */
int
SweetAcceleratorUserClient::addMemory(IOMemoryDescriptor *md, uint64_t *handle, uint64_t *va)
{
	static uint64_t pages[8192];
	uint64_t len = md->getLength();
	if (len == 0 || len > sizeof(pages) / sizeof(pages[0]) * PAGE_SIZE || md->prepare() != kIOReturnSuccess) {
		return -1;
	}
	IOLockLock(fLock);
	uint32_t n = 0, slot = 0;
	while (slot < fMem->getCount() && fMem->getObject(slot) != kOSBooleanFalse) {
		slot++;         /* a freed handle, else a new one */
	}
	bool ok = slot < SWEET_MTL_MAX_BUFFERS;
	for (uint64_t off = 0; ok && off < len; off += PAGE_SIZE) {
		IOByteCount seg = 0;
		addr64_t pa = md->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
		ok = pa != 0 && (pa & PAGE_MASK) == 0;
		pages[n++] = pa;
	}
	/* Clean the CPU's view to memory through a kernel mapping (the GPU does not snoop). */
	IOMemoryMap *km = ok ? md->map() : NULL;
	if (km != NULL) {
		flush_dcache64((addr64_t)km->getVirtualAddress(), (unsigned)len, FALSE);
	}
	if (!ok || km == NULL || sweet_gpu_map_user(va, pages, n, PAGE_SIZE)) {
		IOLockUnlock(fLock);
		OSSafeReleaseNULL(km);
		md->complete();
		return -1;
	}
	if (slot < fMem->getCount()) {
		fMaps->replaceObject(slot, km);
		fMem->replaceObject(slot, md);
	} else {
		fMaps->setObject(km);
		fMem->setObject(md);
	}
	km->release();
	fVA[slot] = *va;
	fLen[slot] = len;
	*handle = slot + 1;
	IOLockUnlock(fLock);
	return 0;
}

IOReturn
SweetAcceleratorUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOLockLock(fLock);
	IOMemoryDescriptor *md = type >= 1 && type <= fMem->getCount() ?
	    OSDynamicCast(IOMemoryDescriptor, fMem->getObject(type - 1)) : NULL;
	if (md != NULL) {
		md->retain();
	}
	IOLockUnlock(fLock);
	if (md == NULL) {
		return kIOReturnBadArgument;
	}
	/*
	 * Default (cacheable) mapping. A write-combined one panicked
	 * "phystokv_range: illegal PA 0xf87e8000" for page 0x1f87e8000: the cache
	 * mode change cleans pages by a 32-bit-truncated physical address. The
	 * GPU does not snoop, so userland cleans what it wrote (sys_dcache_flush)
	 * before a submit and invalidates what it reads after.
	 */
	*options = (*options & ~kIOMapCacheMask) | kIOMapDefaultCache;
	*memory = md;
	return kIOReturnSuccess;
}

IOReturn
SweetAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	const uint64_t *in = args->scalarInput;
	uint64_t *out = args->scalarOutput;
	uint32_t nin = args->scalarInputCount;

	switch (selector) {
	case 0:
		if (args->scalarOutputCount < 3) {
			return kIOReturnBadArgument;
		}
		out[0] = 640;
		out[1] = 0x06040001;
		out[2] = sweet_gpu_ready();
		return kIOReturnSuccess;
	case 1: {
		if (nin < 1 || args->scalarOutputCount < 3 || in[0] == 0 || in[0] > (1ULL << 30)) {
			return kIOReturnBadArgument;
		}
		uint64_t size = round_page_64(in[0]);
		IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
		if (bmd == NULL) {
			return kIOReturnNoMemory;
		}
		bzero(bmd->getBytesNoCopy(), size);
		uint64_t handle = 0, va = 0;
		int rc = addMemory(bmd, &handle, &va);
		bmd->release();
		if (rc) {
			return kIOReturnNoResources;
		}
		out[0] = handle;
		out[1] = va;
		out[2] = size;
		return kIOReturnSuccess;
	}
	case 2: {
		if (nin < 1) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		IOMemoryDescriptor *md = in[0] >= 1 && in[0] <= fMem->getCount() ?
		    OSDynamicCast(IOMemoryDescriptor, fMem->getObject((unsigned)in[0] - 1)) : NULL;
		if (md != NULL) {
			sweet_gpu_unmap_user(fVA[in[0] - 1], fLen[in[0] - 1]);
			md->complete();
			fMem->replaceObject((unsigned)in[0] - 1, kOSBooleanFalse);
			fMaps->replaceObject((unsigned)in[0] - 1, kOSBooleanFalse);
		}
		IOLockUnlock(fLock);
		return md != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
	}
	case 3: {
		if (nin < 1 || args->scalarOutputCount < 3) {
			return kIOReturnBadArgument;
		}
		SweetSurface *s = sweet_surface_copy((uint32_t)in[0]);
		if (s == NULL || s->memory == NULL) {
			OSSafeReleaseNULL(s);
			return kIOReturnNotFound;
		}
		uint64_t handle = 0, va = 0;
		int rc = addMemory(s->memory, &handle, &va);
		out[2] = s->memory->getLength();
		s->release();
		if (rc) {
			return kIOReturnNoResources;
		}
		out[0] = handle;
		out[1] = va;
		return kIOReturnSuccess;
	}
	case 4: {
		if (nin < 3 || args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		uint32_t fence = 0;
		int rc = sweet_gpu_submit_user(in[0], (uint32_t)in[1], (uint32_t)in[2], &fence);
		out[0] = fence;
		return rc ? kIOReturnTimeout : kIOReturnSuccess;
	}
	case 5: {
		if (nin < 3) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		IOMemoryMap *km = in[0] >= 1 && in[0] <= fMaps->getCount() ?
		    OSDynamicCast(IOMemoryMap, fMaps->getObject((unsigned)in[0] - 1)) : NULL;
		IOReturn ret = kIOReturnBadArgument;
		if (km != NULL && in[1] < km->getLength() && in[2] <= km->getLength() - in[1]) {
			flush_dcache64((addr64_t)(km->getVirtualAddress() + in[1]), (unsigned)in[2], FALSE);
			ret = kIOReturnSuccess;
		}
		IOLockUnlock(fLock);
		return ret;
	}
	case 6: {
		if (nin < 2 || args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = sweet_gpu_submit_user_async(in[0], (uint32_t)in[1]);
		return out[0] ? kIOReturnSuccess : kIOReturnNotReady;
	}
	case 7:
		if (nin < 2) {
			return kIOReturnBadArgument;
		}
		return sweet_gpu_wait_fence((uint32_t)in[0], (uint32_t)in[1]) ? kIOReturnTimeout : kIOReturnSuccess;
	default:
		return kIOReturnUnsupported;
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
