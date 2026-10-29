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
 *
 * v2 (IOServiceOpen type 0x4147 or 2): the NativeAdreno ABI v1 (SweetGPUABI.h) that Turnip's darwin
 * backend speaks (work/turnip, tu_knl_darwin.cc): struct methods, either selector = method or tunnelled
 * as {'NTNL', method, payload} through any selector. The client picks GPU addresses inside its own
 * 4 GB window; buffers are write-combined for the CPU (pageable IOBufferMemoryDescriptors with the WC
 * cache mode: their own VM object, so every alias of the pages agrees), non-cacheable for the GPU;
 * imports pin the client's own pages (IOSurfaces), whose CPU caches the client maintains itself
 * (EL0 dc civac: SCTLR_EL1.UCI is set). Submits take up to 16 IBs (msm's a6xx_submit wrapping).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include "SweetSurface.h"
#include "SweetGPUABI.h"
#include "SweetTrace.h"     /* 0994 */
#define SWEET_V2_IB_HW  128     /* 1038: = A6_SUBMIT_MAX_HW (bsd/dev/sweet_a6xx.h) */

extern "C" {
void proc_selfname(char *buf, int size);
int proc_selfpid(void);
bool sweet_gpu_ready(void);
void sweet_swaptrace(char ev, uint32_t arg);    /* 0987 */
int sweet_gpu_map_user(uint64_t *iova_out, const uint64_t *pages, uint32_t npages, uint32_t page_size);
void sweet_gpu_unmap_user(uint64_t iova, uint64_t len);
int sweet_gpu_submit_user(uint64_t ib_iova, uint32_t dwords, uint32_t timeout_ms, uint32_t *fence_out);
uint32_t sweet_gpu_submit_user_async(uint64_t ib_iova, uint32_t dwords);
int sweet_gpu_wait_fence(uint32_t fence, uint32_t timeout_ms);
void flush_dcache64(addr64_t addr, unsigned count, int phys);
int sweet_gpu_window_alloc(uint64_t *base, uint64_t *size);
void sweet_gpu_window_free(uint64_t base);
int sweet_gpu_map_at(uint64_t iova, const uint64_t *pages, uint32_t npages);
void sweet_gpu_unmap_at(uint64_t iova, uint64_t len);
uint32_t sweet_gpu_submit_v2(const uint64_t *iova, const uint32_t *dwords, unsigned n, bool *busy);
unsigned sweet_gpu_ibmax(void);         /* 1038: kern.sweet_gpu_ibmax */
int sweet_gpu_wait_fence_ns(uint32_t fence, uint64_t timeout_ns);
bool sweet_gpu_lost(uint64_t *epoch, uint32_t last_fence);
bool sweet_gpu_lost_spare(uint64_t *epoch, uint32_t last_fence, int self_pid, uint32_t *spared);
uint64_t sweet_gpu_recoveries(void);
uint64_t sweet_gpu_timestamp(void);
int sweet_gpu_pool_alloc(uint64_t bytes, uint64_t *pa);
uint64_t sweet_gpu_pool_avail(void);
void sweet_gpu_pool_free(uint64_t pa, uint64_t bytes);
}

#define SWEET_V2_OPEN_TYPE      0x4147  /* the darwin backend's default ('AG', as the AGX plugin's) */
#define SWEET_V2_OPEN_DIRECT    2
#define SWEET_V2_TUNNEL         0x4e544e4cu     /* 'NTNL' */
#define SWEET_V2_MAX_BOS        65536
#define SWEET_V2_PAGEBUF        1024            /* 4K pages per sweet_gpu_map_at call */

/* One v2 buffer object (slot + 1 = handle = CPU-map token) */
struct SweetV2BO {
	IOMemoryDescriptor *md;         /* NULL: free slot */
	uint64_t bytes;                 /* 4K multiple, as asked */
	uint64_t iova;                  /* GPU address while mapped, else 0 */
	uint64_t pool_pa;               /* allocated: its memory in the GPU pool (sweet_gpu_pool_alloc) */
	bool imported;                  /* the client's own pages (no CPU-map token) */
};

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
	/* sweetmtl=1: SweetMetal (host-made templates, phone/metal); sweetmtl=2: SweetMetal2 (metal2vulkan + Turnip, phone/metal2) */
	uint32_t which = 1;
	PE_parse_boot_argn("sweetmtl", &which, sizeof(which));
	const char *plugin = which == 2 ? "SweetMetal2" : "SweetMetal";
	setProperty("MetalPluginName", plugin);
	setProperty("MetalPluginClassName", which == 2 ? "SweetMTL2Device" : "SweetMTLDevice");
	setProperty("IOGLBundleName", plugin);
	setProperty("model", "Adreno 620");	/* redfin (0904) */
	setProperty(kIOUserClientClassKey, "SweetAcceleratorUserClient");
	registerService();
	IOLog("SWEET MTL: IOAcceleratorES published (MetalPluginName %s)\n", plugin);
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
	uint32_t fLastFence;    /* of this client's last submit: waited for before its memory is unmapped */

	/* v2 */
	bool fV2;
	uint64_t fWinBase, fWinSize;    /* this connection's GPU addresses */
	SweetV2BO *fBO;
	uint32_t fBOCap;
	uint64_t *fPageBuf;
	uint64_t fEpoch, fFaults;       /* GPU resets seen; those that lost this client's work */
	uint64_t fPoolBytes, fPoolPeak; /* GPU pool memory this client holds */
	uint32_t fAllocs, fFrees, fImports, fImportFrees;	/* 0154: census for the holds line */
	uint32_t fV2Errors;                     /* 0928: refused v2 calls (the first 40 logged) */
	uint32_t fSpared, fImportRetries;       /* 0941: resets not reported (another's job), imports pinned on a retry */
	int fPid;
	char fName[32];
	bool fSystem;                   /* backboardd / SpringBoard: no pool cap, and the reserve is theirs */
	bool fCapLogged;
	bool v2PoolAllowed(uint64_t len);

	int addMemory(IOMemoryDescriptor *md, uint64_t *handle, uint64_t *va);
	IOReturn v2Method(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize);
	SweetV2BO *v2Find(uint64_t handle);
	IOReturn v2NewSlot(IOMemoryDescriptor *md, uint64_t bytes, bool imported, uint64_t *handle);
	void v2Release(SweetV2BO *bo);
	/* 0156: freed pool buffers >= 1 MB kept for this client's next allocation of the same size (no new pool
	 * range, no 10 MB bzero per CoreAnimation intermediate); never shared across clients */
	struct { IOMemoryDescriptor *md; uint64_t pa, len; } fReuse[16];
	uint64_t fReuseBytes;
	uint32_t fReuseHits;
	void v2ReuseDrain(void);

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
	if (fLock == NULL || fMem == NULL || fMaps == NULL || fVA == NULL || fLen == NULL) {
		return false;
	}
	if (type == SWEET_V2_OPEN_TYPE || type == SWEET_V2_OPEN_DIRECT) {
		fBOCap = 256;
		fBO = IONewZero(SweetV2BO, fBOCap);
		fPageBuf = IONew(uint64_t, SWEET_V2_PAGEBUF);
		if (fBO == NULL || fPageBuf == NULL || sweet_gpu_window_alloc(&fWinBase, &fWinSize)) {
			IOLog("SWEET MTL: v2 client: no GPU window\n");
			return false;
		}
		fEpoch = sweet_gpu_recoveries();
		proc_selfname(fName, sizeof(fName));
		fSystem = !strcmp(fName, "backboardd") || !strcmp(fName, "SpringBoard");
		fPid = proc_selfpid();
		fV2 = true;
	}
	return true;
}

/*
 * The GPU pool is shared by every process (944 MB): one app must not take what the compositor needs. Others
 * hold at most sweetgpucap MB each (default 256) and never take the last sweetgpureserve MB (default 160),
 * which stay for backboardd and SpringBoard. A refused buffer is VK_ERROR_OUT_OF_DEVICE_MEMORY in Turnip.
 */
bool
SweetAcceleratorUserClient::v2PoolAllowed(uint64_t len)
{
	static uint32_t cap_mb, reserve_mb;
	if (cap_mb == 0) {
		uint32_t v = 256;
		PE_parse_boot_argn("sweetgpucap", &v, sizeof(v));
		reserve_mb = 160;
		PE_parse_boot_argn("sweetgpureserve", &reserve_mb, sizeof(reserve_mb));
		cap_mb = v ? v : 0xffffffffu;
	}
	if (fSystem) {
		return true;
	}
	const char *why = NULL;
	if (fPoolBytes + len > ((uint64_t)cap_mb << 20)) {
		why = "its cap";
	} else if (sweet_gpu_pool_avail() < len + ((uint64_t)reserve_mb << 20)) {
		why = "the compositor's reserve";
	}
	if (why != NULL && !fCapLogged) {
		fCapLogged = true;
		IOLog("SWEET MTL: v2 %s: %llu MB buffer refused (holds %llu MB, %llu MB of the pool free): %s\n", fName,
		    len >> 20, fPoolBytes >> 20, sweet_gpu_pool_avail() >> 20, why);
	}
	return why == NULL;
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
	/*
	 * Submits are asynchronous: a client that exits (or crashes) may leave work queued. Unmapping its
	 * memory under the GPU would fault, or hand the VAs and pages to someone else while the GPU still
	 * writes them: wait for its last fence first.
	 */
	if (fLastFence != 0 && (fV2 ? sweet_gpu_wait_fence_ns(fLastFence, 2000000000ULL) : sweet_gpu_wait_fence(fLastFence, 2000))) {
		IOLog("SWEET MTL: client freed with fence %u still pending\n", fLastFence);
	}
	if (fBO != NULL) {
		IOLog("SWEET MTL: v2 %s closed (GPU pool: %llu MB held, peak %llu MB)\n", fName, fPoolBytes >> 20, fPoolPeak >> 20);
		for (uint32_t i = 0; i < fBOCap; i++) {
			v2Release(&fBO[i]);
		}
		v2ReuseDrain();
		IODelete(fBO, SweetV2BO, fBOCap);
		fBO = NULL;
	}
	if (fPageBuf != NULL) {
		IODelete(fPageBuf, uint64_t, SWEET_V2_PAGEBUF);
		fPageBuf = NULL;
	}
	if (fWinSize != 0) {
		sweet_gpu_window_free(fWinBase);
		fWinSize = 0;
	}
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
	if (fV2) {
		/* token = slot + 1; write-combined, as the buffer was made (imports keep the client's own mapping) */
		IOLockLock(fLock);
		IOMemoryDescriptor *md = type >= 1 && type <= fBOCap && !fBO[type - 1].imported ? fBO[type - 1].md : NULL;
		if (md != NULL) {
			md->retain();
		}
		IOLockUnlock(fLock);
		if (md == NULL) {
			return kIOReturnBadArgument;
		}
		*options = (*options & ~kIOMapCacheMask) | kIOMapWriteCombineCache;
		*memory = md;
		return kIOReturnSuccess;
	}
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

	if (fV2) {
		/* struct methods: tunnelled {'NTNL', method, payload} through any selector, or selector = method */
		const uint32_t *w = (const uint32_t *)args->structureInput;
		uint32_t isz = args->structureInput != NULL ? args->structureInputSize : 0;
		void *o = args->structureOutputDescriptor == NULL ? args->structureOutput : NULL;
		uint32_t osz = o != NULL ? args->structureOutputSize : 0;
		IOReturn r;
		uint32_t m = isz >= 8 && w[0] == SWEET_V2_TUNNEL ? w[1] : selector;      /* 0994 */
		SWEET_TRACE_START(SWT_ACCEL_SEL, m, fPid, 0, 0);
		if (isz >= 8 && w[0] == SWEET_V2_TUNNEL) {
			r = w[1] == 0xffffffffu ? kIOReturnSuccess : v2Method(w[1], w + 2, isz - 8, o, &osz);
		} else {
			r = v2Method(selector, isz ? args->structureInput : NULL, isz, o, &osz);
		}
		SWEET_TRACE_END(SWT_ACCEL_SEL, m, r, fLastFence, 0);
		/* 0928: every refused v2 call, the first 40 of each client (Turnip loses its device over most of them) */
		if (r != kIOReturnSuccess && fV2Errors++ < 40) {
			IOLog("SWEET MTL: v2 %s: method %u refused: 0x%x (#%u)\n", fName,
			    isz >= 8 && w[0] == SWEET_V2_TUNNEL ? w[1] : selector, r, fV2Errors);
		}
		if (o != NULL) {
			args->structureOutputSize = r == kIOReturnSuccess ? osz : 0;
		}
		return r;
	}

	switch (selector) {
	case 0:
		if (args->scalarOutputCount < 3) {
			return kIOReturnBadArgument;
		}
		out[0] = 620;		/* redfin (0904): bsd/dev/sweet_a6xx.h A6_GPU_ID / A6_CHIP_ID */
		out[1] = 0x06020000;
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
		if (out[0]) {
			fLastFence = (uint32_t)out[0];
		}
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

/* ---- v2: the NativeAdreno ABI (SweetGPUABI.h) ---- */

SweetV2BO *
SweetAcceleratorUserClient::v2Find(uint64_t handle)
{
	return handle >= 1 && handle <= fBOCap && fBO[handle - 1].md != NULL ? &fBO[handle - 1] : NULL;
}

/* With fLock held: the first free slot (the table doubles when full) */
IOReturn
SweetAcceleratorUserClient::v2NewSlot(IOMemoryDescriptor *md, uint64_t bytes, bool imported, uint64_t *handle)
{
	uint32_t i = 0;
	while (i < fBOCap && fBO[i].md != NULL) {
		i++;
	}
	if (i == fBOCap) {
		if (fBOCap >= SWEET_V2_MAX_BOS) {
			return kIOReturnNoResources;
		}
		SweetV2BO *n = IONewZero(SweetV2BO, fBOCap * 2);
		if (n == NULL) {
			return kIOReturnNoMemory;
		}
		memcpy(n, fBO, sizeof(*n) * fBOCap);
		IODelete(fBO, SweetV2BO, fBOCap);
		fBO = n;
		fBOCap *= 2;
	}
	fBO[i].md = md;
	fBO[i].bytes = bytes;
	fBO[i].iova = 0;
	fBO[i].imported = imported;
	*handle = i + 1;
	return kIOReturnSuccess;
}

/*
 * Pool memory still mapped into some task (a client that freed a buffer before unmapping it, or one
 * that died): its descriptor waits here until IOKit has dropped every mapping (retain count back to
 * ours), then the memory goes back to the pool. Swept on every v2 allocation.
 */
static IOLock *sweet_zombie_lock;
static OSArray *sweet_zombies;          /* OSData {IOMemoryDescriptor *, pa} */

struct sweet_zombie {
	IOMemoryDescriptor *md;
	uint64_t pa;
};

static void
sweet_zombie_sweep(void)
{
	if (sweet_zombie_lock == NULL) {
		return;
	}
	IOLockLock(sweet_zombie_lock);
	for (unsigned i = 0; sweet_zombies != NULL && i < sweet_zombies->getCount();) {
		OSData *d = OSDynamicCast(OSData, sweet_zombies->getObject(i));
		const struct sweet_zombie *z = d != NULL ? (const struct sweet_zombie *)d->getBytesNoCopy() : NULL;
		if (z != NULL && z->md->getRetainCount() <= 1) {
			uint64_t len = z->md->getLength(), pa = z->pa;
			z->md->release();
			sweet_gpu_pool_free(pa, len);
			sweet_zombies->removeObject(i);
			continue;
		}
		i++;
	}
	IOLockUnlock(sweet_zombie_lock);
}

static void
sweet_pool_release(IOMemoryDescriptor *md, uint64_t pa)
{
	if (md->getRetainCount() <= 1) {
		uint64_t len = md->getLength();
		md->release();
		sweet_gpu_pool_free(pa, len);
		return;
	}
	if (sweet_zombie_lock == NULL) {
		IOLock *l = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, l, &sweet_zombie_lock)) {
			IOLockFree(l);
		}
	}
	IOLockLock(sweet_zombie_lock);
	if (sweet_zombies == NULL) {
		sweet_zombies = OSArray::withCapacity(16);
	}
	struct sweet_zombie z = { md, pa };
	OSData *d = OSData::withBytes(&z, sizeof(z));
	if (sweet_zombies != NULL && d != NULL) {
		sweet_zombies->setObject(d);
	}
	OSSafeReleaseNULL(d);
	IOLockUnlock(sweet_zombie_lock);
}

/* Unmap from the GPU, unwire, drop (pool memory: once no task maps it any more) */
void
SweetAcceleratorUserClient::v2ReuseDrain(void)
{
	for (unsigned i = 0; i < 16; i++) {
		if (fReuse[i].md != NULL) {
			sweet_pool_release(fReuse[i].md, fReuse[i].pa);
			fReuse[i].md = NULL;
		}
	}
	fReuseBytes = 0;
}

void
SweetAcceleratorUserClient::v2Release(SweetV2BO *bo)
{
	if (bo->md == NULL) {
		return;
	}
	if (bo->iova != 0) {
		sweet_gpu_unmap_at(bo->iova, bo->bytes);
	}
	if (bo->imported) {
		fImportFrees++;
		bo->md->complete();
		bo->md->release();
	} else {
		fFrees++;
		uint64_t len = bo->md->getLength();
		fPoolBytes -= len;
		unsigned slot = 16;
		if (len >= (1u << 20) && bo->md->getRetainCount() <= 1 && fReuseBytes + len <= (64ULL << 20)) {
			for (unsigned i = 0; i < 16; i++) {
				if (fReuse[i].md == NULL) {
					slot = i;
					break;
				}
			}
		}
		if (slot < 16) {
			fReuse[slot].md = bo->md;
			fReuse[slot].pa = bo->pool_pa;
			fReuse[slot].len = len;
			fReuseBytes += len;
		} else {
			sweet_pool_release(bo->md, bo->pool_pa);
		}
	}
	bzero(bo, sizeof(*bo));
}

IOReturn
SweetAcceleratorUserClient::v2Method(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize)
{
	static uint32_t logged;
/* 0153: a v2 error back to a client (Turnip turns most into device lost): logged with the client, the first
 * 20 per client, then every 1000th */
#define v2_err_log(what, why) do { \
		static uint32_t v2_errs_##__LINE__; \
		uint32_t e_ = ++v2_errs_##__LINE__; \
		if (e_ <= 20 || e_ % 1000 == 0) \
			IOLog("SWEET MTL: v2 %s: %s %s (#%u)\n", fName, what, why, e_); \
	} while (0)
#define V2_IN(T)        if (in == NULL || inSize != sizeof(T)) return kIOReturnBadArgument; const T *rq = (const T *)in
#define V2_OUT(T)       if (out == NULL || *outSize < sizeof(T)) return kIOReturnBadArgument; T *rp = (T *)out; \
	                bzero(rp, sizeof(T)); *outSize = sizeof(T)
	if (!(logged & (1u << (method & 31)))) {
		logged |= 1u << (method & 31);
		IOLog("SWEET MTL: v2 method %u first used (in %u)\n", method, inSize);
	}
	switch (method) {
	case NativeAdrenoMethodQuery: {
		V2_OUT(NativeAdrenoAbiParams);
		rp->abi_version = NativeAdrenoAbiVersion;
		rp->input_bits = 39;
		rp->output_bits = 36;
		rp->flags = NativeAdrenoParamsHostImport;
		rp->max_bo_bytes = 1ULL << NativeAdrenoAbiMaxBoBytesShift;
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodGetDeviceInfo: {
		V2_OUT(NativeAdrenoAbiDeviceInfo);
		/* what bsd/dev/sweet_a6xx.c's hw_init programs: GMEM 1 MB at 0x100000, UBWC 3 (hbb 15, lvl 2+3 swizzle) */
		/* sunfish A618 (0145): chip id, 512 KB GMEM, highest bank bit 14, UBWC 2 (DT qcom,ubwc-mode) */
		/*
		 * redfin A620 (0904): chip id 0x06020000 (Mesa FD620), 512 KB GMEM at 0x100000, highest bank bit 14,
		 * UBWC 3 (DT qcom,ubwc-mode = 3: sweet_a6xx.c programs AMSBC), level 2+3 swizzle as on flame.
		 */
		rp->chip_id = 0x06020000;
		rp->gmem_bytes = 0x80000;
		rp->gmem_base = 0x100000;
		rp->uche_trap_base = 0x1fffffffff000ULL;
		rp->va_start = fWinBase;
		rp->va_size = fWinSize;
		rp->highest_bank_bit = 14;
		rp->ubwc_swizzle = 0x6;
		rp->macrotile_mode = 0;
		rp->ubwc_version = 3;
		rp->flags = NativeAdrenoDeviceClientIova;
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodBoAlloc: {
		V2_IN(NativeAdrenoAbiBoAlloc);
		V2_OUT(NativeAdrenoAbiBoAllocReply);
		if (rq->bytes == 0 || (rq->bytes & 4095) || rq->bytes > (1ULL << NativeAdrenoAbiMaxBoBytesShift) ||
		    rq->align_log2 < 12 || rq->align_log2 > 24 || rq->contiguous > 1) {
			return kIOReturnBadArgument;
		}
		/* memory XNU does not manage (sweet_gpu.c: no cacheable alias), zeroed through a WC kernel view */
		sweet_zombie_sweep();
		uint64_t len = round_page_64(rq->bytes), pa = 0;
		if (!v2PoolAllowed(len)) {
			return kIOReturnNoMemory;
		}
		IOMemoryDescriptor *md = NULL;
		IOLockLock(fLock);
		for (unsigned i = 0; len >= (1u << 20) && i < 16; i++) {
			if (fReuse[i].md != NULL && fReuse[i].len == len) {
				md = fReuse[i].md;
				pa = fReuse[i].pa;
				fReuse[i].md = NULL;
				fReuseBytes -= len;
				fReuseHits++;
				break;
			}
		}
		IOLockUnlock(fLock);
		if (md == NULL) {
			if (sweet_gpu_pool_alloc(len, &pa)) {
				IOLockLock(fLock);
				v2ReuseDrain();		/* give the cache back and try once more */
				IOLockUnlock(fLock);
				if (sweet_gpu_pool_alloc(len, &pa)) {
					return kIOReturnNoMemory;
				}
			}
			md = IOMemoryDescriptor::withPhysicalAddress(pa, len, kIODirectionInOut);
			IOMemoryMap *km = md != NULL ? md->map(kIOMapWriteCombineCache) : NULL;
			if (km == NULL) {
				OSSafeReleaseNULL(md);
				sweet_gpu_pool_free(pa, len);
				return kIOReturnNoMemory;
			}
			bzero((void *)km->getVirtualAddress(), len);
			__asm__ volatile ("dsb sy" ::: "memory");
			km->release();
		}
		IOLockLock(fLock);
		uint64_t h = 0;
		IOReturn r = v2NewSlot(md, rq->bytes, false, &h);
		if (r == kIOReturnSuccess) {
			fBO[h - 1].pool_pa = pa;
			fPoolBytes += len;
			if (fPoolBytes > fPoolPeak) {
				fPoolPeak = fPoolBytes;
			}
			fAllocs++;
			if ((fPoolBytes >> 25) != ((fPoolBytes - len) >> 25) || fAllocs % 500 == 0) {
				/* 0154: live pool buffers by size: <256K, <1M, <8M, 8M, >8M (count/MB) */
				uint32_t n[5] = { 0 };
				uint64_t mb[5] = { 0 };
				for (uint32_t i = 0; i < fBOCap; i++) {
					if (fBO[i].md == NULL || fBO[i].imported) {
						continue;
					}
					uint64_t b = fBO[i].md->getLength();
					unsigned k = b < (256u << 10) ? 0 : b < (1u << 20) ? 1 : b < (8u << 20) ? 2 : b == (8u << 20) ? 3 : 4;
					n[k]++;
					mb[k] += b;
				}
				IOLog("SWEET MTL: v2 %s holds %llu MB of GPU pool (%llu MB buffer); allocs %u frees %u imports %u/%u; "
				    "live <256K %u/%lluM <1M %u/%lluM <8M %u/%lluM 8M %u/%lluM >8M %u/%lluM; reuse %u hits %lluM kept\n", fName, fPoolBytes >> 20,
				    len >> 20, fAllocs, fFrees, fImports, fImportFrees, n[0], mb[0] >> 20, n[1], mb[1] >> 20, n[2], mb[2] >> 20,
				    n[3], mb[3] >> 20, n[4], mb[4] >> 20, fReuseHits, fReuseBytes >> 20);
			}
		}
		IOLockUnlock(fLock);
		if (r != kIOReturnSuccess) {
			md->release();
			sweet_gpu_pool_free(pa, len);
			return r;
		}
		rp->handle = h;
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodBoImport: {
		V2_IN(NativeAdrenoAbiBoImport);
		V2_OUT(NativeAdrenoAbiBoAllocReply);
		if (rq->address == 0 || ((rq->address | rq->bytes) & 4095) || rq->bytes == 0 ||
		    rq->bytes > (1ULL << NativeAdrenoAbiMaxBoBytesShift)) {
			return kIOReturnBadArgument;
		}
		/*
		 * 0941: the render server's imports (CA's surfaces, the camera's pictures) are tried again, up to 4 times
		 * 2-8 ms apart, before they are refused: a pin that fails while the pages are busy (being faulted in,
		 * wired by another import) succeeds a moment later, and a refused import of backboardd's is a missing
		 * layer. The prepare() results are logged with the refusal.
		 */
		IOMemoryDescriptor *md = NULL;
		IOReturn pin_rw = kIOReturnSuccess, pin_ro = kIOReturnSuccess;
		for (uint32_t attempt = 0; md == NULL && attempt < (fSystem ? 4u : 1u); attempt++) {
			if (attempt) {
				IOSleep(2u << attempt);
			}
			md = IOMemoryDescriptor::withAddressRange(rq->address, rq->bytes, kIODirectionInOut, fTask);
			if (md != NULL && (pin_rw = md->prepare()) != kIOReturnSuccess) {
				md->release();  /* read-only pages: pinned for reading */
				md = IOMemoryDescriptor::withAddressRange(rq->address, rq->bytes, kIODirectionOut, fTask);
				if (md != NULL && (pin_ro = md->prepare()) != kIOReturnSuccess) {
					md->release();
					md = NULL;
				}
			}
			if (md != NULL && attempt) {
				fImportRetries++;
				IOLog("SWEET MTL: v2 %s: import of %llu KB pinned on try %u (%u so far)\n", fName, rq->bytes >> 10,
				    attempt + 1, fImportRetries);
			}
		}
		if (md == NULL) {
			v2_err_log("import", "the pages could not be pinned");
			IOLog("SWEET MTL: v2 %s: import of %llu KB at 0x%llx refused (live imports %u, pin 0x%x/0x%x)\n", fName,
			    rq->bytes >> 10, rq->address, fImports - fImportFrees, pin_rw, pin_ro);
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		uint64_t h = 0;
		IOReturn r = v2NewSlot(md, rq->bytes, true, &h);
		if (r == kIOReturnSuccess) {
			fImports++;
		}
		IOLockUnlock(fLock);
		if (r != kIOReturnSuccess) {
			md->complete();
			md->release();
			return r;
		}
		rp->handle = h;
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodBoMap: {
		V2_IN(NativeAdrenoAbiBoMap);
		V2_OUT(NativeAdrenoAbiBoMapReply);
		if (!(rq->permissions & 1) || (rq->permissions & ~3u) || (rq->iova & 4095) || rq->reserved) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		SweetV2BO *bo = v2Find(rq->handle);
		IOReturn r = kIOReturnSuccess;
		if (bo == NULL) {
			r = kIOReturnBadArgument;
		} else if (bo->iova != 0) {
			r = kIOReturnBusy;
		} else if (rq->iova < fWinBase || rq->iova + bo->bytes > fWinBase + fWinSize || rq->iova + bo->bytes < rq->iova) {
			r = kIOReturnBadArgument;
		}
		/* the pages in 4K steps (16K VM pages, any 4K-aligned import), mapped in pieces */
		for (uint64_t off = 0; r == kIOReturnSuccess && off < bo->bytes;) {
			uint32_t n = 0;
			uint64_t start = off;
			for (; n < SWEET_V2_PAGEBUF && off < bo->bytes; n++, off += 4096) {
				IOByteCount seg = 0;
				addr64_t pa = bo->md->getPhysicalSegment(off, &seg, kIOMemoryMapperNone);
				if (pa == 0 || (pa & 4095)) {
					r = kIOReturnVMError;
					break;
				}
				fPageBuf[n] = pa;
			}
			if (r == kIOReturnSuccess && sweet_gpu_map_at(rq->iova + start, fPageBuf, n)) {
				r = kIOReturnNoResources;
			}
		}
		if (r == kIOReturnSuccess) {
			bo->iova = rq->iova;
			rp->iova = rq->iova;
		} else if (bo != NULL && bo->iova == 0 && r != kIOReturnBadArgument && r != kIOReturnBusy) {
			sweet_gpu_unmap_at(rq->iova, bo->bytes);
		}
		IOLockUnlock(fLock);
		if (r != kIOReturnSuccess) {
			/* 0928: Turnip takes any error here for a lost device (backboardd's screen stopped with the camera) */
			IOLog("SWEET MTL: v2 %s: map of handle %llu at 0x%llx refused: 0x%x\n", fName, rq->handle, rq->iova, r);
		}
		return r;
	}
	case NativeAdrenoMethodBoFree: {
		V2_IN(NativeAdrenoAbiBoFree);
		IOLockLock(fLock);
		SweetV2BO *bo = v2Find(rq->handle);
		if (bo != NULL) {
			v2Release(bo);
		}
		IOLockUnlock(fLock);
		return bo != NULL ? kIOReturnSuccess : kIOReturnBadArgument;
	}
	case NativeAdrenoMethodBoCpuMap: {
		V2_IN(NativeAdrenoAbiBoCpuMap);
		V2_OUT(NativeAdrenoAbiBoCpuMapReply);
		IOLockLock(fLock);
		SweetV2BO *bo = v2Find(rq->handle);
		IOReturn r = bo == NULL || bo->imported ? kIOReturnBadArgument : kIOReturnSuccess;
		if (r == kIOReturnSuccess) {
			rp->token = (uint32_t)rq->handle;       /* clientMemoryForType type */
			rp->bytes = bo->md->getLength();
		}
		IOLockUnlock(fLock);
		return r;
	}
	case NativeAdrenoMethodBoCpuUnmap:
		return kIOReturnSuccess;        /* the mapping is the client's (IOConnectUnmapMemory64) */
	case NativeAdrenoMethodSubmit: {
		/*
		 * 1038: up to kern.sweet_gpu_ibmax buffers (default 16 = NativeAdrenoAbiMaxBuffers): past 16 the extra
		 * NativeAdrenoAbiSubmitBuffer entries follow the fixed array, the record grows by 24 bytes per entry.
		 */
		if (in == NULL || inSize < sizeof(NativeAdrenoAbiSubmit)) {
			return kIOReturnBadArgument;
		}
		const NativeAdrenoAbiSubmit *rq = (const NativeAdrenoAbiSubmit *)in;
		uint32_t nmax = sweet_gpu_ibmax();
		if (nmax > SWEET_V2_IB_HW) {
			nmax = SWEET_V2_IB_HW;
		}
		uint32_t nbuf = rq->buffer_count;
		uint32_t extra = nbuf > NativeAdrenoAbiMaxBuffers ? nbuf - NativeAdrenoAbiMaxBuffers : 0;
		if (nbuf == 0 || nbuf > nmax || nbuf > SWEET_V2_IB_HW || rq->reserved ||
		    inSize != sizeof(NativeAdrenoAbiSubmit) + extra * sizeof(NativeAdrenoAbiSubmitBuffer)) {
			return kIOReturnBadArgument;
		}
		V2_OUT(NativeAdrenoAbiSubmitReply);
		uint64_t iova[SWEET_V2_IB_HW];
		uint32_t dw[SWEET_V2_IB_HW];
		for (uint32_t i = 0; i < nbuf; i++) {
			const NativeAdrenoAbiSubmitBuffer *b = &rq->buffers[0] + i;    /* 1038: past [15] = the appended ones */
			if ((b->bytes & 3) || b->bytes == 0 || b->bytes / 4 > 0xfffff || b->iova < fWinBase ||
			    b->iova + b->bytes > fWinBase + fWinSize) {
				return kIOReturnBadArgument;
			}
			iova[i] = b->iova;
			dw[i] = (uint32_t)(b->bytes / 4);
		}
		if (rq->wait_fence != 0 && sweet_gpu_wait_fence_ns((uint32_t)rq->wait_fence, rq->wait_timeout_ns)) {
			v2_err_log("submit", "wait_fence timeout");
			return kIOReturnTimeout;
		}
		bool busy = false;
		uint32_t fence = sweet_gpu_submit_v2(iova, dw, nbuf, &busy);
		if (fence == 0) {
			v2_err_log("submit", busy ? "busy (ring full)" : "not ready (GPU down)");
			return busy ? kIOReturnBusy : kIOReturnNotReady;
		}
		fLastFence = fence;
		rp->fence = fence;
		if (fSystem && fName[0] == 'b') {
			sweet_swaptrace('S', fence);    /* 0987: SweetFramebuffer.cpp (p swapdone trace) */
		}
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodWaitFence: {
		V2_IN(NativeAdrenoAbiWaitFence);
		if (sweet_gpu_wait_fence_ns((uint32_t)rq->fence, rq->timeout_ns)) {
			if (rq->timeout_ns >= 1000000000ULL) {	/* Turnip waits in 100 ms slices: only long waits are news */
				v2_err_log("wait fence", "timeout");
			}
			return kIOReturnTimeout;
		}
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodGetTimestamp: {
		V2_OUT(NativeAdrenoAbiTimestamp);
		rp->ticks = sweet_gpu_timestamp();
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodGetStatus: {
		V2_OUT(NativeAdrenoAbiStatus);
		if (fSystem ? sweet_gpu_lost_spare(&fEpoch, fLastFence, fPid, &fSpared) : sweet_gpu_lost(&fEpoch, fLastFence)) {
			fFaults++;
			v2_err_log("status", "work lost in a GPU reset: device lost for this client");
		}
		rp->faults = fFaults;
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodSetMetadata:
		return kIOReturnSuccess;        /* only for dma-buf exports, which Darwin has not */
	case NativeAdrenoMethodGetMetadata: {
		V2_OUT(NativeAdrenoAbiMetadata);
		return kIOReturnSuccess;        /* bytes 0 */
	}
	case NativeAdrenoMethodCreateQueue: {
		V2_OUT(NativeAdrenoAbiQueueReply);
		rp->queue = 1;                  /* one ring: every queue is queue 0 */
		return kIOReturnSuccess;
	}
	case NativeAdrenoMethodCloseQueue:
		return kIOReturnSuccess;
	default:
		return kIOReturnUnsupported;
	}
#undef V2_IN
#undef V2_OUT
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
