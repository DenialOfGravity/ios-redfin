// native_adreno_transport.cpp against the in-process NativeAdreno service (fake_native_adreno.cpp), on Linux through
// the IOKit shim: the msm requests upstream's core sends, each answered through our ABI.
#include <cerrno>
#include <cstring>
#include <thread>
#include <cstdlib>

#include "fake_native_adreno.hpp"
#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/pm4.hpp"
#include "native_adreno_transport.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {

struct Rig {
    Rig() {
        fake = std::make_unique<FakeGpu>(*FakeGpu::defaultChip(), true);
        fakena::install(fake.get());
        std::string why;
        auto t = openNativeAdrenoGpu(&why);
        CHECK(t != nullptr);
        if (t == nullptr) std::printf("       %s\n", why.c_str());
        gpu = std::make_unique<Gpu>(std::move(t));
    }
    ~Rig() {
        gpu.reset();
        fakena::removeSurfaces();
        fakena::uninstall();
    }
    std::unique_ptr<FakeGpu> fake;
    std::unique_ptr<Gpu> gpu;
};

// a CP_MEM_WRITE of WORDS at DST: the smallest stream the fake runs and that writes memory
std::vector<uint32_t> memWrite(uint64_t dst, std::initializer_list<uint32_t> words) {
    pm4::Stream s;
    std::vector<uint32_t> w{uint32_t(dst), uint32_t(dst >> 32)};
    w.insert(w.end(), words);
    s.packet(pm4::kMemWrite, w.data(), uint32_t(w.size()));
    return s.words();
}

int run(Rig& r, Bo& ib, const std::vector<uint32_t>& words, const std::vector<BoUse>& uses, uint32_t* fence) {
    memcpy(ib.cpu(), words.data(), 4 * words.size());
    return r.gpu->submit(0, ib, 0, uint32_t(words.size()), uses, fence);
}

}  // namespace

TEST(transport_opens_and_answers_the_chip) {
    Rig r;
    CHECK_EQ(r.gpu->chipId(), FakeGpu::defaultChip()->chipId);
    CHECK(&a6xx::chip() == FakeGpu::defaultChip());   // Gpu's constructor selected the row from what the transport said
    uint64_t v = 0;
    CHECK_EQ(r.gpu->param(kParamGmemSize, &v), 0);
    CHECK_EQ(v, uint64_t(FakeGpu::defaultChip()->gmemBytes));
    CHECK_EQ(r.gpu->param(kParamMaxFreq, &v), 0);
    CHECK_EQ(v, 625000000ull);
    CHECK_EQ(r.gpu->param(kParamFaults, &v), 0);
    CHECK_EQ(v, 0ull);
    CHECK_EQ(r.gpu->param(kParamGpuId, &v), 0);
    CHECK_EQ(v, 0ull);
    CHECK_EQ(r.gpu->param(kParamHighestBankBit, &v), 0);
    CHECK_EQ(v, 14ull);
    CHECK(r.gpu->ubwc().fromDriver && r.gpu->ubwc().highestBankBit == 14);   // (redfin) DeviceInfo's, through the transport
    CHECK_EQ(r.gpu->ubwc().macrotileMode, FakeGpu::defaultChip()->macrotileMode);
    CHECK_EQ(r.gpu->param(0x55, &v), EINVAL);   // an unknown param: EINVAL as hakios's kernel
}

TEST(transport_alloc_map_info_iova_inside_the_window) {
    Rig r;
    int e = 0;
    auto a = r.gpu->newBo(5000, 0, &e);   // 5000 -> 16384 (the core's 16 KiB page), 4 KiB aligned
    auto b = r.gpu->newBo(4096, 0, &e);
    CHECK(a != nullptr && b != nullptr);
    CHECK(a->gpuAddress() >= fakena::windowStart() && a->gpuAddress() + a->size() <= fakena::windowStart() + fakena::windowSize());
    CHECK_EQ(a->gpuAddress() % 4096, 0ull);
    CHECK(b->gpuAddress() >= a->gpuAddress() + a->size());   // no overlap
    CHECK(a->cpu() != nullptr);
    // the CPU's mapping and the GPU's memory are the same bytes (the BoCpuMap token's)
    static_cast<uint32_t*>(a->cpu())[3] = 0x12345678;
    auto m = r.fake->memory(a->gpuAddress() + 12, 4);
    CHECK(m.size() == 4 && m[0] == 0x78 && m[3] == 0x12);
    const auto c = fakena::counts();
    CHECK_EQ(c.allocs, 2u);
    CHECK_EQ(c.maps, 2u);
    CHECK_EQ(c.cpuMaps, 2u);
    // freed: its address is free again, and the next of its size takes it
    const uint64_t at = b->gpuAddress();
    b.reset();
    CHECK_EQ(fakena::counts().frees, 1u);
    auto d = r.gpu->newBo(4096, 0, &e);
    CHECK(d != nullptr && d->gpuAddress() == at);
    // too big for the ABI (max_bo_bytes 256 MiB): refused before the kernel sees it
    auto big = r.gpu->newBo((256ull << 20) + 1, 0, &e);
    CHECK(big == nullptr && e == EINVAL);
}

TEST(transport_submit_and_wait) {
    Rig r;
    int e = 0;
    auto ib = r.gpu->newBo(4096, 0, &e);
    auto dst = r.gpu->newBo(4096, 0, &e);
    uint32_t fence = 0;
    CHECK_EQ(run(r, *ib, memWrite(dst->gpuAddress() + 8, {0xcafe0001, 0xcafe0002}), {{dst.get(), true}}, &fence), 0);
    CHECK(fence != 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(static_cast<uint32_t*>(dst->cpu())[2], 0xcafe0001u);
    CHECK_EQ(static_cast<uint32_t*>(dst->cpu())[3], 0xcafe0002u);
    // the IB went to the kernel at its own address and length (one IB, the BO list not sent)
    const auto c = fakena::counts();
    CHECK_EQ(c.submits, 1u);
    CHECK_EQ(c.lastSubmitIova, ib->gpuAddress());
    CHECK_EQ(c.lastSubmitBytes, 5ull * 4);   // header, address (2), two words
    CHECK_EQ(r.fake->stats().faults, 0u);
    // a queue of its own: the ids are the transport's, the kernel's ring is one
    uint32_t q = 0;
    CHECK_EQ(r.gpu->newQueue(0, &q), 0);
    CHECK(q != 0);
    CHECK_EQ(run(r, *ib, memWrite(dst->gpuAddress(), {7}), {}, &fence), 0);
    CHECK_EQ(r.gpu->wait(q, fence, 1000000000), 0);
    CHECK_EQ(r.gpu->wait(q + 5, fence, 1000000000), EINVAL);   // a queue never made
    r.gpu->closeQueue(q);
}

TEST(transport_wait_times_out_on_a_paused_gpu) {
    Rig r;
    int e = 0;
    auto ib = r.gpu->newBo(4096, 0, &e);
    auto dst = r.gpu->newBo(4096, 0, &e);
    r.fake->pause();
    uint32_t fence = 0;
    CHECK_EQ(run(r, *ib, memWrite(dst->gpuAddress(), {1}), {}, &fence), 0);
    const int64_t before = r.gpu->transport().nowNs();
    CHECK_EQ(r.gpu->wait(0, fence, 20000000), ETIMEDOUT);
    CHECK(r.gpu->transport().nowNs() - before >= 15000000);
    r.fake->resume();
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
}

TEST(transport_cpu_prep_waits_for_the_bos_last_fence) {
    Rig r;
    int e = 0;
    auto ib = r.gpu->newBo(4096, 0, &e);
    auto dst = r.gpu->newBo(4096, 0, &e);
    auto other = r.gpu->newBo(4096, 0, &e);
    CHECK_EQ(r.gpu->cpuPrep(*dst, 1, 0), 0);   // never submitted: nothing to wait for
    r.fake->pause();
    uint32_t fence = 0;
    CHECK_EQ(run(r, *ib, memWrite(dst->gpuAddress(), {0xabcd}), {{dst.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->cpuPrep(*other, 1, 1000000), 0);          // not named by the submission: no wait
    CHECK_EQ(r.gpu->cpuPrep(*dst, 1, 10000000), ETIMEDOUT);   // named: waits for its fence
    std::thread later([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        r.fake->resume();
    });
    CHECK_EQ(r.gpu->cpuPrep(*dst, 1, 2000000000), 0);
    later.join();
    CHECK_EQ(static_cast<uint32_t*>(dst->cpu())[0], 0xabcdu);
    r.gpu->cpuFini(*dst);
}

// (ws-hm-perf) The clean before a submission is skipped while the surface's seed is the one of this client's last clean or
// invalidate of it (phone/metal2 flushSurfaces' rule); a new seed (a CPU write unlock) cleans again. HM_FLUSH=all: always.
TEST(transport_surface_clean_skipped_by_seed) {
    if (getenv("HM_FLUSH") != nullptr) return;
    Rig r;
    const uint32_t id = 78, rowBytes = 256, w = 64, h = 16;
    uint8_t* pixels = fakena::addSurface(id, w, h, rowBytes, 0x42475241);
    fakena::setSurfaceSeed(id, 5);
    memset(pixels, 0x22, rowBytes * h);
    SurfaceInfo info;
    int e = 0;
    auto bo = r.gpu->surfaceBo(id, &info, &e);
    CHECK(bo != nullptr);
    if (bo == nullptr) return;
    auto ib = r.gpu->newBo(4096, 0, &e);
    uint32_t fence = 0;
    const auto c0 = fakena::counts();
    CHECK_EQ(run(r, *ib, memWrite(ib->gpuAddress() + 2048, {1}), {{bo.get(), false}}, &fence), 0);
    CHECK_EQ(fakena::counts().cleans, c0.cleans + 1);   // never cleaned here: cleaned
    CHECK(fakena::gpuViewOfImport(pixels, 4)[0] == 0x22);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(run(r, *ib, memWrite(ib->gpuAddress() + 2048, {2}), {{bo.get(), false}}, &fence), 0);
    CHECK_EQ(fakena::counts().cleans, c0.cleans + 1);   // the same seed: skipped
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    // the CPU writes and unlocks: a new seed, cleaned again
    memset(pixels, 0x33, 4);
    fakena::setSurfaceSeed(id, 6);
    CHECK_EQ(run(r, *ib, memWrite(ib->gpuAddress() + 2048, {3}), {{bo.get(), false}}, &fence), 0);
    CHECK_EQ(fakena::counts().cleans, c0.cleans + 2);
    CHECK(fakena::gpuViewOfImport(pixels, 4)[0] == 0x33);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    // the GPU writes it: invalidated after the fence (the CPU sees it), and the next submission at that seed skips
    CHECK_EQ(run(r, *ib, memWrite(bo->gpuAddress() + 64, {0xfeedf00d}), {{bo.get(), true}}, &fence), 0);
    CHECK_EQ(fakena::counts().cleans, c0.cleans + 2);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(fakena::counts().invalidates, c0.invalidates + 1);
    uint32_t got = 0;
    memcpy(&got, pixels + 64, 4);
    CHECK_EQ(got, 0xfeedf00du);
    CHECK_EQ(run(r, *ib, memWrite(ib->gpuAddress() + 2048, {4}), {{bo.get(), false}}, &fence), 0);
    CHECK_EQ(fakena::counts().cleans, c0.cleans + 2);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    bo.reset();
}

// An IOSurface's pages: imported, mapped, described; the GPU sees what the CPU wrote only after the clean, and the
// CPU sees what the GPU wrote only after the invalidate at the fence
// (redfin hm-int5) HM_GATE_FLAG=1: a submission that writes a surface of the display's size (HM_GATE_FLAG_SURF) carries
// the flag, one that only reads it or writes another surface does not; a kernel that refuses the flag (the fake's
// default, as every kernel so far) gets the submission again bare and the flag is off for the process from then on
TEST(transport_flags_the_submission_that_writes_the_display_surface) {
    setenv("HM_GATE_FLAG", "1", 1);
    setenv("HM_GATE_FLAG_SURF", "64x16", 1);
    struct Back { ~Back() { unsetenv("HM_GATE_FLAG"); unsetenv("HM_GATE_FLAG_SURF"); fakena::setSubmitFlagsAccepted(false); } } back;
    fakena::setSubmitFlagsAccepted(true);
    Rig r;
    uint8_t* display = fakena::addSurface(81, 64, 16, 256, 0x42475241);
    fakena::addSurface(82, 32, 16, 128, 0x42475241);
    (void)display;
    SurfaceInfo info;
    int e = 0;
    auto disp = r.gpu->surfaceBo(81, &info, &e);
    auto other = r.gpu->surfaceBo(82, &info, &e);
    auto ib = r.gpu->newBo(4096, 0, &e);
    CHECK(disp != nullptr && other != nullptr && ib != nullptr);
    if (disp == nullptr || other == nullptr || ib == nullptr) return;
    uint32_t fence = 0;
    const auto c0 = fakena::counts();
    CHECK_EQ(run(r, *ib, memWrite(disp->gpuAddress() + 64, {1}), {{disp.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(fakena::counts().flaggedSubmits, c0.flaggedSubmits + 1);
    // (ws-hm-int6) gate mode 3's surface id: bit 0 | id << 8 (NativeAdrenoAbiSubmitSurfaceShift), bits 1-7 zero
    CHECK_EQ(fakena::counts().lastSubmitFlags, 1u | (81u << 8));
    CHECK_EQ(run(r, *ib, memWrite(ib->gpuAddress() + 2048, {2}), {{disp.get(), false}}, &fence), 0);   // read only
    CHECK_EQ(fakena::counts().lastSubmitFlags, 0u);
    CHECK_EQ(run(r, *ib, memWrite(other->gpuAddress() + 64, {3}), {{other.get(), true}}, &fence), 0);   // another size
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(fakena::counts().flaggedSubmits, c0.flaggedSubmits + 1);
    CHECK_EQ(fakena::counts().submits, c0.submits + 3);
    {
        // (ws-hm-int6) an id past 24 bits: bit 0 alone (gate 1|2 still holds it; gate 3 cannot name it)
        const uint32_t big = (1u << 24) + 5;
        fakena::addSurface(big, 64, 16, 256, 0x42475241);
        auto wide = r.gpu->surfaceBo(big, &info, &e);
        CHECK(wide != nullptr);
        if (wide != nullptr) {
            CHECK_EQ(run(r, *ib, memWrite(wide->gpuAddress() + 64, {6}), {{wide.get(), true}}, &fence), 0);
            CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
            CHECK_EQ(fakena::counts().lastSubmitFlags, 1u);
            CHECK_EQ(fakena::counts().flaggedSubmits, c0.flaggedSubmits + 2);
        }
        wide.reset();
    }
    const auto c1 = fakena::counts();
    // a kernel without the flag: refused once, sent again bare, and not flagged again
    fakena::setSubmitFlagsAccepted(false);
    CHECK_EQ(run(r, *ib, memWrite(disp->gpuAddress() + 64, {4}), {{disp.get(), true}}, &fence), 0);
    CHECK_EQ(run(r, *ib, memWrite(disp->gpuAddress() + 64, {5}), {{disp.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(fakena::counts().submits, c1.submits + 2);
    CHECK_EQ(fakena::counts().flaggedSubmits, c1.flaggedSubmits);
    CHECK_EQ(fakena::counts().lastSubmitFlags, 0u);
    disp.reset();
    other.reset();
}

TEST(transport_surface_import_cleans_and_invalidates) {
    Rig r;
    const uint32_t id = 77, rowBytes = 256, w = 64, h = 16;
    uint8_t* pixels = fakena::addSurface(id, w, h, rowBytes, 0x42475241 /* 'BGRA' */);
    memset(pixels, 0x11, rowBytes * h);
    SurfaceInfo info;
    int e = 0;
    auto bo = r.gpu->surfaceBo(id, &info, &e);
    CHECK(bo != nullptr);
    if (bo == nullptr) return;
    CHECK_EQ(info.rowBytes, rowBytes);
    CHECK_EQ(info.width, w);
    CHECK_EQ(info.height, h);
    CHECK_EQ(info.pixelFormat, 0x42475241u);
    CHECK_EQ(info.planes, 1u);
    CHECK_EQ(info.offset, 0u);
    CHECK_EQ(info.size, 16384ull);
    CHECK(bo->cpu() == pixels);   // the caller's own mapping
    CHECK_EQ(fakena::counts().imports, 1u);
    CHECK_EQ(fakena::surfaceRefs(id), 1);   // held while the buffer lives
    // the same surface again: the same handle (the core disowns the second)
    SurfaceInfo again;
    auto bo2 = r.gpu->surfaceBo(id, &again, &e);
    CHECK(bo2 != nullptr && bo2->handle() == bo->handle());
    CHECK_EQ(fakena::counts().imports, 1u);
    r.gpu->disown(std::move(bo2));
    // the CPU wrote 0x11s; nothing cleaned yet: the GPU's view is still the pinned zeros
    CHECK(fakena::gpuViewOfImport(pixels, 4)[0] == 0x00);
    auto ib = r.gpu->newBo(4096, 0, &e);
    uint32_t fence = 0;
    CHECK_EQ(run(r, *ib, memWrite(bo->gpuAddress() + 64, {0xdeadbeef}), {{bo.get(), true}}, &fence), 0);
    CHECK(fakena::counts().cleans >= 1);
    CHECK(fakena::gpuViewOfImport(pixels, 4)[0] == 0x11);   // cleaned before the submission
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK(fakena::counts().invalidates >= 1);
    uint32_t got = 0;
    memcpy(&got, pixels + 64, 4);
    CHECK_EQ(got, 0xdeadbeefu);   // invalidated after the fence: the CPU sees the GPU's write
    CHECK_EQ(pixels[0], 0x11);    // and the rest is what the CPU wrote
    // a submission that only reads it: cleaned, not invalidated
    const auto before = fakena::counts();
    CHECK_EQ(run(r, *ib, memWrite(ib->gpuAddress() + 2048, {1}), {{bo.get(), false}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(fakena::counts().cleans, before.cleans + 1);
    CHECK_EQ(fakena::counts().invalidates, before.invalidates);
    // no such surface
    auto none = r.gpu->surfaceBo(999, &again, &e);
    CHECK(none == nullptr && e == ENOENT);
    bo.reset();
    CHECK_EQ(fakena::surfaceRefs(id), 0);   // released with its buffer
}

TEST(transport_userptr_is_an_import) {
    Rig r;
    alignas(16384) static uint8_t mine[16384];
    nativeadreno::UserMemory u{uint64_t(uintptr_t(mine)), sizeof(mine), 0, 0};
    CHECK_EQ(r.gpu->transport().call(nativeadreno::kUserMemory, &u, sizeof(u)), 0);
    CHECK(u.handle != 0);
    MsmGemInfo i{u.handle, kInfoGetIova, 0, 0, 0};
    CHECK_EQ(r.gpu->transport().call(kGemInfo, &i, sizeof(i)), 0);
    CHECK(i.value >= fakena::windowStart());
    CHECK(r.gpu->transport().map(u.handle, sizeof(mine)) == mine);
    nativeadreno::UserMemory odd{uint64_t(uintptr_t(mine)) + 8, 4096, 0, 0};
    CHECK_EQ(r.gpu->transport().call(nativeadreno::kUserMemory, &odd, sizeof(odd)), EINVAL);   // not page aligned
    DrmGemClose c{u.handle, 0};
    CHECK_EQ(r.gpu->transport().call(kGemClose, &c, sizeof(c)), 0);
    CHECK_EQ(r.gpu->transport().call(kGemClose, &c, sizeof(c)), ENOENT);
}

TEST(transport_unknown_request_is_enosys) {
    Rig r;
    uint32_t junk[4] = {};
    CHECK_EQ(r.gpu->transport().call(0x7f, junk, sizeof(junk)), ENOSYS);   // hakios's "show": not ours
    CHECK_EQ(r.gpu->transport().call(0x48, junk, sizeof(junk)), ENOSYS);   // MSM_GEM_MADVISE
    CHECK_EQ(r.gpu->transport().call(kGemNew, junk, 3), EINVAL);            // a known request with the wrong size
}

// upstream's own objects, end to end: a blit and a fill through CommandQueue on the transport
TEST(transport_runs_the_cores_blits) {
    Rig r;
    std::string why;
    auto queue = CommandQueue::make(*r.gpu, &why);
    CHECK(queue != nullptr);
    auto a = newBuffer(*r.gpu, 65536, &why);
    auto b = newBuffer(*r.gpu, 65536, &why);
    for (uint32_t i = 0; i < 16384; i++) static_cast<uint32_t*>(a->contents())[i] = 0x10000000 + i;
    auto cb = queue->commandBuffer();
    CHECK(cb->fill(*b, 0, 65536, 0xee));
    CHECK(cb->copy(*a, 4096, *b, 8192, 20000));
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    if (cb->status() != CommandBuffer::kCompleted) std::printf("       %s | %s\n", cb->error().c_str(), r.fake->stats().lastFault.c_str());
    const auto* got = static_cast<const uint8_t*>(b->contents());
    CHECK(got[0] == 0xee && got[8191] == 0xee && got[8192 + 20000] == 0xee);
    CHECK(memcmp(got + 8192, static_cast<const uint8_t*>(a->contents()) + 4096, 20000) == 0);
    CHECK_EQ(r.fake->stats().faults, 0u);
}

// (ws-hm-int6) HM_FLUSH_AFTER=lazy: a display-size surface (HM_FLUSH_AFTER_SURF) the GPU wrote is not invalidated at
// the fence -- the CPU still sees what it cached -- but at the CPU's access (kHakiosCpuAccess by buffer or by surface,
// as Texture::read / replace / the XR write-back and hakios_metal_surface_cpu_access ask it) or at the buffer's close
// (a CPU write's dc civac before the next submission also clears it; the fake models a clean as a copy, so that is
// not asked here); any other surface is invalidated at the fence as before. Unset
// (always, the default): every one at the fence.
TEST(transport_flush_after_lazy_leaves_the_display_surfaces_invalidate_for_the_cpu) {
    setenv("HM_FLUSH_AFTER_SURF", "64x16", 1);
    struct Back { ~Back() { unsetenv("HM_FLUSH_AFTER"); unsetenv("HM_FLUSH_AFTER_SURF"); } } back;
    Rig r;
    uint8_t* disp = fakena::addSurface(91, 64, 16, 256, 0x42475241);
    uint8_t* other = fakena::addSurface(92, 32, 16, 128, 0x42475241);
    memset(disp, 0x11, 256 * 16);
    memset(other, 0x22, 128 * 16);
    fakena::setSurfaceSeed(91, 5);   // no CPU writer: one clean, at the first submission
    fakena::setSurfaceSeed(92, 5);
    SurfaceInfo info;
    int e = 0;
    auto d = r.gpu->surfaceBo(91, &info, &e);
    auto o = r.gpu->surfaceBo(92, &info, &e);
    auto ib = r.gpu->newBo(4096, 0, &e);
    CHECK(d != nullptr && o != nullptr && ib != nullptr);
    if (d == nullptr || o == nullptr || ib == nullptr) return;
    uint32_t fence = 0;
    auto word = [](const uint8_t* p) { uint32_t v = 0; memcpy(&v, p, 4); return v; };
    // always (unset): both invalidated at the fence
    CHECK_EQ(run(r, *ib, memWrite(d->gpuAddress() + 64, {0xa1}), {{d.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(word(disp + 64), 0xa1u);
    // lazy
    setenv("HM_FLUSH_AFTER", "lazy", 1);
    auto c0 = fakena::counts();
    CHECK_EQ(run(r, *ib, memWrite(d->gpuAddress() + 128, {0xb2}), {{d.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);   // (one command buffer, ib, rewritten by the next run)
    CHECK_EQ(fakena::counts().invalidates, c0.invalidates);
    CHECK_EQ(run(r, *ib, memWrite(o->gpuAddress() + 64, {0xc3}), {{o.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK_EQ(fakena::counts().invalidates, c0.invalidates + 1);   // the other surface's alone
    CHECK_EQ(word(other + 64), 0xc3u);
    CHECK(word(disp + 128) != 0xb2u);   // the display surface's CPU view: not invalidated yet
    // on demand, by buffer
    CHECK_EQ(r.gpu->cpuAccess(d->handle(), 0), 0);
    CHECK_EQ(fakena::counts().invalidates, c0.invalidates + 2);
    CHECK_EQ(word(disp + 128), 0xb2u);
    CHECK_EQ(r.gpu->cpuAccess(d->handle(), 0), 0);                // nothing pending: nothing done
    CHECK_EQ(fakena::counts().invalidates, c0.invalidates + 2);
    // on demand, by surface id (hakios_metal_surface_cpu_access)
    CHECK_EQ(run(r, *ib, memWrite(d->gpuAddress() + 192, {0xd4}), {{d.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK(word(disp + 192) != 0xd4u);
    CHECK_EQ(r.gpu->cpuAccess(0, 91), 0);
    CHECK_EQ(word(disp + 192), 0xd4u);
    CHECK_EQ(r.gpu->cpuAccess(0, 4242), ENOENT);
    // pending at the buffer's close: done then
    CHECK_EQ(run(r, *ib, memWrite(d->gpuAddress() + 320, {0xf6}), {{d.get(), true}}, &fence), 0);
    CHECK_EQ(r.gpu->wait(0, fence, 1000000000), 0);
    CHECK(word(disp + 320) != 0xf6u);
    d.reset();
    CHECK_EQ(word(disp + 320), 0xf6u);
    o.reset();
}
