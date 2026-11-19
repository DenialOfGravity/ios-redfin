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
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "NativeAdrenoABI.h"
#include "hakios-metal/prof.hpp"

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

// (redfin hmv4, from ws-hm-perf 663a295) HM_FLUSH=all: every imported BO a submission names is cleaned before it, every
// time (as before). Default: an IOSurface's pages are cleaned only when its seed (moved on by every write unlock: a CPU
// wrote it, in this process or another) differs from the seed at this client's last clean or invalidate of them, as
// phone/metal2's flushSurfaces. Userptr imports have no seed: always cleaned. Was: every surface a submission named,
// ~11 MB of dc civac per display surface on every commit (backboardd's commit 0.7-1.2 ms, 302 ms of 2.1 s in CC).
// The core's own CPU writes into surface rows (XR write-back) clean their rows themselves.
bool flushAll() {
    static const bool all = [] {
        const char* e = getenv("HM_FLUSH");
        return e != nullptr && strcmp(e, "all") == 0;
    }();
    return all;
}

// (ws-hm-int6) HM_FLUSH_AFTER=always (default) or lazy: the invalidate of an imported surface's pages after a fence
// that covers a GPU write of them (waitFence, "flushAfter": 0.85-1.86 ms a buffer for the 11.2 MB display surface in
// perf2's profile). It is for a CPU that reads the pages next: without it the CPU may read lines it (or a speculative
// fill) cached before the GPU wrote memory, and a partial-line CPU write may merge into such a line and write it back
// over the GPU's bytes. lazy: for the display surfaces (an IOSurface of HM_FLUSH_AFTER_SURF, default HM_GATE_FLAG_SURF's
// size, default 1125x2436: CoreAnimation's swap-chain surfaces, read by the display engine and the GPU only) the
// invalidate is left pending and done on demand -- kHakiosCpuAccess (the core's Texture read/replace and its XR
// write-back, hakios_metal_surface_cpu_access for the plug-in), a CPU-written seed's clean before the next submission
// (dc civac: the same invalidate), or the buffer's close here. NOT covered: a CPU read of the surface in ANOTHER
// process (IOSurfaceLock there: a screenshot, a recorder) and a CPU copy in the kernel; hence the default always.
bool flushAfterLazy() {   // (read at every fence that passes with work to flush: a test switches it)
    const char* e = getenv("HM_FLUSH_AFTER");
    return e != nullptr && strcmp(e, "lazy") == 0;
}
// the display surfaces' size: HM_FLUSH_AFTER_SURF, else HM_GATE_FLAG_SURF, else 1125x2436 (WxH)
void displaySurfaceSize(const char* first, uint32_t* w, uint32_t* h) {
    *w = 1125, *h = 2436;
    for (const char* name : {first, "HM_GATE_FLAG_SURF"}) {
        const char* sz = name != nullptr ? getenv(name) : nullptr;
        unsigned a = 0, b = 0;
        if (sz != nullptr && sscanf(sz, "%ux%u", &a, &b) == 2) {
            *w = a, *h = b;
            return;
        }
    }
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


// (ws-hm-csdiff) HM_RD_DUMP=<dir>: the submissions this process sends, as Mesa's freedreno .rd files (cffdump, crashdec,
// rddecompiler read them), so that hm's command stream and Turnip's (FD_RD_DUMP=trigger) can be decoded and diffed.
// Unset, empty or "0" (the default): off, one cached branch per submission and nothing else. Set: the trigger file
// <dir>/hm_<process>_<pid>.trigger (made at the first submission, mode 0666) is read before every submission; a number N
// written into it dumps the next N submissions (-1: until 0 is written; HM_RD_DUMP_START=N arms N at the start), each as
// <dir>/hm_<process>_<pid>_submit<NNNNN>.rd (plain, not gzipped): RD_CMD, RD_GPU_ID, then per BO an RD_GPUADDR and, for a
// driver-made BO of at most HM_RD_DUMP_MAXKB KiB (default 4096) that the CPU has mapped, its RD_BUFFER_CONTENTS, then the
// IB's RD_CMDSTREAM_ADDR. HM_RD_DUMP_BOS=all: every BO of this process, not only those the submission names (what the
// core does not name, e.g. a texture read through a descriptor, is then decodable too). Imported memory (IOSurfaces,
// userptr) gets its address and size only, unless HM_RD_DUMP_SURF=1. Taken before the kernel's Submit, under the lock.
namespace rddump {
struct Env {
    std::string dir;
    uint32_t maxBytes = 4096u << 10;
    bool allBos = false, surfaces = false;
    int start = 0;
};
Env readEnv() {
        Env v;
        const char* d = getenv("HM_RD_DUMP");
        if (d != nullptr && d[0] != 0 && strcmp(d, "0") != 0) v.dir = d;
        if (const char* m = getenv("HM_RD_DUMP_MAXKB"); m != nullptr && m[0] != 0) v.maxBytes = uint32_t(strtoul(m, nullptr, 10)) << 10;
        if (const char* b = getenv("HM_RD_DUMP_BOS"); b != nullptr && strcmp(b, "all") == 0) v.allBos = true;
        if (const char* f = getenv("HM_RD_DUMP_SURF"); f != nullptr && atoi(f) != 0) v.surfaces = true;
        if (const char* n = getenv("HM_RD_DUMP_START"); n != nullptr && n[0] != 0) v.start = atoi(n);
        return v;
}
const char* progName() {
#ifdef __APPLE__
    return getprogname();
#else
    return program_invocation_short_name;
#endif
}
// Mesa's redump.h section ids
enum : uint32_t { kRdCmd = 2, kRdGpuAddr = 3, kRdCmdstreamAddr = 6, kRdBufferContents = 12, kRdGpuId = 13 };
bool section(FILE* f, uint32_t type, const void* data, uint32_t bytes) {
    const uint32_t h[2] = {type, bytes};
    return fwrite(h, 4, 2, f) == 2 && (bytes == 0 || fwrite(data, 1, bytes, f) == bytes);
}
}  // namespace rddump

class NativeAdreno : public Transport {
public:
    explicit NativeAdreno(io_connect_t c) : c_(c) { mach_timebase_info(&timebase_); }
    ~NativeAdreno() override {
        if (rdTriggerFd_ >= 0) close(rdTriggerFd_);   // (ws-hm-csdiff) HM_RD_DUMP
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
        rd_ = rddump::readEnv();   // (ws-hm-csdiff) HM_RD_DUMP
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
        case kGemClose: return size == sizeof(DrmGemClose) ? closeBo(static_cast<DrmGemClose*>(data)->handle, &g) : EINVAL;
        case kGemCpuPrep: {
            if (size != sizeof(MsmGemCpuPrep)) return EINVAL;
            auto* p = static_cast<MsmGemCpuPrep*>(data);
            auto b = bos_.find(p->handle);
            if (b == bos_.end()) return ENOENT;
            return b->second.lastFence == 0 ? 0 : waitFence(g, b->second.lastFence, p->timeout);
        }
        case kGemCpuFini: return size == sizeof(MsmGemCpuFini) ? 0 : EINVAL;
        case kHakiosCpuAccess: {   // (ws-hm-int6) HM_FLUSH_AFTER=lazy: a pending invalidate done before the CPU's access
            if (size != sizeof(HakiosCpuAccess)) return EINVAL;
            auto* a = static_cast<HakiosCpuAccess*>(data);
            return cpuAccess(g, a->handle, a->surface);
        }
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
        case kHakiosSurfaceUbwc: return size == sizeof(HakiosSurfaceUbwc) ? surfaceUbwc(*static_cast<HakiosSurfaceUbwc*>(data)) : EINVAL;
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
        uint32_t cleanSeed = 0;  // a surface's: its seed at the last clean/invalidate here (seedKnown)
        bool seedKnown = false;
        bool invalPending = false;   // (ws-hm-int6) HM_FLUSH_AFTER=lazy: the fence's invalidate left for a CPU access
    };

    kern_return_t method(uint32_t m, const void* in, size_t inSize, void* out, size_t outSize) {
        // {'NTNL', method, payload} through selector 0, as v2test and the darwin backend call it
        uint8_t buf[4096];   // (redfin xnu 1258) a submission with its BO list: up to the inline struct input
        const uint32_t hdr[2] = {kTunnel, m};
        memcpy(buf, hdr, 8);
        if (inSize > sizeof(buf) - 8) return kIOReturnBadArgument;
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

    // (redfin ws-hm-ubwc) a surface's UBWC layout: the kernel says it as the imported surface buffer's metadata
    // (NativeAdrenoMethodGetMetadata on the BO: kSurfaceUbwcMagic, then HakiosSurfaceUbwc's words from VERSION on,
    // offsets from the surface's base address). A buffer without (the kernel's answer today: 0 bytes) is linear: ENOENT.
    // Asked after kHakiosSurface imported the surface (Texture::wrapSurface).
    int surfaceUbwc(HakiosSurfaceUbwc& u) {
        const Bo* found = nullptr;
        for (const auto& [h, b] : bos_) {
            if (b.surfaceId == u.surface && b.surface != nullptr) found = &b;
        }
        if (found == nullptr) return ENOENT;
        NativeAdrenoAbiMetadata in{};
        in.handle = found->handle;
        in.bytes = NativeAdrenoAbiMetadataBytes;
        NativeAdrenoAbiMetadata out{};
        if (kern_return_t k = method(NativeAdrenoMethodGetMetadata, &in, sizeof(in), &out, sizeof(out))) return errnoOf(k, NativeAdrenoMethodGetMetadata);
        uint32_t w[7] = {};   // version, metaOffset, metaPitch, metaSize, dataOffset, dataPitch, highestBankBit
        if (out.bytes < 4 + sizeof(w) || out.bytes > NativeAdrenoAbiMetadataBytes) return ENOENT;
        uint32_t magic = 0;
        memcpy(&magic, out.text, 4);
        if (magic != kSurfaceUbwcMagic) return ENOENT;
        memcpy(w, out.text + 4, sizeof(w));
        const uint32_t surface = u.surface;
        u = HakiosSurfaceUbwc{surface, w[0], w[1], w[2], w[3], w[4], w[5], w[6]};
        return u.version != 0 ? 0 : ENOENT;
    }

    void unmapLocked(Bo& bo) {
        IOConnectUnmapMemory64(c_, bo.token, mach_task_self(), mach_vm_address_t(uintptr_t(bo.cpu)));
        NativeAdrenoAbiBoCpuUnmap u{bo.handle, bo.token, 0};
        method(NativeAdrenoMethodBoCpuUnmap, &u, sizeof(u), nullptr, 0);
        bo.cpu = nullptr;
        bo.token = 0;
    }

    // (ws-hm-int6) the pending invalidates of the buffer HANDLE (non-zero) or of every buffer over IOSurface SURFACE,
    // done now (outside the lock as waitFence's are; the buffers held by flushing_ meanwhile)
    uint32_t lazySkipped_ = 0, lazyOnDemand_ = 0;
    uint64_t lazySkippedBytes_ = 0;
    int cpuAccess(std::unique_lock<std::mutex>& g, uint32_t handle, uint32_t surface) {
        std::vector<std::pair<void*, uint64_t>> ranges;
        bool found = false;
        for (auto& [h, b] : bos_) {
            if (!(handle != 0 ? h == handle : (surface != 0 && b.surfaceId == surface))) continue;
            found = true;
            if (!b.invalPending) continue;
            b.invalPending = false;
            ranges.emplace_back(b.importBase, b.bytes);
        }
        if (!ranges.empty()) {
            prof::Scope t(prof::kFlushAfter);
            lazyOnDemand_++;
            flushing_++;
            g.unlock();
            for (const auto& [p, n] : ranges) {
                dcacheFlush(p, n, false);
                prof::bytes(false, n, false);
            }
            g.lock();
            flushing_--;
            flushed_.notify_all();
        }
        return found ? 0 : ENOENT;
    }

    int closeBo(uint32_t handle, std::unique_lock<std::mutex>* g = nullptr) {
        // (an imported buffer's pages stay the caller's while a fence's flush outside the lock may still touch them:
        // wait for that before the import goes)
        if (g != nullptr) flushed_.wait(*g, [this] { return flushing_ == 0; });
        auto b = bos_.find(handle);
        if (b == bos_.end()) return ENOENT;
        Bo bo = b->second;
        bos_.erase(b);
        // (ws-hm-int6) a pending invalidate is done before the import goes: the surface outlives this buffer, and a CPU
        // that reads it later (here or elsewhere) must not find lines cached before the GPU wrote it
        if (bo.invalPending && bo.importBase != nullptr) {
            dcacheFlush(bo.importBase, bo.bytes, false);
            lazyOnDemand_++;
        }
        // (redfin hmv6) what it was, for a submission that names it after this (ENOENT): the last 256 closed
        closed_[handle] = Closed{bo.surfaceId, bo.bytes, bo.iova, bo.imported, nowNs()};
        if (closed_.size() > 256) closed_.erase(closed_.begin());
        if (bo.cpu != nullptr) unmapLocked(bo);
        NativeAdrenoAbiBoFree f{bo.handle};
        const int e = errnoOf(method(NativeAdrenoMethodBoFree, &f, sizeof(f), nullptr, 0), NativeAdrenoMethodBoFree);
        // the kernel unmapped the range from the GPU with the BO; only now may another BO take the address
        vaGive(bo.iova, bo.vaBytes);
        if (bo.surface != nullptr) CFRelease(bo.surface);
        return e;
    }

    // (redfin hm-int5) HM_GATE_FLAG=1 (default 0: the kernel side does not exist yet): a submission that writes the display
    // surface -- an imported IOSurface named for writing whose size is HM_GATE_FLAG_SURF (default 1125x2436, the panel's
    // surfaces as CoreAnimation's window server draws them; WxH) -- carries kSubmitFlagWritesDisplay in
    // NativeAdrenoAbiSubmit.reserved, for a kernel whose scanout gate (p scanout gate 1|2) holds only flagged submissions
    // of the swapper; every other submission is sent bare and passes the gate. A kernel that refuses the field (all before
    // it) turns the flag off for the process at its first refusal (the submission is sent again without it).
    // (ws-hm-int6) ...and, for the kernel's gate mode 3 (redfin xnu 1105, ws-int-k2: NativeAdrenoAbiSubmit.flags, the
    // union over reserved), the surface's IOSurface id in bits 8-31 (NativeAdrenoAbiSubmitSurfaceShift/Mask): gate 3
    // holds the submission only while that surface is on screen or queued for a latch. An id that does not fit in 24
    // bits is left out (bit 0 alone, which gate 1|2 reads as before); bits 1-7 stay zero.
    static constexpr uint32_t kSubmitFlagWritesDisplay = 1u;
    static constexpr uint32_t kSubmitSurfaceShift = 8;
    bool gateFlagRefused_ = false;
    uint32_t gateFlagged_ = 0, gateBare_ = 0;
    uint32_t gateFlag(const MsmSubmitBo* bos, const std::vector<Bo*>& named, uint32_t n) {
        const char* e = getenv("HM_GATE_FLAG");
        if (gateFlagRefused_ || e == nullptr || atoi(e) == 0) return 0;
        uint32_t w = 1125, h = 2436;
        displaySurfaceSize(nullptr, &w, &h);
        for (uint32_t i = 0; i < n; i++) {
            const Bo* b = named[i];
            if ((bos[i].flags & kSubmitBoWrite) == 0 || !b->imported || b->surface == nullptr) continue;
            if (IOSurfaceGetWidth(b->surface) == w && IOSurfaceGetHeight(b->surface) == h) {
                if (gateFlagged_++ == 0) fprintf(stderr, "hakios-metal: HM_GATE_FLAG: first submission flagged (writes IOSurface %u, %ux%u)\n", b->surfaceId, w, h);
                return kSubmitFlagWritesDisplay | (b->surfaceId < (1u << 24) ? b->surfaceId << kSubmitSurfaceShift : 0);
            }
        }
        gateBare_++;
        return 0;
    }

    // (redfin xnu 1258) HM_SUBMIT_BOLIST (default 1): each submission names its imported BOs (write flag from the msm
    // BO table) after the buffers, so the kernel holds exactly the IOSurfaces the job uses until its fence lands
    // (IOSurfaceIsInUse, kern.sweet_surf_inuse); a kernel before 1258 refuses the flag once and it goes off for the
    // process. More imports than fit in one record: sent without the list (the kernel then holds every import).
    static constexpr uint32_t kMaxListedBos = (4096 - 8 - sizeof(NativeAdrenoAbiSubmit) - sizeof(NativeAdrenoAbiSubmitBoList)) /
                                              sizeof(NativeAdrenoAbiSubmitBo);
    struct SubmitWithBos {
        NativeAdrenoAbiSubmit q;
        NativeAdrenoAbiSubmitBoList list;
        NativeAdrenoAbiSubmitBo bos[kMaxListedBos];
    };
    bool boListRefused_ = false;
    int boListOn_ = -1;
    uint32_t boListed_ = 0, boListTooMany_ = 0;
    uint32_t listBos(const MsmSubmitBo* bos, const std::vector<Bo*>& named, uint32_t n, NativeAdrenoAbiSubmitBo* out) {
        if (boListOn_ < 0) {
            const char* e = getenv("HM_SUBMIT_BOLIST");
            boListOn_ = e == nullptr || atoi(e) != 0;
        }
        if (!boListOn_ || boListRefused_) return UINT32_MAX;
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++) {
            const Bo* b = named[i];
            if (!b->imported) continue;
            const uint32_t w = (bos[i].flags & kSubmitBoWrite) ? NativeAdrenoAbiSubmitBoWrite : 0;
            uint32_t j = 0;
            while (j < k && out[j].handle != b->handle) j++;
            if (j < k) {
                out[j].flags |= w;
                continue;
            }
            if (k == kMaxListedBos) {
                boListTooMany_++;
                return UINT32_MAX;
            }
            out[k++] = NativeAdrenoAbiSubmitBo{b->handle, w, 0};
        }
        return k;
    }

    int submit(MsmGemSubmit& s) {
        if (s.nr_cmds != 1 || s.nr_bos == 0 || (s.queueid != 0 && queues_.count(s.queueid) == 0)) return EINVAL;
        const auto* bos = reinterpret_cast<const MsmSubmitBo*>(uintptr_t(s.bos));
        const auto* cmd = reinterpret_cast<const MsmSubmitCmd*>(uintptr_t(s.cmds));
        if (cmd->type != kSubmitCmdBuf || cmd->submit_idx >= s.nr_bos || cmd->size == 0 || cmd->size % 4 != 0) return EINVAL;
        std::vector<Bo*> named(s.nr_bos);
        for (uint32_t i = 0; i < s.nr_bos; i++) {
            auto b = bos_.find(bos[i].handle);
            if (b == bos_.end()) {
                // (redfin hmv6) say which: a handle the core still names after it was closed (hmt7: refused from frame 993
                // on, after turning back to portrait, with no word of which buffer); the first 20, then every 100th
                if (++enoents_ <= 20 || enoents_ % 100 == 0) {
                    auto c = closed_.find(bos[i].handle);
                    if (c != closed_.end()) {
                        fprintf(stderr, "hakios-metal: submission %u names BO %u (entry %u of %u%s), closed %.1f ms ago: %s%u, %llu KB at 0x%llx: ENOENT\n",
                                enoents_, bos[i].handle, i, s.nr_bos, (bos[i].flags & kSubmitBoWrite) ? ", written" : "",
                                double(nowNs() - c->second.at) / 1e6, c->second.surfaceId ? "IOSurface " : (c->second.imported ? "user memory " : "own BO "),
                                c->second.surfaceId, (unsigned long long)(c->second.bytes >> 10), (unsigned long long)c->second.iova);
                    } else {
                        fprintf(stderr, "hakios-metal: submission %u names BO %u (entry %u of %u), never this client's or closed long ago: ENOENT\n",
                                enoents_, bos[i].handle, i, s.nr_bos);
                    }
                }
                return ENOENT;
            }
            named[i] = &b->second;
        }
        const Bo& ib = *named[cmd->submit_idx];
        if (uint64_t(cmd->submit_offset) + cmd->size > ib.bytes) return EINVAL;
        // imported pages the GPU reads (or overwrites in part): what the CPU wrote goes to memory first
        const bool all = flushAll();
        for (Bo* b : named) {
            if (!b->imported) continue;
            if (b->surface != nullptr && !all) {
                const uint32_t seed = IOSurfaceGetSeed(b->surface);   // before the flush: a write after it moves it on
                if (b->seedKnown && b->cleanSeed == seed) continue;
                b->cleanSeed = seed;
                b->seedKnown = true;
            }
            dcacheFlush(b->importBase, b->bytes, true);
            b->invalPending = false;   // (ws-hm-int6) dc civac: cleaned and invalidated
        }
        if (!rd_.dir.empty() && rdArmed() != 0) rdDump(s, bos, named, ib, *cmd);   // (ws-hm-csdiff) HM_RD_DUMP
        SubmitWithBos m{};
        NativeAdrenoAbiSubmit& q = m.q;
        q.buffer_count = 1;
        q.queue = 0;   // one ring (xnu 0115: every queue is queue 0)
        q.reserved = gateFlag(bos, named, s.nr_bos);   // (redfin hm-int5) HM_GATE_FLAG
        q.buffers[0].handle = ib.handle;
        q.buffers[0].iova = ib.iova + cmd->submit_offset;
        q.buffers[0].bytes = cmd->size;
        NativeAdrenoAbiSubmitReply r{};
        kern_return_t k;
        {
            prof::Scope t(prof::kIoSubmit);
            const uint32_t nl = listBos(bos, named, s.nr_bos, m.bos);
            if (nl != UINT32_MAX) {
                q.flags |= NativeAdrenoAbiSubmitBoListFlag;
                m.list.count = nl;
                k = method(NativeAdrenoMethodSubmit, &m, sizeof(q) + sizeof(m.list) + nl * sizeof(NativeAdrenoAbiSubmitBo), &r, sizeof(r));
                if (k == kIOReturnBadArgument) {
                    // a kernel before 1258: off for this process, sent again without the list
                    boListRefused_ = true;
                    fprintf(stderr, "hakios-metal: HM_SUBMIT_BOLIST: the kernel refuses a submission's BO list (before redfin xnu 1258); off for this process\n");
                    q.flags &= ~NativeAdrenoAbiSubmitBoListFlag;
                    k = method(NativeAdrenoMethodSubmit, &q, sizeof(q), &r, sizeof(r));
                } else if (k == KERN_SUCCESS && boListed_++ == 0) {
                    fprintf(stderr, "hakios-metal: HM_SUBMIT_BOLIST: first submission names its %u imported BOs\n", nl);
                }
            } else {
                k = method(NativeAdrenoMethodSubmit, &q, sizeof(q), &r, sizeof(r));
            }
            if (k == kIOReturnBadArgument && q.reserved != 0) {
                // (redfin hm-int5) a kernel without the flag (it wants the field 0): off for this process, sent again bare
                gateFlagRefused_ = true;
                fprintf(stderr, "hakios-metal: HM_GATE_FLAG: the kernel refuses a submission's flags (its NativeAdrenoAbiSubmit.reserved must be 0); "
                                "flag off for this process\n");
                q.reserved = 0;
                k = method(NativeAdrenoMethodSubmit, &q, sizeof(q), &r, sizeof(r));
            }
        }
        if (k != KERN_SUCCESS) return errnoOf(k, NativeAdrenoMethodSubmit);
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
        int e;
        {
            prof::Scope t(prof::kWaitIo);
            e = errnoOf(method(NativeAdrenoMethodWaitFence, &w, sizeof(w), nullptr, 0), NativeAdrenoMethodWaitFence);
        }
        g.lock();
        if (e != 0) return e;
        if (fence > passedFence_) passedFence_ = fence;
        // the ranges are taken under the lock and flushed without it: a submission on the caller's thread does not
        // wait for megabytes of dc civac here. The BO is held meanwhile (flushing_: closeBo waits for it).
        std::vector<std::pair<void*, uint64_t>> ranges;
        const bool lazy = flushAfterLazy();
        uint32_t dw = 0, dh = 0;
        if (lazy) displaySurfaceSize("HM_FLUSH_AFTER_SURF", &dw, &dh);
        for (auto& [h, b] : bos_) {
            if (b.imported && b.writeFence != 0 && b.writeFence <= passedFence_) {
                if (lazy && b.surface != nullptr && IOSurfaceGetWidth(b.surface) == dw && IOSurfaceGetHeight(b.surface) == dh) {
                    // (ws-hm-int6) HM_FLUSH_AFTER=lazy: a display surface's invalidate waits for a CPU access
                    b.writeFence = 0;
                    b.invalPending = true;
                    if (lazySkipped_++ == 0) {
                        fprintf(stderr, "hakios-metal: HM_FLUSH_AFTER=lazy: first fence invalidate left pending (IOSurface %u, %ux%u, %llu KB)\n", b.surfaceId, dw, dh,
                                (unsigned long long)(b.bytes >> 10));
                    }
                    lazySkippedBytes_ += b.bytes;
                    continue;
                }
                ranges.emplace_back(b.importBase, b.bytes);
                b.writeFence = 0;
                if (b.surface != nullptr && b.seedKnown) b.cleanSeed = IOSurfaceGetSeed(b.surface);   // (just invalidated: clean)
            }
        }
        if (!ranges.empty()) {
            prof::Scope t(prof::kFlushAfter);
            flushing_++;
            g.unlock();
            for (const auto& [p, n] : ranges) {
                dcacheFlush(p, n, false);
                prof::bytes(false, n, false);
            }
            g.lock();
            flushing_--;
            flushed_.notify_all();
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

    // (ws-hm-csdiff) HM_RD_DUMP (see rddump above): how many submissions are still to be dumped, read from the trigger
    int rdArmed() {
        const auto& e = rd_;
        if (rdTriggerFd_ == -2) {   // first submission with the switch on
            char path[512];
            snprintf(path, sizeof path, "%s/hm_%s_%d.trigger", e.dir.c_str(), rddump::progName(), int(getpid()));
            rdTriggerFd_ = open(path, O_RDWR | O_CREAT, 0666);
            if (rdTriggerFd_ >= 0) fchmod(rdTriggerFd_, 0666);
            rdLeft_ = e.start;
            fprintf(stderr, "hakios-metal: HM_RD_DUMP: trigger %s (%s), %d submissions armed\n", path,
                    rdTriggerFd_ >= 0 ? "open" : strerror(errno), rdLeft_);
        }
        if (rdTriggerFd_ >= 0) {
            struct stat st {};
            if (fstat(rdTriggerFd_, &st) == 0 && st.st_size > 0) {
                char buf[32] = {};
                const ssize_t n = pread(rdTriggerFd_, buf, sizeof buf - 1, 0);
                if (ftruncate(rdTriggerFd_, 0) != 0) {}
                if (n > 0) {
                    rdLeft_ = int(strtol(buf, nullptr, 0));
                    fprintf(stderr, "hakios-metal: HM_RD_DUMP: trigger says %d\n", rdLeft_);
                }
            }
        }
        return rdLeft_;
    }

    void rdDump(const MsmGemSubmit& s, const MsmSubmitBo* bos, const std::vector<Bo*>& named, const Bo& ib, const MsmSubmitCmd& cmd) {
        const auto& e = rd_;
        char path[600];
        snprintf(path, sizeof path, "%s/hm_%s_%d_submit%05u.rd", e.dir.c_str(), rddump::progName(), int(getpid()), rdSeq_);
        FILE* f = fopen(path, "wb");
        if (f == nullptr) {
            fprintf(stderr, "hakios-metal: HM_RD_DUMP: %s: %s\n", path, strerror(errno));
            rdLeft_ = 0;
            return;
        }
        char what[96];
        const int wn = snprintf(what, sizeof what, "hm-dump %s submit %u", rddump::progName(), rdSeq_);
        bool ok = rddump::section(f, rddump::kRdCmd, what, uint32_t(wn + 1));
        // Mesa's gpu_id from the chip id 0xMMmmPPxx (A620 0x06020000 -> 620)
        const uint32_t c = info_.chip_id;
        const uint32_t gpuId = ((c >> 24) & 0xff) * 100 + ((c >> 16) & 0xff) * 10 + ((c >> 8) & 0xff);
        ok = ok && rddump::section(f, rddump::kRdGpuId, &gpuId, 4);
        std::set<uint32_t> done;
        uint64_t contents = 0;
        unsigned nbo = 0;
        auto one = [&](const Bo& b) {
            const uint32_t a[3] = {uint32_t(b.iova), uint32_t(b.bytes), uint32_t(b.iova >> 32)};
            ok = ok && rddump::section(f, rddump::kRdGpuAddr, a, 12);
            const void* p = b.imported ? (e.surfaces ? b.importBase : nullptr) : b.cpu;
            if (p != nullptr && (b.bytes <= e.maxBytes || &b == &ib)) {   // the IB's own BO always
                ok = ok && rddump::section(f, rddump::kRdBufferContents, p, uint32_t(b.bytes));
                contents += b.bytes;
            }
            nbo++;
        };
        if (e.allBos) {
            for (const auto& [h, b] : bos_) one(b);
        } else {
            for (uint32_t i = 0; i < s.nr_bos; i++)
                if (done.insert(bos[i].handle).second) one(*named[i]);
        }
        const uint64_t at = ib.iova + cmd.submit_offset;
        const uint32_t a[3] = {uint32_t(at), cmd.size / 4, uint32_t(at >> 32)};
        ok = ok && rddump::section(f, rddump::kRdCmdstreamAddr, a, 12);
        ok = (fclose(f) == 0) && ok;
        fprintf(stderr, "hakios-metal: HM_RD_DUMP: %s: %u BOs, %llu KB of contents, IB %u dwords at 0x%llx%s\n", path, nbo,
                (unsigned long long)(contents >> 10), cmd.size / 4, (unsigned long long)at, ok ? "" : " (WRITE FAILED)");
        rdSeq_++;
        if (rdLeft_ > 0) rdLeft_--;
    }

    io_connect_t c_;
    mach_timebase_info_data_t timebase_{};
    std::mutex lock_;
    std::condition_variable flushed_;   // flushing_ back to 0
    unsigned flushing_ = 0;             // fence flushes running outside lock_
    NativeAdrenoAbiParams params_{};
    NativeAdrenoAbiDeviceInfo info_{};
    std::map<uint64_t, uint64_t> free_;   // GPU address -> bytes free there
    std::map<uint32_t, Bo> bos_;          // the core's handles
    std::map<uint32_t, uint64_t> queues_; // the core's queue ids -> the kernel's
    std::set<uint32_t> unserved_;
    struct Closed { uint32_t surfaceId; uint64_t bytes, iova; bool imported; int64_t at; };
    std::map<uint32_t, Closed> closed_;   // (redfin hmv6) the last 256 handles closed, for the ENOENT line
    uint32_t enoents_ = 0;
    uint32_t nextHandle_ = 1, nextQueue_ = 1;
    uint32_t highestFence_ = 0, passedFence_ = 0;
    rddump::Env rd_;         // (ws-hm-csdiff) HM_RD_DUMP's switches, read when the transport starts
    int rdTriggerFd_ = -2;   // (ws-hm-csdiff) HM_RD_DUMP: -2 not looked at yet, -1 no trigger file
    int rdLeft_ = 0;
    uint32_t rdSeq_ = 0;
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
