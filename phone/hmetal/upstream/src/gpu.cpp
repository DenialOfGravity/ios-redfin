#include "hakios-metal/gpu.hpp"

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/trace.hpp"   // (redfin hm-trace)

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hakios::metal {

namespace {
constexpr uint64_t kPage = 0x4000;  // Darwin's page; the driver rounds to its own (4 KB) inside it

MsmTimespec deadline(Transport& t, int64_t ns) {
    // the driver takes a deadline, not a length: a time of the clock that goes on while the machine sleeps
    // (mach_continuous_time, which is Darwin's CLOCK_MONOTONIC), as Linux's msm takes one of CLOCK_MONOTONIC
    if (ns < 0) {
        ns = 0;
    }
    int64_t at = t.nowNs() + ns;
    return MsmTimespec{at / 1000000000, at % 1000000000};
}
}  // namespace

// (redfin) the chip this process drives, one GPU per process: the A635 until a Gpu names another
namespace a6xx {
namespace {
std::atomic<const ChipInfo*> gChip{&kChipA635};
}
const ChipInfo& chip() { return *gChip.load(std::memory_order_relaxed); }
void selectChip(const ChipInfo& c) { gChip.store(&c, std::memory_order_relaxed); }
}  // namespace a6xx

Gpu::Gpu(std::unique_ptr<Transport> transport) : t_(std::move(transport)) {
    const a6xx::ChipInfo* c = a6xx::chipFor(chipId());
    if (c != nullptr) a6xx::selectChip(*c);
    // (redfin) UBWC as the kernel programmed it: the driver's words when it says all three, else the chip table's
    uint64_t hbb = 0, swz = 0, mt = 0;
    const a6xx::ChipInfo& t = c != nullptr ? *c : a6xx::chip();
    if (param(kParamHighestBankBit, &hbb) == 0 && param(kParamUbwcSwizzle, &swz) == 0 && param(kParamMacrotileMode, &mt) == 0 &&
        hbb >= 13 && hbb <= 16) {
        ubwc_ = UbwcConfig{uint32_t(hbb), uint32_t(swz), uint32_t(mt), true};
    } else {
        ubwc_ = UbwcConfig{t.highestBankBit, t.ubwcSwizzle, t.macrotileMode, false};
    }
    // (ws-hm-pool) HM_RT_CACHE / HM_RT_CACHE_SLACK / HM_RT_CACHE_ZERO_KB, and HM_RT_PREWARM
    BoCacheConfig cc;
    if (const char* e = getenv("HM_RT_CACHE"); e != nullptr && e[0] != 0) cc.limitBytes = uint64_t(strtoull(e, nullptr, 10)) << 20;
    if (const char* e = getenv("HM_RT_CACHE_SLACK"); e != nullptr && e[0] != 0) cc.slackPct = uint32_t(atoi(e));
    if (const char* e = getenv("HM_RT_CACHE_ZERO_KB"); e != nullptr && e[0] != 0) cc.zeroBytes = uint64_t(strtoull(e, nullptr, 10)) << 10;
    cacheCfg_ = cc;
    if (const char* e = getenv("HM_RT_PREWARM"); e != nullptr && e[0] != 0 && cc.limitBytes != 0) {
        // "KB[xN],KB[xN],...": made now, into the cache, so that the first render targets of those sizes cost nothing
        const char* p = e;
        while (*p != 0) {
            char* end = nullptr;
            const uint64_t kb = strtoull(p, &end, 10);
            if (end == p) break;
            p = end;
            unsigned count = 1;
            if (*p == 'x' || *p == 'X') {
                count = unsigned(strtoul(p + 1, &end, 10));
                p = end;
            }
            for (unsigned i = 0; i < count && kb != 0; i++) {
                int err = 0;
                std::unique_ptr<Bo> bo = newBo(kb << 10, kBoWriteCombined, &err);
                if (bo == nullptr) break;
                bo = cacheGive(std::move(bo));
                if (bo != nullptr) break;   // (did not fit: closed here)
            }
            while (*p != 0 && *p != ',') p++;
            if (*p == ',') p++;
        }
    }
}

// (ws-hm-pool) the reuse cache --------------------------------------------------------------------------------------
namespace {
int64_t cacheNowNs() {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

Gpu::~Gpu() { cacheDrop(0); }   // (the cached Bos close through this Gpu's transport: before it goes)

void Gpu::setBoCache(const BoCacheConfig& c) {
    {
        std::lock_guard<std::mutex> g(cacheLock_);
        cacheCfg_ = c;
    }
    if (c.limitBytes == 0) cacheDrop(0);
}

Gpu::BoCacheConfig Gpu::boCache() const {
    std::lock_guard<std::mutex> g(cacheLock_);
    return cacheCfg_;
}

std::unique_ptr<Bo> Gpu::cacheTake(uint64_t size, bool count) {
    const uint64_t want = (size + kPage - 1) & ~(kPage - 1);
    std::lock_guard<std::mutex> g(cacheLock_);
    if (cacheCfg_.limitBytes == 0) return nullptr;
    auto it = cacheBySize_.lower_bound(want);
    if (it == cacheBySize_.end() || it->first > want + want * cacheCfg_.slackPct / 100) {
        if (count) cacheStats_.misses++;
        return nullptr;
    }
    std::unique_ptr<Bo> bo = std::move(it->second->bo);
    cacheLru_.erase(it->second);
    cacheBySize_.erase(it);
    cacheStats_.held -= bo->size();
    cacheStats_.hits++;
    cacheStats_.hitBytes += bo->size();
    return bo;
}

std::unique_ptr<Bo> Gpu::cacheGive(std::unique_ptr<Bo> bo) {
    if (bo == nullptr || !bo->owned_ || bo->cpu_ == nullptr || &bo->gpu_ != this) return bo;
    std::vector<std::unique_ptr<Bo>> drop;   // (closed outside the lock)
    {
        std::lock_guard<std::mutex> g(cacheLock_);
        if (cacheCfg_.limitBytes == 0) return bo;
        if (bo->size() > cacheCfg_.limitBytes) {
            cacheStats_.refused++;
            return bo;
        }
        const uint64_t size = bo->size();
        cacheLru_.push_front(Cached{std::move(bo), cacheNowNs()});
        cacheBySize_.emplace(size, cacheLru_.begin());
        cacheStats_.held += size;
        // past the limit: the least recently released let go (never the one just kept: it fits alone)
        while (cacheStats_.held > cacheCfg_.limitBytes && cacheLru_.size() > 1) {
            auto last = std::prev(cacheLru_.end());
            const uint64_t s = last->bo->size();
            for (auto [a, b] = cacheBySize_.equal_range(s); a != b; ++a) {
                if (a->second == last) {
                    cacheBySize_.erase(a);
                    break;
                }
            }
            cacheStats_.held -= s;
            cacheStats_.evicted++;
            drop.push_back(std::move(last->bo));
            cacheLru_.erase(last);
        }
    }
    return nullptr;
}

uint64_t Gpu::cacheDrop(int64_t olderThanNs) {
    std::vector<std::unique_ptr<Bo>> drop;
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> g(cacheLock_);
        const int64_t now = cacheNowNs();
        while (!cacheLru_.empty()) {
            auto last = std::prev(cacheLru_.end());
            if (olderThanNs != 0 && now - last->atNs < olderThanNs) break;   // (the rest are newer)
            const uint64_t s = last->bo->size();
            for (auto [a, b] = cacheBySize_.equal_range(s); a != b; ++a) {
                if (a->second == last) {
                    cacheBySize_.erase(a);
                    break;
                }
            }
            cacheStats_.held -= s;
            cacheStats_.dropped++;
            bytes += s;
            drop.push_back(std::move(last->bo));
            cacheLru_.erase(last);
        }
    }
    return bytes;
}

Gpu::BoCacheStats Gpu::cacheStats() {
    std::lock_guard<std::mutex> g(cacheLock_);
    BoCacheStats s = cacheStats_;
    s.entries = cacheLru_.size();
    s.limit = cacheCfg_.limitBytes;
    s.gemNew = gemNew_.load(std::memory_order_relaxed);
    s.gemNewNs = gemNewNs_.load(std::memory_order_relaxed);
    s.retries = gemRetries_.load(std::memory_order_relaxed);
    return s;
}

int Gpu::param(uint32_t which, uint64_t* value) {
    MsmParam p{};
    p.pipe = kPipe3D;
    p.param = which;
    int e = t_->call(kGetParam, &p, sizeof(p));
    if (e == 0) {
        *value = p.value;
    }
    return e;
}

uint32_t Gpu::chipId() {
    uint64_t v = 0;
    return param(kParamChipId, &v) == 0 ? uint32_t(v) : 0;
}

uint64_t Gpu::faults() {
    uint64_t v = 0;
    return param(kParamFaults, &v) == 0 ? v : 0;
}

std::unique_ptr<Bo> Gpu::newBo(uint64_t size, uint32_t flags, int* error) {
    *error = 0;
    if (size == 0) {
        *error = EINVAL;
        return nullptr;
    }
    const int64_t trT0 = hakios::trace::nowNs();   // (redfin hm-trace) a large GEM object made: an event
    struct TraceNew {
        int64_t t0;
        uint64_t size;
        const bool* ok;
        ~TraceNew() {
            if (*ok && size >= hakios::trace::bigBytes() && hakios::trace::on()) hakios::trace::event("gem-new", "", t0, hakios::trace::nowNs() - t0, size);
        }
    };
    bool trOk = false;
    TraceNew trace{trT0, (size + kPage - 1) & ~(kPage - 1), &trOk};
    MsmGemNew n{};
    n.size = (size + kPage - 1) & ~(kPage - 1);
    n.flags = flags;
    gemNew_.fetch_add(1, std::memory_order_relaxed);   // (ws-hm-pool) counted, and timed below
    struct TimeNew {
        std::atomic<uint64_t>& ns;
        int64_t t0;
        ~TimeNew() { ns.fetch_add(uint64_t(cacheNowNs() - t0), std::memory_order_relaxed); }
    } timeNew{gemNewNs_, cacheNowNs()};
    if ((*error = t_->call(kGemNew, &n, sizeof(n))) != 0) {
        // (ws-hm-pool) the kernel's pool short: what the reuse cache holds given back, and asked once more
        if (cacheDrop(0) == 0) return nullptr;
        gemRetries_.fetch_add(1, std::memory_order_relaxed);
        n = MsmGemNew{};
        n.size = (size + kPage - 1) & ~(kPage - 1);
        n.flags = flags;
        if ((*error = t_->call(kGemNew, &n, sizeof(n))) != 0) return nullptr;
    }
    MsmGemInfo info{};
    info.handle = n.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    void* cpu = t_->map(n.handle, n.size);
    if (cpu == nullptr) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        *error = ENOMEM;
        return nullptr;
    }
    std::unique_ptr<Bo> bo(new Bo(*this, n.handle, n.size, info.value, cpu));
    bo->counted_ = true;
    allocated_ += n.size;
    trOk = true;
    return bo;
}

std::unique_ptr<Bo> Gpu::surfaceBo(uint32_t surfaceId, SurfaceInfo* out, int* error) {
    HakiosSurface s{};
    s.surface = surfaceId;
    if ((*error = t_->call(kHakiosSurface, &s, sizeof(s))) != 0) {
        return nullptr;
    }
    MsmGemInfo info{};
    info.handle = s.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{s.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    *out = SurfaceInfo{s.size, s.offset, s.rowBytes, s.width, s.height, s.pixelFormat, s.planes};
    const int64_t trT0 = hakios::trace::nowNs();   // (redfin hm-trace) the surface's mapping (the kernel calls before it are short)
    std::unique_ptr<Bo> bo(new Bo(*this, s.handle, s.size, info.value, t_->map(s.handle, s.size)));
    if (hakios::trace::on())
        hakios::trace::event("surface-map", "surface " + std::to_string(surfaceId) + " " + std::to_string(s.width) + "x" + std::to_string(s.height), trT0,
                             hakios::trace::nowNs() - trT0, s.size);
    return bo;
}

int Gpu::surfaceUbwc(uint32_t surfaceId, HakiosSurfaceUbwc* out) {
    HakiosSurfaceUbwc u{};
    u.surface = surfaceId;
    const int e = t_->call(kHakiosSurfaceUbwc, &u, sizeof(u));
    if (e != 0) return e;
    if (u.version == 0) return ENOENT;
    *out = u;
    return 0;
}

void Gpu::disown(std::unique_ptr<Bo> bo) {
    if (bo == nullptr) return;
    if (bo->cpu_ != nullptr) {
        t_->unmap(bo->handle_, bo->cpu_);
        bo->cpu_ = nullptr;
    }
    bo->owned_ = false;
    if (bo->counted_) {
        allocated_ -= bo->size_;
        bo->counted_ = false;
    }
}

Bo::~Bo() { gpu_.release(*this); }

void Gpu::release(Bo& bo) {
    if (!bo.owned_) {
        return;
    }
    if (bo.counted_) {
        allocated_ -= bo.size_;
        bo.counted_ = false;
    }
    if (bo.cpu_ != nullptr) {
        t_->unmap(bo.handle_, bo.cpu_);
    }
    DrmGemClose c{bo.handle_, 0};
    t_->call(kGemClose, &c, sizeof(c));
}

int Gpu::newQueue(uint32_t priority, uint32_t* id) {
    MsmSubmitQueue q{0, priority, 0};
    int e = t_->call(kSubmitQueueNew, &q, sizeof(q));
    if (e == 0) {
        *id = q.id;
    }
    return e;
}

void Gpu::closeQueue(uint32_t id) {
    if (id != 0) {
        uint32_t q = id;
        t_->call(kSubmitQueueClose, &q, sizeof(q));
    }
}

int Gpu::submit(uint32_t queue, const Bo& commands, uint32_t offsetBytes, uint32_t words, const std::vector<BoUse>& uses,
                uint32_t* fence) {
    if (words == 0 || offsetBytes % 4 != 0 || offsetBytes + 4ull * words > commands.size()) {
        return EINVAL;
    }
    // the command buffer is the first of the buffers; the others follow it once each, read or written
    std::vector<MsmSubmitBo> bos;
    bos.push_back(MsmSubmitBo{kSubmitBoRead, commands.handle(), commands.gpuAddress()});
    for (const BoUse& u : uses) {
        if (u.bo == &commands) {
            continue;
        }
        bool seen = false;
        for (MsmSubmitBo& b : bos) {
            if (b.handle == u.bo->handle()) {
                b.flags |= u.write ? kSubmitBoWrite : kSubmitBoRead;
                seen = true;
            }
        }
        if (!seen) {
            bos.push_back(MsmSubmitBo{u.write ? kSubmitBoWrite : kSubmitBoRead, u.bo->handle(), u.bo->gpuAddress()});
        }
    }
    MsmSubmitCmd cmd{};
    cmd.type = kSubmitCmdBuf;
    cmd.submit_idx = 0;
    cmd.submit_offset = offsetBytes;
    cmd.size = words * 4;
    MsmGemSubmit s{};
    s.flags = kPipe3D;
    s.nr_bos = uint32_t(bos.size());
    s.nr_cmds = 1;
    s.bos = uint64_t(uintptr_t(bos.data()));
    s.cmds = uint64_t(uintptr_t(&cmd));
    s.fence_fd = -1;
    s.queueid = queue;
    int e = t_->call(kGemSubmit, &s, sizeof(s));
    if (e == 0) {
        *fence = s.fence;
    }
    return e;
}

int Gpu::wait(uint32_t queue, uint32_t fence, int64_t timeoutNs) {
    MsmWaitFence w{};
    w.fence = fence;
    w.timeout = deadline(*t_, timeoutNs);
    w.queueid = queue;
    return t_->call(kWaitFence, &w, sizeof(w));
}

int Gpu::cpuPrep(const Bo& bo, uint32_t op, int64_t timeoutNs) {
    MsmGemCpuPrep p{bo.handle(), op, deadline(*t_, timeoutNs)};
    return t_->call(kGemCpuPrep, &p, sizeof(p));
}

void Gpu::cpuFini(const Bo& bo) {
    MsmGemCpuFini f{bo.handle()};
    t_->call(kGemCpuFini, &f, sizeof(f));
}

int Gpu::cpuAccess(uint32_t handle, uint32_t surface) {
    HakiosCpuAccess a{handle, surface};
    return t_->call(kHakiosCpuAccess, &a, sizeof(a));
}

void Bo::cpuAccess() const { gpu_.cpuAccess(handle_, 0); }

}  // namespace hakios::metal
