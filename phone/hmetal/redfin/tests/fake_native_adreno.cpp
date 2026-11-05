// The fake NativeAdreno service and the IOKit / IOSurface shim functions: see fake_native_adreno.hpp.
#include "fake_native_adreno.hpp"

#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "NativeAdrenoABI.h"
#include "native_adreno_transport.hpp"

using namespace hakios::metal;

struct FakeIOSurface {
    fakena::Surface s;
    int refs = 0;
};

namespace fakena {
namespace {

constexpr uint32_t kTunnel = 0x4e544e4cu;
constexpr uint64_t kWinBase = 0x1000000000ull, kWinSize = 0x100000000ull;   // xnu 0115: 4 GB windows from 64 GB up
constexpr io_service_t kService = 0x51;

struct Bo {
    uint32_t fakeHandle;      // the FakeGpu buffer: the GPU's memory
    uint64_t bytes;
    uint64_t iova = 0;
    bool imported = false;
    uint8_t* importAddress = nullptr;   // the CPU's pages
};

struct Service {
    FakeGpu* gpu = nullptr;
    bool open = false;
    std::map<uint64_t, Bo> bos;   // our handles (the kernel's slot + 1)
    uint64_t nextHandle = 1;
    Counts counts;
};

std::recursive_mutex gLock;
// one service per installed FakeGpu (tests make several rigs at once); the newest is what IOServiceOpen opens, and a
// connection is the service's index + 0x4147 << 8
std::vector<std::unique_ptr<Service>> gServices;
Service* gOpening = nullptr;
Service* gLast = nullptr;   // counts(), gpuViewOfImport: the newest
Service* serviceOf(io_connect_t c) {
    const uint32_t i = c - (0x4147u << 8);
    return i < gServices.size() && gServices[i] && gServices[i]->open ? gServices[i].get() : nullptr;
}
std::map<uint32_t, std::unique_ptr<FakeIOSurface>> gSurfaces;

IOReturn kr(int e) {
    switch (e) {
    case 0: return kIOReturnSuccess;
    case ENOMEM: return kIOReturnNoMemory;
    case ETIMEDOUT: return kIOReturnTimeout;
    case ENOENT: return kIOReturnNotFound;
    case EBUSY: return kIOReturnBusy;
    default: return kIOReturnBadArgument;
    }
}

Bo* importAt(const void* address, size_t bytes, Service** in = nullptr) {
    for (auto& sv : gServices) {
        if (!sv) continue;
        for (auto& [h, b] : sv->bos) {
            if (b.imported && b.importAddress <= address && static_cast<const uint8_t*>(address) + bytes <= b.importAddress + b.bytes) {
                if (in) *in = sv.get();
                return &b;
            }
        }
    }
    return nullptr;
}

// the transport's dc civac: before a submission a clean (CPU -> memory, the GPU's copy), after a fence an invalidate
// (memory -> CPU)
void cacheHook(const void* address, size_t bytes, bool before) {
    std::lock_guard<std::recursive_mutex> g(gLock);
    Service* sv = nullptr;
    Bo* b = importAt(address, bytes, &sv);
    if (b == nullptr) return;
    auto* gpuView = static_cast<uint8_t*>(sv->gpu->map(b->fakeHandle, b->bytes));
    const size_t at = static_cast<const uint8_t*>(address) - b->importAddress;
    if (before) {
        memcpy(gpuView + at, address, bytes);
        sv->counts.cleans++;
    } else {
        memcpy(const_cast<void*>(address), gpuView + at, bytes);
        sv->counts.invalidates++;
    }
}

#define IN(T)                                                     \
    if (in == nullptr || inSize != sizeof(T)) return kIOReturnBadArgument; \
    const T* rq = static_cast<const T*>(in)
#define OUT(T)                                                         \
    if (out == nullptr || *outSize < sizeof(T)) return kIOReturnBadArgument; \
    T* rp = static_cast<T*>(out);                                     \
    memset(rp, 0, sizeof(T));                                         \
    *outSize = sizeof(T)

IOReturn method(Service& s, uint32_t m, const void* in, size_t inSize, void* out, size_t* outSize) {
    FakeGpu& gpu = *s.gpu;
    switch (m) {
    case NativeAdrenoMethodQuery: {
        OUT(NativeAdrenoAbiParams);
        rp->abi_version = NativeAdrenoAbiVersion;
        rp->input_bits = 39;
        rp->output_bits = 36;
        rp->flags = NativeAdrenoParamsHostImport;
        rp->max_bo_bytes = 1ull << NativeAdrenoAbiMaxBoBytesShift;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodGetDeviceInfo: {
        OUT(NativeAdrenoAbiDeviceInfo);
        // the chip the FakeGpu is; the rest as xnu 0904 answers for redfin, the UBWC words as the FakeGpu's params
        MsmParam p{};
        rp->chip_id = gpu.chip().chipId;
        rp->gmem_bytes = gpu.chip().gmemBytes;
        rp->gmem_base = 0x100000;
        rp->uche_trap_base = 0x1fffffffff000ull;
        rp->va_start = kWinBase;
        rp->va_size = kWinSize;
        p.param = kParamHighestBankBit;
        gpu.call(kGetParam, &p, sizeof(p));
        rp->highest_bank_bit = uint32_t(p.value);
        p.param = kParamUbwcSwizzle;
        gpu.call(kGetParam, &p, sizeof(p));
        rp->ubwc_swizzle = uint32_t(p.value);
        p.param = kParamMacrotileMode;
        gpu.call(kGetParam, &p, sizeof(p));
        rp->macrotile_mode = uint32_t(p.value);
        rp->ubwc_version = 3;
        rp->flags = NativeAdrenoDeviceClientIova;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoAlloc: {
        IN(NativeAdrenoAbiBoAlloc);
        OUT(NativeAdrenoAbiBoAllocReply);
        if (rq->bytes == 0 || (rq->bytes & 4095) || rq->bytes > (1ull << NativeAdrenoAbiMaxBoBytesShift) || rq->align_log2 < 12 ||
            rq->align_log2 > 24 || rq->contiguous > 1) {
            return kIOReturnBadArgument;
        }
        MsmGemNew n{rq->bytes, 0, 0};
        if (int e = gpu.call(kGemNew, &n, sizeof(n))) return kr(e);
        const uint64_t h = s.nextHandle++;
        s.bos[h] = Bo{n.handle, rq->bytes};
        s.counts.allocs++;
        rp->handle = h;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoImport: {
        IN(NativeAdrenoAbiBoImport);
        OUT(NativeAdrenoAbiBoAllocReply);
        if (rq->address == 0 || ((rq->address | rq->bytes) & 4095) || rq->bytes == 0 || rq->bytes > (1ull << NativeAdrenoAbiMaxBoBytesShift)) {
            return kIOReturnBadArgument;
        }
        // the GPU's copy starts as the pages are in memory now (the import pins them; nothing cleaned yet)
        MsmGemNew n{rq->bytes, 0, 0};
        if (int e = gpu.call(kGemNew, &n, sizeof(n))) return kr(e);
        const uint64_t h = s.nextHandle++;
        Bo b{n.handle, rq->bytes};
        b.imported = true;
        b.importAddress = reinterpret_cast<uint8_t*>(uintptr_t(rq->address));
        s.bos[h] = b;
        s.counts.imports++;
        rp->handle = h;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoMap: {
        IN(NativeAdrenoAbiBoMap);
        OUT(NativeAdrenoAbiBoMapReply);
        if (!(rq->permissions & 1) || (rq->permissions & ~3u) || (rq->iova & 4095) || rq->reserved || rq->writeback > 1 ||
            (rq->shareability != 2 && rq->shareability != 3)) {
            return kIOReturnBadArgument;
        }
        auto b = s.bos.find(rq->handle);
        if (b == s.bos.end()) return kIOReturnBadArgument;
        if (b->second.iova != 0) return kIOReturnBusy;
        if (rq->iova < kWinBase || rq->iova + b->second.bytes > kWinBase + kWinSize) return kIOReturnBadArgument;
        if (!gpu.placeAt(b->second.fakeHandle, rq->iova)) return kIOReturnNoResources;   // overlaps a live mapping
        b->second.iova = rq->iova;
        rp->iova = rq->iova;
        s.counts.maps++;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoFree: {
        IN(NativeAdrenoAbiBoFree);
        auto b = s.bos.find(rq->handle);
        if (b == s.bos.end()) return kIOReturnBadArgument;
        DrmGemClose c{b->second.fakeHandle, 0};
        gpu.call(kGemClose, &c, sizeof(c));
        s.bos.erase(b);
        s.counts.frees++;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoCpuMap: {
        IN(NativeAdrenoAbiBoCpuMap);
        OUT(NativeAdrenoAbiBoCpuMapReply);
        auto b = s.bos.find(rq->handle);
        if (b == s.bos.end() || b->second.imported) return kIOReturnBadArgument;   // an import has no token (the ABI)
        rp->token = uint32_t(rq->handle);   // slot + 1, as the kernel's
        rp->bytes = b->second.bytes;
        s.counts.cpuMaps++;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodBoCpuUnmap: return kIOReturnSuccess;
    case NativeAdrenoMethodSubmit: {
        IN(NativeAdrenoAbiSubmit);
        OUT(NativeAdrenoAbiSubmitReply);
        if (rq->buffer_count == 0 || rq->buffer_count > NativeAdrenoAbiMaxBuffers || rq->reserved) return kIOReturnBadArgument;
        if (rq->buffer_count != 1) return kIOReturnUnsupported;   // (this fake: one IB, as the transport sends)
        const NativeAdrenoAbiSubmitBuffer& ib = rq->buffers[0];
        if ((ib.bytes & 3) || ib.bytes == 0 || ib.iova < kWinBase || ib.iova + ib.bytes > kWinBase + kWinSize) return kIOReturnBadArgument;
        // the kernel runs the IB at its address; the fake runs a buffer at an offset: find the buffer holding it
        const Bo* holder = nullptr;
        for (const auto& [h, b] : s.bos) {
            if (b.iova != 0 && ib.iova >= b.iova && ib.iova + ib.bytes <= b.iova + b.bytes) holder = &b;
        }
        if (holder == nullptr) return kIOReturnBadArgument;
        MsmSubmitBo bo{kSubmitBoRead, holder->fakeHandle, holder->iova};
        MsmSubmitCmd cmd{};
        cmd.type = kSubmitCmdBuf;
        cmd.submit_offset = uint32_t(ib.iova - holder->iova);
        cmd.size = uint32_t(ib.bytes);
        MsmGemSubmit sub{};
        sub.flags = kPipe3D;
        sub.nr_bos = 1;
        sub.nr_cmds = 1;
        sub.bos = uint64_t(uintptr_t(&bo));
        sub.cmds = uint64_t(uintptr_t(&cmd));
        sub.fence_fd = -1;
        if (int e = gpu.call(kGemSubmit, &sub, sizeof(sub))) return kr(e);
        s.counts.submits++;
        s.counts.lastSubmitIova = ib.iova;
        s.counts.lastSubmitBytes = ib.bytes;
        rp->fence = sub.fence;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodWaitFence: {
        IN(NativeAdrenoAbiWaitFence);
        const int64_t at = gpu.nowNs() + int64_t(rq->timeout_ns);
        MsmWaitFence w{};
        w.fence = uint32_t(rq->fence);
        w.timeout = MsmTimespec{at / 1000000000, at % 1000000000};
        s.counts.waits++;
        // (the fake's wait blocks on its own lock: ours is let go meanwhile, as the kernel's ioctl does not hold the
        // client's)
        gLock.unlock();
        const int e = gpu.call(kWaitFence, &w, sizeof(w));
        gLock.lock();
        return kr(e);
    }
    case NativeAdrenoMethodGetTimestamp: {
        OUT(NativeAdrenoAbiTimestamp);
        rp->ticks = uint64_t(gpu.nowNs() / 52);   // ~19.2 MHz
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodGetStatus: {
        OUT(NativeAdrenoAbiStatus);
        MsmParam p{};
        p.param = kParamFaults;
        gpu.call(kGetParam, &p, sizeof(p));
        rp->faults = p.value;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodCreateQueue: {
        OUT(NativeAdrenoAbiQueueReply);
        rp->queue = 1;   // one ring, as the kernel
        s.counts.queuesMade++;
        return kIOReturnSuccess;
    }
    case NativeAdrenoMethodCloseQueue: return kIOReturnSuccess;
    default: s.counts.unsupported++; return kIOReturnUnsupported;
    }
}

}  // namespace

void install(FakeGpu* fake) {
    std::lock_guard<std::recursive_mutex> g(gLock);
    auto sv = std::make_unique<Service>();
    sv->gpu = fake;
    gOpening = gLast = sv.get();
    gServices.push_back(std::move(sv));
    nativeadreno::gCacheHook = cacheHook;
}

void uninstall(FakeGpu* fake) {
    std::lock_guard<std::recursive_mutex> g(gLock);
    for (auto& sv : gServices) {
        if (sv && (fake == nullptr || sv->gpu == fake)) {
            if (gOpening == sv.get()) gOpening = nullptr;
            if (gLast == sv.get()) gLast = nullptr;
            sv.reset();
        }
    }
}

Counts counts() {
    std::lock_guard<std::recursive_mutex> g(gLock);
    return gLast != nullptr ? gLast->counts : Counts{};
}
uint64_t windowStart() { return kWinBase; }
uint64_t windowSize() { return kWinSize; }

uint8_t* addSurface(uint32_t id, uint32_t width, uint32_t height, uint32_t rowBytes, uint32_t pixelFormat) {
    std::lock_guard<std::recursive_mutex> g(gLock);
    auto f = std::make_unique<FakeIOSurface>();
    const size_t bytes = (size_t(rowBytes) * height + 16383) & ~size_t(16383);
    auto* storage = new std::vector<uint8_t>(bytes + 16384);
    uint8_t* base = reinterpret_cast<uint8_t*>((uintptr_t(storage->data()) + 16383) & ~uintptr_t(16383));
    f->s = Surface{id, width, height, rowBytes, pixelFormat, storage, base, bytes};
    uint8_t* b = f->s.base;
    gSurfaces[id] = std::move(f);
    return b;
}

void removeSurfaces() {
    std::lock_guard<std::recursive_mutex> g(gLock);
    for (auto& [id, s] : gSurfaces) delete s->s.storage;
    gSurfaces.clear();
}

int surfaceRefs(uint32_t id) {
    std::lock_guard<std::recursive_mutex> g(gLock);
    auto it = gSurfaces.find(id);
    return it == gSurfaces.end() ? -1 : it->second->refs;
}

std::vector<uint8_t> gpuViewOfImport(const void* address, size_t bytes) {
    std::lock_guard<std::recursive_mutex> g(gLock);
    Bo* b = importAt(address, bytes);
    if (b == nullptr) return {};
    Service* sv = nullptr;
    importAt(address, bytes, &sv);
    const auto* v = static_cast<const uint8_t*>(sv->gpu->map(b->fakeHandle, b->bytes));
    const size_t at = static_cast<const uint8_t*>(address) - b->importAddress;
    return std::vector<uint8_t>(v + at, v + at + bytes);
}

}  // namespace fakena

// --- the shim's functions --------------------------------------------------------------------------------------------

CFMutableDictionaryRef IOServiceMatching(const char* name) { return const_cast<char*>(name); }

io_service_t IOServiceGetMatchingService(mach_port_t, CFMutableDictionaryRef matching) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    return fakena::gOpening != nullptr && strcmp(static_cast<const char*>(matching), "IOAcceleratorES") == 0 ? fakena::kService : 0;
}

kern_return_t IOServiceOpen(io_service_t service, task_port_t, uint32_t type, io_connect_t* connect) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    if (service != fakena::kService || (type != 0x4147 && type != 2) || fakena::gOpening == nullptr) return kIOReturnBadArgument;
    for (size_t i = 0; i < fakena::gServices.size(); i++) {
        if (fakena::gServices[i].get() == fakena::gOpening) {
            fakena::gOpening->open = true;
            *connect = io_connect_t((0x4147u << 8) + i);
            fakena::gOpening = nullptr;   // one connection per installed fake
            return KERN_SUCCESS;
        }
    }
    return kIOReturnBadArgument;
}

kern_return_t IOServiceClose(io_connect_t connect) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    fakena::Service* sv = fakena::serviceOf(connect);
    if (sv == nullptr) return kIOReturnBadArgument;
    sv->open = false;
    return KERN_SUCCESS;
}

kern_return_t IOObjectRelease(io_object_t) { return KERN_SUCCESS; }

kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void* input, size_t inputSize, void* output,
                                        size_t* outputSize) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    fakena::Service* sv = fakena::serviceOf(connection);
    if (sv == nullptr) return kIOReturnBadArgument;
    const auto* w = static_cast<const uint32_t*>(input);
    size_t none = 0;
    size_t* osz = outputSize != nullptr ? outputSize : &none;
    if (inputSize >= 8 && w[0] == fakena::kTunnel) {
        return fakena::method(*sv, w[1], inputSize > 8 ? w + 2 : nullptr, inputSize - 8, output, osz);
    }
    return fakena::method(*sv, selector, inputSize ? input : nullptr, inputSize, output, osz);
}

kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t type, task_port_t, mach_vm_address_t* atAddress, mach_vm_size_t* ofSize,
                                   uint32_t options) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    fakena::Service* sv = fakena::serviceOf(connect);
    if (sv == nullptr || !(options & kIOMapAnywhere)) return kIOReturnBadArgument;
    auto b = sv->bos.find(type);
    if (b == sv->bos.end() || b->second.imported) return kIOReturnBadArgument;
    void* p = sv->gpu->map(b->second.fakeHandle, b->second.bytes);
    if (p == nullptr) return kIOReturnNoMemory;
    *atAddress = mach_vm_address_t(uintptr_t(p));
    *ofSize = b->second.bytes;
    return KERN_SUCCESS;
}

kern_return_t IOConnectUnmapMemory64(io_connect_t, uint32_t, task_port_t, mach_vm_address_t) { return KERN_SUCCESS; }

IOSurfaceRef IOSurfaceLookup(IOSurfaceID id) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    auto it = fakena::gSurfaces.find(id);
    if (it == fakena::gSurfaces.end()) return nullptr;
    it->second->refs++;
    return it->second.get();
}
void CFRelease(CFTypeRef ref) {
    std::lock_guard<std::recursive_mutex> g(fakena::gLock);
    auto* s = static_cast<FakeIOSurface*>(const_cast<void*>(ref));
    s->refs--;
}
IOSurfaceID IOSurfaceGetID(IOSurfaceRef s) { return s->s.id; }
void* IOSurfaceGetBaseAddress(IOSurfaceRef s) { return s->s.base; }
size_t IOSurfaceGetAllocSize(IOSurfaceRef s) { return s->s.allocSize; }
size_t IOSurfaceGetWidth(IOSurfaceRef s) { return s->s.width; }
size_t IOSurfaceGetHeight(IOSurfaceRef s) { return s->s.height; }
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s) { return s->s.rowBytes; }
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s) { return s->s.pixelFormat; }
size_t IOSurfaceGetPlaneCount(IOSurfaceRef) { return 0; }   // IOSurface's answer for a surface without planes
void* IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s, size_t) { return s->s.base; }
size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s, size_t) { return s->s.rowBytes; }

// --- upstream's tests through the transport ---------------------------------------------------------------------------

namespace {
// the transport, and the FakeGpu it reaches through the fake service, kept together: the fake outlives the transport
class Owning : public Transport {
public:
    Owning(std::unique_ptr<FakeGpu> fake, std::unique_ptr<Transport> t) : fake_(std::move(fake)), t_(std::move(t)) {}
    ~Owning() override {
        t_.reset();
        fakena::uninstall(fake_.get());
    }
    int call(uint32_t r, void* d, size_t s) override { return t_->call(r, d, s); }
    void* map(uint32_t h, uint64_t s) override { return t_->map(h, s); }
    void unmap(uint32_t h, void* a) override { t_->unmap(h, a); }
    int64_t nowNs() override { return t_->nowNs(); }

private:
    std::unique_ptr<FakeGpu> fake_;
    std::unique_ptr<Transport> t_;
};
}  // namespace

std::unique_ptr<Transport> hmetalViaNativeAdreno(std::unique_ptr<FakeGpu> fake) {
    fakena::install(fake.get());
    std::string why;
    std::unique_ptr<Transport> t = openNativeAdrenoGpu(&why);
    if (t == nullptr) {
        fprintf(stderr, "hmetalViaNativeAdreno: %s\n", why.c_str());
        abort();
    }
    return std::unique_ptr<Transport>(new Owning(std::move(fake), std::move(t)));
}
