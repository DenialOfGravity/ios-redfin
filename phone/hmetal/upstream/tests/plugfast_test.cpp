// (redfin ws-hm-plugcpu) HM_PLUG_FAST: the C interface's draws as the plug-in makes them (hakios_metal_cb_draw3 and
// hakios_metal_cb_draw_indexed, CoreAnimation's shape), with the switch off and on: the same words, every object the
// GPU reads still kept until the buffer completes, fewer keeps; and (HMETAL_BENCH=N) ns per draw through C both ways.
//
// What this does not measure: the Objective-C plug-in's own share (bench/plugin.py's HM_PLUG_FAST edits: the
// specialisation cache, the encoder's bound-slot handles, the hooks read once an encoder) -- it needs Metal's classes and
// runs on the phone alone, where HM_TRACE=2's draw lines time it.
//
//   HMETAL_BENCH=N tests plugfast_bench   N draws a run (default 2000 when HMETAL_BENCH is 1)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hakios-metal/hakios_metal.h"
#include "test.hpp"

namespace hakios::metal {
void cApiPlugFastForTesting(int on);
std::vector<uint32_t> cApiStreamForTesting(hakios_metal_cb* cb);
size_t cApiKeptForTesting(hakios_metal_cb* cb);
}  // namespace hakios::metal

namespace {
using hakios::metal::cApiKeptForTesting;
using hakios::metal::cApiPlugFastForTesting;
using hakios::metal::cApiStreamForTesting;

std::vector<uint8_t> pfRead(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return out;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) != 0) out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return out;
}

struct PfScene {
    hakios_metal_device* d = nullptr;
    hakios_metal_queue* q = nullptr;
    // tri_attr_vs + tri_color_fs (its fragment buffer 0 bound though unread, as CA binds its uniforms); tri_color_vs +
    // tri_tex_fs. (Not tri_cflat_fs: 7 of its draws and then a tri_tex_fs one hang the fake GPU on "fragment constants
    // ... past the stage's constlen of 0" whatever HM_PLUG_FAST says, GMEM or not, one pass or two -- left for the core.)
    hakios_metal_program *attr = nullptr, *tex = nullptr;
    hakios_metal_texture *rt = nullptr, *rt2 = nullptr;
    hakios_metal_texture* t[4] = {};
    hakios_metal_sampler* s = nullptr;
    hakios_metal_buffer *vb = nullptr, *ub = nullptr, *ib = nullptr;
    struct hakios_metal_render_pass pass = {}, pass2 = {};
    bool ok = false;
    uint32_t quads = 64;

    PfScene() {
        d = hakios_metal_open_fake();
        if (!hakios_metal_has_compiler()) return;
        std::vector<uint8_t> avs = pfRead("out/host/kernels/tri_attr_vs.bc"), cfs = pfRead("out/host/kernels/tri_color_fs.bc"),
                             cvs = pfRead("out/host/kernels/tri_color_vs.bc"), tfs = pfRead("out/host/kernels/tri_tex_fs.bc");
        CHECK(!avs.empty() && !cfs.empty() && !cvs.empty() && !tfs.empty());
        attr = hakios_metal_program_new_bitcode(avs.data(), avs.size(), cfs.data(), cfs.size(), 70);
        tex = hakios_metal_program_new_bitcode(cvs.data(), cvs.size(), tfs.data(), tfs.size(), 70);
        CHECK(attr != nullptr && tex != nullptr);
        if (attr == nullptr || tex == nullptr) {
            std::printf("       %s\n", hakios_metal_error());
            return;
        }
        CHECK_EQ(hakios_metal_program_set_vertex_attribute(attr, 0, 29, 0, 0), 0);   // Float2 at 0
        CHECK_EQ(hakios_metal_program_set_vertex_attribute(attr, 1, 9, 8, 0), 0);    // UChar4Normalized at 8
        CHECK_EQ(hakios_metal_program_set_vertex_layout(attr, 0, 12, 1, 1), 0);      // stride 12, per vertex
        rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
        rt2 = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
        for (auto& x : t) x = hakios_metal_texture_new(d, 2, 70, 8, 8, 1);
        struct hakios_metal_sampler_desc sd = {};
        sd.normalized_coordinates = 1;
        sd.lod_max_clamp = 1000.0f;
        sd.max_anisotropy = 1;
        s = hakios_metal_sampler_new(d, &sd);
        vb = hakios_metal_buffer_new(d, uint64_t(quads) * 6 * 12 + 64);
        ub = hakios_metal_buffer_new(d, 256);
        ib = hakios_metal_buffer_new(d, 64);
        CHECK(rt != nullptr && s != nullptr && vb != nullptr && ub != nullptr && ib != nullptr);
        uint8_t* v = static_cast<uint8_t*>(hakios_metal_buffer_contents(vb));
        for (uint32_t k = 0; k < quads; k++) {
            const float x0 = -1 + 2.0f * (k % 8) / 8, y0 = -1 + 2.0f * ((k / 8) % 8) / 8, x1 = x0 + 0.25f, y1 = y0 + 0.25f;
            const float xy[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
            const uint32_t colour = 0x80000000u | (k * 2654435761u >> 8);
            for (int i = 0; i < 6; i++) {
                memcpy(v + (k * 6 + i) * 12, xy[i], 8);
                memcpy(v + (k * 6 + i) * 12 + 8, &colour, 4);
            }
        }
        const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        memcpy(hakios_metal_buffer_contents(ub), c, sizeof c);
        uint16_t* idx = static_cast<uint16_t*>(hakios_metal_buffer_contents(ib));
        for (uint16_t i = 0; i < 6; i++) idx[i] = i;
        q = hakios_metal_queue_new(d);
        pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        pass2 = pass;
        pass2.color0 = rt2;
        ok = true;
    }
    ~PfScene() {
        if (q != nullptr) hakios_metal_queue_free(q);
        for (auto& x : t) if (x != nullptr) hakios_metal_texture_free(x);
        if (rt != nullptr) hakios_metal_texture_free(rt);
        if (rt2 != nullptr) hakios_metal_texture_free(rt2);
        if (s != nullptr) hakios_metal_sampler_free(s);
        if (vb != nullptr) hakios_metal_buffer_free(vb);
        if (ub != nullptr) hakios_metal_buffer_free(ub);
        if (ib != nullptr) hakios_metal_buffer_free(ib);
        if (attr != nullptr) hakios_metal_program_free(attr);
        if (tex != nullptr) hakios_metal_program_free(tex);
        hakios_metal_close(d);
    }
    // draw I: 7 of 8 the attribute pipeline over its own quad (every fourth of them indexed, from the quad's vertices by
    // the vertex buffer's offset), the uniform buffer the same; 1 of 8 the textured one into a second target, its
    // texture changing
    int draw(hakios_metal_cb* cb, uint32_t i) {
        const struct hakios_metal_viewport vp = {0, 0, 64, 64, 0, 1};
        if (i % 8 != 7) {
            const uint32_t k = i % quads;
            if (i % 4 == 1) {
                hakios_metal_buffer* vs[1] = {vb};
                const uint64_t vo[1] = {uint64_t(k) * 6 * 12};
                hakios_metal_buffer* fs[1] = {ub};
                const uint64_t fo[1] = {0};
                return hakios_metal_cb_draw_indexed(cb, attr, &pass, &vp, nullptr, 3, 6, 0, ib, 0, 1, 0, 0, vs, vo, 1, fs, fo, 1, nullptr, 0,
                                                    nullptr, 0);
            }
            hakios_metal_buffer* vs[1] = {vb};
            const uint64_t vo[1] = {0};
            hakios_metal_buffer* fs[1] = {ub};
            const uint64_t fo[1] = {0};
            return hakios_metal_cb_draw3(cb, attr, &pass, &vp, nullptr, 3, 6 * k, 6, 1, 0, vs, vo, 1, fs, fo, 1, nullptr, 0, nullptr, 0);
        }
        hakios_metal_texture* ft[1] = {t[(i / 8) % 4]};
        hakios_metal_sampler* fsm[1] = {s};
        return hakios_metal_cb_draw3(cb, tex, &pass2, &vp, nullptr, 3, 0, 6, 1, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, ft, 1, fsm, 1);
    }
    // a command buffer drawing in sysmem, as the phone's (bench/hm-env.default HM_GMEM=0)
    hakios_metal_cb* newCb() {
        hakios_metal_cb* cb = hakios_metal_cb_new(q);
        hakios_metal_cb_set_gmem(cb, 0);
        return cb;
    }
    // N draws into a new buffer with HM_PLUG_FAST as FAST: its words, and how many objects it keeps
    std::vector<uint32_t> run(int fast, uint32_t n, size_t* kept) {
        cApiPlugFastForTesting(fast);
        hakios_metal_cb* cb = newCb();
        for (uint32_t i = 0; i < n; i++) CHECK_EQ(draw(cb, i), 0);
        std::vector<uint32_t> w = cApiStreamForTesting(cb);
        if (kept != nullptr) *kept = cApiKeptForTesting(cb);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        CHECK(hakios_metal_cb_error(cb)[0] == 0);
        if (hakios_metal_cb_error(cb)[0] != 0) std::printf("       HM_PLUG_FAST=%d: %s\n", fast, hakios_metal_cb_error(cb));
        hakios_metal_cb_free(cb);
        cApiPlugFastForTesting(-1);
        return w;
    }
};

double pfNow() { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}  // namespace

// HM_PLUG_FAST=1 records the same words as 0 (and as unset), keeping each object once instead of once a draw
TEST(plugfast_byte_identical) {
    if (!PfScene().ok) {
        std::printf("       (no compiler in this build: the C interface's draws did not run)\n");
        return;
    }
    // (each run on a device of its own: the GPU addresses in the words are then the same)
    size_t keptOff = 0, keptOn = 0;
    const std::vector<uint32_t> off = PfScene().run(0, 96, &keptOff), on = PfScene().run(1, 96, &keptOn), unset = PfScene().run(-1, 96, nullptr);
    CHECK(!off.empty());
    CHECK(off == on);
    CHECK(off == unset);
    CHECK(keptOn < keptOff);
    CHECK(keptOn > 0);
    std::printf("       %zu words both ways; objects kept: %zu (HM_PLUG_FAST=0), %zu (=1)\n", off.size(), keptOff, keptOn);
}

// HM_PLUG_FAST=1: what the draws read stays alive until the buffer completes though every handle is freed before the
// commit, and the next buffer keeps the same objects again (its own table)
TEST(plugfast_keeps_what_the_gpu_reads) {
    hakios_metal_device* d = hakios_metal_open_fake();
    if (!hakios_metal_has_compiler()) {
        hakios_metal_close(d);
        return;
    }
    std::vector<uint8_t> vs = pfRead("out/host/kernels/tri_color_vs.bc"), fs = pfRead("out/host/kernels/tri_cflat_fs.bc");
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vs.data(), vs.size(), fs.data(), fs.size(), 70);
    CHECK(p != nullptr);
    if (p == nullptr) {
        hakios_metal_close(d);
        return;
    }
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    cApiPlugFastForTesting(1);
    for (int round = 0; round < 2; round++) {
        hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
        hakios_metal_buffer* colour = hakios_metal_buffer_new(d, 16);
        struct hakios_metal_render_pass pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        hakios_metal_buffer* fsTable[1] = {colour};
        hakios_metal_cb* cb = hakios_metal_cb_new(q);
        hakios_metal_cb_set_gmem(cb, 0);
        size_t kept1 = 0;
        for (int i = 0; i < 4; i++) {
            CHECK_EQ(hakios_metal_cb_draw3(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 1, 0, nullptr, nullptr, 0, fsTable, nullptr, 1, nullptr, 0,
                                           nullptr, 0), 0);
            if (i == 0) kept1 = cApiKeptForTesting(cb);
            if (i == 3) std::printf("       kept after 1 draw %zu, after 4 %zu (HM_PLUG_FAST=1)\n", kept1, cApiKeptForTesting(cb));
        }
        CHECK(kept1 >= 4);   // the constant buffer, the target's memory and object, the program (and the core's own)
        const size_t kept4 = cApiKeptForTesting(cb);
        CHECK(kept4 >= kept1);
        hakios_metal_buffer_free(colour);
        hakios_metal_texture_free(rt);
        if (round == 1) hakios_metal_program_free(p);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        hakios_metal_cb_free(cb);
    }
    cApiPlugFastForTesting(-1);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// HMETAL_BENCH: ns a draw through the C interface (hakios_metal_cb_draw3 / _draw_indexed, the core's encode included),
// HM_PLUG_FAST=0 and 1; without HMETAL_BENCH a short run of both (no timing printed)
TEST(plugfast_bench) {
    PfScene sc;
    if (!sc.ok) return;
    const char* e = getenv("HMETAL_BENCH");
    uint32_t n = e != nullptr ? uint32_t(atoi(e)) : 32;
    if (n == 1) n = 2000;
    const int rounds = e != nullptr ? 7 : 1;
    double best[2] = {1e30, 1e30};
    for (int k = 0; k < rounds; k++) {
        for (int fast = 0; fast < 2; fast++) {
            cApiPlugFastForTesting(fast);
            hakios_metal_cb* cb = sc.newCb();
            const double t0 = pfNow();
            for (uint32_t i = 0; i < n; i++) CHECK_EQ(sc.draw(cb, i), 0);
            const double t1 = pfNow();
            hakios_metal_cb_commit(cb, nullptr, nullptr);
            hakios_metal_cb_wait(cb);
            CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
            hakios_metal_cb_free(cb);
            if ((t1 - t0) / n < best[fast]) best[fast] = (t1 - t0) / n;
        }
    }
    cApiPlugFastForTesting(-1);
    if (e != nullptr) {
        std::printf("       plugfast bench: %u draws (7/8 attr, 1/4 of them indexed; 1/8 textured), best of %d: "
                    "HM_PLUG_FAST=0 %.0f ns/draw, =1 %.0f ns/draw (C interface + core encode)\n",
                    n, rounds, best[0], best[1]);
    }
}
