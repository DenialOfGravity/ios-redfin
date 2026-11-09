// hakios-metal's Transport over our kernel's NativeAdreno ABI v1: see native_adreno_transport.hpp.
//
// Request map (msm request -> NativeAdreno method):
//   GET_PARAM        CHIP_ID (DeviceInfo.chip_id, no speed bin above it: ABI v1 has none), GMEM_SIZE/GMEM_BASE,
//                    HIGHEST_BANK_BIT, UBWC_SWIZZLE, MACROTILE_MODE (DeviceInfo), MAX_FREQ (redfin's top level),
//                    TIMESTAMP (GetTimestamp), FAULTS/SUSPENDS (GetStatus), GPU_ID 0; others EINVAL as hakios's kernel
//   GEM_NEW          BoAlloc (4 KiB multiple, <= max_bo_bytes) + BoMap at an address of our own VA allocator
//   GEM_INFO         IOVA: the address we mapped; OFFSET: 0
//   GEM_CLOSE        BoFree (CPU mapping undone first if the core left one) + the VA range back
//   GEM_SUBMIT       Submit of the one command IB at bo.iova + submit_offset; the BO list is not sent (our kernel maps
//                    every BO of this client always) but each BO named records the fence, for CPU_PREP; imported BOs
//                    are cleaned before, and the written ones invalidated after their fence
//   WAIT_FENCE       WaitFence(the deadline less now), ETIMEDOUT on timeout
//   GEM_CPU_PREP     WaitFence on the BO's last fence; GEM_CPU_FINI nothing
//   SUBMITQUEUE_NEW  CreateQueue; SUBMITQUEUE_CLOSE CloseQueue (the kernel has one ring: ids are kept for the core)
//   0x7e surface     IOSurfaceLookup, BoImport of its whole pages, BoMap; the HakiosSurface reply from IOSurface's own
//                    getters; the same handle again while this client holds the surface's buffer (as hakios's kernel)
//   0x7d userptr     BoImport of the caller's pages + BoMap
//   anything else    ENOSYS, one log line per request number
#include "native_adreno_transport.hpp"

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <IOSurface/IOSurfaceRef.h>   // (on Linux: tests/shim's)

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "NativeAdrenoABI.h"

namespace hakios::metal {

namespace nativeadreno {
void (*gCacheHook)(const void*, size_t, bool) = nullptr;
}  // namespace nativeadreno

namespace {

constexpr uint32_t kTunnel = 0x4e544e4cu;     // 'NTNL' (xnu/patches/0115 SWEET_V2_TUNNEL)
constexpr uint32_t kOpenType = 0x4147;        // SWEET_V2_OPEN_TYPE, as Turnip's darwin backend and v2test open it
constexpr uint64_t kPage = 4096;              // the ABI's granule (the GPU's page); Darwin's 16 KiB pages are multiples

// clean and invalidate [p, p + n) to the point of coherency, as phone/metal2/SweetMetal2.mm's sys_dcache_flush
void dcacheFlush(const void* p, size_t n, bool before) {
    if (nativeadreno::gCacheHook != nullptr) nativeadreno::gCacheHook(p, n, before);
#if defined(__aarch64__) || defined(__arm64__)
    uintptr_t a = uintptr_t(p) & ~uintptr_t(63), end = uintptr_t(p) + n;
    for (; a < end; a += 64) __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

int errnoOf(kern_return_t r, uint32_t method) {
    switch (r) {
    case KERN_SUCCESS: return 0;
    case kIOReturnNoMemory: case kIOReturnNoResources: return ENOMEM;
    case kIOReturnBadArgument: return EINVAL;
    case kIOReturnTimeout: return ETIMEDOUT;
    case kIOReturnBusy: return EBUSY;
    case kIOReturnNotFound: return ENOENT;
    case kIOReturnUnsupported: return ENOSYS;
    case kIOReturnNotReady: return EAGAIN;
    default:
        fprintf(stderr, "hakios-metal: NativeAdreno method %u answered %#x\n", method, unsigned(r));
        return EIO;
    }
}

class NativeAdreno : public Transport {
public:
    explicit NativeAdreno(io_connect_t c) : c_(c) { mach_timebase_info(&timebase_); }
    ~NativeAdreno() override {
        // what the core left open goes with the connection in the kernel too; free it here so a test's fake sees it
        std::vector<uint32_t> left;
        for (const auto& [h, b] : bos_) left.push_back(h);
        for (uint32_t h : left) closeBo(h);
        IOServiceClose(c_);
    }

    bool start(std::string* why) {
        NativeAdrenoAbiParams q{};
        if (kern_return_t r = method(NativeAdrenoMethodQuery, nullptr, 0, &q, sizeof(q))) {
            *why = "NativeAdreno Query failed: " + std::to_string(r);
            return false;
        }
        if (q.abi_version != NativeAdrenoAbiVersion) {
            *why = "NativeAdreno ABI version " + std::to_string(q.abi_version) + ", this transport speaks " + std::to_string(NativeAdrenoAbiVersion);
            return false;
        }
        params_ = q;
        if (kern_return_t r = method(NativeAdrenoMethodGetDeviceInfo, nullptr, 0, &info_, sizeof(info_))) {
            *why = "NativeAdreno GetDeviceInfo failed: " + std::to_string(r);
            return false;
        }
        if (!(info_.flags & NativeAdrenoDeviceClientIova) || info_.va_size == 0) {
            *why = "NativeAdreno: the kernel does not let the client choose GPU addresses (flags " + std::to_string(info_.flags) + ")";
            return false;
        }
        free_[info_.va_start] = info_.va_size;
        // (redfin) what the kernel says it programmed, once per open: UBWC is the kernel's (xnu 0904 writes
        // RB/TPL1/SP_NC_MODE_CNTL and UCHE_MODE_CNTL), Gpu::ubwc() takes these words over the chip table's
        fprintf(stderr, "hakios-metal: NativeAdreno chip %#x, GMEM %u KiB at %#llx, UBWC %u: highest bank bit %u, swizzle levels %#x, "
                        "macrotile %u, flags %#x\n", info_.chip_id, info_.gmem_bytes / 1024, (unsigned long long)info_.gmem_base,
                info_.ubwc_version, info_.highest_bank_bit, info_.ubwc_swizzle, info_.macrotile_mode, info_.flags);
        return true;
    }

    int call(uint32_t request, void* data, size_t size) override {
        std::unique_lock<std::mutex> g(lock_);
        switch (request) {
        case kGetParam: return size == sizeof(MsmParam) ? getParam(*static_cast<MsmParam*>(data)) : EINVAL;
        case kGemNew: return size == sizeof(MsmGemNew) ? gemNew(*static_cast<MsmGemNew*>(data)) : EINVAL;
        case kGemInfo: {
            if (size != sizeof(MsmGemInfo)) return EINVAL;
            auto* i = static_cast<MsmGemInfo*>(data);
            auto b = bos_.find(i->handle);
            if (b == bos_.end()) return ENOENT;
            if (i->info == kInfoGetIova) { i->value = b->second.iova; return 0; }
            if (i->info == kInfoGetOffset) { i->value = 0; return 0; }
            return EINVAL;
        }
        case kGemClose: return size == sizeof(DrmGemClose) ? closeBo(static_cast<DrmGemClose*>(data)->handle) : EINVAL;
        case kGemCpuPrep: {
            if (size != sizeof(MsmGemCpuPrep)) return EINVAL;
            auto* p = static_cast<MsmGemCpuPrep*>(data);
            auto b = bos_.find(p->handle);
            if (b == bos_.end()) return ENOENT;
            return b->second.lastFence == 0 ? 0 : waitFence(g, b->second.lastFence, p->timeout);
        }
        case kGemCpuFini: return size == sizeof(MsmGemCpuFini) ? 0 : EINVAL;
        case kGemSubmit: return size == sizeof(MsmGemSubmit) ? submit(*static_cast<MsmGemSubmit*>(data)) : EINVAL;
        case kWaitFence: {
            if (size != sizeof(MsmWaitFence)) return EINVAL;
            auto* w = static_cast<MsmWaitFence*>(data);
            if (w->queueid != 0 && queues_.count(w->queueid) == 0) return EINVAL;
            return waitFence(g, w->fence, w->timeout);
        }
        case kSubmitQueueNew: {
            if (size != sizeof(MsmSubmitQueue)) return EINVAL;
            NativeAdrenoAbiQueue q{0};
            NativeAdrenoAbiQueueReply r{};
            if (int e = errnoOf(method(NativeAdrenoMethodCreateQueue, &q, sizeof(q), &r, sizeof(r)), NativeAdrenoMethodCreateQueue)) return e;
            // the kernel has one ring and answers 1 for every queue: the ids the core sees are ours, each mapped to it
            const uint32_t id = nextQueue_++;
            queues_[id] = r.queue;
            static_cast<MsmSubmitQueue*>(data)->id = id;
            return 0;
        }
        case kSubmitQueueClose: {
            if (size != sizeof(uint32_t)) return EINVAL;
            auto q = queues_.find(*static_cast<uint32_t*>(data));
            if (q == queues_.end()) return EINVAL;
            NativeAdrenoAbiQueue c{q->second};
            queues_.erase(q);
            return errnoOf(method(NativeAdrenoMethodCloseQueue, &c, sizeof(c), nullptr, 0), NativeAdrenoMethodCloseQueue);
        }
        case kHakiosSurface: return size == sizeof(HakiosSurface) ? surface(*static_cast<HakiosSurface*>(data)) : EINVAL;
        case nativeadreno::kUserMemory: {
            if (size != sizeof(nativeadreno::UserMemory)) return EINVAL;
            auto* u = static_cast<nativeadreno::UserMemory*>(data);
            return import(u->address, u->size, nullptr, &u->handle);
        }
        default:
            if (unserved_.insert(request).second) {
                fprintf(stderr, "hakios-metal: msm request %#x has no NativeAdreno method: ENOSYS\n", request);
            }
            return ENOSYS;
        }
    }

    void* map(uint32_t handle, uint64_t) override {
        std::lock_guard<std::mutex> g(lock_);
        auto b = bos_.find(handle);
        if (b == bos_.end()) return nullptr;
        Bo& bo = b->second;
        if (bo.imported) return bo.importBase;   // the caller's own pages: its mapping is the one
        if (bo.cpu != nullptr) return bo.cpu;
        NativeAdrenoAbiBoCpuMap m{bo.handle};
        NativeAdrenoAbiBoCpuMapReply r{};
        if (method(NativeAdrenoMethodBoCpuMap, &m, sizeof(m), &r, sizeof(r)) != KERN_SUCCESS) return nullptr;
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(c_, r.token, mach_task_self(), &address, &size, kIOMapAnywhere | kIOMapWriteCombineCache) != KERN_SUCCESS) {
            NativeAdrenoAbiBoCpuUnmap u{bo.handle, r.token, 0};
            method(NativeAdrenoMethodBoCpuUnmap, &u, sizeof(u), nullptr, 0);
            return nullptr;
        }
        bo.token = r.token;
        bo.cpu = reinterpret_cast<void*>(uintptr_t(address));
        return bo.cpu;
    }

    void unmap(uint32_t handle, void* address) override {
        std::lock_guard<std::mutex> g(lock_);
        auto b = bos_.find(handle);
        if (b == bos_.end() || b->second.imported || b->second.cpu == nullptr || b->second.cpu != address) return;
        unmapLocked(b->second);
    }

    int64_t nowNs() override {
        // hakios's deadlines are of mach_continuous_time, as upstream's iokit_transport.cpp
        return int64_t(mach_continuous_time() * timebase_.numer / timebase_.denom);
    }

private:
    struct Bo {
        uint64_t handle = 0;     // the kernel's (NativeAdreno), nonzero
        uint64_t bytes = 0;      // as allocated / imported (4 KiB multiple)
        uint64_t iova = 0;
        uint64_t vaBytes = 0;    // the VA range taken for it
        void* cpu = nullptr;     // BoCpuMap'd (driver-made only)
        uint32_t token = 0;
        bool imported = false;
        void* importBase = nullptr;   // imported: the caller's address (cleaned/invalidated around the GPU's use)
        uint32_t surfaceId = 0;  // an IOSurface's buffer: the surface's ID (kept retained while the buffer lives)
        IOSurfaceRef surface = nullptr;
        uint32_t lastFence = 0;  // the fence of the last submission that named it
        uint32_t writeFence = 0; // ...that named it as written (imported: invalidate once this passed)
    };

    kern_return_t method(uint32_t m, const void* in, size_t inSize, void* out, size_t outSize) {
        // {'NTNL', method, payload} through selector 0, as v2test and the darwin backend call it
        uint8_t buf[8 + sizeof(NativeAdrenoAbiSubmit)];
        const uint32_t hdr[2] = {kTunnel, m};
        memcpy(buf, hdr, 8);
        if (inSize > sizeof(NativeAdrenoAbiSubmit)) return kIOReturnBadArgument;
        if (inSize != 0) memcpy(buf + 8, in, inSize);
        size_t o = outSize;
        return IOConnectCallStructMethod(c_, 0, buf, 8 + inSize, outSize ? out : nullptr, outSize ? &o : nullptr);
    }

    // --- the GPU addresses: first fit in [va_start, va_start + va_size), 4 KiB granules (a BO aligned to its own
    // size class would be kinder to the GPU's TLB; the kernel maps 4 KiB pages whatever we choose)
    uint64_t vaTake(uint64_t bytes) {
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second >= bytes) {
                const uint64_t at = it->first, left = it->second - bytes;
                free_.erase(it);
                if (left != 0) free_[at + bytes] = left;
                return at;
            }
        }
        return 0;
    }
    void vaGive(uint64_t at, uint64_t bytes) {
        auto next = free_.lower_bound(at);
        if (next != free_.end() && at + bytes == next->first) {
            bytes += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) {
                prev->second += bytes;
                return;
            }
        }
        free_[at] = bytes;
    }

    // BoMap at our own address; on failure the BO is freed and the address given back
    int mapAt(uint64_t kernelHandle, uint64_t bytes, uint32_t* handle, Bo bo) {
        const uint64_t va = vaTake(bytes);
        if (va == 0) {
            NativeAdrenoAbiBoFree f{kernelHandle};
            method(NativeAdrenoMethodBoFree, &f, sizeof(f), nullptr, 0);
            return ENOMEM;
        }
        // read|write, normal non-coherent (writeback 0), outer shareable (2): v2test's and the darwin backend's values
        NativeAdrenoAbiBoMap m{kernelHandle, va, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        if (kern_return_t k = method(NativeAdrenoMethodBoMap, &m, sizeof(m), &r, sizeof(r))) {
            vaGive(va, bytes);
            NativeAdrenoAbiBoFree f{kernelHandle};
            method(NativeAdrenoMethodBoFree, &f, sizeof(f), nullptr, 0);
            return errnoOf(k, NativeAdrenoMethodBoMap);
        }
        bo.handle = kernelHandle;
        bo.bytes = bytes;
        bo.iova = r.iova != 0 ? r.iova : va;
        bo.vaBytes = bytes;
        const uint32_t h = nextHandle_++;
        bos_[h] = bo;
        *handle = h;
        return 0;
    }

    int gemNew(MsmGemNew& n) {
        const uint64_t bytes = (n.size + kPage - 1) & ~(kPage - 1);
        if (bytes == 0 || bytes > params_.max_bo_bytes) return EINVAL;
        NativeAdrenoAbiBoAlloc a{bytes, 12, 0};
        NativeAdrenoAbiBoAllocReply r{};
        if (kern_return_t k = method(NativeAdrenoMethodBoAlloc, &a, sizeof(a), &r, sizeof(r))) return errnoOf(k, NativeAdrenoMethodBoAlloc);
        return mapAt(r.handle, bytes, &n.handle, Bo{});
    }

    // the caller's own pages [address, address + bytes) as a BO; SURFACE when they are an IOSurface's
    int import(uint64_t address, uint64_t bytes, IOSurfaceRef surface, uint32_t* handle) {
        if (address == 0 || (address & (kPage - 1)) || bytes == 0 || (bytes & (kPage - 1)) || bytes > params_.max_bo_bytes) return EINVAL;
        if (!(params_.flags & NativeAdrenoParamsHostImport)) return ENOTSUP;
        NativeAdrenoAbiBoImport im{address, bytes};
        NativeAdrenoAbiBoAllocReply r{};
        if (kern_return_t k = method(NativeAdrenoMethodBoImport, &im, sizeof(im), &r, sizeof(r))) return errnoOf(k, NativeAdrenoMethodBoImport);
        Bo bo;
        bo.imported = true;
        bo.importBase = reinterpret_cast<void*>(uintptr_t(address));
        bo.surface = surface;
        bo.surfaceId = surface != nullptr ? IOSurfaceGetID(surface) : 0;
        return mapAt(r.handle, bytes, handle, bo);
    }

    int surface(HakiosSurface& s) {
        // the same handle again while this client holds the surface's buffer (hakios.gpu.cpp surfaceBuffer; the core
        // keeps one Bo per handle and disowns the second answer)
        IOSurfaceRef ref = IOSurfaceLookup(s.surface);
        if (ref == nullptr) return ENOENT;
        void* base = IOSurfaceGetBaseAddress(ref);
        const size_t alloc = IOSurfaceGetAllocSize(ref);
        const uint64_t start = uint64_t(uintptr_t(base)) & ~(kPage - 1);
        const uint64_t end = (uint64_t(uintptr_t(base)) + alloc + kPage - 1) & ~(kPage - 1);
        const size_t planes = IOSurfaceGetPlaneCount(ref);
        s.offset = uint32_t(uint64_t(uintptr_t(planes > 1 ? IOSurfaceGetBaseAddressOfPlane(ref, 0) : base)) - start);
        s.rowBytes = uint32_t(planes > 1 ? IOSurfaceGetBytesPerRowOfPlane(ref, 0) : IOSurfaceGetBytesPerRow(ref));
        s.width = uint32_t(IOSurfaceGetWidth(ref));
        s.height = uint32_t(IOSurfaceGetHeight(ref));
        s.pixelFormat = uint32_t(IOSurfaceGetPixelFormat(ref));
        s.planes = uint32_t(planes == 0 ? 1 : planes);
        s.size = end - start;
        if (s.handle == kHakiosDescribe) {
            CFRelease(ref);
            return 0;
        }
        for (const auto& [h, b] : bos_) {
            if (b.surfaceId == s.surface && b.surface != nullptr) {
                CFRelease(ref);
                s.handle = h;
                return 0;
            }
        }
        // (the lookup's reference is the buffer's: released with it)
        if (int e = import(start, end - start, ref, &s.handle)) {
            CFRelease(ref);
            return e;
        }
        return 0;
    }

    void unmapLocked(Bo& bo) {
        IOConnectUnmapMemory64(c_, bo.token, mach_task_self(), mach_vm_address_t(uintptr_t(bo.cpu)));
        NativeAdrenoAbiBoCpuUnmap u{bo.handle, bo.token, 0};
        method(NativeAdrenoMethodBoCpuUnmap, &u, sizeof(u), nullptr, 0);
        bo.cpu = nullptr;
        bo.token = 0;
    }

    int closeBo(uint32_t handle) {
        auto b = bos_.find(handle);
        if (b == bos_.end()) return ENOENT;
        Bo bo = b->second;
        bos_.erase(b);
        if (bo.cpu != nullptr) unmapLocked(bo);
        NativeAdrenoAbiBoFree f{bo.handle};
        const int e = errnoOf(method(NativeAdrenoMethodBoFree, &f, sizeof(f), nullptr, 0), NativeAdrenoMethodBoFree);
        // the kernel unmapped the range from the GPU with the BO; only now may another BO take the address
        vaGive(bo.iova, bo.vaBytes);
        if (bo.surface != nullptr) CFRelease(bo.surface);
        return e;
    }

    int submit(MsmGemSubmit& s) {
        if (s.nr_cmds != 1 || s.nr_bos == 0 || (s.queueid != 0 && queues_.count(s.queueid) == 0)) return EINVAL;
        const auto* bos = reinterpret_cast<const MsmSubmitBo*>(uintptr_t(s.bos));
        const auto* cmd = reinterpret_cast<const MsmSubmitCmd*>(uintptr_t(s.cmds));
        if (cmd->type != kSubmitCmdBuf || cmd->submit_idx >= s.nr_bos || cmd->size == 0 || cmd->size % 4 != 0) return EINVAL;
        std::vector<Bo*> named(s.nr_bos);
        for (uint32_t i = 0; i < s.nr_bos; i++) {
            auto b = bos_.find(bos[i].handle);
            if (b == bos_.end()) return ENOENT;
            named[i] = &b->second;
        }
        const Bo& ib = *named[cmd->submit_idx];
        if (uint64_t(cmd->submit_offset) + cmd->size > ib.bytes) return EINVAL;
        // imported pages the GPU reads (or overwrites in part): what the CPU wrote goes to memory first
        for (Bo* b : named) {
            if (b->imported) dcacheFlush(b->importBase, b->bytes, true);
        }
        NativeAdrenoAbiSubmit q{};
        q.buffer_count = 1;
        q.queue = 0;   // one ring (xnu 0115: every queue is queue 0)
        q.buffers[0].handle = ib.handle;
        q.buffers[0].iova = ib.iova + cmd->submit_offset;
        q.buffers[0].bytes = cmd->size;
        NativeAdrenoAbiSubmitReply r{};
        if (kern_return_t k = method(NativeAdrenoMethodSubmit, &q, sizeof(q), &r, sizeof(r))) return errnoOf(k, NativeAdrenoMethodSubmit);
        const uint32_t fence = uint32_t(r.fence);
        for (uint32_t i = 0; i < s.nr_bos; i++) {
            named[i]->lastFence = fence;
            if ((bos[i].flags & kSubmitBoWrite) && named[i]->imported) named[i]->writeFence = fence;
        }
        s.fence = fence;
        if (fence > highestFence_) highestFence_ = fence;
        return 0;
    }

    // until FENCE (a deadline of nowNs's clock), then the imported BOs whose GPU writes that fence covers invalidated
    int waitFence(std::unique_lock<std::mutex>& g, uint32_t fence, const MsmTimespec& deadline) {
        const int64_t at = deadline.seconds * 1000000000 + deadline.nanoseconds;
        const int64_t left = at - nowNs();
        NativeAdrenoAbiWaitFence w{fence, uint64_t(left > 0 ? left : 0)};
        // the wait without our lock: the core's completion thread waits while its commit thread submits
        g.unlock();
        const int e = errnoOf(method(NativeAdrenoMethodWaitFence, &w, sizeof(w), nullptr, 0), NativeAdrenoMethodWaitFence);
        g.lock();
        if (e != 0) return e;
        if (fence > passedFence_) passedFence_ = fence;
        for (auto& [h, b] : bos_) {
            if (b.imported && b.writeFence != 0 && b.writeFence <= passedFence_) {
                dcacheFlush(b.importBase, b.bytes, false);
                b.writeFence = 0;
            }
        }
        return 0;
    }

    int getParam(MsmParam& p) {
        switch (p.param) {
        case kParamGpuId: p.value = 0; return 0;
        case kParamChipId: p.value = info_.chip_id; return 0;   // no speed bin: ABI v1 has none
        case kParamGmemSize: p.value = info_.gmem_bytes; return 0;
        case kParamGmemBase: p.value = info_.gmem_base; return 0;
        case kParamHighestBankBit: p.value = info_.highest_bank_bit; return 0;
        case kParamUbwcSwizzle: p.value = info_.ubwc_swizzle; return 0;
        case kParamMacrotileMode: p.value = info_.macrotile_mode; return 0;
        case kParamMaxFreq: p.value = nativeadreno::kMaxFrequencyHz; return 0;
        case kParamPriorities: p.value = 1; return 0;
        case kParamPpPgtable: p.value = 0; return 0;
        case kParamRaytracing: p.value = 0; return 0;
        case kParamTimestamp: {
            NativeAdrenoAbiTimestamp t{};
            if (int e = errnoOf(method(NativeAdrenoMethodGetTimestamp, nullptr, 0, &t, sizeof(t)), NativeAdrenoMethodGetTimestamp)) return e;
            p.value = t.ticks;
            return 0;
        }
        case kParamFaults:
        case kParamSuspends: {
            NativeAdrenoAbiStatus st{};
            if (int e = errnoOf(method(NativeAdrenoMethodGetStatus, nullptr, 0, &st, sizeof(st)), NativeAdrenoMethodGetStatus)) return e;
            p.value = p.param == kParamFaults ? st.faults : st.suspends;
            return 0;
        }
        default: return EINVAL;
        }
    }

    io_connect_t c_;
    mach_timebase_info_data_t timebase_{};
    std::mutex lock_;
    NativeAdrenoAbiParams params_{};
    NativeAdrenoAbiDeviceInfo info_{};
    std::map<uint64_t, uint64_t> free_;   // GPU address -> bytes free there
    std::map<uint32_t, Bo> bos_;          // the core's handles
    std::map<uint32_t, uint64_t> queues_; // the core's queue ids -> the kernel's
    std::set<uint32_t> unserved_;
    uint32_t nextHandle_ = 1, nextQueue_ = 1;
    uint32_t highestFence_ = 0, passedFence_ = 0;
};

}  // namespace

std::unique_ptr<Transport> nativeAdrenoTransportOn(unsigned int connection, std::string* why) {
    std::unique_ptr<NativeAdreno> t(new NativeAdreno(io_connect_t(connection)));
    if (!t->start(why)) return nullptr;
    return t;
}

std::unique_ptr<Transport> openNativeAdrenoGpu(std::string* why) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
    if (s == 0) {
        *why = "no IOAcceleratorES service: the kernel has not published the GPU (SweetAccelerator)";
        return nullptr;
    }
    io_connect_t c = 0;
    kern_return_t r = IOServiceOpen(s, mach_task_self(), kOpenType, &c);
    IOObjectRelease(s);
    if (r != KERN_SUCCESS) {
        *why = "IOAcceleratorES would not open as a NativeAdreno (v2) client: error " + std::to_string(r);
        return nullptr;
    }
    return nativeAdrenoTransportOn(c, why);
}

}  // namespace hakios::metal
