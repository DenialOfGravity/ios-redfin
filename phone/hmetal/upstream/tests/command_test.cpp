// The Metal model against the fake GPU, which runs the packets: what a blit leaves in memory, the order of
// completion, the states, and the refusals.
#include <atomic>
#include <cerrno>
#include <cstring>
#include <random>

#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "test.hpp"
#include "transport_hook.hpp"

using namespace hakios::metal;

namespace {
struct Rig {
    explicit Rig(bool async = false, uint32_t delayUs = 0, bool recovers = true) {
        auto f = std::make_unique<FakeGpu>(async, delayUs, recovers);
        fake = f.get();
        gpu = std::make_unique<Gpu>(HMETAL_TRANSPORT_OF(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
    ~Rig() {
        queue.reset();
    }
    FakeGpu* fake;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
};

std::shared_ptr<Buffer> buffer(Rig& r, uint64_t n) {
    std::string why;
    auto b = newBuffer(*r.gpu, n, &why);
    CHECK(b != nullptr);
    return b;
}
}  // namespace

TEST(gpu_params) {
    // what the kernel answers (hakios: bsd/dev/hakios.adreno.c, hakios_gpu_parameter), written here from that source by
    // hand, so that the fake and this test are two readings of it and not one
    Rig r;
    uint64_t v = 0;
    CHECK_EQ(r.gpu->param(kParamGpuId, &v), 0);
    CHECK_EQ(v, 0ull);                                      // "GPU_ID: none, the chip ID says it"
    CHECK_EQ(r.gpu->param(kParamChipId, &v), 0);
    const uint32_t chipId = r.fake->chip().chipId == a6xx::kChipA635.chipId ? 0x06030500u : 0x06020000u;   // (redfin) as which chip
    CHECK_EQ(uint32_t(v), chipId);                          // GPU_CHIP_ID (dt qcom,chipid) ...
#ifndef HMETAL_TEST_TRANSPORT
    CHECK_EQ(v >> 32, FakeGpu::kSpeedBin);                  // ... "with the speed bin above it"
#else
    CHECK_EQ(v >> 32, 0ull);                                // (redfin) our kernel's ABI v1 has no speed bin
#endif
    CHECK_EQ(r.gpu->chipId(), chipId);
    CHECK_EQ(r.gpu->param(kParamGmemSize, &v), 0);
    CHECK_EQ(v, 512ull * 1024);
    CHECK_EQ(r.gpu->param(kParamMaxFreq, &v), 0);
#ifndef HMETAL_TEST_TRANSPORT
    CHECK_EQ(v, 550000000ull);                              // core_khz * 1000, the default clock
#else
    CHECK_EQ(v, 625000000ull);                              // (redfin) the transport's: redfin's top level (xnu 0904)
#endif
    CHECK_EQ(r.gpu->param(kParamFaults, &v), 0);
    CHECK_EQ(v, 0ull);
    CHECK_EQ(r.gpu->param(kParamPriorities, &v), 0);
    CHECK_EQ(v, 1ull);
    CHECK_EQ(r.gpu->param(kParamHighestBankBit, &v), 0);
    CHECK_EQ(v, 14ull);                                     // both chips' kernels (A620: xnu 0904)
    CHECK_EQ(r.gpu->param(kParamUbwcSwizzle, &v), 0);
    CHECK_EQ(v, 6ull);
    CHECK_EQ(r.gpu->param(kParamMacrotileMode, &v), 0);
    CHECK_EQ(v, uint64_t(FakeGpu::defaultChip()->macrotileMode));   // (redfin) A635 1, A620 0
    CHECK_EQ(r.gpu->ubwc().highestBankBit, 14u);
    CHECK(r.gpu->ubwc().fromDriver);
    CHECK_EQ(r.gpu->param(0x0e, &v), EINVAL);               // VA_START: "addresses are ours to give"
    CHECK_EQ(r.gpu->param(0x0f, &v), EINVAL);               // VA_SIZE
    CHECK_EQ(r.gpu->param(0x99, &v), EINVAL);
}

TEST(copy_moves_the_bytes) {
    Rig r;
    auto a = buffer(r, 1 << 20), b = buffer(r, 1 << 20);
    std::mt19937 rng(7);
    auto* pa = static_cast<uint32_t*>(a->contents());
    for (size_t i = 0; i < (1 << 18); i++) pa[i] = rng();
    memset(b->contents(), 0, 1 << 20);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->copy(*a, 4096, *b, 8192, 500000));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK(memcmp(static_cast<uint8_t*>(b->contents()) + 8192, static_cast<uint8_t*>(a->contents()) + 4096, 500000) == 0);
    // nothing before or after the copied range was touched
    auto* pb = static_cast<uint8_t*>(b->contents());
    bool clean = true;
    for (size_t i = 0; i < 8192; i++) clean = clean && pb[i] == 0;
    for (size_t i = 8192 + 500000; i < (1 << 20); i++) clean = clean && pb[i] == 0;
    CHECK(clean);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(fill_any_length) {
    Rig r;
    auto b = buffer(r, 1 << 20);
    for (uint64_t len : {4ull, 64ull, 256ull, 260ull, 4096ull, 65540ull, 999996ull}) {
        memset(b->contents(), 0x11, 1 << 20);
        auto cb = r.queue->commandBuffer();
        CHECK(cb->fill(*b, 16, len, 0xa5));
        cb->commit();
        cb->waitUntilCompleted();
        auto* p = static_cast<uint8_t*>(b->contents());
        bool ok = p[15] == 0x11 && p[16 + len] == 0x11;
        for (uint64_t i = 16; i < 16 + len; i++) ok = ok && p[i] == 0xa5;
        CHECK(ok);
    }
    CHECK_EQ(r.fake->stats().faults, 0u);
}

TEST(copy_then_fill_in_one_buffer_keeps_order) {
    Rig r;
    auto a = buffer(r, 4096), b = buffer(r, 4096);
    memset(a->contents(), 0x5a, 4096);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->fill(*b, 0, 4096, 0x00));
    CHECK(cb->copy(*a, 0, *b, 1024, 1024));
    CHECK(cb->fill(*b, 1536, 256, 0xff));
    cb->commit();
    cb->waitUntilCompleted();
    auto* p = static_cast<uint8_t*>(b->contents());
    CHECK_EQ(p[1023], 0x00);
    CHECK_EQ(p[1024], 0x5a);
    CHECK_EQ(p[1535], 0x5a);
    CHECK_EQ(p[1536], 0xff);
    CHECK_EQ(p[1791], 0xff);
    CHECK_EQ(p[1792], 0x5a);
    CHECK_EQ(p[2048], 0x00);
}

TEST(unaligned_copies_go_through_the_2d_engine_byte_for_byte) {
    Rig r;
    auto a = buffer(r, 65536), b = buffer(r, 65536);
    auto* pa = static_cast<uint8_t*>(a->contents());
    auto* pb = static_cast<uint8_t*>(b->contents());
    // every byte of the source its own (a period of 251, prime, so no shift of a few bytes lands on the same values)
    for (uint32_t i = 0; i < 65536; i++) pa[i] = uint8_t(i % 251 + 1);
    memset(pb, 0xee, 65536);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->copy(*a, 1, *b, 70, 37));          // across a 64-byte line, source and destination at different offsets in it
    CHECK(cb->copy(*a, 3, *b, 1029, 20000));     // longer than one row of the engine (0x4000 texels)
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->stats().faults, 0u);
    CHECK(memcmp(pb + 70, pa + 1, 37) == 0);
    CHECK(memcmp(pb + 1029, pa + 3, 20000) == 0);
    // the bytes on either side of each range are what they were
    CHECK_EQ(pb[69], 0xee);
    CHECK_EQ(pb[107], 0xee);
    CHECK_EQ(pb[1028], 0xee);
    CHECK_EQ(pb[21029], 0xee);
}

TEST(copies_outside_a_buffer_and_unaligned_fills_are_refused) {
    Rig r;
    auto a = buffer(r, 4096), b = buffer(r, 4096);
    auto cb = r.queue->commandBuffer();
    CHECK(!cb->copy(*a, 0, *b, 4000, 200));  // past the end
    std::atomic<int> handled{0};
    cb->addCompletedHandler([&](CommandBuffer& c) { handled += c.status() == CommandBuffer::kError ? 1 : 100; });
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kError);
    CHECK(!cb->error().empty());
    CHECK_EQ(handled.load(), 1);
    CHECK_EQ(r.fake->stats().submits, 0u);    // nothing of it reached the GPU
    auto cb2 = r.queue->commandBuffer();
    CHECK(!cb2->fill(*b, 2, 4, 0));
}

TEST(handlers_run_in_commit_order_while_the_gpu_is_behind) {
    Rig r(true, 200);
    auto b = buffer(r, 65536);
    std::vector<int> order;
    std::mutex m;
    std::vector<std::shared_ptr<CommandBuffer>> all;
    r.fake->pause();
    for (int i = 0; i < 40; i++) {
        auto cb = r.queue->commandBuffer();
        cb->fill(*b, 4 * i, 4, uint8_t(i));
        cb->addCompletedHandler([&, i](CommandBuffer&) {
            std::lock_guard<std::mutex> g(m);
            order.push_back(i);
        });
        cb->commit();
        CHECK(cb->status() == CommandBuffer::kScheduled);   // committed and handed over, the GPU not done
        all.push_back(cb);
    }
    r.fake->resume();
    all.back()->waitUntilCompleted();
    // the last one done means every one before it is done, with its handler run
    for (auto& cb : all) cb->waitUntilCompleted();
    CHECK_EQ(order.size(), size_t(40));
    bool inOrder = true;
    for (int i = 0; i < int(order.size()); i++) inOrder = inOrder && order[i] == i;
    CHECK(inOrder);
    auto* p = static_cast<uint8_t*>(b->contents());
    CHECK_EQ(p[4 * 39], 39);
}

TEST(command_memory_is_used_again) {
    Rig r(true);
    auto b = buffer(r, 4096);
    for (int i = 0; i < 100; i++) {
        auto cb = r.queue->commandBuffer();
        cb->fill(*b, 0, 4096, uint8_t(i));
        cb->commit();
        cb->waitUntilCompleted();
    }
    // one command stream's buffer, taken back after every completion: not a hundred of them
    CHECK(r.queue->idleCommandBuffers() <= 2);
    CHECK(r.fake->liveBuffers() <= 4);
}

TEST(buffers_are_given_back) {
    Rig r;
    {
        auto a = buffer(r, 1 << 16);
        auto b = buffer(r, 1 << 16);
        CHECK(r.fake->liveBuffers() >= 2);
    }
    CHECK_EQ(r.fake->liveBuffers(), 0u);
}

TEST(a_bad_stream_is_a_hang_the_kernel_resets_the_gpu_for) {
    // a packet the real GPU hangs on, with the kernel's gpu_recover=1: the hung submission's own wait fails, FAULTS
    // counts the reset, and the GPU takes work again
    Rig r;
    int e = 0;
    auto bo = r.gpu->newBo(4096, 0, &e);
    auto* w = static_cast<uint32_t*>(bo->cpu());
    w[0] = pm4::type7(0x7f, 0) ^ (1u << 15);   // parity broken
    uint32_t fence = 0;
    CHECK_EQ(r.gpu->submit(0, *bo, 0, 1, {}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), ETIMEDOUT);
    CHECK_EQ(r.fake->stats().faults, 1u);
    CHECK_EQ(r.gpu->faults(), 1ull);
    w[0] = pm4::type7(pm4::kNop, 0);
    CHECK_EQ(r.gpu->submit(0, *bo, 0, 1, {}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(r.gpu->faults(), 1ull);
}

TEST(a_command_buffer_behind_a_hang_is_given_up_not_run) {
    // The reset gives up everything the GPU had been given: the fence word is set to the last submission's number, so
    // a command buffer behind the hung one completes without having run. It must say so, not claim completion over
    // memory it never touched (two zero buffers were once counted as a match that way).
    Rig r(true);
    auto b = buffer(r, 4096);
    memset(b->contents(), 0, 4096);
    r.fake->pause();
    int e = 0;
    auto bad = r.gpu->newBo(4096, 0, &e);
    static_cast<uint32_t*>(bad->cpu())[0] = pm4::type7(0x7f, 0) ^ (1u << 15);
    uint32_t fence = 0;
    CHECK_EQ(r.gpu->submit(0, *bad, 0, 1, {}, &fence), 0);   // first in the ring: hangs
    auto cb = r.queue->commandBuffer();                        // behind it
    CHECK(cb->fill(*b, 0, 4096, 0xee));
    cb->commit();
    r.fake->resume();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kError);
    CHECK(cb->error().find("given up") != std::string::npos);
    CHECK_EQ(static_cast<uint8_t*>(b->contents())[0], 0);     // and indeed nothing of it ran
    CHECK_EQ(r.gpu->faults(), 1ull);
    // committed after the reset: runs
    auto cb2 = r.queue->commandBuffer();
    CHECK(cb2->fill(*b, 0, 4096, 0xee));
    cb2->commit();
    cb2->waitUntilCompleted();
    CHECK(cb2->status() == CommandBuffer::kCompleted);
    CHECK_EQ(static_cast<uint8_t*>(b->contents())[4095], 0xee);
}

TEST(without_recovery_the_first_hang_ends_every_later_wait) {
    // the kernel without gpu_recover=1: gGPUReported is set by the first wait that times out and never cleared
    Rig r(false, 0, false);
    int e = 0;
    auto bo = r.gpu->newBo(4096, 0, &e);
    auto* w = static_cast<uint32_t*>(bo->cpu());
    w[0] = pm4::type7(0x7f, 0) ^ (1u << 15);
    uint32_t fence = 0;
    CHECK_EQ(r.gpu->submit(0, *bo, 0, 1, {}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), ETIMEDOUT);
    CHECK_EQ(r.gpu->faults(), 0ull);                           // no reset to count
    w[0] = pm4::type7(pm4::kNop, 0);
    CHECK_EQ(r.gpu->submit(0, *bo, 0, 1, {}, &fence), 0);     // still taken, as the ring takes them ...
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), ETIMEDOUT);    // ... and never run
    auto b = buffer(r, 4096);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->fill(*b, 0, 4096, 0x11));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kError);
    CHECK(cb->error().find("without gpu_recover=1") != std::string::npos);
}

TEST(a_fence_never_handed_out_is_a_hang) {
    // the kernel does not know such a fence: it waits the whole patience, and that is a hang like any other
    Rig r;
    CHECK_EQ(r.gpu->wait(0, 7, 1000000), ETIMEDOUT);
    CHECK_EQ(r.gpu->faults(), 1ull);
}

TEST(big_copies_are_cut_into_packets_the_gpu_takes) {
    Rig r;
    auto a = buffer(r, 16 << 20), b = buffer(r, 16 << 20);
    auto* pa = static_cast<uint32_t*>(a->contents());
    for (size_t i = 0; i < (4u << 20); i++) pa[i] = uint32_t(i * 2654435761u);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->copy(*a, 0, *b, 0, 16 << 20));
    CHECK(cb->fill(*a, 0, 16 << 20, 0x77));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(static_cast<uint32_t*>(b->contents())[(4u << 20) - 1], uint32_t(((4u << 20) - 1) * 2654435761u));
    CHECK_EQ(static_cast<uint8_t*>(a->contents())[(16 << 20) - 1], 0x77);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// (redfin hmv6) HM_RESET_RECOVER: the buffer the waiter was on when the reset gave everything up fails; the ones of the
// queue given up behind it are sent again (the kernel keeps BOs and mappings over a reset) and complete, having run
TEST(after_a_reset_the_buffers_behind_the_failed_one_are_sent_again) {
    Rig r(true);
    auto a = buffer(r, 4096);
    auto b = buffer(r, 4096);
    memset(a->contents(), 0, 4096);
    memset(b->contents(), 0, 4096);
    r.fake->pause();
    int e = 0;
    auto bad = r.gpu->newBo(4096, 0, &e);
    static_cast<uint32_t*>(bad->cpu())[0] = pm4::type7(0x7f, 0) ^ (1u << 15);
    uint32_t fence = 0;
    CHECK_EQ(r.gpu->submit(0, *bad, 0, 1, {}, &fence), 0);   // hangs
    auto cb1 = r.queue->commandBuffer();
    CHECK(cb1->fill(*a, 0, 4096, 0x11));
    cb1->commit();
    auto cb2 = r.queue->commandBuffer();
    CHECK(cb2->fill(*b, 0, 4096, 0x22));
    cb2->commit();
    r.fake->resume();
    cb2->waitUntilCompleted();
    CHECK(cb1->status() == CommandBuffer::kError);              // the one the waiter was on: failed
    CHECK(cb1->error().find("given up") != std::string::npos);
    CHECK(cb2->status() == CommandBuffer::kCompleted);          // behind it: sent again, and run
    CHECK_EQ(static_cast<uint8_t*>(b->contents())[4095], 0x22);
    CHECK(cb1->hangReport().find("1 copies/fills") != std::string::npos);
}

// (redfin hmv7/hmv9) HM_HANG_BLAME: every program of a hung buffer gets a hang (one per buffer however often drawn in
// it; HM_HANG_BLAME_WORDS, default 0); a line for a big one (1024 words or more) at once, for a small one from its second
// hung buffer, then a count line
TEST(a_hung_buffers_programs_are_blamed_once_per_buffer_small_ones_from_two) {
    Rig r(true);
    static int big = 0, small = 0;
    auto cb = r.queue->commandBuffer();
    using M = CommandBuffer::Mark;
    cb->mark(M{0, CommandBuffer::kMarkDraw, 0, 64, 64, 80, 6, 32064, 0, &big, "uber"});
    cb->mark(M{10, CommandBuffer::kMarkDraw, 0, 64, 64, 80, 6, 32064, 0, &big, "uber"});
    cb->mark(M{20, CommandBuffer::kMarkDraw, 0, 64, 64, 80, 6, 32, 0, &small, "small"});
    CHECK_EQ(CommandBuffer::blameWords(), 0u);
    CHECK_EQ(CommandBuffer::hangsOf(&big), 0u);
    auto lines = cb->blame();
    CHECK_EQ(lines.size(), size_t(2));
    CHECK(lines[0].find("uber") != std::string::npos && lines[0].find("in 1 hung buffer now") != std::string::npos);
    CHECK(lines[1].find("2 of 2 programs tallied, 1 of them small") != std::string::npos);
    CHECK_EQ(CommandBuffer::hangsOf(&big), 1u);
    CHECK_EQ(CommandBuffer::hangsOf(&small), 1u);
    auto again = cb->blame();
    CHECK_EQ(again.size(), size_t(3));
    CHECK(again[0].find("uber") != std::string::npos && again[0].find("in 2 hung buffers now") != std::string::npos);
    CHECK(again[1].find("small") != std::string::npos && again[1].find("fsw 32: in 2 hung buffers now") != std::string::npos);
    CHECK_EQ(CommandBuffer::hangsOf(&big), 2u);
    CHECK_EQ(CommandBuffer::hangsOf(&small), 2u);
    auto cb2 = r.queue->commandBuffer();
    auto none = cb2->blame();
    CHECK(none.size() == 1 && none[0].find("nothing blamed") != std::string::npos);
}

// (redfin hmv9) the hang dump names every program of a pass, 8 a line (was: 8 and "...")
TEST(a_hang_dump_names_every_program_of_a_pass) {
    Rig r(true);
    static int p[12];
    auto cb = r.queue->commandBuffer();
    using M = CommandBuffer::Mark;
    for (int i = 0; i < 12; i++) cb->mark(M{uint32_t(i), CommandBuffer::kMarkDraw, 0, 64, 64, 80, 6, 32, 0, &p[i], i == 11 ? "last" : "p"});
    cb->mark(M{20, CommandBuffer::kMarkPassEnd, 0, 64, 64, 80, 12, 0, 0, nullptr, nullptr});
    const std::string rep = cb->hangReport();
    CHECK(rep.find("last(") != std::string::npos);
    CHECK(rep.find("programs (more):") != std::string::npos);
    CHECK(rep.find("...") == std::string::npos);
}

namespace {
Gpu* gHandoverGpu = nullptr;
std::shared_ptr<Buffer> gHandoverNew;
void makeAnotherWhileDying() {
    // what another thread does in the dying Buffer's window: asks for the same surface, finds the entry expired
    SurfaceInfo info;
    std::string why;
    gHandoverNew = surfaceBuffer(*gHandoverGpu, 31, &info, &why);
}
}  // namespace

// (redfin hmv6) a surface Buffer dying while another is made over the same kernel handle leaves the handle open for the
// new one (was: closed it, and every later submission naming the surface was refused, ENOENT)
TEST(a_surface_buffer_dying_while_a_new_one_is_made_hands_its_handle_over) {
    Rig r;
    r.fake->addSurface(31, 64, 64, 256, 0x42475241u);
    SurfaceInfo info;
    std::string why;
    auto first = surfaceBuffer(*r.gpu, 31, &info, &why);
    CHECK(first != nullptr);
    const uint64_t before = surfaceHandovers();
    gHandoverGpu = r.gpu.get();
    gSurfaceDeleterHook = makeAnotherWhileDying;
    first.reset();
    gSurfaceDeleterHook = nullptr;
    CHECK(gHandoverNew != nullptr);
    CHECK_EQ(surfaceHandovers(), before + 1);
    auto cb = r.queue->commandBuffer();
    CHECK(cb->fill(*gHandoverNew, 0, 4096, 0x5a));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(r.fake->surfaceBytes(31)[100], 0x5a);
    gHandoverNew.reset();
}
