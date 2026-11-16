// Metal's command model on the GPU: a queue, the command buffers committed to it in order, and the blit work the
// command processor can do on its own. It is what MTLCommandQueue, MTLCommandBuffer and MTLBlitCommandEncoder are
// in the plug-in (objc/), and it is testable without them.
//
//   CommandQueue        a submission queue of the driver's own; its command buffers complete in commit order, and
//                       a thread of its own waits for their fences and runs their completed handlers in that order
//   CommandBuffer       Metal's states (not enqueued, enqueued, committed, scheduled, completed, error); its
//                       packets go into a buffer of the queue's (returned to the queue once the GPU is done with it)
//   BlitEncoder         copy and fill of buffers, by the command processor: CP_MEMCPY and CP_MEM_WRITE (pm4.hpp).
//                       Offsets and lengths are in bytes and must be multiples of 4: the command processor moves
//                       words. Metal on macOS asks the same of copies; a blit that is not aligned is refused with
//                       the reason, and the command buffer ends in the error state, as Metal's does on a bad blit.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "hakios-metal/compute.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/gpu.hpp"
#include "hakios-metal/pm4.hpp"

namespace hakios::metal {

class CommandQueue;
class Texture;

// a buffer a program sees: a GEM object with its length (MTLBuffer's -length may be less than the object's size)
// (redfin hm-int4) what this process's GPU memory is held for, by kind (HM_POOL accounting: hakios_metal_pool_stats):
// a Buffer made by newBuffer is counted under its kind until it is destroyed
enum PoolKind : uint8_t { kPoolApp = 0, kPoolTexture, kPoolPieces, kPoolProgram, kPoolPrivate, kPoolTable, kPoolKinds };
// [kind] bytes held now; [kPoolKinds] the queues' reuse pools' free bytes; [kPoolKinds + 1] every GEM object of the process
void poolStats(uint64_t out[kPoolKinds + 2]);
// every queue's idle memory let go (CommandQueue::trim); FORCE: what is idle at all, not only what has been for HM_POOL_IDLE_S.
// Returns the bytes freed. Called by newBuffer when the kernel refuses an allocation, and past HM_POOL_SOFT_MB.
uint64_t trimAllQueues(bool force);
// (redfin hm-int4) HM_DEADFILL's draws taken back in this process, every command buffer (render.cpp)
uint64_t fillsDropped();
// (ws-hm-cull) HM_CULL / HM_DEADFILL=2 counts in this process (render.cpp cullResolve): OUT[0] draws culled, [1] their
// pixels (each draw's area), [2] passes resolved, [3] culled under a full-target cover, [4] under a part cover, [5] draws
// a reader kept, [6] HM_DEADFILL=2 take-backs under a full-target cover (also in fillsDropped), [7] their pixels
void cullStats(uint64_t out[8]);
// (ws-hm-cull) HM_CULL: one draw of the open pass, as cullNote (render.cpp) saw it
struct PassCullNote {
    size_t start = 0;                 // stream_ word where its words begin
    uint32_t at[2] = {0, 0};          // stream_ words of its draw packets (CP_DRAW_INDX_OFFSET / CP_DRAW_INDIRECT_MULTI)
    uint8_t nAt = 0;
    uint8_t writes = 0, over = 0;     // attachments it writes; of them, those it overwrites opaquely (a cover)
    bool victim = false;              // may be culled: no side effect, its area known, its draw packets found
    bool cover = false, full = false; // a cover (CA's opaque rectangle), over the whole window
    bool fetch = false;               // reads attachments where it draws (fetch, [[color(n)]], an attachment sampled)
    bool blends = false;              // blending on an attachment it writes
    bool dead = false;
    // (ws-hm-int11) HM_DRAW_SKIP under HM_CULL: the notes whose program / fragment-texture loads this draw left out and
    // relies on (-1: it loaded its own), and the stream word holding the value of its HLSQ_INVALIDATE_CMD write
    int32_t progLoader = -1, texLoader = -1;
    uint32_t inv = 0;
    bool hasInv = false;
    int32_t r[4] = {0, 0, 0, 0};      // pixels it may touch, X0 Y0 X1 Y1 (X1 Y1 past the last)
    int32_t c[4] = {0, 0, 0, 0};      // a cover's pixels written for certain, the same form
    std::string name;                 // its label (HM_DD only)
};
class Buffer {
public:
    Buffer(std::unique_ptr<Bo> bo, uint64_t length) : bo_(std::move(bo)), length_(length) {}
    ~Buffer();
    void countAs(PoolKind kind);   // (redfin hm-int4) newBuffer's: counted under KIND until destroyed
    uint64_t length() const { return length_; }
    void* contents() const { return bo_->cpu(); }
    uint64_t gpuAddress() const { return bo_->gpuAddress(); }
    const Bo& bo() const { return *bo_; }
    // (redfin hmv6) the Bo out of a Buffer being destroyed whose handle another Buffer now holds (surfaceBuffer's deleter)
    std::unique_ptr<Bo> takeBo() { return std::move(bo_); }
    // (ws-hm-pool) HM_RT_CACHE: a render target's memory, offered to the Gpu's reuse cache when the Buffer goes; and
    // whether it came from there
    bool recyclable() const { return recyclable_; }
    bool reused() const { return reused_; }
    // (ws-hm-int12) HM_NOUC: a cacheable copy of the buffer's first N bytes, for a buffer whose bytes the CPU writes once
    // as it is made and never again (the plug-in's -setVertexBytes:/-setFragmentBytes: scratch): the caller has written
    // the same N bytes into contents(). The core's CPU reads of the buffer (render::cpuRead) take them from here instead
    // of the write-combined (on the phone: non-cacheable) mapping. Set once, before the buffer is bound; never changed.
    void setShadow(const void* bytes, uint64_t n);
    const uint8_t* shadow() const { return shadow_.empty() ? nullptr : shadow_.data(); }
    uint64_t shadowLength() const { return shadow_.size(); }

private:
    friend std::shared_ptr<Buffer> newBuffer(Gpu&, uint64_t, std::string*, PoolKind, bool);
    std::unique_ptr<Bo> bo_;
    uint64_t length_;
    uint64_t counted_ = 0;
    PoolKind kind_ = kPoolApp;
    bool recyclable_ = false;
    bool reused_ = false;
    std::vector<uint8_t> shadow_;   // (ws-hm-int12) HM_NOUC
};

// RECYCLABLE (ws-hm-pool): memory whose old contents nobody may read (a render target's, undefined when made, as
// Metal's are): taken from the Gpu's reuse cache when it holds one of the size, and given back to it when the Buffer goes
std::shared_ptr<Buffer> newBuffer(Gpu& gpu, uint64_t length, std::string* why, PoolKind kind = kPoolApp, bool recyclable = false);
// (ws-hm-pool) HM_POOL_KEEP_MB=N (default 0): an idle trim keeps up to N MB of a queue's free pieces and its current
// private buffer (a forced trim, the kernel's pool short, still lets all of it go); HM_RT_CACHE_IDLE_S=S (default 0:
// never) the reuse cache's entries older than S s let go with the idle trims
uint64_t poolKeepBytes();
// the reuse cache's counters for the process's Gpu (Gpu::BoCacheStats in this order: hits, misses, hit bytes, held
// bytes, entries, evicted, dropped, refused, gem-new calls, gem-new ns, retries after a refused allocation, the limit)
void rtCacheStats(uint64_t out[12]);
// the live Buffer whose memory holds GPU address ADDRESS, and *OFFSET its offset in it; nullptr when none does. An
// argument buffer names a buffer member by the address it was encoded with (setBuffer:offset:atIndex:, or the
// gpuAddress an application writes itself), and this is how the draw finds the Buffer to bind and keep.
std::shared_ptr<Buffer> bufferAt(uint64_t address, uint64_t* offset);
// IOSurface SURFACE_ID's memory as a Buffer, *INFO what the kernel says of the surface. One Buffer per surface while it
// lives: a second ask for the same surface gets the same Buffer (the kernel's handle for it is one), so that the
// handle is closed once, by the last owner. nullptr with *why.
std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why);
// (redfin hmv6) test hook: run by a surface Buffer's deleter before it takes the surface lock (the window another thread
// can make a new Buffer over the same handle in); and how many dying Buffers handed their handle over so far
extern void (*gSurfaceDeleterHook)();
uint64_t surfaceHandovers();

class CommandBuffer : public std::enable_shared_from_this<CommandBuffer> {
public:
    enum Status { kNotEnqueued, kEnqueued, kCommitted, kScheduled, kCompleted, kError };

    Status status() const { return status_.load(); }
    const std::string& error() const { return error_; }

    // run after completion (or error), on the queue's thread, in the order of commit; only before commit
    void addCompletedHandler(std::function<void(CommandBuffer&)> handler);
    void enqueue();
    void commit();
    void waitUntilCompleted();
    // how long the GPU had it: from commit to the fence, in nanoseconds (Metal's GPUStartTime..GPUEndTime, roughly)
    // (redfin hmv4) 0 for a buffer that never went through the queue's waiter (refused, failed or finished empty: its
    // completedAt_ stays 0, and the difference was minus the commit's clock -- HM_PROF's "GPU -44796 ms a frame", metal_fps's
    // "GPU busy -558252356 ms")
    int64_t gpuNanoseconds() const { return committedAt_ != 0 && completedAt_ > committedAt_ ? completedAt_ - committedAt_ : 0; }
    // (redfin hm-cache) gpuNanoseconds split: queued behind the buffers committed before it on the one ring (commit to
    // the completion of the one before), and running (from then to its own fence). Both 0 as gpuNanoseconds is.
    // (redfin hm-int3) the decode buffers submit sends just before this one (ycbcrDecodeDeferred, xrDecodeDeferred) are
    // this buffer's work: their running time is counted as this buffer's running, not as its queue, so a slow or hung
    // decode is reported on the frame it belongs to (HM_HANG_EXEC) and never as a buffer of its own
    int64_t queuedNanoseconds() const {
        if (gpuNanoseconds() == 0 || startedAt_ <= committedAt_) return 0;
        const int64_t q = startedAt_ - committedAt_ - preRunningNs_.load(std::memory_order_relaxed);
        return q > 0 ? q : 0;
    }
    int64_t runningNanoseconds() const { return gpuNanoseconds() == 0 ? 0 : gpuNanoseconds() - queuedNanoseconds(); }
    // (redfin hm-int3) of runningNanoseconds, the decode buffers sent just before it (0: none, or not complete yet)
    int64_t decodeRunningNanoseconds() const { return preRunningNs_.load(std::memory_order_relaxed); }
    // (redfin) the GPU's own start and end of it, from CP_ALWAYS_ON_COUNTER stamps the submission writes before its first
    // packet and after its final wait for idle (HM_GPU_STAMPS=0: none), as nanoseconds of the transport's nowNs clock
    // (the counter's 19.2 MHz put on that clock by the queue's calibration against GET_PARAM TIMESTAMP). False when the
    // buffer has not completed, carried no stamps, or the stamps are not in order; committedNs is nowNs at the commit.
    bool gpuTimes(int64_t* startNs, int64_t* endNs) const;
    int64_t committedNs() const { return committedAt_; }
    // (redfin hm-int8) HM_PROF=2's pace line: the kernel submit call's entry and return (the transport's clock; 0 not sent)
    int64_t kernelInNs() const { return kernelInAt_; }
    int64_t kernelOutNs() const { return kernelOutAt_; }

    // --- the blit encoder's work, recorded into this command buffer ---
    bool copy(const Buffer& from, uint64_t fromOffset, const Buffer& to, uint64_t toOffset, uint64_t length);
    bool fill(const Buffer& buffer, uint64_t offset, uint64_t length, uint8_t value);
    // --- the compute encoder's work (compute.hpp): KERNEL over GROUPS work groups of LOCAL threads each (nullptr:
    // the kernel's own size), BUFFERS bound by index, IMMEDIATES in place of the kernel's constants (nullptr: its own).
    // The program and the descriptors go into buffers of their own that live until this completes.
    bool dispatch(const Kernel& kernel, const uint32_t groups[3], const uint32_t* local, const std::vector<Binding>& buffers,
                  const uint32_t* immediates = nullptr, const compute::Options& options = compute::Options{});
    // the same with textures and samplers (compute.hpp: Bindings); the textures' memory is used and kept as buffers are
    bool dispatch(const Kernel& kernel, const uint32_t groups[3], const uint32_t* local, const Bindings& bindings,
                  const uint32_t* immediates = nullptr, const compute::Options& options = compute::Options{});
    // the group counts three 32-bit words in ARGS at OFFSET (a multiple of 4), read when the GPU runs it
    bool dispatchIndirect(const Kernel& kernel, const Buffer& args, uint64_t offset, const uint32_t* local, const Bindings& bindings,
                          const uint32_t* immediates = nullptr, const compute::Options& options = compute::Options{});
    // --- the render encoder's work (render.hpp): one draw of PROGRAM into PASS's colour target, one complete pass per
    // call (begin, state, draw, end). The two programs' words and the pass's table go into buffers of their own that live
    // until this completes; the target's memory is used for write and kept. False with the reason recorded (render::encode's).
    bool draw(const Program& program, const RenderPass& pass, const Draw& draw, const Bindings& vsBindings, const Bindings& fsBindings,
              const render::Options& options = render::Options{});
    // a pass with no draw whose load action is Clear (render::encodeClear): the target filled with the clear colour, a
    // complete pass of its own. What an encoder that recorded no draw needs at its end; anything else is refused.
    bool clear(const RenderPass& pass, const render::Options& options = render::Options{});
    // (redfin hmv4) PASS with its extended-range attachments drawn through their companions (render.cpp); false with *why
    bool xrRedirect(const RenderPass& given, RenderPass* out, const RenderPass** use, std::string* why);
    // -copyFromTexture:...toTexture:... and the scaled copies under it: rectangle S of SRC's mip level SL into rectangle D
    // of DST's level DL by the 2D engine, a pass of its own (render::encodeBlit says what it takes)
    bool blit(const Texture& src, uint32_t srcLevel, render::BlitRect s, const Texture& dst, uint32_t dstLevel, render::BlitRect d, bool linear,
              const render::Options& options = render::Options{});
    // the compressed textures among B's sampled ones made current (Texture::staging); a draw and a dispatch do it first
    bool freshen(const Bindings& b, const render::Options& options = {});
private:
    bool blitInto(const Texture& src, uint32_t srcLevel, render::BlitRect s, const Texture& dst, uint32_t dstLevel, render::BlitRect d, bool linear,
                  const render::Options& options);
public:
    // -generateMipmapsForTexture:: every level past 0 made from the one above it, halved with linear filtering, in order
    bool generateMipmaps(const Texture& texture, const render::Options& options = render::Options{});
    // a blit the caller could not record (a buffer not in the GPU's memory, say): the buffer ends in the error state
    void refuse(const std::string& why) { fail(why); }
    // a buffer it uses, kept alive until it completes (the owner may let go of it before that)
    void keep(std::shared_ptr<const Buffer> b) { kept_.push_back(std::move(b)); }
    // anything else it uses (a compiled kernel's words), the same way
    void keepAlive(std::shared_ptr<const void> p) { keptAny_.push_back(std::move(p)); }
    // (ws-hm-plugcpu) HM_PLUG_FAST: keep() and keepAlive() left out for an object this buffer keeps already. An address
    // in the small tables below was kept by this buffer (kept_/keptAny_ hold it until it completes, or the buffer it is
    // appended to does), so it cannot have been freed and its address reused meanwhile; a slot overwritten by another
    // address costs one more keep, as before. The tables are emptied with the lists.
    template <class T>
    void keepOnce(const std::shared_ptr<T>& b) {
        if (b != nullptr && firstKeep(keepSeen_, b.get())) kept_.push_back(b);
    }
    template <class T>
    void keepAliveOnce(const std::shared_ptr<T>& p) {
        if (p != nullptr && firstKeep(keepAliveSeen_, static_cast<const void*>(p.get()))) keptAny_.push_back(p);
    }
    // OTHER's recorded work after this one's, as if it had been recorded here: its packets, what it names and what it
    // keeps alive -- a parallel render encoder's sub-encoders each record into a command buffer of their own, from any
    // thread in any order, and are appended in the order they were made. OTHER is left empty and is not committed; an
    // error it ended in is this one's. False after commit (of either).
    bool append(CommandBuffer& other);
    // What becomes of a draw, dispatch, copy or clear the encoder refuses (a binding it cannot serve, a shape it does not
    // take): by default the command buffer ends in error with the reason, as every test here expects; with skipRefused
    // that one piece of work is left out, counted and its reason kept, and the rest of the buffer still runs. Apple's
    // driver validates nothing and runs a frame whatever is wrong in it; a game whose every frame has one dispatch this
    // does not serve otherwise shows nothing at all (Resident Evil 4). Running out of memory, or work after commit,
    // still fails the buffer either way.
    void setSkipRefused(bool on) { skipRefused_ = on; }
    uint32_t skipped() const { return skipped_; }
    const std::string& lastSkipped() const { return lastSkipped_; }
    // the encoder's refusal of one piece of work, by the rule above: false either way
    bool refuseWork(const std::string& why);
    // What the buffer holds, counted as it is recorded: passes opened (each a pass start, its flushes and the wait for
    // idle at its tail, which is a fixed cost whatever is drawn), the draws in them, and dispatches, copies and clears
    // (each a pass of its own). What a frame's GPU time goes to is read off these beside the time (metal_fps).
    struct Counts { uint32_t passes = 0, draws = 0, dispatches = 0, copies = 0, clears = 0, gmemPasses = 0, tiles = 0, binnedPasses = 0,
                    fillsDropped = 0,
                    // (ws-hm-perf2) HM_PASS_FLUSH=lazy / HM_PASS_STATIC / HM_PASS_MERGE: pass tails left out (the pass ended with
                    // its colour in the CCU), barriers put down before a read of such colour (full: the CCU flushed; invalidate
                    // alone: what an earlier tail flushed), full tails put down for what lazily ended passes left (before a copy,
                    // a blit, a dispatch, a clear or the end), heads that went on from the pass before (no batch start, no
                    // CCU set-up), draws joined to the open pass although their store actions differ; and the command
                    // processor's own copies and fills (copy, fill), which no pass stamp times, with their bytes
                    tailsSkipped = 0, barriers = 0, invalidates = 0, lazyTails = 0, headsCarried = 0, storeMerges = 0, cpCopies = 0,
                    // (ws-hm-int6) the UBWC pass classes (HM_PROF's "prof: sync: ubwc" line): draw passes with a compressed
                    // colour attachment, of them drawn in GMEM, 2D copies with a compressed source or destination, and
                    // draw passes whose compressed-or-linear class differs from the draw pass before (each re-emits
                    // RB_MRT_FLAG_BUFFER and RB_RENDER_CNTL.FLAG_MRTS whatever HM_PASS_STATIC carried)
                    ubwcPasses = 0, ubwcGmemPasses = 0, ubwcCopies = 0, ubwcSwitches = 0;
                    uint64_t cpCopyBytes = 0; };   // (redfin hm-int4) fillsDropped: HM_DEADFILL's draws taken back (render.cpp)   // (gmemPasses of the passes drawn in tiles, binnedPasses of those binned)
    const Counts& counts() const { return counts_; }
    // With profiling on, the command processor writes its always-on counter (CP_ALWAYS_ON_COUNTER, 19.2 MHz) at the start
    // of each pass and after its tail -- whose wait for idle makes the end the end -- for the first kMaxProfiled passes:
    // what each costs, beside what it was (a draw pass with its target's size and its draws, a copy, a clear, a
    // dispatch). Read once the buffer has completed. metal_fps names a frame's slowest passes with it.
    enum PassKind : uint32_t { kPassDraw = 0, kPassCopy = 1, kPassClear = 2, kPassDispatch = 3 };
    struct PassTime { PassKind kind; uint32_t width, height, draws; double us; uint32_t tiles = 0; uint64_t tuneKey = 0; uint8_t attached = 0, touched = 0; std::string sysmemWhy;
                      // (redfin hmv2, HM_PROF) colour attachment 0's MTLPixelFormat and layout (0 linear, 1 UBWC), and the
                      // bytes the pass's colour attachments had cleared (the sysmem fill, or a GMEM clear's tiles), loaded
                      // into GMEM and stored back from it (a sysmem pass loads and stores nothing: it draws into memory)
                      uint32_t format = 0; uint8_t tiling = 0; uint64_t clearBytes = 0, loadBytes = 0, storeBytes = 0;
                      // (redfin hmv3, HM_PROF) colour attachment 0's MTLLoadAction and MTLStoreAction as the pass asked them
                      // (0xff: no colour attachment)
                      uint8_t loadAction = 0xff, storeAction = 0xff; };   // ATTACHED / TOUCHED: the colour attachments set, and those its draws wrote or read (a GMEM pass's)   // tiles 0: in sysmem; tuneKey: CommandQueue::preferGmem's
    static constexpr uint32_t kMaxProfiled = 64;
    void setProfile(bool on) { profile_ = on; }
    // ...and each draw of a sysmem pass on its own, the first kMaxProfiledDraws: a wait for idle before it and after it,
    // the counter at both (so the draws no longer overlap: what each costs alone), with its program's name. Which
    // draws of a heavy frame are heavy is read off these (metal_fps=draws).
    struct DrawTime { std::string name; double us; uint32_t pass = 0, area[4] = {}; };   // PASS: its pass among passTimes'; AREA render::drawArea's
    static constexpr uint32_t kMaxProfiledDraws = 512;
    void setProfileDraws(bool on) { profileDraws_ = on; }
    std::vector<DrawTime> drawTimes() const;
    // (redfin hm-trace) HM_TRACE=2: each draw named for the trace without changing how it runs (mode 1), or with an
    // RB_DONE_TS event carrying the GPU's timestamp written after it (mode 2, HM_TRACE_DRAW_TS=1): no wait for idle, so
    // the draws still overlap and the difference between two stamps is the time the second one added at the end of the
    // pipe. A GMEM pass's draws are named but never stamped (each runs once per tile). PASS: its pass among passTimes'
    // (0xffffffff: past kMaxProfiled or the profile off); GPU_US < 0: not stamped.
    struct TraceDraw { std::string name; uint32_t fsWords = 0, vertices = 0, pass = 0xffffffffu, area[4] = {}; bool gmem = false, stamped = false; double gpuUs = -1, cpuUs = -1; };
    static constexpr uint32_t kMaxTraceDraws = 2048;
    void setTraceDraws(int mode) { traceDrawMode_ = mode; }
    // the caller's CPU time for the draw just recorded (the plug-in's whole draw call), when it has none yet
    void traceDrawCpu(double us) {
        if (!traceDraws_.empty() && traceDraws_.back().cpuUs < 0) traceDraws_.back().cpuUs = us;
    }
    std::vector<TraceDraw> traceDraws() const;
    // render::Options::gmem for this buffer's draws besides each draw's own (both must allow it): the C API's switch
    void setGmem(bool on) { gmem_ = on; }
    // GMEM's refinements off, for a bisect on the phone: kGmemWholeArea tiles the whole window whatever the draws touch,
    // kGmemEveryAttachment loads and stores every attachment, kGmemFetchBarrier orders a fetch by the barrier
    enum : uint32_t { kGmemWholeArea = 1, kGmemEveryAttachment = 2, kGmemFetchBarrier = 4 };
    void setGmemOff(uint32_t bits) { gmemOff_ = bits; }
    // GMEM or sysmem for each pass by CommandQueue::preferGmem (each pass stamped for it) rather than GMEM whenever a
    // pass can have it: what the plug-in asks; off by default, so that a test gets the mode it asks for
    void setGmemTuned(bool on) { tuning_ = on; }
    // GMEM for a pass of at least DRAWS draws, sysmem for one of fewer; 0: not by the draws. The draws are counted as
    // they come, so the pass is recorded both ways until it closes. SpringBoard's passes of 63 to 161 draws took 13 to
    // 22 ms in GMEM against 17 to 31 in sysmem, its passes of 45 to 49 draws 8 to 10 against 6 to 8, its light ones 5
    // against 0.13.
    void setGmemMinDraws(uint32_t draws) { gmemMinDraws_ = draws; }
    // (redfin hmv3, HM_GMEM=N,T) GMEM only for a pass whose whole-window plan (as it opens) has at most TILES tiles (bins); 0: no limit. A620's
    // 512 KiB GMEM holds a full 1125x2436 BGRA8 screen in 52 tiles, each a load and a store of its bin and every draw
    // replayed: a pass of many tiles loses to drawing it straight into memory
    void setGmemMaxTiles(uint32_t tiles) { gmemMaxTiles_ = tiles; }
    // (redfin ws-hm-ubwc) HM_GMEM=auto: with the tuner (setGmemTuned), a pass whose like had 5 draws or fewer last time
    // stays in sysmem unmeasured (turnip's tu_autotune.cc fallback_use_bypass); the rest is measured both ways
    void setGmemAuto(bool on) { gmemAuto_ = on; }
    // render::Options::gmemBinning for this buffer's draws besides each draw's own (either asks for it)
    void setGmemBinning(bool on) { gmemBinning_ = on; }
    // every fragment texture the null texture (1x1, always in the texture cache): wrong pictures, but a draw's time then
    // says what its shader costs without its texture reads -- a measurement switch, as setGmemOff is a bisect's
    void setNullFsTextures(bool on) { nullFsTextures_ = on; }
    // why the last pass that could not be drawn in GMEM was not (gmemPlan's or gmemDrawOk's reason), empty when none was refused
    const std::string& gmemRefusal() const { return gmemWhy_; }
    // the words recorded so far (what the command processor will read)
    size_t streamWords() const { return stream_.words().size(); }
    const std::vector<uint32_t>& streamForTesting() const { return stream_.words(); }   // (ws-hm-drawcpu)
    std::vector<PassTime> passTimes() const;
    // why profiled pass I of a draw was drawn in sysmem ("" for a GMEM pass, or one not profiled); valid while the buffer lives
    const char* passSysmemWhy(size_t i) const { return i < profiled_.size() ? profiled_[i].sysmemWhy.c_str() : ""; }

    // (redfin hmv6) HM_HANG_DUMP=1 (default): what the buffer holds, recorded as it is encoded (a few words a draw), for
    // the lines a hung or given-up buffer prints (CommandQueue's waiter, through render::notify): the command IB's GPU
    // address and size (the kernel's 'ib1 <base> rem <n>' then points at word size-n, less the CP's prefetch), the
    // passes (target size and format, GMEM or sysmem, draws, word range), the programs drawn (name, address, fragment
    // words, private memory), dispatches, copies, blits, clears and the XR companions. Empty when off.
    std::string hangReport() const;
    enum MarkKind : uint8_t { kMarkDraw = 0, kMarkPassEnd = 1, kMarkDispatch = 2, kMarkCopy = 3, kMarkBlit = 4, kMarkClear = 5 };
    struct Mark { uint32_t word; MarkKind kind; uint8_t gmem; uint32_t w, h, fmt, count, words, pvt; const void* what; const char* name; };
    void mark(const Mark& m) { if (marks_.size() < kMaxMarks) marks_.push_back(m); else marksLost_++; }
    static bool hangDumpOn();
    // (redfin hmv7) HM_HANG_BLAME: the programs of a hung buffer with at least blameWords() fragment words (the uber
    // shaders: the only ones with loops CoreAnimation's data steer) each get one more hang to their name; hangsOf()
    // answers the count (0 for a program never blamed), blame() says what this hang added
    static uint32_t blameWords();
    static uint32_t hangsOf(const void* program);
    std::vector<std::string> blame() const;
    static constexpr size_t kMaxMarks = 4096;

private:
    friend class CommandQueue;
    explicit CommandBuffer(CommandQueue& queue) : queue_(queue) { stream_.adoptRecycled(); }   // (ws-hm-int13)
    std::vector<Mark> marks_;
    uint32_t marksLost_ = 0;
    uint32_t submittedWords_ = 0;   // (redfin hmv6) the command IB as submitted: its size and GPU address
    uint64_t submittedIova_ = 0;
    uint32_t resubmits_ = 0;        // (redfin hmv6) times sent again after a reset gave it up (HM_RESET_RECOVER)
public:
    ~CommandBuffer();   // (its stamp buffers back to the queue: read until the last reader lets go, which is now)
private:
    bool fail(const std::string& why);
    bool skipRefused_ = false;
    Counts counts_;
    bool profile_ = false;
    bool tuning_ = false;     // setGmemTuned's (cleared by a failed stamp buffer too)
    bool profileDraws_ = false;
    std::vector<DrawTime> drawsProfiled_;
    int traceDrawMode_ = 0;                      // (redfin hm-trace)
    uint32_t tracePass_ = 0xffffffffu;           // the open draw pass's index among profiled_
    std::vector<TraceDraw> traceDraws_;
    std::shared_ptr<Buffer> traceStamps_;
    void traceStamp(uint32_t slot);
    std::shared_ptr<Buffer> drawStamps_;
    void drawStamp(uint32_t slot);
    bool gmem_ = true;
    uint32_t gmemOff_ = 0;
    uint32_t gmemMinDraws_ = 0;
    uint32_t gmemMaxTiles_ = 0;   // (redfin hmv3) setGmemMaxTiles'
    bool gmemAuto_ = false;       // (redfin ws-hm-ubwc) setGmemAuto's
    bool gmemBinning_ = false;
    bool nullFsTextures_ = false;
    uint32_t gmemClears_ = 0, gmemClearWords_[kMaxColorOutputs][4] = {};   // (draw's, for the pass it opens)
    std::string gmemWhy_;
    std::unordered_map<uint64_t, uint32_t> ordinals_;   // passes opened so far into each kind of target
    std::vector<PassTime> profiled_;   // as recorded; us filled in by passTimes from the stamps
    std::shared_ptr<Buffer> profileStamps_;
    // the counter into stamp slot SLOT (two a pass: start, end), when profiling and there is room
    void stamp(uint32_t slot);
    void stampStart(PassKind kind, uint32_t width, uint32_t height);
    void stampEnd();
    uint32_t skipped_ = 0;
    std::string lastSkipped_;
    // dispatch and dispatchIndirect: GROUPS, or (GROUPS null) the counts at OFFSET of ARGS
    bool dispatchAny(const Kernel& kernel, const uint32_t* groups, const Buffer* args, uint64_t offset, const uint32_t* local, const Bindings& bindings,
                     const uint32_t* immediates, const compute::Options& options);
    // the end of it, however it ends: the status, the handlers (which see that status), and only then the waiters
    void finish(Status s);
    void use(const Buffer& b, bool write) { uses_.push_back(BoUse{&b.bo(), write}); }
public:
    // What each register written in a pass holds, for appendDeduped: a draw writes some 500 of them and almost all
    // hold their value already, so each is looked up once, in an open-addressed table of its own (std::unordered_map's
    // find and insert, two lookups and a node a register, were most of a draw's 20 us of deduplication on the phone)
    class RegShadow {
    public:
        // whether REG held VALUE; it holds VALUE from now on
        bool hold(uint32_t reg, uint32_t value) {
            if (slots_.empty() || 2 * (used_ + 1) > slots_.size()) grow();
            const size_t mask = slots_.size() - 1;
            for (size_t i = (reg * 2654435761u) & mask;; i = (i + 1) & mask) {
                Slot& sl = slots_[i];
                if (sl.key == reg) {
                    const bool same = sl.value == value;
                    if (!same && journaling_ && !snapped_) log_.push_back(Undo{reg, sl.value, true});
                    sl.value = value;
                    return same;
                }
                if (sl.key == kEmpty) {
                    if (journaling_ && !snapped_) log_.push_back(Undo{reg, 0, false});
                    sl.key = reg;
                    sl.value = value;
                    used_++;
                    return false;
                }
            }
        }
        void clear() {
            if (journaling_ && !snapped_) snapshot();
            for (Slot& sl : slots_) sl.key = kEmpty;
            used_ = 0;
        }
        // (ws-hm-int13) HM_DEADFILL's take-back: what the table holds now is what rollback() returns it to. Was a copy
        // of the whole table every draw (two vectors of 1024+ words: the draw path's biggest memmove after cullNote);
        // now each change after mark() is logged (a draw changes a few dozen registers) and undone in reverse, the
        // whole table copied only when clear() empties it while marked (a packet that made deduplication forget).
        void mark() {
            journaling_ = true;
            snapped_ = false;
            log_.clear();
        }
        void unmark() {
            journaling_ = false;
            snapped_ = false;
            log_.clear();
        }
        void rollback() {
            if (!journaling_) return;
            journaling_ = false;   // (the undo below is not logged)
            if (snapped_) {
                slots_.swap(snap_);
                used_ = snapUsed_;
            } else {
                for (size_t k = log_.size(); k-- > 0;) {
                    const Undo& u = log_[k];
                    if (u.present) hold(u.reg, u.value);
                    else erase(u.reg);
                }
            }
            snapped_ = false;
            log_.clear();
        }
    private:
        static constexpr uint32_t kEmpty = ~0u;
        // (ws-hm-int13) a register and its value side by side: one cache line a lookup (were two vectors, two lines)
        struct Slot {
            uint32_t key, value;
        };
        struct Undo {
            uint32_t reg, value;
            bool present;   // REG held VALUE before (false: it was not in the table)
        };
        void snapshot() {
            snap_ = slots_;
            snapUsed_ = used_;
            snapped_ = true;
        }
        // REG out of the table (linear probing: the entries after it in its run moved back where their probe allows)
        void erase(uint32_t reg) {
            if (slots_.empty()) return;
            const size_t mask = slots_.size() - 1;
            size_t i = (reg * 2654435761u) & mask;
            while (slots_[i].key != reg) {
                if (slots_[i].key == kEmpty) return;
                i = (i + 1) & mask;
            }
            slots_[i].key = kEmpty;
            used_--;
            for (size_t j = (i + 1) & mask; slots_[j].key != kEmpty; j = (j + 1) & mask) {
                const size_t home = (slots_[j].key * 2654435761u) & mask;
                // the entry at J may move to the hole at I when I lies cyclically in [HOME, J)
                const bool movable = i <= j ? (home <= i || home > j) : (home <= i && home > j);
                if (movable) {
                    slots_[i] = slots_[j];
                    slots_[j].key = kEmpty;
                    i = j;
                }
            }
        }
        void grow() {
            std::vector<Slot> old = std::move(slots_);
            slots_.assign(std::max<size_t>(1024, old.size() * 2), Slot{kEmpty, 0});
            used_ = 0;
            const bool j = journaling_;
            journaling_ = false;   // (ws-hm-int13: the rehash is not a change)
            for (const Slot& sl : old) {
                if (sl.key != kEmpty) hold(sl.key, sl.value);
            }
            journaling_ = j;
        }
        std::vector<Slot> slots_;
        size_t used_ = 0;
        bool journaling_ = false, snapped_ = false;
        std::vector<Undo> log_;
        std::vector<Slot> snap_;
        size_t snapUsed_ = 0;
    };

    // (ws-hm-int13) what decided a draw's program-and-pass state (render.cpp: draw), in place: was a std::vector<uint8_t>
    // built by five inserts and copied twice a draw (three allocations); the same bytes, compared the same way
    struct StaticKey {
        static constexpr size_t kBytes = 2 * sizeof(const void*) + 3 * sizeof(uint32_t) + sizeof(ColorBlend) + sizeof(Program::moreBlend) +
                                         sizeof(render::Options);
        size_t n = 0;
        uint8_t bytes[kBytes];
        StaticKey() = default;
        StaticKey(const StaticKey& o) : n(o.n) { if (n != 0) memcpy(bytes, o.bytes, n); }
        StaticKey& operator=(const StaticKey& o) {
            n = o.n;
            if (n != 0 && this != &o) memcpy(bytes, o.bytes, n);
            return *this;
        }
        bool empty() const { return n == 0; }
        void add(const void* p, size_t k) {
            memcpy(bytes + n, p, k);
            n += k;
        }
        bool operator==(const StaticKey& o) const { return n == o.n && memcmp(bytes, o.bytes, n) == 0; }
    };

private:
    // the pass draws are joining (render.cpp: draw), closed by its tail before anything else is recorded or it is submitted
    struct OpenPass {
        RenderPass pass;
        render::Options options;
        uint64_t table;   // the GPU address of the pass's table: a piece (below), kept by the command buffer
        RegShadow regs;   // what each register written in the pass holds (render.cpp: appendDeduped)
        StaticKey staticKey;   // what decided the last draw's program-and-pass state (render.cpp: draw)
        // A pass drawn in GMEM (gmemPlan and every draw gmemDrawOk) is recorded rather than streamed: its draws gather
        // in BODIES, deduplicated among themselves from nothing (BODY_REGS) since each tile replays them after the
        // tile's own register writes, and closePass puts them in the tiles.
        bool gmem = false;
        render::GmemPlan plan;
        pm4::Stream bodies;
        RegShadow bodyRegs;
        // what the draws touch: the render area (X0 Y0 X1 Y1, the union of render::drawArea's) and the attachments they
        // write and read (a framebuffer fetch), as masks -- what closePass tiles, loads and stores
        uint32_t area[4] = {~0u, ~0u, 0, 0};
        uint32_t written = 0, read = 0;
        // Both ways at once (setGmemMinDraws): the draws gathered for tiles in BODIES and, beside them, as a sysmem pass in
        // SYS_STREAM (its head included, deduplicated through REGS), the GMEM head held in HEAD_GMEM; closePass takes the
        // one the pass's draws call for, now that they are known
        bool dual = false;
        pm4::Stream sysStream, headGmem;
        uint32_t clears = 0;        // the attachments a GMEM pass clears in its tiles (render::gmemClearWords), and their words
        uint32_t clearWords[kMaxColorOutputs][4] = {};
        uint64_t kind = 0;          // render::tuneKeyOf the pass and its ordinal in the buffer among passes of that kind
        uint32_t ordinal = 0, draws = 0;
        // (redfin hm-int4) HM_DEADFILL: the pass's last draw, when a later draw of the same geometry may take it back
        // (render.cpp deadFillKey): where its words are in the stream and the dedup state from before them
        struct DeadFill {
            bool armed = false;
            size_t start = 0, end = 0;
            // (the dedup state from before it: REGS marked, RegShadow::mark / rollback, ws-hm-int13)
            StaticKey staticKey;
            render::DrawCache cache;   // (ws-hm-drawcpu)
            bool lastFetched = false;
            std::vector<uint8_t> key;
        } df;
        render::DrawCache cache;   // (ws-hm-drawcpu) HM_DRAW_SKIP: what the pass's last draw left loaded (render::DrawCache)
        bool lastFetched = false;   // the last draw read its attachments (render::encodeInPass's AFTER_FETCH for the next)
        // (ws-hm-cull) HM_DEADFILL=2: the held draw's attachments written, and whether it may be taken back by a cover
        uint8_t dfWrites = 0;
        bool dfVictim = false;
        uint64_t dfPx = 0;
        // (ws-hm-cull) HM_CULL=1..3: the level read as the pass opened (0 off), whether the pass may cull (sysmem, no
        // depth/stencil, one sample; never GMEM or both ways), and a note of each draw (render.cpp cullNote/cullResolve)
        int cullLevel = 0;
        bool cullOk = false;
        std::vector<PassCullNote> cull;
    };
    void cullResolve();   // (ws-hm-cull) HM_CULL: the open pass's covered draws made CP_NOPs (closePass, before anything else)
    // (ws-hm-perf2) LAZY (a draw opening the next pass, HM_PASS_FLUSH=lazy): a sysmem pass ends without its tail, its
    // attachments noted as left in the CCU (dirty_); otherwise the tail, and what earlier lazily ended passes left
    // flushed with it (or alone, with no pass open)
    void closePass(bool lazy = false);
    void useAttachments(const RenderPass& pass);
    // (ws-hm-perf2) memory a lazily ended pass drew into and left in the CCU (dirty_: colour or depth), and memory a tail
    // flushed whose texture caches were not dropped since (flushed_): GPU address ranges, so that two textures over
    // one surface or one buffer are the same memory
    struct Range { uint64_t lo, hi; bool depth; };
    std::vector<Range> dirty_, flushed_;
    render::Options dirtyOptions_;
    void flushDirty();               // the full tail for dirty_, with no pass open (closePass(false))
    void noteFlushed(const RenderPass& pass);
    // the head of the next draw pass may go on from the pass closed last (HM_PASS_STATIC): its register shadow and where
    // the stream ended then (anything recorded since, but the stamps and barriers that keep it up to date, ends that)
    RegShadow carryRegs_;
    size_t carryEnd_ = size_t(-1);
    bool carryOk_ = false;
    void carryKeep(size_t before) { if (carryOk_ && before == carryEnd_) carryEnd_ = stream_.size(); }
    // a barrier as encodeBarrier puts it, into the stream (a draw's read of dirty or flushed memory); false: no memory
    bool barrier(bool full);
    int lastPassUbwc_ = -1;   // (ws-hm-int6) the class of the draw pass opened last: 1 compressed, 0 linear, -1 none yet
    std::unique_ptr<OpenPass> open_;
    const Texture* lastClosed_ = nullptr;   // (redfin hmv4) the colour attachment 0 of the pass closed last (pass splits)
public:
    // (redfin hmv4) a draw pass opened on the same colour attachment 0 as the pass just before it, by why it did not
    // join that one: 0 it asked for a Clear, 1 its store/options/other attachments differ, 2 GMEM ended it, 3 something
    // else came between (a copy, a dispatch, a clear-only pass, a blit, the end of an encoder that closes) -- process-wide
    static void passSplits(uint64_t out[4]);
    // (redfin hmv8) extended-range copies so far: [0] decoded on the processor (all), [1] of them over an IOSurface,
    // [2] ring barriers before such a decode (HM_XR_SYNC), [3] microseconds waited in them, [4] barriers timed out
    // (redfin hmv10) [5] decodes deferred to the commit (HM_XR_LATE), [6] of them checked at completion (HM_XR_CHECK),
    // [7] of those whose rows had changed by then (another process's GPU wrote them after the decode: a stale copy drawn),
    // [8] write-backs on the completion thread (HM_XR_WB_SYNC), [9] microseconds in them
    static void xrStats(uint64_t out[10]);
    // (redfin hmv10) HM_XR_LATE: the decodes deferred to the commit, done now (CommandQueue::submit, before the IB goes)
    // after one ring barrier
    // (redfin hmv11) HM_XR_GPU=1 (default): decoded on the GPU instead, by the xr_decode kernel (tools/kernels/xr_decode.ll)
    // in a command buffer of their own that the queue submits just before this one: the one kernel ring runs it after
    // every submission any process (WebKit's GPU process) made before, and this buffer's draws after it -- no ring barrier,
    // no processor decode. Returned for submit to send first; nullptr when nothing went to the GPU (the processor
    // decoded whatever there was, as hmv10: HM_XR_GPU=0, no compiler, or the kernel refused)
    std::shared_ptr<CommandBuffer> xrDecodeDeferred();
    // (redfin hmv11) [0] surfaces decoded on the GPU, [1] decode buffers sent, [2] fallbacks to the processor (no kernel /
    // a dispatch refused), [3] GPU decodes checked against a processor decode (HM_XR_GPU_VERIFY), [4] of them that differed,
    // [5] microseconds encoding the decode buffers
    static void xrGpuStats(uint64_t out[6]);
    // (redfin hmv11) tests: 1 / 0 HM_XR_GPU on / off whatever the environment says, -1 the environment's again
    static void setXrGpuForTesting(int on);
    // (ws-hm-perf2) the pass switches, read once from the environment: HM_PASS_FLUSH=lazy (default) or always (hm-int5's
    // tail after every pass), HM_PASS_STATIC=1 (default) or 0 (every pass its batch start and CCU set-up),
    // HM_PASS_MERGE=1 (default) or 0 (a draw into the open sysmem pass's attachments joins it only with the same store
    // actions, as hm-int5). Tests: 1 / 0 whatever the environment says, -1 the environment's again
    static bool passFlushLazy();
    static bool passStatic();
    static bool passMerge();
    static void setPassModesForTesting(int lazy, int carry, int merge);
    // (ws-hm-perf2) HM_PROF's sync line: the submitted stream's census (render::census, made at submit when profiling)
    // and the GPU's own time for it and for the decode buffers sent before it (CP_ALWAYS_ON_COUNTER stamps, ns; 0 none)
    const render::SyncCensus& syncCensus() const { return census_; }
    void setCensus(bool on) { censusOn_ = on; }
    // (ws-hm-int6) tests: STREAM appended to this buffer's stream as it stands (an open pass stays open)
    bool appendRawForTesting(const pm4::Stream& stream);   // (the census without profiling: a test's, of the stream as it runs unstamped)
    int64_t gpuStampedNanoseconds() const;
    int64_t preGpuNanoseconds() const { return int64_t(preGpuTicks_.load(std::memory_order_relaxed)) * 625 / 12; }
    // (redfin hm-int5) HM_XR_WB=gpu (default): every extended-range companion this buffer drew is written into its
    // surface's rows by xr_encode (tools/kernels/xr_encode.ll) dispatched at the end of this buffer, at commit: the rows
    // are whole when its fence passes, ordered on the ring before any later GPU work of any process, and no processor
    // write-back is left. A companion the kernel could not take is written back on the processor at completion as before.
    void xrEncodeAppend();
    // (redfin hm-int5) [0] companions encoded on the GPU, [1] buffers that carried encodes, [2] fallbacks to the
    // processor, [3] encodes checked against the processor's codes (HM_XR_VERIFY), [4] of them that differed
    static void xrEncodeStats(uint64_t out[5]);
    // (redfin hmyc) HM_YCBCR=native: the two-plane YCbCr copies this buffer's draws sample, decoded by ycbcr_decode
    // (tools/kernels/ycbcr_decode.ll) in a buffer of their own that submit sends just before this one; what the GPU could
    // not take (HM_YCBCR_GPU=0, no compiler, a dispatch refused) converted on the processor now. nullptr: nothing on the GPU
    std::shared_ptr<CommandBuffer> ycbcrDecodeDeferred();
    // [0] GPU decodes, [1] decode buffers, [2] processor conversions, [3] GPU decodes verified (HM_YCBCR_VERIFY), [4] of
    // them that differed, [5] microseconds encoding, [6] draws that took a copy decoded before, [7] copies left undecoded
    static void ycbcrStats(uint64_t out[8]);
    // tests: 1 | 0 the GPU decode on | off whatever HM_YCBCR_GPU says, -1 the environment's again
    static void setYcbcrGpuForTesting(int on);
private:
    static void ycbcrCountReuse();
    // A piece of a larger buffer, for a draw's or a dispatch's table and a program's words. A buffer of its own for each
    // was three requests to the kernel for every draw, which on the phone cost about 200 us a draw (probe 9r) while the
    // GPU spent 3 us on it. A piece is the command buffer's: written once, used for writing, kept until completion.
    struct Piece {
        std::shared_ptr<Buffer> buffer;
        uint64_t offset = 0;
        uint64_t gpuAddress() const { return buffer->gpuAddress() + offset; }
        uint8_t* contents() const { return static_cast<uint8_t*>(buffer->contents()) + offset; }
    };
    bool piece(uint64_t bytes, Piece* out, std::string* why);
    std::shared_ptr<Buffer> slab_;
    uint64_t slabUsed_ = 0;
    std::vector<std::shared_ptr<Buffer>> slabs_;   // every slab this buffer took, given back to the queue when it is done
    // A program's words where they are already in this command buffer, so that every draw with the program does not
    // upload them again. Found by the caller's pointer, but taken only when the words still look the same: the pointer
    // is the caller's to free and reuse. They are compared at every 64th word and the last, not all: the whole compare,
    // once a draw, was 7-10 us of a draw's 50 on the phone, and CoreAnimation's uber shader is 31 000 words. (The copy
    // compared is a CPU one: reading the piece back is uncached memory.)
    struct Uploaded {
        std::vector<uint32_t> words;
        uint64_t gpuAddress;
    };
    // the GPU address of K's words in this buffer, uploading them when they are not there; 0 with *WHY when out of memory
    uint64_t programAddress(const Kernel& k, std::string* why);
    std::unordered_multimap<const uint32_t*, Uploaded> uploaded_;

    CommandQueue& queue_;
    std::atomic<Status> status_{kNotEnqueued};
    std::string error_;
    pm4::Stream stream_;
    std::vector<BoUse> uses_;
    std::vector<std::shared_ptr<const Buffer>> kept_;   // the buffers it uses stay alive until it completes
    std::vector<std::shared_ptr<const void>> keptAny_;
    // (ws-hm-plugcpu) keepOnce/keepAliveOnce: addresses kept already, direct-mapped
  public:
    size_t keptCountForTesting() const { return kept_.size() + keptAny_.size(); }
  private:
    static constexpr unsigned kKeepSeen = 64;
    const void* keepSeen_[kKeepSeen] = {};
    const void* keepAliveSeen_[kKeepSeen] = {};
    static bool firstKeep(const void** table, const void* p) {
        const unsigned h = unsigned((uint64_t(uintptr_t(p)) >> 4) * 0x9E3779B97F4A7C15ull >> 58);   // 6 bits: kKeepSeen
        if (table[h] == p) return false;
        table[h] = p;
        return true;
    }
    void forgetKeeps() {
        for (auto& e : keepSeen_) e = nullptr;
        for (auto& e : keepAliveSeen_) e = nullptr;
    }
    std::vector<std::pair<const Texture*, std::shared_ptr<Texture>>> xrDecoded_;   // (redfin hmv2) this buffer's decoded extended-range copies
    bool xrSynced_ = false;   // (redfin hmv8) HM_XR_SYNC's ring barrier done for this buffer
    struct XrLate {   // (redfin hmv10) HM_XR_LATE: a copy to decode at commit, and its rows' sum then (HM_XR_CHECK)
        std::shared_ptr<const Texture> source;
        std::shared_ptr<Texture> copy;
        uint64_t sum = 0;
        bool decoded = false;
        bool gpu = false;   // (redfin hmv11) decoded by xr_decode on the GPU (HM_XR_GPU): no processor sum to check
    };
    std::vector<XrLate> xrLate_;
    // (redfin hmyc) this buffer's two-plane YCbCr copies (render.cpp), and those to decode at commit
    std::vector<std::pair<const Texture*, std::shared_ptr<Texture>>> ycDecoded_;
    struct YcLate {
        std::shared_ptr<const Texture> source;
        std::shared_ptr<Texture> copy;
        uint64_t gen = 0;    // the surface's generation it will hold
        bool gpu = false;
    };
    std::vector<YcLate> ycLate_;
    std::shared_ptr<Buffer> paramsYc_;   // a decode buffer's parameters, 64 bytes a dispatch
    uint32_t paramsYcUsed_ = 0;
    std::shared_ptr<Buffer> paramsXr_;   // (redfin hmv11) HM_XR_GPU: a decode buffer's parameters, 32 bytes a dispatch
    uint32_t paramsXrUsed_ = 0;
    std::vector<std::pair<const void*, std::shared_ptr<void>>> xrDrawn_;   // (redfin hmv4) the XR companions it drew into (render.cpp)
    std::vector<std::function<void(CommandBuffer&)>> handlers_;
    std::mutex lock_;
    std::condition_variable done_;
    bool finished_ = false;                             // (under lock_) the handlers have run: waitUntilCompleted returns
    uint32_t fence_ = 0;
    uint64_t faultsAtCommit_ = 0;                       // the GPU's reset count then: one more by completion = given up, not run
    std::unique_ptr<Bo> commands_;
    int64_t committedAt_ = 0, completedAt_ = 0;
    int64_t kernelInAt_ = 0, kernelOutAt_ = 0;   // (redfin hm-int8) gpu_.submit's entry and return
    int64_t startedAt_ = 0;   // (redfin hm-cache) when the ring got to it: the completion of the buffer before it
    // (redfin hm-int3) a decode buffer: the buffer it was sent for (its running time is added to that one's
    // preRunningNs_ at completion); and, on that buffer, the sum of its decode buffers' running times
    std::weak_ptr<CommandBuffer> preFor_;
    std::atomic<int64_t> preRunningNs_{0};
    std::atomic<uint64_t> preGpuTicks_{0};   // (ws-hm-perf2) the decode buffers' own GPU time, stamped (preGpuNanoseconds)
    render::SyncCensus census_;
    bool censusOn_ = false;
    Piece gpuStamps_;                                   // (redfin) the two counter stamps, when gpuStamps_.buffer (until finish)
    uint64_t gpuTicks_[2] = {0, 0};                     // ...as finish read them
    bool unsubmitted_ = false;                          // (redfin) empty: completed in order without the GPU
    pm4::Stream scratch_[4];                            // (redfin) draw()'s head, body, headSys, bodySys, reused
    std::vector<uint8_t> dfKeyScratch_;                 // (ws-hm-int13) draw()'s deadFillKey, reused (swapped with df.key)
};

class CommandQueue {
public:
    // a queue of its own with the driver; nullptr with *why when the driver has none to give
    static std::unique_ptr<CommandQueue> make(Gpu& gpu, std::string* why);
    ~CommandQueue();

    std::shared_ptr<CommandBuffer> commandBuffer();
    Gpu& gpu() { return gpu_; }
    // the buffers kept for command streams, idle (to see that they are given back and used again)
    size_t idleCommandBuffers();
    // (redfin hm-int4) the queue's idle memory let go: the reuse pool, command BOs past 8, private memory no buffer in flight
    // uses, and (FORCE) programs no command buffer holds. Without FORCE only what has not been used for HM_POOL_IDLE_S
    // seconds (default 10; 0: never by time) and what is never used again (an outgrown private buffer). The bytes freed.
    uint64_t trim(bool force);
    uint64_t freeBytes();
    // What a draw binds at the slot the front end reserves for Metal's null texture (Kernel: KernelTexture::isNull): a
    // 1x1 RGBA8Unorm texture of zeros, made at the first ask, the same for every draw of the queue. Zero is an
    // assumption, not a reading -- nothing reachable says what Apple's null texture samples as (nir_air.h) -- and this
    // is the one place it lives; a probe may write the texel to see which instructions read it. nullptr with *why.
    std::shared_ptr<Texture> nullTexture(std::string* why);
    // Private memory for a stage that wants BYTES a fiber in the PERWAVE layout: one buffer for every program of the
    // queue, as turnip keeps one a device (tu_setup_pvtmem, tu_shader.cc:2132-2190) -- a buffer a pipeline is 4 MiB for
    // 512 bytes a fiber on this GPU -- grown to the next power of two when a program wants more. A grown-out buffer is
    // kept as long as the queue: a draw recorded against it may still be in flight. The buffer and *OUT its layout;
    // nullptr with *why.
    std::shared_ptr<Buffer> privateMemory(uint32_t bytes, bool perWave, PrivateMemory* out, std::string* why);
    // (redfin) the probe's private-memory experiments (compute::Options pvtFiberScale / pvtBufferScale): a buffer of
    // its own, not the queue's shared one, sized as if an SP had FIBERSCALE times the chip's fibers (TOTALPVTMEMSIZE
    // and the stride follow) and BUFFERSCALE times that again in bytes behind it (the registers do not follow)
    std::shared_ptr<Buffer> privateMemoryScaled(uint32_t bytes, bool perWave, uint32_t fiberScale, uint32_t bufferScale, PrivateMemory* out,
                                                std::string* why);
    // GMEM or sysmem for a pass into the target at KEY (its memory's address), by what the GPU spent on such passes:
    // each mode's time kept as a running average from the passes' own counter stamps (tuned), the cheaper one taken,
    // the other tried again now and then, sysmem tried first and GMEM next -- turnip's autotune, by time instead of samples.
    // GMEM is no gain for a pass that touches a corner of the screen (its tiles load and store the whole target: 23
    // draws of CoreAnimation took 5 ms so, 0.1 ms in sysmem), nor for one of framebuffer fetches (the texture unit
    // reads GMEM at a quarter of the rate it reads memory: probe "full-screen cost").
    // the visibility streams' memory binned GMEM passes share (render::vscBytes): one is enough, the GPU running one
    // submission at a time and each pass's binning before its tiles; nullptr with *why
    std::shared_ptr<Buffer> vscBuffer(std::string* why);
    // K's words in a buffer of their own, made the first time a command buffer of this queue draws with them and kept
    // for the next: copying them into each command buffer was most of a draw's first cost (CoreAnimation's uber shader is
    // 124 KB, into write-combined memory). Found by the words' pointer and taken only when every word is the same -- a
    // freed program's pointer may come back with other words, and then its entry goes. nullptr with *why.
    std::shared_ptr<Buffer> programBuffer(const Kernel& k, std::string* why);
    // A slab for draw tables (CommandBuffer::piece): one a completed command buffer gave back, zeroed as a new one comes,
    // or a new one. The kernel's making of a 256 KB buffer, once every 32 draws, was most of a draw's CPU time on the
    // phone (40 us a draw on a fast core, where the same encoding on the host takes 5).
    std::shared_ptr<Buffer> takeSlab(uint64_t bytes, std::string* why);
    void giveSlabs(std::vector<std::shared_ptr<Buffer>>* slabs);
    // ...and any other buffer a command buffer makes for itself (a large piece, its counter stamps): BYTES rounded up to
    // a power of two from 4 KB, zeroed; given back the same way, 32 MB of them kept at most
    std::shared_ptr<Buffer> takeBuffer(uint64_t bytes, std::string* why);
    void giveBuffer(std::shared_ptr<Buffer> b);
    // whether GMEM passes may still be binned: no pass's visibility streams have overrun their pitch (render::kVscOverflowAt,
    // read by checkVsc once a buffer with a binned pass has completed)
    bool binningOk() const { return !vscOverflowed_.load(); }
    void checkVsc();
    bool preferGmem(uint64_t key);
    void tuned(uint64_t key, bool gmem, double us);
    // What tells passes into one kind of target apart: the draws the pass at that place in a command buffer (the Nth into
    // this kind of target) had the last time, which preferGmem's key takes in as a class (a frame of CoreAnimation is
    // the same passes in the same order as the one before it, light or heavy). 0 when not seen yet.
    uint32_t lastDraws(uint64_t kind, uint32_t ordinal);
    // ...and the most it had in the times since its peak, the peak let go after 16 times below it: a pass that is heavy
    // only now and then (CoreAnimation's: light frames between the heavy ones of an animation) counts as heavy
    uint32_t peakDraws(uint64_t kind, uint32_t ordinal);
    void noteDraws(uint64_t kind, uint32_t ordinal, uint32_t draws);
    // (redfin hmv8) until the GPU has run everything any client put on the ring before now (one ring, in order: an
    // empty submission's fence covers it), or TIMEOUT_NS: 0, ETIMEDOUT or an errno. HM_XR_SYNC's barrier.
    int ringBarrier(int64_t timeoutNs);
    // (redfin) whether the pass at that place has had the same number of draws the last 4 times, and *DRAWS that number
    bool stableDraws(uint64_t kind, uint32_t ordinal, uint32_t* draws);


private:
    friend class CommandBuffer;
    CommandQueue(Gpu& gpu, uint32_t id);
    void maybeTrim();   // (redfin hm-int4) trim(false) at most once a second
    std::atomic<int64_t> lastTrimNs_{0}, lastPoolUseNs_{0}, lastPrivateUseNs_{0};
    void submit(std::shared_ptr<CommandBuffer> cb);
    void waiter();
    // (redfin) the always-on counter (19.2 MHz ticks) against nowNs: a pair read together, again at most every 0.5 s
    void calibrate();
    bool ticksToNs(uint64_t ticks, int64_t* ns);
    std::mutex calibLock_;
    int64_t calibNs_ = 0;
    uint64_t calibTicks_ = 0;
    bool calibrated_ = false;
    std::atomic<uint64_t> faultsSeen_{0};   // (redfin) GET_PARAM FAULTS as the waiter read it last
    std::atomic<bool> faultsKnown_{false};
    std::unique_ptr<Bo> takeCommandBo(size_t bytes, int* error);
    void recoverAfterReset(CommandBuffer& hung, uint64_t faults);   // (redfin hmv6) HM_RESET_RECOVER, lock_ held
    int64_t lastCompletedAt_ = 0;   // (redfin hm-cache) the waiter's last completion (CommandBuffer::startedAt_)
    uint64_t hangsReported_ = 0;                                       // (redfin hmv6) the reset count the waiter last acted on

    Gpu& gpu_;
    std::shared_ptr<Texture> null_;   // (under lock_) nullTexture's
    std::shared_ptr<Buffer> vsc_;     // (under lock_) vscBuffer's
    // (under poolLock_, a lock of its own: a command buffer gives its stamps back as it is destroyed, which can be under
    // lock_ -- the queue letting go of the last one in flight -- and lock_ there was the window server hung at boot)
    std::mutex poolLock_;
    std::unordered_map<uint64_t, std::vector<std::shared_ptr<Buffer>>> freeBuffers_;   // by size: giveSlabs', giveBuffer's
    uint64_t freeBytes_ = 0;
    struct Resident { std::vector<uint32_t> words; std::shared_ptr<Buffer> buffer; };
    std::unordered_multimap<const uint32_t*, Resident> resident_;   // (under lock_) programBuffer's
    uint64_t residentBytes_ = 0;
    std::atomic<bool> vscOverflowed_{false};
    std::vector<std::shared_ptr<Buffer>> private_[2];   // (under lock_) privateMemory's, by layout, the last the current one
    uint32_t privatePerFiber_[2] = {0, 0};
    std::vector<std::shared_ptr<Buffer>> privateScaled_;   // (under lock_) privateMemoryScaled's, kept with the queue
    uint32_t id_;
    std::mutex lock_;
    struct Tune { double us[2] = {0, 0}; uint32_t runs[2] = {0, 0}; uint32_t passes = 0; };
    std::unordered_map<uint64_t, Tune> tune_;   // (under lock_) preferGmem's, by target
    std::unordered_map<uint64_t, uint32_t> lastDraws_;   // (under lock_) noteDraws', by kind and ordinal
    struct Peak { uint32_t draws = 0, below = 0, last = ~0u, same = 0; };   // (redfin: last, same -- stableDraws')
    std::unordered_map<uint64_t, Peak> peakDraws_;        // (under lock_) noteDraws' too: peakDraws'
    std::condition_variable changed_;
    std::deque<std::shared_ptr<CommandBuffer>> inFlight_;
    std::vector<std::unique_ptr<Bo>> idle_;
    std::vector<std::unique_ptr<Bo>> barrierLate_;   // (redfin hmv8) a barrier's IB the GPU had not passed in time: never reused
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace hakios::metal
