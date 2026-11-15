#include <atomic>
#include <chrono>
#include <condition_variable>
#include <pthread.h>
#include <sched.h>
#include <deque>
#include <mutex>
#include <thread>
// hakios_metal.h over the C++ core.
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <pthread/qos.h>
#endif

#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/metallib.hpp"
#include "../tests/fixtures/null_fs_bc.hpp"
#include "hakios-metal/texture.hpp"
#include "hakios-metal/trace.hpp"   // (redfin hm-trace)

using namespace hakios::metal;

namespace {
thread_local std::string lastError;

const char* failed(const std::string& why) {
    lastError = why;
    return nullptr;
}
}  // namespace

namespace {
// Deferred recording (hakios_metal_queue_set_deferred): what a command buffer is told to record is done by one thread
// of the queue's own, in the order it was told, while the caller goes on -- CoreAnimation's render thread then spends
// on a draw only the copy of its arguments, and the encoding (some 40 us a draw on the phone, 2 November) runs on
// another core beside its next work. One thread for all of a queue's command buffers: CommandQueue's pools are not made
// for two recorders at once, and commits stay in order. What the caller asks back waits for that buffer's work first.
//
// (redfin ws-hm-defer) the thread is named (hakios_metal_queue_set_deferred2; default "hakios.metal.defer"), takes the
// scheduling of the thread that posts to it (pthread policy and priority, and on Darwin the QoS class: CoreAnimation's
// render thread is fixed priority 54), read at the first post and at each commit; a command buffer's tally (jobs, the
// thread's time on them, the queue's depth when its commit was posted) and the process's (hakios_metal_defer_counts:
// busy time, jobs, the caller's waits for the thread) are kept for HM_PROF / HM_TRACE. The tally is shared with the
// jobs, so a command buffer freed on the thread itself (its callback, run there when its commit is refused) leaves
// nothing the thread touches after; the thread's own state is shared with it, so the last reference to a Deferrer let
// go on its own thread detaches it instead of joining itself.
struct DeferTally {
    std::atomic<int> pending{0};          // jobs posted and not yet done
    std::atomic<int64_t> workNs{0};       // the thread's time on them
    std::atomic<uint32_t> jobs{0};        // posted
    std::atomic<uint32_t> depthAtCommit{0};   // jobs queued (all buffers, the one running included) when the commit was posted
};
std::atomic<uint64_t> gDeferBusyNs{0}, gDeferJobs{0}, gDeferWaits{0}, gDeferWaitNs{0}, gDeferPosts{0}, gDeferMaxDepth{0};
int64_t deferNowNs() {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// the scheduling a posting thread runs with, for the deferred thread to take
struct Sched {
    int policy = -1, priority = 0, qos = 0;
    bool operator==(const Sched& o) const { return policy == o.policy && priority == o.priority && qos == o.qos; }
};
Sched schedOfThisThread() {
    Sched s;
    sched_param p{};
    if (pthread_getschedparam(pthread_self(), &s.policy, &p) != 0) return Sched{};
    s.priority = p.sched_priority;
#ifdef __APPLE__
    qos_class_t q = QOS_CLASS_UNSPECIFIED;
    int rel = 0;
    if (pthread_get_qos_class_np(pthread_self(), &q, &rel) == 0) s.qos = int(q);
#endif
    return s;
}
class Deferrer {
public:
    explicit Deferrer(const char* name = nullptr) : st_(std::make_shared<State>()) {
        st_->name = name != nullptr && name[0] != '\0' ? name : "hakios.metal.defer";
        std::shared_ptr<State> st = st_;
        thread_ = std::thread([st] { run(st); });
    }
    ~Deferrer() {
        {
            std::lock_guard<std::mutex> g(st_->m);
            st_->quit = true;
        }
        st_->more.notify_all();
        if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();   // let go on its own thread: it ends by itself
        else thread_.join();
    }
    void post(const std::shared_ptr<DeferTally>& t, std::function<void()> f, bool commit = false) {
        t->pending.fetch_add(1);
        t->jobs.fetch_add(1, std::memory_order_relaxed);
        gDeferPosts.fetch_add(1, std::memory_order_relaxed);
        if (commit || !st_->schedSeen.load(std::memory_order_relaxed)) follow();
        size_t depth;
        {
            std::lock_guard<std::mutex> g(st_->m);
            st_->q.push_back(Job{t, std::move(f)});
            depth = st_->q.size() + (st_->busy ? 1 : 0);
        }
        if (commit) t->depthAtCommit.store(uint32_t(depth), std::memory_order_relaxed);
        uint64_t seen = gDeferMaxDepth.load(std::memory_order_relaxed);
        while (depth > seen && !gDeferMaxDepth.compare_exchange_weak(seen, depth)) {}
        st_->more.notify_one();
    }
    // until the jobs posted for one command buffer are done
    void drain(const std::shared_ptr<DeferTally>& t) {
        if (t->pending.load() == 0 || inWorker()) return;
        const int64_t t0 = deferNowNs();
        std::unique_lock<std::mutex> g(st_->m);
        st_->done.wait(g, [&] { return t->pending.load() == 0; });
        g.unlock();
        gDeferWaits.fetch_add(1, std::memory_order_relaxed);
        gDeferWaitNs.fetch_add(uint64_t(deferNowNs() - t0), std::memory_order_relaxed);
    }
    // until every job is done (before the caller records something itself)
    void drainAll() {
        if (inWorker()) return;
        std::unique_lock<std::mutex> g(st_->m);
        if (st_->q.empty() && !st_->busy) return;
        const int64_t t0 = deferNowNs();
        st_->done.wait(g, [&] { return st_->q.empty() && !st_->busy; });
        g.unlock();
        gDeferWaits.fetch_add(1, std::memory_order_relaxed);
        gDeferWaitNs.fetch_add(uint64_t(deferNowNs() - t0), std::memory_order_relaxed);
    }
    static bool inWorker() { return worker_; }
    // the thread's name, and the scheduling it asked for last (APPLIED: 1 taken, 0 not yet, -1 refused)
    void describe(char* name, size_t size, int* policy, int* priority, int* qos, int* applied) const {
        if (name != nullptr && size != 0) snprintf(name, size, "%s", st_->name.c_str());
        std::lock_guard<std::mutex> g(st_->m);
        if (policy) *policy = st_->want.policy;
        if (priority) *priority = st_->want.priority;
        if (qos) *qos = st_->want.qos;
        if (applied) *applied = st_->applied;
    }
private:
    struct Job { std::shared_ptr<DeferTally> t; std::function<void()> f; };
    struct State {
        std::mutex m;
        std::condition_variable more, done;
        std::deque<Job> q;
        bool busy = false, quit = false;
        std::string name;
        Sched want, have;
        std::atomic<bool> schedSeen{false};
        bool schedChanged = false;
        int applied = 0;
    };
    // the poster's scheduling, for the thread to take before its next job
    void follow() {
        const Sched s = schedOfThisThread();
        if (s.policy < 0) return;
        st_->schedSeen.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(st_->m);
        if (!(s == st_->want)) {
            st_->want = s;
            st_->schedChanged = true;
        }
    }
    static void apply(State& st, const Sched& s) {
        int ok = 1;
#ifdef __APPLE__
        if (s.qos != 0 && s.qos != int(QOS_CLASS_UNSPECIFIED) && pthread_set_qos_class_self_np(qos_class_t(s.qos), 0) != 0) ok = -1;
#endif
        sched_param p{};
        p.sched_priority = s.priority;
        if (pthread_setschedparam(pthread_self(), s.policy, &p) != 0) ok = -1;
        st.have = s;
        st.applied = ok;
    }
    static void run(std::shared_ptr<State> sp) {
        State& st = *sp;
        worker_ = true;
#ifdef __APPLE__
        pthread_setname_np(st.name.c_str());
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), st.name.substr(0, 15).c_str());   // (Linux: 15 bytes at most)
#endif
        std::unique_lock<std::mutex> g(st.m);
        for (;;) {
            st.more.wait(g, [&] { return st.quit || !st.q.empty(); });
            if (st.q.empty()) return;
            if (st.schedChanged) {
                st.schedChanged = false;
                apply(st, st.want);
            }
            Job j = std::move(st.q.front());
            st.q.pop_front();
            st.busy = true;
            g.unlock();
            const int64_t t0 = deferNowNs();
            j.f();
            const int64_t d = deferNowNs() - t0;
            std::shared_ptr<DeferTally> t = std::move(j.t);
            j = Job{};   // the job's captures let go before the lock is taken (their destructors may take others)
            t->workNs.fetch_add(d, std::memory_order_relaxed);
            gDeferBusyNs.fetch_add(uint64_t(d), std::memory_order_relaxed);
            gDeferJobs.fetch_add(1, std::memory_order_relaxed);
            g.lock();
            t->pending.fetch_sub(1);
            st.busy = false;
            st.done.notify_all();
        }
    }
    std::shared_ptr<State> st_;
    static thread_local bool worker_;
    std::thread thread_;   // last: started once the members above exist
};
thread_local bool Deferrer::worker_ = false;
}  // namespace

struct hakios_metal_device {
    std::unique_ptr<Gpu> gpu;
};
struct hakios_metal_buffer {
    std::shared_ptr<Buffer> buffer;
};
struct hakios_metal_library {
    std::vector<uint8_t> bytes;   // the library's own copy: MetalLibrary points into it
    MetalLibrary lib;
};
struct hakios_metal_kernel {
    std::shared_ptr<CompiledKernel> kernel;   // shared with every command buffer that dispatched it and has not completed
};
struct hakios_metal_queue {
    std::unique_ptr<CommandQueue> queue;
    std::shared_ptr<Deferrer> deferrer;   // hakios_metal_queue_set_deferred's; destroyed first (its thread drains)
    ~hakios_metal_queue() { deferrer.reset(); }
};
struct hakios_metal_cb {
    std::shared_ptr<CommandBuffer> cb;
    // hakios_metal_cb_set_vertex_textures's tables, sticky as an encoder's state is: the texture objects held so that
    // what a later draw names is alive, the samplers copied (their four words are all a draw takes)
    std::vector<std::shared_ptr<Texture>> vsTextures;
    std::vector<std::optional<Sampler>> vsSamplers;
    uint32_t threadgroupLength = 0;   // hakios_metal_cb_set_threadgroup_length's, for the dispatches that follow
    std::shared_ptr<Buffer> indirect;  // hakios_metal_cb_set_indirect's, for the next draw alone
    std::vector<CommandBuffer::DrawTime> drawTimes;   // hakios_metal_cb_draw_time's, read at its first call (I 0)
    std::vector<CommandBuffer::TraceDraw> traceDraws; // (redfin hm-trace) hakios_metal_cb_trace_draw's, read at I 0
    uint64_t indirectOffset = 0;
    std::shared_ptr<Deferrer> deferrer;   // the queue's, when it records deferred
    std::shared_ptr<DeferTally> pending = std::make_shared<DeferTally>();   // its jobs (ws-hm-defer: shared with them)
    Bindings scratchVs, scratchFs;   // (ws-hm-plugcpu) HM_PLUG_FAST: the draws' tables, their arrays reused draw after draw
};
struct hakios_metal_texture {
    std::shared_ptr<Texture> texture;   // its Buffer is kept by every command buffer that dispatched over it
};
struct hakios_metal_sampler {
    Sampler sampler;                    // four words, copied into each dispatch's table: nothing of it stays on the GPU
};
struct hakios_metal_program {
    std::shared_ptr<CompiledProgram> program;   // shared with every command buffer that drew with it and has not completed
    unsigned color = 0;                         // the colour format it was linked for (hakios_metal_program_specialize links again)
};

namespace {
// deferred recording: whether this call is to be posted rather than done here, and copies of what it names that outlive
// the caller's (the handles are a shared_ptr each: a copy keeps the object alive whatever the caller frees meanwhile)
bool deferring(hakios_metal_cb* cb) { return cb->deferrer != nullptr && !Deferrer::inWorker(); }
// (ws-hm-plugcpu) HM_PLUG_FAST=1: a draw through the C interface with no heap allocation of its own once warm (its
// binding tables reused, each object kept once a command buffer: CommandBuffer::keepOnce); 0 (default) as before.
// The same words either way (c_api_test plugfast_*). Read once; the tests set it with cApiPlugFastForTesting.
std::atomic<int> plugFastSet{-1};
bool plugFast() {
    int v = plugFastSet.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = getenv("HM_PLUG_FAST");
        v = e != nullptr && e[0] == '1';
        plugFastSet.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}
void syncAll(hakios_metal_cb* cb) { if (cb->deferrer != nullptr) cb->deferrer->drainAll(); }
void syncOne(hakios_metal_cb* cb) { if (cb->deferrer != nullptr) cb->deferrer->drain(cb->pending); }
template <class H>
struct Handles {
    std::vector<H> copies;
    std::vector<H*> ptrs;
    bool given = false;
    void take(H* const* a, unsigned n) {
        given = a != nullptr;
        copies.resize(n);
        ptrs.assign(n, nullptr);
        for (unsigned i = 0; a != nullptr && i < n; i++) {
            if (a[i] != nullptr) {
                copies[i] = *a[i];
                ptrs[i] = &copies[i];
            }
        }
    }
    H* const* get() const { return given ? ptrs.data() : nullptr; }
};
template <class T>
std::vector<T> copyOf(const T* a, unsigned n) { return a != nullptr ? std::vector<T>(a, a + n) : std::vector<T>(); }
template <class T>
const T* ptrOf(const std::vector<T>& v, const T* original) { return original != nullptr ? v.data() : nullptr; }
// a render pass and the textures and buffer it names (made in place: it points into itself)
struct PassCopy {
    hakios_metal_render_pass pass;
    hakios_metal_texture color0, depth, stencil, more[7];
    hakios_metal_buffer visibility;
    bool given = false;
    void take(const hakios_metal_render_pass* p) {
        if (p == nullptr) return;
        given = true;
        pass = *p;
        if (p->color0 != nullptr) { color0 = *p->color0; pass.color0 = &color0; }
        if (p->depth != nullptr) { depth = *p->depth; pass.depth = &depth; }
        if (p->stencil != nullptr) { stencil = *p->stencil; pass.stencil = &stencil; }
        for (int i = 0; i < 7; i++) {
            if (p->more[i].texture != nullptr) { more[i] = *p->more[i].texture; pass.more[i].texture = &more[i]; }
        }
        if (p->visibility != nullptr) { visibility = *p->visibility; pass.visibility = &visibility; }
    }
    const hakios_metal_render_pass* get() const { return given ? &pass : nullptr; }
};
// a draw done on the queue's thread is refused there: what the caller would have done with a refusal, done there
void refusedThere(hakios_metal_cb* cb, int r) {
    if (r == 0) return;
    // (redfin ws-hm-defer) into the plug-in's log too (the notice sink), the first 20 and then every 100th: the caller's
    // own "refused:" line is not printed for a draw it did not wait for (its HM_FAILLOG line still comes at completion)
    static std::atomic<uint32_t> said{0};
    const uint32_t n = said.fetch_add(1);
    if (n < 20 || n % 100 == 0) render::notify("refused on the deferred thread (#" + std::to_string(n + 1) + "): " + lastError);
    else std::fprintf(stderr, "HakiosMetal: refused (deferred): %s\n", lastError.c_str());
    cb->cb->refuseWork(lastError);
}
}  // namespace

namespace hakios::metal { extern std::atomic<uint32_t> gBuffersMade, gBuffersReused; extern std::atomic<uint64_t> gBytesMade; }
extern "C" {
void hakios_metal_buffer_counts(unsigned* made, unsigned* reused, unsigned long long* bytes) {
    *made = hakios::metal::gBuffersMade.exchange(0);
    *reused = hakios::metal::gBuffersReused.exchange(0);
    *bytes = hakios::metal::gBytesMade.exchange(0);
}


const char* hakios_metal_error(void) { return lastError.c_str(); }

// Tiles, GMEM offsets and the shader compiler are built for one chip (a6xx.hpp). A kernel that does not say (0) is taken at its
// word that it is that one: only a chip it names as another is refused, so that the window server stays on OpenGL ES there.
// (redfin) every chip a6xx.hpp has a row for (the A635, the A620)
bool hakios_metal_chip_known(uint32_t chip) { return chip == 0 || a6xx::chipFor(chip) != nullptr; }
[[maybe_unused]] static bool chipKnown(uint32_t chip) { return hakios_metal_chip_known(chip); }

static hakios_metal_device* deviceOn(std::unique_ptr<Transport> t) {
    auto* d = new hakios_metal_device;
    d->gpu = std::make_unique<Gpu>(std::move(t));
    return d;
}

hakios_metal_device* hakios_metal_open(void) {
#ifdef __APPLE__
#if TARGET_OS_IPHONE || defined(HAKIOS_METAL_IOKIT)
    std::string why;
#ifdef HAKIOS_METAL_NATIVE_ADRENO
    // (redfin) our kernel's GPU (IOAcceleratorES, NativeAdreno ABI): phone/hmetal/redfin/native_adreno_transport.cpp
    std::unique_ptr<Transport> t = openNativeAdrenoGpu(&why);
#else
    std::unique_ptr<Transport> t = openKernelGpu(&why);
#endif
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    hakios_metal_device* d = deviceOn(std::move(t));
    if (!chipKnown(d->gpu->chipId())) {
        char text[160];
        snprintf(text, sizeof(text), "the GPU's chip ID is %#x: this driver has tables for chip IDs %#x and %#x (a6xx.hpp), another's would hang it",
                 d->gpu->chipId(), a6xx::kChipA635.chipId, a6xx::kChipA620.chipId);
        failed(text);
        delete d;
        return nullptr;
    }
    return d;
#endif
#endif
    failed("no kernel GPU on this system (the driver talks to hakios's HakiosGPU, on the phone)");
    return nullptr;
}

hakios_metal_device* hakios_metal_open_fake(void) { return deviceOn(std::make_unique<FakeGpu>(true)); }

void hakios_metal_close(hakios_metal_device* d) { delete d; }

uint64_t hakios_metal_gpu_id(hakios_metal_device* d) { return d->gpu->chipId(); }
int hakios_metal_ubwc(hakios_metal_device* d, unsigned* hbb, unsigned* swizzle, unsigned* macrotile) {
    const UbwcConfig& u = d->gpu->ubwc();
    if (hbb != nullptr) *hbb = u.highestBankBit;
    if (swizzle != nullptr) *swizzle = u.bankSwizzleLevels;
    if (macrotile != nullptr) *macrotile = u.macrotileMode;
    return u.fromDriver ? 1 : 0;
}

int hakios_metal_surface_cpu_access(hakios_metal_device* d, uint32_t surface) {   // (ws-hm-int6)
    return d->gpu->cpuAccess(0, surface);
}

void hakios_metal_ubwc_counts(unsigned long long out[3]) {   // (ws-hm-int6)
    uint64_t made[2];
    ubwcMadeCounts(made);
    out[0] = render::blendsMadeWrites();
    out[1] = made[0];
    out[2] = made[1];
}

void hakios_metal_fetch_counts(unsigned long long out[3]) {   // (ws-hm-sbar)
    uint64_t v[3];
    render::fetchCounts(v);
    for (int i = 0; i < 3; i++) out[i] = v[i];
}

int hakios_metal_ubwc_rt_ok(unsigned width, unsigned height, unsigned usage) {   // (ws-hm-ubwc2)
    return ubwcRenderTargetEligible(width, height, usage) ? 1 : 0;
}

int hakios_metal_has_compiler(void) { return hasCompiler() ? 1 : 0; }

void hakios_metal_compile_stats(double* waitMs, double* compileMs, unsigned* hits, unsigned* misses) {
    const CompileStats s = compileStats();
    if (waitMs != nullptr) *waitMs = double(s.waitNs) / 1e6;
    if (compileMs != nullptr) *compileMs = double(s.compileNs) / 1e6;
    if (hits != nullptr) *hits = s.hits;
    if (misses != nullptr) *misses = s.misses;
}

void hakios_metal_compile_background(int on) { setCompileBackground(on != 0); }

void hakios_metal_trace_set_sink(hakios_metal_trace_sink sink, void* ctx) {   // (redfin hm-trace)
    hakios::trace::state().ctx.store(ctx);
    hakios::trace::state().sink.store(sink, std::memory_order_release);
}
int64_t hakios_metal_trace_now_ns(void) { return hakios::trace::nowNs(); }
void hakios_metal_cache_totals(unsigned long long out[12]) {
    uint64_t t[12];
    cacheTotals(t);
    for (int i = 0; i < 12; i++) out[i] = t[i];
}

unsigned hakios_metal_compile_stats_more(double* out, unsigned count) {
    const CompileStats s = compileStats();
    const double v[7] = {double(s.cpuNs) / 1e6, double(s.translateNs) / 1e6, double(s.foldNs) / 1e6, double(s.memoHits), double(s.seedHits),
                         double(s.hits), double(s.misses)};
    unsigned n = 0;
    for (; out != nullptr && n < count && n < 7; n++) out[n] = v[n];
    return n;
}

const char* hakios_metal_cache_key(void) { return stageCacheKeyText(); }

void hakios_metal_cache_rewarm(int (*busy)(void*), void* ctx, unsigned out[5]) {
    const RewarmCounts n = stageCacheRewarm(busy, ctx);
    if (out != nullptr) {
        out[0] = n.manifests, out[1] = n.present, out[2] = n.made, out[3] = n.failed, out[4] = n.unreadable;
    }
}

void hakios_metal_pass_splits(unsigned long long out[4]) {
    uint64_t v[4];
    CommandBuffer::passSplits(v);
    for (int i = 0; i < 4; i++) out[i] = v[i];
}

void hakios_metal_xr_stats(unsigned long long out[10]) {
    uint64_t v[10];
    CommandBuffer::xrStats(v);
    for (int i = 0; i < 10; i++) out[i] = v[i];
}

void hakios_metal_ycbcr_stats(unsigned long long out[8]) {
    uint64_t v[8];
    CommandBuffer::ycbcrStats(v);
    for (int i = 0; i < 8; i++) out[i] = v[i];
}

int hakios_metal_ycbcr_mode(void) { return Texture::ycbcrMode(); }

void hakios_metal_ycbcr_convert(const void* luma, uint64_t lumaRow, const void* chroma, uint64_t chromaRow, unsigned width, unsigned height,
                                unsigned matrix, int full, int planes, void* out, uint64_t outRow) {
    Texture::ycbcrConvertRows(static_cast<const uint8_t*>(luma), lumaRow, static_cast<const uint8_t*>(chroma), chromaRow, width, height, int(matrix),
                              full != 0, planes != 0, static_cast<uint8_t*>(out), outRow);
}

void hakios_metal_xr_gpu_stats(unsigned long long out[6]) {
    uint64_t v[6];
    CommandBuffer::xrGpuStats(v);
    for (int i = 0; i < 6; i++) out[i] = v[i];
}

void hakios_metal_set_notice(void (*fn)(const char*)) { render::setNotice(fn); }

hakios_metal_library* hakios_metal_library_open(const void* data, size_t size) {
    auto* l = new hakios_metal_library;
    l->bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    if (!l->lib.parse(l->bytes.data(), l->bytes.size())) {
        failed(l->lib.why());
        delete l;
        return nullptr;
    }
    return l;
}

void hakios_metal_library_free(hakios_metal_library* l) { delete l; }
unsigned hakios_metal_library_count(hakios_metal_library* l) { return unsigned(l->lib.functions().size()); }

const char* hakios_metal_library_name(hakios_metal_library* l, unsigned i) {
    return i < l->lib.functions().size() ? l->lib.functions()[i].name.c_str() : nullptr;
}

int hakios_metal_library_type(hakios_metal_library* l, unsigned i) {
    return i < l->lib.functions().size() ? int(l->lib.functions()[i].type) : -1;
}

static FunctionConstants constantsOf(const hakios_metal_constant* c, unsigned count) {
    FunctionConstants out;
    for (unsigned i = 0; c != nullptr && i < count; i++) out.push_back(FunctionConstant{c[i].index, c[i].bits});
    return out;
}

hakios_metal_kernel* hakios_metal_kernel_compile_bitcode(const void* bitcode, size_t size) {
    return hakios_metal_kernel_compile_bitcode_constants(bitcode, size, nullptr, 0);
}

hakios_metal_kernel* hakios_metal_kernel_compile_bitcode_constants(const void* bitcode, size_t size, const hakios_metal_constant* constants, unsigned count) {
    std::string why;
    std::unique_ptr<CompiledKernel> k = compileAir(bitcode, size, &why, constantsOf(constants, count));
    if (k == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_kernel;
    r->kernel = std::move(k);
    return r;
}

hakios_metal_kernel* hakios_metal_kernel_compile(hakios_metal_library* l, const char* function) {
    const MetalFunction* f = l->lib.find(function != nullptr ? function : "");
    const uint8_t* bytes = nullptr;
    uint64_t length = 0;
    if (f == nullptr || !l->lib.bitcode(*f, &bytes, &length)) {
        failed(std::string("no function ") + (function ? function : "(null)") + " in the library's " + std::to_string(l->lib.functions().size()));
        return nullptr;
    }
    if (f->type != MetalFunction::kKernel) {
        failed(std::string(function) + " is not a kernel (TYPE " + std::to_string(f->type) + "); only those run here yet");
        return nullptr;
    }
    return hakios_metal_kernel_compile_bitcode(bytes, size_t(length));
}

void hakios_metal_kernel_free(hakios_metal_kernel* k) { delete k; }
const char* hakios_metal_kernel_name(hakios_metal_kernel* k) { return k->kernel->name.c_str(); }
unsigned hakios_metal_kernel_resource_count(hakios_metal_kernel* k) { return unsigned(k->kernel->resources.size()); }

uint64_t hakios_metal_device_allocated_bytes(hakios_metal_device* d) { return d != nullptr && d->gpu ? d->gpu->allocatedBytes() : 0; }

int hakios_metal_argument_buffer_layout(const void* bitcode, size_t size, unsigned set, unsigned* ids, unsigned* offsets, unsigned max) {
    std::vector<std::pair<uint32_t, uint32_t>> members;
    std::string why;
    if (!argumentBufferLayout(bitcode, size, set, &members, &why)) {
        failed(why);
        return -1;
    }
    for (unsigned i = 0; i < members.size() && i < max; i++) {
        if (ids) ids[i] = members[i].first;
        if (offsets) offsets[i] = members[i].second;
    }
    return int(members.size());
}

int hakios_metal_kernel_resource(hakios_metal_kernel* k, unsigned i, unsigned* kind, unsigned* index, unsigned* sizeBytes) {
    if (i >= k->kernel->resources.size()) return -1;
    const Resource& r = k->kernel->resources[i];
    if (kind) *kind = unsigned(r.kind);   // Resource::Kind is numbered as hakios_metal_resource_kind is
    if (index) *index = r.index;
    if (sizeBytes) *sizeBytes = r.sizeBytes;
    return 0;
}

void hakios_metal_kernel_threadgroup_size(hakios_metal_kernel* k, unsigned size[3]) {
    for (int i = 0; i < 3; i++) size[i] = k->kernel->kernel.localSizeVariable ? 0 : k->kernel->kernel.localSize[i];
}

unsigned hakios_metal_kernel_max_threads(hakios_metal_kernel*) { return compute::kMaxLocalThreads; }
unsigned hakios_metal_kernel_execution_width(hakios_metal_kernel* k) { return k->kernel->kernel.doubleThreadsize ? 128 : 64; }
unsigned hakios_metal_kernel_threadgroup_memory(hakios_metal_kernel* k) { return k->kernel->threadgroupBytes; }

hakios_metal_buffer* hakios_metal_buffer_new(hakios_metal_device* d, uint64_t length) {
    std::string why;
    auto b = newBuffer(*d->gpu, length ? length : 1, &why);
    if (b == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_buffer;
    r->buffer = std::move(b);
    return r;
}

void hakios_metal_buffer_free(hakios_metal_buffer* b) { delete b; }
void* hakios_metal_buffer_contents(hakios_metal_buffer* b) { return b->buffer->contents(); }
uint64_t hakios_metal_buffer_length(hakios_metal_buffer* b) { return b->buffer->length(); }
uint64_t hakios_metal_buffer_gpu_address(hakios_metal_buffer* b) { return b->buffer->gpuAddress(); }

// --- textures and samplers ---------------------------------------------------------------------------------------------

hakios_metal_texture* hakios_metal_texture_new(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                               unsigned usage) {
    return hakios_metal_texture_new_levels(d, type, pixelFormat, width, height, 1, usage);
}

unsigned hakios_metal_texture_levels(hakios_metal_texture* t) { return t->texture->levels(); }

hakios_metal_texture* hakios_metal_texture_new_3d(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned depth,
                                                  unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::k3D;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_array(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned layers,
                                                     unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::k2DArray;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = layers;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_layers(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                                      unsigned layers, unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType(type);
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = layers;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_layered(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                                       unsigned layers, unsigned levels, unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType(type);
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = layers;
    desc.mipLevels = levels;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_cube(hakios_metal_device* d, unsigned pixelFormat, unsigned size, unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::kCube;
    desc.format = PixelFormat(pixelFormat);
    desc.width = desc.height = size;
    desc.depth = 6;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_multisample(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned samples,
                                                          unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::k2DMultisample;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.samples = samples;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

unsigned hakios_metal_texture_samples(hakios_metal_texture* t) { return t->texture->samples(); }
unsigned hakios_metal_texture_depth(hakios_metal_texture* t) { return t->texture->depth(); }

int hakios_metal_texture_replace_3d(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d, const void* bytes,
                                    uint64_t bytesPerRow, uint64_t bytesPerImage) {
    std::string why;
    if (!t->texture->replaceSlices(level, x, y, z, w, h, d, bytes, bytesPerRow, bytesPerImage, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

int hakios_metal_texture_read_3d(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d, void* bytes,
                                 uint64_t bytesPerRow, uint64_t bytesPerImage) {
    std::string why;
    if (!t->texture->readSlices(level, x, y, z, w, h, d, bytes, bytesPerRow, bytesPerImage, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

hakios_metal_texture* hakios_metal_texture_new_levels(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                                      unsigned levels, unsigned usage) {
    TextureDesc desc;
    desc.mipLevels = levels;
    desc.type = TextureType(type);           // Metal's own numbers, as the header says: an unknown one is refused by name
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    std::string why;
    const int64_t trT0 = hakios::trace::nowNs();   // (redfin hm-trace)
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    if (hakios::trace::on()) {
        const uint64_t bytes = t->buffer() != nullptr ? t->buffer()->length() : 0;
        if (bytes >= hakios::trace::bigBytes() || (usage & 4u) != 0)   // (4: MTLTextureUsageRenderTarget)
            hakios::trace::event((usage & 4u) != 0 ? (t->buffer() != nullptr && t->buffer()->reused() ? "rt-reuse" : "rt-new") : "texture-new",   // (ws-hm-pool)
                                 std::to_string(width) + "x" + std::to_string(height) + " fmt " + std::to_string(pixelFormat) + " levels " + std::to_string(levels),
                                 trT0, hakios::trace::nowNs() - trT0, bytes);
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

unsigned hakios_metal_texture_origin(hakios_metal_texture* t) {
    const Texture& x = *t->texture;
    return x.compressed() ? 3 : x.overBufferRows() ? 2 : x.surface() != 0 ? 1 : 0;
}

hakios_metal_texture* hakios_metal_texture_new_compressed(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned usage) {
    TextureDesc desc;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    desc.compressed = true;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

namespace {
hakios_metal_texture* textureOverSurface(hakios_metal_device* d, unsigned surfaceId, unsigned plane, unsigned pixelFormat, unsigned width, unsigned height,
                                         unsigned usage, const Texture::SurfacePlane* chroma) {
    if (plane != 0) {
        failed("plane " + std::to_string(plane) + " of IOSurface " + std::to_string(surfaceId) + ": a single-plane surface (plane 0) is what is served");
        return nullptr;
    }
    TextureDesc desc;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::wrapSurface(*d->gpu, surfaceId, desc, &why, chroma);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}
}  // namespace

hakios_metal_texture* hakios_metal_texture_new_iosurface(hakios_metal_device* d, unsigned surfaceId, unsigned plane, unsigned pixelFormat, unsigned width,
                                                         unsigned height, unsigned usage) {
    return textureOverSurface(d, surfaceId, plane, pixelFormat, width, height, usage, nullptr);
}

hakios_metal_texture* hakios_metal_texture_new_iosurface2(hakios_metal_device* d, unsigned surfaceId, unsigned plane, unsigned pixelFormat, unsigned width,
                                                          unsigned height, unsigned usage, uint64_t chromaOffset, unsigned chromaRowBytes) {
    const Texture::SurfacePlane chroma{chromaOffset, chromaRowBytes};
    return textureOverSurface(d, surfaceId, plane, pixelFormat, width, height, usage, &chroma);
}

hakios_metal_texture* hakios_metal_texture_new_ycbcr(hakios_metal_device* d, unsigned surfaceId, unsigned width, unsigned height, uint64_t lumaOffset,
                                                     unsigned lumaRow, uint64_t chromaOffset, unsigned chromaRow, unsigned matrix, int full, int planes) {
    Texture::Ycbcr yc;
    yc.matrix = uint8_t(matrix > 255 ? 0 : matrix);
    yc.full = full != 0;
    yc.planes = planes != 0;
    yc.lumaOffset = lumaOffset;
    yc.lumaRow = lumaRow;
    yc.chromaOffset = chromaOffset;
    yc.chromaRow = chromaRow;
    std::string why;
    std::shared_ptr<Texture> t = Texture::wrapSurfaceYcbcr(*d->gpu, surfaceId, width, height, yc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

void hakios_metal_texture_ycbcr_touch(hakios_metal_texture* t, uint64_t gen) {
    if (t != nullptr && t->texture != nullptr) t->texture->ycbcrTouch(gen);
}

hakios_metal_texture* hakios_metal_texture_new_buffer(hakios_metal_buffer* b, unsigned type, unsigned pixelFormat, unsigned width, unsigned height, unsigned usage,
                                                      uint64_t offset, uint64_t bytesPerRow) {
    TextureDesc desc;
    desc.type = TextureType(type);
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::overBuffer(b->buffer, offset, bytesPerRow, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_view(hakios_metal_texture* t, unsigned pixelFormat, unsigned type, unsigned baseLevel, unsigned levels,
                                                    unsigned baseSlice, unsigned slices) {
    std::string why;
    std::shared_ptr<Texture> v = Texture::view(t->texture, PixelFormat(pixelFormat), TextureType(type), baseLevel, levels, baseSlice, slices, &why);
    if (v == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(v);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_swizzled(hakios_metal_texture* t, unsigned red, unsigned green, unsigned blue, unsigned alpha) {
    std::string why;
    const uint8_t s[4] = {uint8_t(red > 255 ? 255 : red), uint8_t(green > 255 ? 255 : green), uint8_t(blue > 255 ? 255 : blue),
                          uint8_t(alpha > 255 ? 255 : alpha)};
    std::shared_ptr<Texture> v = Texture::swizzled(t->texture, s, &why);
    if (v == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(v);
    return r;
}

int hakios_metal_texture_plan_bytes(unsigned type, unsigned pixelFormat, unsigned width, unsigned height, unsigned depth, unsigned levels, unsigned samples,
                                    unsigned usage, uint64_t* bytes) {
    TextureDesc d;
    d.type = TextureType(type);
    d.format = PixelFormat(pixelFormat);
    d.width = width;
    d.height = height;
    d.depth = depth;
    d.mipLevels = levels;
    d.samples = samples;
    d.usage = usage;
    const FormatInfo* info = nullptr;
    uint64_t pitch = 0;
    std::string why;
    if (!Texture::plan(d, &info, &pitch, bytes, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

uint64_t hakios_metal_texture_allocated_bytes(hakios_metal_texture* t) { return t->texture->buffer()->length(); }

unsigned hakios_metal_texture_iosurface(hakios_metal_texture* t) { return t->texture->surface(); }
uint64_t hakios_metal_texture_resource_id(hakios_metal_texture* t) { return t->texture->resourceId(); }
uint64_t hakios_metal_sampler_resource_id(hakios_metal_sampler* s) { return s->sampler.id; }

void hakios_metal_texture_free(hakios_metal_texture* t) { delete t; }
void* hakios_metal_texture_contents(hakios_metal_texture* t) { return t->texture->contents(); }
uint64_t hakios_metal_texture_bytes_per_row(hakios_metal_texture* t) { return t->texture->bytesPerRow(); }
uint64_t hakios_metal_texture_gpu_address(hakios_metal_texture* t) { return t->texture->gpuAddress(); }
unsigned hakios_metal_texture_width(hakios_metal_texture* t) { return t->texture->width(); }
unsigned hakios_metal_texture_height(hakios_metal_texture* t) { return t->texture->height(); }
unsigned hakios_metal_texture_format(hakios_metal_texture* t) { return unsigned(t->texture->format()); }
unsigned hakios_metal_texture_type(hakios_metal_texture* t) { return unsigned(t->texture->type()); }
unsigned hakios_metal_texture_usage(hakios_metal_texture* t) { return t->texture->usage(); }
unsigned hakios_metal_texture_bytes_per_pixel(hakios_metal_texture* t) { return t->texture->info().bytesPerPixel; }
int hakios_metal_format_block(unsigned pixelFormat, unsigned* blockW, unsigned* blockH, unsigned* bytes) {
    const FormatInfo* f = formatInfo(PixelFormat(pixelFormat));
    if (f == nullptr) {
        failed("pixel format " + std::to_string(pixelFormat) + ": not one of the formats served");
        return 0;
    }
    *blockW = f->blockW;
    *blockH = f->blockH;
    *bytes = f->bytesPerPixel;
    return 1;
}

int hakios_metal_depth16_on(void) { return depth16On() ? 1 : 0; }   // (redfin hm-int5)

int hakios_metal_texture_replace(hakios_metal_texture* t, unsigned x, unsigned y, unsigned w, unsigned h, const void* bytes, uint64_t bytesPerRow) {
    return hakios_metal_texture_replace_level(t, 0, x, y, w, h, bytes, bytesPerRow);
}

int hakios_metal_texture_replace_level(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, const void* bytes,
                                       uint64_t bytesPerRow) {
    std::string why;
    const int64_t trT0 = hakios::trace::nowNs();   // (redfin hm-trace)
    if (!t->texture->replace(level, x, y, w, h, bytes, bytesPerRow, &why)) {
        failed(why);
        return -1;
    }
    if (hakios::trace::on() && uint64_t(h) * bytesPerRow >= hakios::trace::bigBytes())
        hakios::trace::event("upload", std::to_string(w) + "x" + std::to_string(h) + " level " + std::to_string(level), trT0, hakios::trace::nowNs() - trT0,
                             uint64_t(h) * bytesPerRow);
    return 0;
}

int hakios_metal_texture_read(hakios_metal_texture* t, unsigned x, unsigned y, unsigned w, unsigned h, void* bytes, uint64_t bytesPerRow) {
    return hakios_metal_texture_read_level(t, 0, x, y, w, h, bytes, bytesPerRow);
}

int hakios_metal_texture_read_level(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, void* bytes,
                                    uint64_t bytesPerRow) {
    std::string why;
    if (!t->texture->read(level, x, y, w, h, bytes, bytesPerRow, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

hakios_metal_sampler* hakios_metal_sampler_new(hakios_metal_device*, const struct hakios_metal_sampler_desc* c) {
    SamplerDesc d;
    d.minFilter = c->min_filter;
    d.magFilter = c->mag_filter;
    d.mipFilter = c->mip_filter;
    d.sAddress = c->s_address;
    d.tAddress = c->t_address;
    d.rAddress = c->r_address;
    d.normalizedCoordinates = c->normalized_coordinates != 0;
    d.lodMinClamp = c->lod_min_clamp;
    d.lodMaxClamp = c->lod_max_clamp;
    d.maxAnisotropy = c->max_anisotropy;
    d.compareFunction = c->compare_function;
    d.borderColor = c->border_color;
    auto* r = new hakios_metal_sampler;
    std::string why;
    if (!makeSampler(d, &r->sampler, &why)) {
        delete r;
        failed(why);
        return nullptr;
    }
    return r;
}

void hakios_metal_sampler_free(hakios_metal_sampler* s) { delete s; }
const uint32_t* hakios_metal_sampler_words(hakios_metal_sampler* s) { return s->sampler.words; }

int hakios_metal_kernel_resource_texture(hakios_metal_kernel* k, unsigned i, unsigned* textureType, unsigned* element, unsigned* access) {
    if (i >= k->kernel->resources.size()) return -1;
    const Resource& r = k->kernel->resources[i];
    if (r.kind != Resource::kTexture && r.kind != Resource::kImage && r.kind != Resource::kSampler) return -1;
    // MTLTextureType's numbers (MTLTexture.h) from the dimension the instructions address: 1D 0 / 1DArray 1, 2D 2 /
    // 2DArray 3, Cube 5 / CubeArray 6, 3D 7; ~0u for a sampler or a declared texture no instruction touches
    unsigned type = ~0u;
    switch (r.textureDim) {
    case kTexDim1D: type = r.textureArray ? 1 : 0; break;
    case kTexDim2D: type = r.textureArray ? 3 : 2; break;
    case kTexDimCube: type = r.textureArray ? 6 : 5; break;
    case kTexDim3D: type = 7; break;
    default: break;
    }
    // the access the instructions make, when they address the index (Kernel::textures); the declaration's otherwise
    unsigned bits = (r.read ? 1u : 0u) | (r.write ? 2u : 0u);
    if (r.kind != Resource::kSampler && r.index < kMaxTextureSlots) {
        const KernelTexture& t = k->kernel->kernel.textures[r.index];
        if (t.kind != kTexNone) bits = (t.read ? 1u : 0u) | (t.write ? 2u : 0u);
    }
    if (textureType) *textureType = type;
    if (element) *element = r.element;
    if (access) *access = bits;
    return 0;
}

hakios_metal_queue* hakios_metal_queue_new(hakios_metal_device* d) {
    std::string why;
    auto q = CommandQueue::make(*d->gpu, &why);
    if (q == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_queue;
    r->queue = std::move(q);
    return r;
}

void hakios_metal_queue_free(hakios_metal_queue* q) { delete q; }

void hakios_metal_queue_set_deferred(hakios_metal_queue* q, int on) { hakios_metal_queue_set_deferred2(q, on, nullptr); }

// (redfin ws-hm-defer) the thread named NAME (NULL: "hakios.metal.defer")
void hakios_metal_queue_set_deferred2(hakios_metal_queue* q, int on, const char* name) {
    if (on && q->deferrer == nullptr) q->deferrer = std::make_shared<Deferrer>(name);
    if (!on) q->deferrer.reset();
}

int hakios_metal_queue_deferred(hakios_metal_queue* q) { return q->deferrer != nullptr ? 1 : 0; }

int hakios_metal_queue_defer_thread(hakios_metal_queue* q, char* name, unsigned long size, int* policy, int* priority, int* qos, int* applied) {
    if (q->deferrer == nullptr) return -1;
    q->deferrer->describe(name, size_t(size), policy, priority, qos, applied);
    return 0;
}

int hakios_metal_cb_deferred(hakios_metal_cb* cb) { return cb->deferrer != nullptr ? 1 : 0; }

int hakios_metal_cb_pending(hakios_metal_cb* cb) { return cb->deferrer != nullptr ? cb->pending->pending.load() : 0; }

void hakios_metal_cb_post(hakios_metal_cb* cb, void (*fn)(void*), void* context) {
    if (fn == nullptr) return;
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [fn, context] { fn(context); });
    fn(context);
}

int hakios_metal_cb_defer_stats(hakios_metal_cb* cb, int64_t* work_ns, unsigned* jobs, unsigned* depth_at_commit) {
    if (work_ns) *work_ns = 0;
    if (jobs) *jobs = 0;
    if (depth_at_commit) *depth_at_commit = 0;
    if (cb->deferrer == nullptr) return -1;
    syncOne(cb);
    const DeferTally& t = *cb->pending;
    if (work_ns) *work_ns = t.workNs.load();
    if (jobs) *jobs = t.jobs.load();
    if (depth_at_commit) *depth_at_commit = t.depthAtCommit.load();
    return 0;
}

void hakios_metal_defer_counts(unsigned long long out[6]) {
    out[0] = gDeferBusyNs.load();
    out[1] = gDeferJobs.load();
    out[2] = gDeferWaits.load();
    out[3] = gDeferWaitNs.load();
    out[4] = gDeferPosts.load();
    out[5] = gDeferMaxDepth.exchange(0);
}

hakios_metal_cb* hakios_metal_cb_new(hakios_metal_queue* q) {
    auto* r = new hakios_metal_cb;
    r->cb = q->queue->commandBuffer();
    r->deferrer = q->deferrer;
    return r;
}

void hakios_metal_cb_free(hakios_metal_cb* cb) {
    syncOne(cb);
    // one in flight is waited for first: its callback is handed this very pointer. One already completed is not, and
    // must not be: the last reference may go inside its own callback (an Objective-C object released there), and
    // waiting there for the end of what is running would wait forever.
    CommandBuffer::Status s = cb->cb->status();
    if (s == CommandBuffer::kCommitted || s == CommandBuffer::kScheduled) {
        cb->cb->waitUntilCompleted();
    }
    delete cb;
}

int hakios_metal_cb_copy(hakios_metal_cb* cb, hakios_metal_buffer* from, uint64_t fromOffset, hakios_metal_buffer* to,
                         uint64_t toOffset, uint64_t length) {
    syncAll(cb);
    if (!cb->cb->copy(*from->buffer, fromOffset, *to->buffer, toOffset, length)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    // the buffers live until the GPU is done with them, whatever the caller frees meanwhile
    cb->cb->keep(from->buffer);
    cb->cb->keep(to->buffer);
    return 0;
}

int hakios_metal_cb_fill(hakios_metal_cb* cb, hakios_metal_buffer* b, uint64_t offset, uint64_t length, uint8_t value) {
    syncAll(cb);
    if (!cb->cb->fill(*b->buffer, offset, length, value)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keep(b->buffer);
    return 0;
}

int hakios_metal_cb_blit(hakios_metal_cb* cb, hakios_metal_texture* from, unsigned fromLevel, unsigned sx, unsigned sy, unsigned sw, unsigned sh,
                         hakios_metal_texture* to, unsigned toLevel, unsigned dx, unsigned dy, unsigned dw, unsigned dh, int linear) {
    syncAll(cb);
    if (!cb->cb->blit(*from->texture, fromLevel, render::BlitRect{sx, sy, sw, sh}, *to->texture, toLevel, render::BlitRect{dx, dy, dw, dh}, linear != 0)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(from->texture);
    cb->cb->keepAlive(to->texture);
    return 0;
}

int hakios_metal_cb_blit_rotated(hakios_metal_cb* cb, hakios_metal_texture* from, hakios_metal_texture* to, unsigned rotation) {
    syncAll(cb);
    render::Options o;
    o.rotate2d = uint8_t(rotation > 5 ? 6 : rotation);   // (a value past 5 still refused by encodeBlit, not wrapped into range)
    const Texture &f = *from->texture, &t = *to->texture;
    if (!cb->cb->blit(f, 0, render::BlitRect{0, 0, f.width(), f.height()}, t, 0, render::BlitRect{0, 0, t.width(), t.height()}, false, o)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(from->texture);
    cb->cb->keepAlive(to->texture);
    return 0;
}

void hakios_metal_cb_skip_refused(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setSkipRefused(on != 0); });
    cb->cb->setSkipRefused(on != 0);
}

void hakios_metal_cb_set_profile(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setProfile(on != 0); });
    cb->cb->setProfile(on != 0);
}

int hakios_metal_cb_pass_time(hakios_metal_cb* cb, unsigned i, unsigned* kind, unsigned* width, unsigned* height, unsigned* draws, double* us) {
    syncOne(cb);
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    if (i >= t.size()) return i == 0 ? 0 : -1;
    if (kind) *kind = t[i].kind;
    if (width) *width = t[i].width;
    if (height) *height = t[i].height;
    if (draws) *draws = t[i].draws;
    if (us) *us = t[i].us;
    return int(t.size());
}

void hakios_metal_cb_counts(hakios_metal_cb* cb, unsigned* passes, unsigned* draws, unsigned* dispatches, unsigned* copies, unsigned* clears) {
    syncOne(cb);
    const CommandBuffer::Counts& c = cb->cb->counts();
    if (passes) *passes = c.passes;
    if (draws) *draws = c.draws;
    if (dispatches) *dispatches = c.dispatches;
    if (copies) *copies = c.copies;
    if (clears) *clears = c.clears;
}

void hakios_metal_cb_set_gmem(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setGmem(on != 0); });
    cb->cb->setGmem(on != 0);
}

void hakios_metal_cb_set_gmem_off(hakios_metal_cb* cb, unsigned bits) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, bits] { cb->cb->setGmemOff(bits); });
    cb->cb->setGmemOff(bits);
}

void hakios_metal_cb_set_gmem_tuned(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setGmemTuned(on != 0); });
    cb->cb->setGmemTuned(on != 0);
}

void hakios_metal_cb_set_gmem_min_draws(hakios_metal_cb* cb, unsigned draws) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, draws] { cb->cb->setGmemMinDraws(draws); });
    cb->cb->setGmemMinDraws(draws);
}

void hakios_metal_cb_set_gmem_auto(hakios_metal_cb* cb, int on) {   // (redfin ws-hm-ubwc) HM_GMEM=auto
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setGmemAuto(on != 0); });
    cb->cb->setGmemAuto(on != 0);
}

void hakios_metal_cb_set_gmem_max_tiles(hakios_metal_cb* cb, unsigned tiles) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, tiles] { cb->cb->setGmemMaxTiles(tiles); });
    cb->cb->setGmemMaxTiles(tiles);
}

void hakios_metal_cb_set_gmem_binning(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setGmemBinning(on != 0); });
    cb->cb->setGmemBinning(on != 0);
}

void hakios_metal_cb_set_null_fs_textures(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setNullFsTextures(on != 0); });
    cb->cb->setNullFsTextures(on != 0);
}

void hakios_metal_cb_set_trace_draws(hakios_metal_cb* cb, int mode) {   // (redfin hm-trace)
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, mode] { cb->cb->setTraceDraws(mode); });
    cb->cb->setTraceDraws(mode);
}

void hakios_metal_cb_trace_draw_cpu(hakios_metal_cb* cb, double us) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, us] { cb->cb->traceDrawCpu(us); });
    cb->cb->traceDrawCpu(us);
}

int hakios_metal_cb_trace_draw(hakios_metal_cb* cb, unsigned i, const char** name, unsigned* fsWords, unsigned* pass, unsigned area[4], int* gmem,
                               double* gpuUs, double* cpuUs) {
    syncOne(cb);
    if (cb->traceDraws.empty() || i == 0) cb->traceDraws = cb->cb->traceDraws();
    if (i >= cb->traceDraws.size()) return i == 0 ? 0 : -1;
    const CommandBuffer::TraceDraw& t = cb->traceDraws[i];
    if (name) *name = t.name.c_str();
    if (fsWords) *fsWords = t.fsWords;
    if (pass) *pass = t.pass;
    for (int k = 0; area != nullptr && k < 4; k++) area[k] = t.area[k];
    if (gmem) *gmem = t.gmem ? 1 : 0;
    if (gpuUs) *gpuUs = t.gpuUs;
    if (cpuUs) *cpuUs = t.cpuUs;
    return int(cb->traceDraws.size());
}

void hakios_metal_cb_set_profile_draws(hakios_metal_cb* cb, int on) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, on] { cb->cb->setProfileDraws(on != 0); });
    cb->cb->setProfileDraws(on != 0);
}

int hakios_metal_cb_draw_place(hakios_metal_cb* cb, unsigned i, unsigned* pass, unsigned area[4]) {
    syncOne(cb);
    if (cb->drawTimes.empty()) cb->drawTimes = cb->cb->drawTimes();
    if (i >= cb->drawTimes.size()) return -1;
    if (pass) *pass = cb->drawTimes[i].pass;
    for (int k = 0; area != nullptr && k < 4; k++) area[k] = cb->drawTimes[i].area[k];
    return int(cb->drawTimes.size());
}

int hakios_metal_cb_draw_time(hakios_metal_cb* cb, unsigned i, const char** name, double* us) {
    syncOne(cb);
    if (cb->drawTimes.empty() || i == 0) cb->drawTimes = cb->cb->drawTimes();
    if (i >= cb->drawTimes.size()) return i == 0 ? 0 : -1;
    if (name) *name = cb->drawTimes[i].name.c_str();
    if (us) *us = cb->drawTimes[i].us;
    return int(cb->drawTimes.size());
}

void hakios_metal_cb_gmem_counts(hakios_metal_cb* cb, unsigned* passes, unsigned* tiles) {
    syncOne(cb);
    if (passes) *passes = cb->cb->counts().gmemPasses;
    if (tiles) *tiles = cb->cb->counts().tiles;
}

const char* hakios_metal_cb_gmem_refusal(hakios_metal_cb* cb) {
    syncOne(cb); return cb->cb->gmemRefusal().c_str(); }

const char* hakios_metal_cb_pass_sysmem_why(hakios_metal_cb* cb, unsigned i) {
    syncOne(cb); return cb->cb->passSysmemWhy(i); }

int hakios_metal_cb_pass_attachments(hakios_metal_cb* cb, unsigned i, unsigned* attached, unsigned* touched) {
    syncOne(cb);
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    if (i >= t.size()) return -1;
    *attached = t[i].attached;
    *touched = t[i].touched;
    return 0;
}

int hakios_metal_cb_pass_info(hakios_metal_cb* cb, unsigned i, unsigned* format, unsigned* tiling, unsigned long long* cleared, unsigned long long* loaded,
                              unsigned long long* stored) {
    syncOne(cb);
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    if (i >= t.size()) return -1;
    if (format) *format = t[i].format;
    if (tiling) *tiling = t[i].tiling;
    if (cleared) *cleared = t[i].clearBytes;
    if (loaded) *loaded = t[i].loadBytes;
    if (stored) *stored = t[i].storeBytes;
    return int(t.size());
}

int hakios_metal_cb_pass_actions(hakios_metal_cb* cb, unsigned i, unsigned* load, unsigned* store) {
    syncOne(cb);
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    if (i >= t.size()) return -1;
    if (load) *load = t[i].loadAction;
    if (store) *store = t[i].storeAction;
    return int(t.size());
}

int hakios_metal_cb_pass_tiles(hakios_metal_cb* cb, unsigned i) {
    syncOne(cb);
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    return i < t.size() ? int(t[i].tiles) : -1;
}

int hakios_metal_cb_refuse(hakios_metal_cb* cb, const char* why) {
    if (deferring(cb)) {
        std::string w = why != nullptr ? why : "refused";
        cb->deferrer->post(cb->pending, [cb, w] { cb->cb->refuseWork(w); });
        return -1;
    }
    cb->cb->refuseWork(why != nullptr ? why : "refused");
    return -1;
}

unsigned hakios_metal_cb_skipped(hakios_metal_cb* cb, const char** last) {
    syncOne(cb);
    if (last != nullptr) *last = cb->cb->lastSkipped().c_str();
    return cb->cb->skipped();
}

int hakios_metal_cb_generate_mipmaps(hakios_metal_cb* cb, hakios_metal_texture* t) {
    syncAll(cb);
    if (!cb->cb->generateMipmaps(*t->texture)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(t->texture);
    return 0;
}

namespace {
// the dispatches' shared half: the bindings from the C tables, the dispatch itself (DO), and what has to live until
// the GPU is done with it
int dispatchWith(hakios_metal_cb* cb, hakios_metal_kernel* k, hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                 hakios_metal_texture* const* textures, unsigned textureCount, hakios_metal_sampler* const* samplers, unsigned samplerCount,
                 const std::function<bool(const Bindings&)>& dispatch) {
    Bindings b;
    b.buffers.resize(count);
    for (unsigned i = 0; i < count; i++) {
        if (buffers[i] != nullptr) {
            b.buffers[i] = Binding{buffers[i]->buffer.get(), offsets != nullptr ? offsets[i] : 0};
        }
    }
    b.textures.resize(textureCount, nullptr);
    for (unsigned i = 0; i < textureCount; i++) {
        if (textures[i] != nullptr) b.textures[i] = textures[i]->texture.get();
    }
    b.samplers.resize(samplerCount, nullptr);
    for (unsigned i = 0; i < samplerCount; i++) {
        if (samplers[i] != nullptr) b.samplers[i] = &samplers[i]->sampler;
    }
    // the kernel's own constexpr samplers at the indices the front end gave them (past the API's)
    for (const auto& [index, sampler] : k->kernel->inlineSamplers) {
        if (b.samplers.size() <= index) b.samplers.resize(index + 1, nullptr);
        b.samplers[index] = &sampler;
    }
    // the encoder's threadgroup memory goes past the kernel's static arrays, where the front end put the argument
    if (k->kernel->hasThreadgroupArg) b.sharedBytes = k->kernel->threadgroupArgBase + cb->threadgroupLength;
    // the members of its argument buffers, from what the sets hold, at the indices the front end gave them
    std::vector<std::shared_ptr<const void>> members;
    std::string whyMembers;
    if (!bindArgumentBuffers(k->kernel->resources, &b, &members, &whyMembers)) {
        failed(whyMembers);
        cb->cb->refuse(whyMembers);
        return -1;
    }
    for (auto& m : members) cb->cb->keepAlive(std::move(m));
    if (!dispatch(b)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        if (buffers[i] != nullptr) cb->cb->keep(buffers[i]->buffer);
    }
    // the textures' memory as the buffers': kept until the GPU is done, whatever the caller frees meanwhile (the
    // dispatch keeps the Buffer already; the Texture object itself is kept so that a freed handle leaves nothing dangling)
    for (unsigned i = 0; i < textureCount; i++) {
        if (textures[i] != nullptr) {
            cb->cb->keep(textures[i]->texture->buffer());
            cb->cb->keepAlive(textures[i]->texture);
        }
    }
    cb->cb->keepAlive(k->kernel);   // the words the dispatch names live until the GPU is done with them
    return 0;
}
}  // namespace

int hakios_metal_cb_dispatch2(hakios_metal_cb* cb, hakios_metal_kernel* k, const unsigned groups[3], const unsigned threads[3],
                              hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                              hakios_metal_texture* const* textures, unsigned textureCount,
                              hakios_metal_sampler* const* samplers, unsigned samplerCount) {
    syncAll(cb);
    const uint32_t g[3] = {groups[0], groups[1], groups[2]}, t[3] = {threads[0], threads[1], threads[2]};
    return dispatchWith(cb, k, buffers, offsets, count, textures, textureCount, samplers, samplerCount,
                        [&](const Bindings& b) { return cb->cb->dispatch(k->kernel->kernel, g, t, b); });
}

int hakios_metal_cb_dispatch_indirect(hakios_metal_cb* cb, hakios_metal_kernel* k, hakios_metal_buffer* args, uint64_t argsOffset, const unsigned threads[3],
                                      hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                                      hakios_metal_texture* const* textures, unsigned textureCount,
                                      hakios_metal_sampler* const* samplers, unsigned samplerCount) {
    syncAll(cb);
    const uint32_t t[3] = {threads[0], threads[1], threads[2]};
    const int r = dispatchWith(cb, k, buffers, offsets, count, textures, textureCount, samplers, samplerCount,
                               [&](const Bindings& b) { return cb->cb->dispatchIndirect(k->kernel->kernel, *args->buffer, argsOffset, t, b); });
    if (r == 0) cb->cb->keep(args->buffer);   // the GPU reads the counts out of it as it runs
    return r;
}

void hakios_metal_cb_set_threadgroup_length(hakios_metal_cb* cb, unsigned length) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, length] { cb->threadgroupLength = length; });
    cb->threadgroupLength = length;
}

int hakios_metal_cb_append(hakios_metal_cb* cb, hakios_metal_cb* other) {
    syncAll(cb);
    if (!cb->cb->append(*other->cb)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    return 0;
}

void hakios_metal_cb_set_indirect(hakios_metal_cb* cb, hakios_metal_buffer* args, uint64_t offset) {
    if (deferring(cb)) {
        std::shared_ptr<Buffer> b = args != nullptr ? args->buffer : nullptr;
        return cb->deferrer->post(cb->pending, [cb, b, offset] { cb->indirect = b; cb->indirectOffset = offset; });
    }
    cb->indirect = args != nullptr ? args->buffer : nullptr;
    cb->indirectOffset = offset;
}

int hakios_metal_cb_dispatch(hakios_metal_cb* cb, hakios_metal_kernel* k, const unsigned groups[3], const unsigned threads[3],
                             hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count) {
    return hakios_metal_cb_dispatch2(cb, k, groups, threads, buffers, offsets, count, nullptr, 0, nullptr, 0);
}

// --- the render path: a program of two stages, and one draw of it -------------------------------------------------------

static hakios_metal_program* programOf(std::shared_ptr<CompiledStage> vs, std::shared_ptr<CompiledStage> fs, unsigned colorPixelFormat) {
    std::string why;
    std::unique_ptr<CompiledProgram> p = linkProgram(std::move(vs), std::move(fs), PixelFormat(colorPixelFormat), &why);
    if (p == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_program;
    r->program = std::move(p);
    r->color = colorPixelFormat;
    return r;
}

hakios_metal_program* hakios_metal_program_new_bitcode(const void* vsBitcode, size_t vsSize, const void* fsBitcode, size_t fsSize, unsigned colorPixelFormat) {
    return hakios_metal_program_new_bitcode_constants(vsBitcode, vsSize, nullptr, 0, fsBitcode, fsSize, nullptr, 0, colorPixelFormat);
}

hakios_metal_program* hakios_metal_program_new_bitcode_constants(const void* vsBitcode, size_t vsSize, const hakios_metal_constant* vsConstants, unsigned vsCount,
                                                                 const void* fsBitcode, size_t fsSize, const hakios_metal_constant* fsConstants, unsigned fsCount,
                                                                 unsigned colorPixelFormat) {
    std::string why;
    const int64_t trT0 = hakios::trace::nowNs();   // (redfin hm-trace) the whole program: its stages (each an event too) and the link
    struct TraceLink {
        int64_t t0;
        hakios_metal_program** made;
        ~TraceLink() {
            if (hakios::trace::on()) hakios::trace::event("program-new", *made != nullptr ? hakios_metal_program_name(*made) : "(refused)", t0, hakios::trace::nowNs() - t0, 0);
        }
    };
    hakios_metal_program* trMade = nullptr;
    TraceLink trLink{trT0, &trMade};
    std::unique_ptr<CompiledStage> vs = compileStage(vsBitcode, vsSize, &why, constantsOf(vsConstants, vsCount));
    if (vs == nullptr) {
        failed(why);
        return nullptr;
    }
    // no fragment bitcode: a pipeline with no fragment function (a depth-only one), which runs null_fs -- a fragment
    // function of our own that writes nothing (tools/kernels/null_fs.ll), compiled as any other so that it links
    if (fsBitcode == nullptr || fsSize == 0) {
        fsBitcode = kernels::null_fs_bc;
        fsSize = sizeof kernels::null_fs_bc;
        fsConstants = nullptr;
        fsCount = 0;
    }
    // the FS's device buffers past the VS's in the one IBO table the two share
    std::unique_ptr<CompiledStage> fs = compileStage(fsBitcode, fsSize, &why, constantsOf(fsConstants, fsCount), vs->stage.k.numSsbos);
    if (fs == nullptr) {
        failed(why);
        return nullptr;
    }
    trMade = programOf(std::move(vs), std::move(fs), colorPixelFormat);   // (redfin hm-trace)
    return trMade;
}

hakios_metal_program* hakios_metal_program_specialize(hakios_metal_program* base, const void* fsBitcode, size_t fsSize, const hakios_metal_constant* fsConstants,
                                                     unsigned fsCount, unsigned binding, const void* bytes, size_t length) {
    if (base == nullptr || fsBitcode == nullptr || fsSize == 0 || bytes == nullptr) {
        failed("a specialisation needs the program, its fragment function's bitcode and the buffer's bytes");
        return nullptr;
    }
    std::string why;
    KnownBuffer known;
    known.binding = binding;
    known.bytes.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + length);
    const Program& was = base->program->program;
    const int64_t trT0 = hakios::trace::nowNs();   // (redfin hm-trace)
    struct TraceSpec {
        int64_t t0;
        hakios_metal_program* base;
        size_t length;
        ~TraceSpec() {
            if (hakios::trace::on()) hakios::trace::event("program-specialise", hakios_metal_program_name(base), t0, hakios::trace::nowNs() - t0, length);
        }
    } trSpec{trT0, base, length};
    std::unique_ptr<CompiledStage> fs = compileStage(fsBitcode, fsSize, &why, constantsOf(fsConstants, fsCount), was.fsIboBase, &known);
    if (fs == nullptr) {
        failed(why);
        return nullptr;
    }
    std::unique_ptr<CompiledProgram> p = linkProgram(base->program->vs, std::move(fs), PixelFormat(base->color), &why);
    if (p == nullptr) {
        failed(why);
        return nullptr;
    }
    // the pipeline's own state, which the base was given after it was linked
    p->program.vertexDescriptor = was.vertexDescriptor;
    p->program.blend = was.blend;
    for (uint32_t i = 0; i + 1 < kMaxColorOutputs; i++) p->program.moreBlend[i] = was.moreBlend[i];
    p->program.samples = was.samples;
    auto* r = new hakios_metal_program;
    r->program = std::move(p);
    r->color = base->color;
    return r;
}

unsigned long long hakios_metal_fills_dropped(void) { return fillsDropped(); }
void hakios_metal_cull_stats(unsigned long long out[8]) {   // (ws-hm-cull)
    uint64_t v[8];
    cullStats(v);
    for (int i = 0; i < 8; i++) out[i] = v[i];
}

void hakios_metal_pool_stats(unsigned long long out[8]) {
    uint64_t s[kPoolKinds + 2];
    poolStats(s);
    for (int i = 0; i < 8; i++) out[i] = s[i];
}
unsigned long long hakios_metal_pool_trim(int force) { return trimAllQueues(force != 0); }
void hakios_metal_rt_cache_stats(unsigned long long out[12]) {   // (ws-hm-pool)
    uint64_t s[12];
    rtCacheStats(s);
    for (int i = 0; i < 12; i++) out[i] = s[i];
}
void hakios_metal_rt_cache_set(hakios_metal_device* d, unsigned long long limitBytes, unsigned slackPct, unsigned long long zeroBytes) {
    Gpu::BoCacheConfig c;
    c.limitBytes = limitBytes;
    c.slackPct = slackPct;
    c.zeroBytes = zeroBytes;
    d->gpu->setBoCache(c);
}

int hakios_metal_known_read_mask(const void* fsBitcode, size_t fsSize, const hakios_metal_constant* fsConstants, unsigned fsCount,
                                 unsigned binding, unsigned char* mask, size_t length) {
    if (fsBitcode == nullptr || fsSize == 0 || mask == nullptr) {
        failed("a read mask needs the fragment function's bitcode and room for the mask");
        return -1;
    }
    std::string why;
    std::vector<uint8_t> m;
    const int n = knownReadMask(fsBitcode, fsSize, constantsOf(fsConstants, fsCount), binding, length, &m, &why);
    if (n < 0) {
        failed(why);
        return -1;
    }
    for (size_t i = 0; i < length; i++) mask[i] = i < m.size() ? m[i] : 1;
    return n;
}

int hakios_metal_known_read_mask_cached(const void* fsBitcode, size_t fsSize, const hakios_metal_constant* fsConstants, unsigned fsCount,
                                        unsigned binding, unsigned char* mask, size_t length) {   // (redfin hm-trace)
    if (fsBitcode == nullptr || fsSize == 0 || mask == nullptr) return -2;
    std::vector<uint8_t> m;
    const int n = knownReadMaskCached(fsBitcode, fsSize, constantsOf(fsConstants, fsCount), binding, length, &m);
    if (n < 0) return -2;
    for (size_t i = 0; i < length; i++) mask[i] = i < m.size() ? m[i] : 1;
    return n;
}

unsigned hakios_metal_program_stage_words(hakios_metal_program* p, unsigned stage) {
    if (p == nullptr || stage > 1) return 0;
    return (stage == 0 ? p->program->program.vs : p->program->program.fs).k.numWords;
}

// (redfin hmv7) HM_HANG_BLAME: the hangs counted against the program a draw with P names (CompiledProgram::program)
unsigned hakios_metal_program_hangs(hakios_metal_program* p) {
    if (p == nullptr || p->program == nullptr) return 0;
    return CommandBuffer::hangsOf(&p->program->program);
}

// FUNCTION of LIBRARY as WANT (a vertex or a fragment function): its bitcode, or nullptr with the reason left
static bool functionBitcode(hakios_metal_library* l, const char* function, MetalFunction::Type want, const uint8_t** bytes, uint64_t* length) {
    const MetalFunction* f = l->lib.find(function != nullptr ? function : "");
    if (f == nullptr || !l->lib.bitcode(*f, bytes, length)) {
        failed(std::string("no function ") + (function ? function : "(null)") + " in the library's " + std::to_string(l->lib.functions().size()));
        return false;
    }
    if (f->type != want) {
        failed(std::string(function) + " is not a " + (want == MetalFunction::kVertex ? "vertex" : "fragment") + " function (TYPE " + std::to_string(f->type) + ")");
        return false;
    }
    return true;
}

hakios_metal_program* hakios_metal_program_new(hakios_metal_library* vsLibrary, const char* vs, hakios_metal_library* fsLibrary, const char* fs,
                                               unsigned colorPixelFormat) {
    const uint8_t* vsBytes = nullptr;
    const uint8_t* fsBytes = nullptr;
    uint64_t vsLength = 0, fsLength = 0;
    if (!functionBitcode(vsLibrary, vs, MetalFunction::kVertex, &vsBytes, &vsLength) || !functionBitcode(fsLibrary, fs, MetalFunction::kFragment, &fsBytes, &fsLength)) {
        return nullptr;
    }
    return hakios_metal_program_new_bitcode(vsBytes, size_t(vsLength), fsBytes, size_t(fsLength), colorPixelFormat);
}

void hakios_metal_program_free(hakios_metal_program* p) { delete p; }
const char* hakios_metal_program_name(hakios_metal_program* p) { return p->program->name.c_str(); }

// STAGE 0 is the vertex stage, 1 the fragment stage; another number has no resources
static const CompiledStage* stageOf(hakios_metal_program* p, unsigned stage) {
    return stage == 0 ? p->program->vs.get() : stage == 1 ? p->program->fs.get() : nullptr;
}

unsigned hakios_metal_program_resource_count(hakios_metal_program* p, unsigned stage) {
    const CompiledStage* s = stageOf(p, stage);
    return s != nullptr ? unsigned(s->resources.size()) : 0;
}

int hakios_metal_program_resource(hakios_metal_program* p, unsigned stage, unsigned i, unsigned* kind, unsigned* index, unsigned* sizeBytes) {
    const CompiledStage* s = stageOf(p, stage);
    if (s == nullptr || i >= s->resources.size()) return -1;
    const Resource& r = s->resources[i];
    if (kind) *kind = unsigned(r.kind);
    if (index) *index = r.index;
    if (sizeBytes) *sizeBytes = r.sizeBytes;
    return 0;
}

int hakios_metal_program_set_vertex_attribute(hakios_metal_program* p, unsigned index, unsigned format, unsigned offset, unsigned bufferIndex) {
    if (index >= kMaxVertexAttributes) {
        failed("vertex attribute " + std::to_string(index) + ": MTLVertexDescriptor's attributes are 0 to 30");
        return -1;
    }
    p->program->program.vertexDescriptor.attributes[index] = VertexAttribute{VertexFormat(format), offset, bufferIndex};
    return 0;
}

int hakios_metal_program_set_vertex_layout(hakios_metal_program* p, unsigned bufferIndex, unsigned stride, unsigned stepFunction, unsigned stepRate) {
    if (bufferIndex >= kMaxVertexBuffers) {
        failed("vertex layout " + std::to_string(bufferIndex) + ": MTLVertexDescriptor's layouts are 0 to 30");
        return -1;
    }
    p->program->program.vertexDescriptor.layouts[bufferIndex] = VertexLayout{stride, VertexStep(stepFunction), stepRate};
    return 0;
}

void hakios_metal_program_set_blend(hakios_metal_program* p, int enabled, unsigned rgbOp, unsigned alphaOp, unsigned srcRgb, unsigned dstRgb,
                                    unsigned srcAlpha, unsigned dstAlpha, unsigned writeMask) {
    hakios_metal_program_set_blend_at(p, 0, enabled, rgbOp, alphaOp, srcRgb, dstRgb, srcAlpha, dstAlpha, writeMask);
}

int hakios_metal_program_set_samples(hakios_metal_program* p, unsigned samples) {
    if (samples != 1 && samples != 2 && samples != 4) {
        failed("rasterSampleCount " + std::to_string(samples) + ": 1, 2 or 4 are served");
        return -1;
    }
    p->program->program.samples = samples;
    return 0;
}

int hakios_metal_program_set_blend_at(hakios_metal_program* p, unsigned index, int enabled, unsigned rgbOp, unsigned alphaOp, unsigned srcRgb,
                                      unsigned dstRgb, unsigned srcAlpha, unsigned dstAlpha, unsigned writeMask) {
    if (index >= kMaxColorOutputs) {
        failed("blend of colour attachment " + std::to_string(index) + ": there are 8, 0 to 7");
        return -1;
    }
    const ColorBlend b{enabled != 0, rgbOp, alphaOp, srcRgb, dstRgb, srcAlpha, dstAlpha, writeMask};
    (index == 0 ? p->program->program.blend : p->program->program.moreBlend[index - 1]) = b;
    return 0;
}

// the C interface's pass as the core's: attachment 0, 1 to 7, the depth attachment and their actions
static RenderPass passOf(const struct hakios_metal_render_pass* pass) {
    RenderPass rp;
    if (pass == nullptr) return rp;
    rp.color0 = pass->color0 != nullptr ? pass->color0->texture.get() : nullptr;
    rp.level = pass->color0_level;
    rp.slice = pass->color0_slice;
    rp.load = LoadAction(pass->load_action);
    rp.store = StoreAction(pass->store_action);
    for (int i = 0; i < 4; i++) rp.clearColor[i] = pass->clear_color[i];
    for (unsigned i = 0; i < kMaxColorOutputs - 1; i++) {
        const auto& m = pass->more[i];
        rp.more[i].texture = m.texture != nullptr ? m.texture->texture.get() : nullptr;
        rp.more[i].level = m.level;
        rp.more[i].slice = m.slice;
        rp.more[i].load = LoadAction(m.load_action);
        rp.more[i].store = StoreAction(m.store_action);
        for (int c = 0; c < 4; c++) rp.more[i].clearColor[c] = m.clear_color[c];
    }
    rp.depth = pass->depth != nullptr ? pass->depth->texture.get() : nullptr;
    rp.depthLevel = pass->depth_level;
    rp.depthSlice = pass->depth_slice;
    rp.depthLoad = LoadAction(pass->depth_load_action);
    rp.depthStore = StoreAction(pass->depth_store_action);
    rp.clearDepth = pass->clear_depth;
    rp.stencil = pass->stencil != nullptr ? pass->stencil->texture.get() : nullptr;
    rp.stencilLoad = LoadAction(pass->stencil_load_action);
    rp.stencilStore = StoreAction(pass->stencil_store_action);
    rp.clearStencil = pass->clear_stencil;
    return rp;
}

// the pass's texture objects, alive until the GPU is done with them, whatever the caller frees meanwhile
// (ws-hm-plugcpu) HM_PLUG_FAST: the same, each object once a command buffer
static void keepPassAliveOnce(hakios_metal_cb* cb, const struct hakios_metal_render_pass* pass) {
    if (pass->color0 != nullptr) cb->cb->keepAliveOnce(pass->color0->texture);
    for (const auto& m : pass->more) {
        if (m.texture != nullptr) cb->cb->keepAliveOnce(m.texture->texture);
    }
    if (pass->depth != nullptr) cb->cb->keepAliveOnce(pass->depth->texture);
    if (pass->visibility != nullptr) cb->cb->keepOnce(pass->visibility->buffer);
}

static void keepPassAlive(hakios_metal_cb* cb, const struct hakios_metal_render_pass* pass) {
    if (pass->color0 != nullptr) cb->cb->keepAlive(pass->color0->texture);
    for (const auto& m : pass->more) {
        if (m.texture != nullptr) cb->cb->keepAlive(m.texture->texture);
    }
    if (pass->depth != nullptr) cb->cb->keepAlive(pass->depth->texture);
    if (pass->visibility != nullptr) cb->cb->keep(pass->visibility->buffer);   // the result words the draws write
}

int hakios_metal_cb_draw(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                         const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                         unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, hakios_metal_buffer* const* vsBuffers,
                         const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount) {
    return hakios_metal_cb_draw2(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, vsBuffers, vsOffsets, vsCount, fsBuffers,
                                 fsOffsets, fsCount, nullptr, 0, nullptr, 0);
}

namespace {
// the draws of the C interface in one: INDEX_BUFFER non-NULL is an indexed draw
int drawCommon(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass, const struct hakios_metal_viewport* viewport,
               const struct hakios_metal_scissor* scissor, unsigned primitive, unsigned vertexStart, unsigned vertexCount, unsigned instanceCount,
               hakios_metal_buffer* indexBuffer, unsigned indexType, uint64_t indexOffset, int baseVertex, unsigned baseInstance,
               hakios_metal_buffer* const* vsBuffers, const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers,
               const uint64_t* fsOffsets, unsigned fsCount, hakios_metal_texture* const* fsTextures, unsigned fsTextureCount,
               hakios_metal_sampler* const* fsSamplers, unsigned fsSamplerCount) {
    // an indirect draw's words in place of the counts (hakios_metal_cb_set_indirect): taken first, so that this draw
    // alone has them whether it is drawn or refused
    std::shared_ptr<Buffer> indirect = std::move(cb->indirect);
    cb->indirect = nullptr;
    // the two [[buffer(n)]] tables by Metal's index (setVertexBuffer / setFragmentBuffer); render::encode reads what each
    // stage's pushed ranges name and refuses a range with nothing bound by name
    // (ws-hm-plugcpu) HM_PLUG_FAST: into the command buffer's own two tables, emptied (every field as a new one's) with
    // their arrays' memory kept; otherwise new ones, as before
    const bool fast = plugFast();
    Bindings vsOwn, fsOwn;
    auto table = [fast](Bindings& b, hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count) {
        if (fast) {
            std::vector<Binding> bufs = std::move(b.buffers);
            std::vector<const Texture*> texs = std::move(b.textures);
            std::vector<const Sampler*> smps = std::move(b.samplers);
            b = Bindings{};
            bufs.clear(), texs.clear(), smps.clear();
            b.buffers = std::move(bufs), b.textures = std::move(texs), b.samplers = std::move(smps);
        }
        b.buffers.resize(count);
        for (unsigned i = 0; i < count; i++) {
            if (buffers != nullptr && buffers[i] != nullptr) b.buffers[i] = Binding{buffers[i]->buffer.get(), offsets != nullptr ? offsets[i] : 0};
        }
    };
    Bindings& vsb = fast ? cb->scratchVs : vsOwn;
    table(vsb, vsBuffers, vsOffsets, vsCount);
    // the vertex stage's textures and samplers, as hakios_metal_cb_set_vertex_textures left them, and its own constexpr
    // samplers past them, as the fragment stage's below
    for (const auto& t : cb->vsTextures) vsb.textures.push_back(t.get());
    for (const auto& m : cb->vsSamplers) vsb.samplers.push_back(m ? &*m : nullptr);
    for (const auto& [index, sampler] : p->program->vs->inlineSamplers) {
        if (vsb.samplers.size() <= index) vsb.samplers.resize(index + 1, nullptr);
        vsb.samplers[index] = &sampler;
    }
    Bindings& fsb = fast ? cb->scratchFs : fsOwn;
    table(fsb, fsBuffers, fsOffsets, fsCount);
    // the fragment stage's [[texture(n)]] and [[sampler(n)]] by Metal's index (setFragmentTexture / setFragmentSamplerState)
    fsb.textures.resize(fsTextureCount, nullptr);
    for (unsigned i = 0; i < fsTextureCount; i++) {
        if (fsTextures != nullptr && fsTextures[i] != nullptr) fsb.textures[i] = fsTextures[i]->texture.get();
    }
    fsb.samplers.resize(fsSamplerCount, nullptr);
    for (unsigned i = 0; i < fsSamplerCount; i++) {
        if (fsSamplers != nullptr && fsSamplers[i] != nullptr) fsb.samplers[i] = &fsSamplers[i]->sampler;
    }
    // the fragment function's own constexpr samplers at the indices the front end gave them (past the API's)
    for (const auto& [index, sampler] : p->program->fs->inlineSamplers) {
        if (fsb.samplers.size() <= index) fsb.samplers.resize(index + 1, nullptr);
        fsb.samplers[index] = &sampler;
    }
    // both stages' argument-buffer members, from what the sets hold, at the indices the front end gave them
    std::vector<std::shared_ptr<const void>> members;
    std::string whyMembers;
    if (!bindArgumentBuffers(p->program->vs->resources, &vsb, &members, &whyMembers) ||
        !bindArgumentBuffers(p->program->fs->resources, &fsb, &members, &whyMembers)) {
        failed(whyMembers);
        cb->cb->refuse(whyMembers);
        return -1;
    }
    for (auto& m : members) cb->cb->keepAlive(std::move(m));
    const RenderPass rp = passOf(pass);
    // what MTLRenderCommandEncoder's default viewport covers: the first attachment the pass has, colour before depth
    // (a depth-only pass's is the depth texture)
    const Texture* target = rp.color0;
    uint32_t targetLevel = rp.level;
    for (unsigned i = 0; target == nullptr && i < kMaxColorOutputs - 1; i++) {
        target = rp.more[i].texture;
        targetLevel = rp.more[i].level;
    }
    if (target == nullptr) {
        target = rp.depth;
        targetLevel = rp.depthLevel;
    }
    Draw d;
    d.primitive = Primitive(primitive);
    d.vertexStart = vertexStart;
    d.vertexCount = vertexCount;
    d.instanceCount = instanceCount;
    d.indexBuffer = indexBuffer != nullptr ? indexBuffer->buffer.get() : nullptr;
    d.indexType = indexType;
    d.indexOffset = indexOffset;
    d.baseVertex = baseVertex;
    d.baseInstance = baseInstance;
    d.indirect = indirect.get();
    d.indirectOffset = cb->indirectOffset;
    // MTLRenderCommandEncoder's defaults: a viewport over the whole attachment with depth 0..1, no scissor (the whole attachment)
    if (viewport != nullptr) {
        d.viewport = Viewport{viewport->x, viewport->y, viewport->width, viewport->height, viewport->znear, viewport->zfar};
    } else {
        // (the level drawn into: a pass into mip level 1 of a 512 x 512 texture covers 256 x 256)
        d.viewport = Viewport{0, 0, target != nullptr ? double(target->level(targetLevel).width) : 1.0,
                              target != nullptr ? double(target->level(targetLevel).height) : 1.0, 0, 1};
    }
    if (scissor != nullptr) {
        d.scissor = Scissor{scissor->x, scissor->y, scissor->width, scissor->height};
    }
    if (pass != nullptr) {
        for (int i = 0; i < 4; i++) d.blendColor[i] = pass->blend_color[i];
        if (pass->depth_state_set) d.depth = DepthState{pass->depth_compare, pass->depth_write != 0};
        d.cullMode = pass->cull_mode;
        d.depthBias = DepthBias{pass->depth_bias, pass->depth_slope_scale, pass->depth_bias_clamp};   // (redfin hm-int5)
        d.winding = pass->winding;
        if (pass->stencil_state_set) {
            auto face = [](const hakios_metal_render_pass::hakios_metal_stencil_face& f) {
                return StencilFace{f.compare, f.stencil_fail, f.depth_fail, f.pass, f.read_mask, f.write_mask};
            };
            d.stencil.enabled = true;
            d.stencil.front = face(pass->stencil_front);
            d.stencil.back = face(pass->stencil_back);
            d.stencil.frontReference = pass->stencil_front_reference;
            d.stencil.backReference = pass->stencil_back_reference;
        }
        if (pass->visibility_mode != 0) {
            d.visibility = pass->visibility != nullptr ? pass->visibility->buffer.get() : nullptr;
            d.visibilityMode = pass->visibility_mode;
            d.visibilityOffset = pass->visibility_offset;
            d.visibilityReset = pass->visibility_reset != 0;
        }
    }
    if (!cb->cb->draw(p->program->program, rp, d, vsb, fsb)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    if (fast) {   // (ws-hm-plugcpu) what follows, each object once a command buffer
        for (unsigned i = 0; i < vsCount; i++) if (vsBuffers != nullptr && vsBuffers[i] != nullptr) cb->cb->keepOnce(vsBuffers[i]->buffer);
        for (unsigned i = 0; i < fsCount; i++) if (fsBuffers != nullptr && fsBuffers[i] != nullptr) cb->cb->keepOnce(fsBuffers[i]->buffer);
        if (target != nullptr) cb->cb->keepOnce(target->buffer());
        keepPassAliveOnce(cb, pass);
        cb->cb->keepAliveOnce(p->program);
        for (unsigned i = 0; i < fsTextureCount; i++) {
            if (fsTextures != nullptr && fsTextures[i] != nullptr) cb->cb->keepAliveOnce(fsTextures[i]->texture);
        }
        for (const auto& t : cb->vsTextures) {
            if (t != nullptr) cb->cb->keepAliveOnce(t);
        }
        if (indirect != nullptr) cb->cb->keep(indirect);
        return 0;
    }
    // the bound buffers live until the GPU has read them, whatever the caller frees meanwhile
    for (unsigned i = 0; i < vsCount; i++) if (vsBuffers != nullptr && vsBuffers[i] != nullptr) cb->cb->keep(vsBuffers[i]->buffer);
    for (unsigned i = 0; i < fsCount; i++) if (fsBuffers != nullptr && fsBuffers[i] != nullptr) cb->cb->keep(fsBuffers[i]->buffer);
    // the target's memory and object, and the program's words, live until the GPU is done, whatever the caller frees meanwhile
    if (target != nullptr) cb->cb->keep(target->buffer());
    keepPassAlive(cb, pass);
    cb->cb->keepAlive(p->program);
    for (unsigned i = 0; i < fsTextureCount; i++) {
        if (fsTextures != nullptr && fsTextures[i] != nullptr) cb->cb->keepAlive(fsTextures[i]->texture);   // the draw keeps the memory already
    }
    for (const auto& t : cb->vsTextures) {
        if (t != nullptr) cb->cb->keepAlive(t);
    }
    if (indirect != nullptr) cb->cb->keep(indirect);   // the CP reads the draw's words out of it as it runs
    return 0;
}
}  // namespace

void hakios_metal_cb_set_vertex_textures(hakios_metal_cb* cb, hakios_metal_texture* const* textures, unsigned textureCount,
                                         hakios_metal_sampler* const* samplers, unsigned samplerCount) {
    if (deferring(cb)) {
        auto t = std::make_shared<Handles<hakios_metal_texture>>();
        auto m = std::make_shared<Handles<hakios_metal_sampler>>();
        t->take(textures, textureCount);
        m->take(samplers, samplerCount);
        return cb->deferrer->post(cb->pending, [cb, t, m, textureCount, samplerCount] {
            hakios_metal_cb_set_vertex_textures(cb, t->get(), textureCount, m->get(), samplerCount);
        });
    }
    cb->vsTextures.assign(textureCount, nullptr);
    for (unsigned i = 0; i < textureCount; i++) {
        if (textures != nullptr && textures[i] != nullptr) cb->vsTextures[i] = textures[i]->texture;
    }
    cb->vsSamplers.assign(samplerCount, std::nullopt);
    for (unsigned i = 0; i < samplerCount; i++) {
        if (samplers != nullptr && samplers[i] != nullptr) cb->vsSamplers[i] = samplers[i]->sampler;
    }
}

namespace {
// a draw's arguments as the C interface takes them, copied (deferred recording)
struct DrawCopy {
    hakios_metal_program program;
    PassCopy pass;
    std::optional<hakios_metal_viewport> viewport;
    std::optional<hakios_metal_scissor> scissor;
    Handles<hakios_metal_buffer> vs, fs;
    std::vector<uint64_t> vsOffsets, fsOffsets;
    const uint64_t *vsOffsetsGiven = nullptr, *fsOffsetsGiven = nullptr;
    Handles<hakios_metal_texture> fsTextures;
    Handles<hakios_metal_sampler> fsSamplers;
    hakios_metal_buffer index;
    void take(hakios_metal_program* p, const struct hakios_metal_render_pass* rp, const struct hakios_metal_viewport* v,
              const struct hakios_metal_scissor* sc, hakios_metal_buffer* const* vsb, const uint64_t* vso, unsigned vsn,
              hakios_metal_buffer* const* fsb, const uint64_t* fso, unsigned fsn, hakios_metal_texture* const* fst, unsigned fstn,
              hakios_metal_sampler* const* fss, unsigned fssn) {
        program = *p;
        pass.take(rp);
        if (v != nullptr) viewport = *v;
        if (sc != nullptr) scissor = *sc;
        vs.take(vsb, vsn);
        fs.take(fsb, fsn);
        vsOffsets = copyOf(vso, vsn);
        fsOffsets = copyOf(fso, fsn);
        vsOffsetsGiven = vso;
        fsOffsetsGiven = fso;
        fsTextures.take(fst, fstn);
        fsSamplers.take(fss, fssn);
    }
    const hakios_metal_viewport* vp() const { return viewport ? &*viewport : nullptr; }
    const hakios_metal_scissor* sc() const { return scissor ? &*scissor : nullptr; }
    const uint64_t* vso() const { return ptrOf(vsOffsets, vsOffsetsGiven); }
    const uint64_t* fso() const { return ptrOf(fsOffsets, fsOffsetsGiven); }
};
}  // namespace

int hakios_metal_cb_draw2(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                          const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                          unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, hakios_metal_buffer* const* vsBuffers,
                          const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount,
                          hakios_metal_texture* const* fsTextures, unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers,
                          unsigned fsSamplerCount) {
    if (deferring(cb)) {
        auto c = std::make_shared<DrawCopy>();
        c->take(p, pass, viewport, scissor, vsBuffers, vsOffsets, vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
        cb->deferrer->post(cb->pending, [=] {
            refusedThere(cb, hakios_metal_cb_draw2(cb, &c->program, c->pass.get(), c->vp(), c->sc(), primitive, vertexStart, vertexCount, instanceCount,
                                                   c->vs.get(), c->vso(), vsCount, c->fs.get(), c->fso(), fsCount, c->fsTextures.get(), fsTextureCount,
                                                   c->fsSamplers.get(), fsSamplerCount));
        });
        return 0;
    }
    return drawCommon(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, nullptr, 0, 0, 0, 0, vsBuffers, vsOffsets, vsCount,
                      fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
}

int hakios_metal_cb_draw3(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                          const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                          unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, unsigned baseInstance, hakios_metal_buffer* const* vsBuffers,
                          const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount,
                          hakios_metal_texture* const* fsTextures, unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers,
                          unsigned fsSamplerCount) {
    if (deferring(cb)) {
        auto c = std::make_shared<DrawCopy>();
        c->take(p, pass, viewport, scissor, vsBuffers, vsOffsets, vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
        cb->deferrer->post(cb->pending, [=] {
            refusedThere(cb, hakios_metal_cb_draw3(cb, &c->program, c->pass.get(), c->vp(), c->sc(), primitive, vertexStart, vertexCount, instanceCount,
                                                   baseInstance, c->vs.get(), c->vso(), vsCount, c->fs.get(), c->fso(), fsCount, c->fsTextures.get(),
                                                   fsTextureCount, c->fsSamplers.get(), fsSamplerCount));
        });
        return 0;
    }
    return drawCommon(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, nullptr, 0, 0, 0, baseInstance, vsBuffers, vsOffsets,
                      vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
}

int hakios_metal_cb_draw_indexed(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                                 const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                                 unsigned indexCount, unsigned indexType, hakios_metal_buffer* indexBuffer, uint64_t indexOffset, unsigned instanceCount,
                                 int baseVertex, unsigned baseInstance, hakios_metal_buffer* const* vsBuffers, const uint64_t* vsOffsets, unsigned vsCount,
                                 hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount, hakios_metal_texture* const* fsTextures,
                                 unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers, unsigned fsSamplerCount) {
    if (deferring(cb) && indexBuffer != nullptr) {
        auto c = std::make_shared<DrawCopy>();
        c->take(p, pass, viewport, scissor, vsBuffers, vsOffsets, vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
        c->index = *indexBuffer;
        cb->deferrer->post(cb->pending, [=] {
            refusedThere(cb, hakios_metal_cb_draw_indexed(cb, &c->program, c->pass.get(), c->vp(), c->sc(), primitive, indexCount, indexType, &c->index,
                                                          indexOffset, instanceCount, baseVertex, baseInstance, c->vs.get(), c->vso(), vsCount,
                                                          c->fs.get(), c->fso(), fsCount, c->fsTextures.get(), fsTextureCount, c->fsSamplers.get(),
                                                          fsSamplerCount));
        });
        return 0;
    }
    if (indexBuffer == nullptr) {
        failed("an indexed draw with no index buffer");
        cb->cb->refuse("an indexed draw with no index buffer");
        return -1;
    }
    int r = drawCommon(cb, p, pass, viewport, scissor, primitive, 0, indexCount, instanceCount, indexBuffer, indexType, indexOffset, baseVertex, baseInstance,
                       vsBuffers, vsOffsets, vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
    if (r == 0 && plugFast()) cb->cb->keepOnce(indexBuffer->buffer);   // (ws-hm-plugcpu)
    else if (r == 0) cb->cb->keep(indexBuffer->buffer);   // the indices live until the GPU has read them, whatever the caller frees
    return r;
}

int hakios_metal_cb_clear(hakios_metal_cb* cb, const struct hakios_metal_render_pass* pass) {
    if (deferring(cb) && pass != nullptr) {
        auto c = std::make_shared<PassCopy>();
        c->take(pass);
        cb->deferrer->post(cb->pending, [cb, c] { refusedThere(cb, hakios_metal_cb_clear(cb, c->get())); });
        return 0;
    }
    if (!cb->cb->clear(passOf(pass))) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    keepPassAlive(cb, pass);
    return 0;
}

void hakios_metal_cb_fail(hakios_metal_cb* cb, const char* why) {
    if (deferring(cb)) {
        std::string w = why != nullptr ? why : "refused";
        return cb->deferrer->post(cb->pending, [cb, w] { cb->cb->refuse(w); });
    }
    cb->cb->refuse(why != nullptr ? why : "refused");
}

void hakios_metal_cb_commit(hakios_metal_cb* cb, void (*callback)(hakios_metal_cb*, void*), void* context) {
    if (deferring(cb)) return cb->deferrer->post(cb->pending, [cb, callback, context] { hakios_metal_cb_commit(cb, callback, context); }, true);
    if (callback != nullptr) {
        cb->cb->addCompletedHandler([cb, callback, context](CommandBuffer&) { callback(cb, context); });
    }
    cb->cb->commit();
}

void hakios_metal_cb_wait(hakios_metal_cb* cb) {
    syncOne(cb);
    cb->cb->waitUntilCompleted();
}

enum hakios_metal_status hakios_metal_cb_status(hakios_metal_cb* cb) {
    syncOne(cb);
    return static_cast<enum hakios_metal_status>(cb->cb->status());
}

const char* hakios_metal_cb_error(hakios_metal_cb* cb) {
    syncOne(cb);
    return cb->cb->error().c_str();
}
int64_t hakios_metal_cb_gpu_nanoseconds(hakios_metal_cb* cb) {
    syncOne(cb);
    return cb->cb->gpuNanoseconds();
}
void hakios_metal_cb_gpu_split(hakios_metal_cb* cb, int64_t* queued, int64_t* running) {
    syncOne(cb);
    if (queued != nullptr) *queued = cb->cb->queuedNanoseconds();
    if (running != nullptr) *running = cb->cb->runningNanoseconds();
}
int hakios_metal_cb_sync_counts(hakios_metal_cb* cb, unsigned long long* out, int n) {
    syncOne(cb);
    const CommandBuffer& c = *cb->cb;
    const render::SyncCensus& s = c.syncCensus();
    const CommandBuffer::Counts& k = c.counts();
    const unsigned long long v[28] = {(unsigned long long)std::max<int64_t>(0, c.gpuStampedNanoseconds()),
                                      (unsigned long long)std::max<int64_t>(0, c.preGpuNanoseconds()),
                                      s.words, s.registerWords, s.wfis, s.ccuColorFlushes, s.ccuDepthFlushes, s.cacheFlushes, s.waits,
                                      s.cacheInvalidates, s.ccuInvalidates, s.memcpyWords, k.tailsSkipped, k.barriers, k.invalidates,
                                      k.lazyTails, k.headsCarried, k.storeMerges, k.cpCopies, k.cpCopyBytes, k.passes, k.draws, s.blits,
                                      s.markers, k.ubwcPasses, k.ubwcGmemPasses, k.ubwcCopies, k.ubwcSwitches};
    if (out == nullptr || n <= 0) return 0;
    const int m = std::min(n, 28);
    for (int i = 0; i < m; i++) out[i] = v[i];
    return m;
}

int hakios_metal_cb_submit_times(hakios_metal_cb* cb, int64_t* committed_ns, int64_t* kernel_in_ns, int64_t* kernel_out_ns) {
    syncOne(cb);
    const CommandBuffer& c = *cb->cb;
    if (committed_ns != nullptr) *committed_ns = c.committedNs();
    if (kernel_in_ns != nullptr) *kernel_in_ns = c.kernelInNs();
    if (kernel_out_ns != nullptr) *kernel_out_ns = c.kernelOutNs();
    return c.kernelOutNs() != 0 ? 0 : -1;
}

int hakios_metal_cb_gpu_times(hakios_metal_cb* cb, int64_t* start_ns, int64_t* end_ns) {
    syncOne(cb);
    int64_t s = 0, e = 0;
    if (!cb->cb->gpuTimes(&s, &e)) return -1;
    *start_ns = s - cb->cb->committedNs();
    *end_ns = e - cb->cb->committedNs();
    return 0;
}

}  // extern "C"

// (ws-hm-plugcpu) for the host tests: HM_PLUG_FAST as if set (1), unset (0), or read again from the environment (-1);
// the words a C command buffer recorded so far; how many objects its buffer keeps
namespace hakios::metal {
void cApiPlugFastForTesting(int on) { plugFastSet.store(on, std::memory_order_relaxed); }
std::vector<uint32_t> cApiStreamForTesting(hakios_metal_cb* cb) {
    syncAll(cb);
    return cb->cb->streamForTesting();
}
size_t cApiKeptForTesting(hakios_metal_cb* cb) {
    syncAll(cb);
    return cb->cb->keptCountForTesting();
}
}  // namespace hakios::metal
