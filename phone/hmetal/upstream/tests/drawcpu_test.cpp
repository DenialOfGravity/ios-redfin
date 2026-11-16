// (redfin ws-hm-drawcpu) the CPU a CoreAnimation-like draw costs to encode, on the fake GPU, and what the draw-state
// switches (HM_DRAW_SKIP) leave out: byte-identical streams where a switch is off, the same pixels and no fault where
// it is on, and the state skipped only where the last draw of the same pass left it in the GPU.
//
//   HMETAL_BENCH=N tests drawcpu_bench   N draws a run (default 2000 when HMETAL_BENCH is 1): ns per draw and per commit
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"
#include "fixtures/tri_attr_vs_air.hpp"
#include "fixtures/tri_color_fs_air.hpp"
#include "fixtures/tri_color_vs_air.hpp"
#include "fixtures/tri_tex_fs_air.hpp"
#include "test.hpp"
#include "transport_hook.hpp"

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

namespace {
struct DcRig {
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
    FakeGpu* fake = nullptr;
    DcRig() {
        auto f = std::make_unique<FakeGpu>();
        fake = f.get();
        gpu = std::make_unique<Gpu>(HMETAL_TRANSPORT_OF(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
};

std::shared_ptr<Texture> dcTexture(DcRig& r, uint32_t w, uint32_t h, uint32_t usage) {
    TextureDesc d;
    d.format = PixelFormat::kRGBA8Unorm;
    d.width = w;
    d.height = h;
    d.usage = usage;
    std::string why;
    auto t = Texture::make(*r.gpu, d, &why);
    CHECK(t != nullptr);
    return t;
}

// CoreAnimation's shape: one pipeline (a vertex function with a vertex buffer and a uniform buffer, as
// fixed_vert_lph_spc), many quads, each its own range of one big vertex buffer, the uniforms the same buffer at the
// same offset (CA rebinds the same bytes), and a textured second pipeline (tri_tex) whose texture changes now and then
Program dcAttr() {
    Program p{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_attr"};
    p.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
    p.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
    p.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
    p.blend.enabled = true;
    p.blend.srcRgb = p.blend.srcAlpha = 1;   // One
    p.blend.dstRgb = p.blend.dstAlpha = 5;   // OneMinusSourceAlpha
    return p;
}
Program dcTex() { return Program{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"}; }

struct Scene {
    DcRig r;
    std::shared_ptr<Texture> rt, tex[4];
    std::shared_ptr<Buffer> vb, ub;
    Sampler smp;
    Program attr = dcAttr(), texp = dcTex();
    RenderPass pass;
    render::Options o;
    uint32_t quads;
    explicit Scene(uint32_t n) : quads(n) {
        rt = dcTexture(r, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead);
        for (auto& t : tex) t = dcTexture(r, 8, 8, kTextureUsageRead);
        std::string why;
        CHECK(makeSampler(SamplerDesc{}, &smp, &why));
        vb = newBuffer(*r.gpu, uint64_t(n) * 6 * 12 + 64, &why);
        ub = newBuffer(*r.gpu, 256, &why);
        CHECK(vb != nullptr && ub != nullptr);
        uint8_t* v = static_cast<uint8_t*>(vb->contents());
        for (uint32_t q = 0; q < n; q++) {
            const float x0 = -1 + 2.0f * (q % 8) / 8, y0 = -1 + 2.0f * ((q / 8) % 8) / 8, x1 = x0 + 0.25f, y1 = y0 + 0.25f;
            const float xy[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
            const uint32_t colour = 0x80000000u | (q * 2654435761u >> 8);
            for (int i = 0; i < 6; i++) {
                memcpy(v + (q * 6 + i) * 12, xy[i], 8);
                memcpy(v + (q * 6 + i) * 12 + 8, &colour, 4);
            }
        }
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(ub->contents(), m, 64);
        pass.color0 = rt.get();
        pass.load = LoadAction::kLoad;
        o.gmem = false;
    }
    // draw I of the scene: 7 of 8 the attribute pipeline over its own quad, 1 of 8 the textured one (its texture
    // changing every other time)
    bool allTex = false;   // (every draw the textured pipeline, its texture changing every fourth draw: one pipeline as CA's)
    bool draw(CommandBuffer& cb, uint32_t i) {
        Draw d;
        d.viewport = Viewport{0, 0, 64, 64, 0, 1};
        if (allTex) {
            d.vertexCount = 3;
            d.vertexStart = 3 * (i % quads);   // (each its own vertices: like draws are not taken back, HM_DEADFILL)
            Bindings fs;
            fs.textures = {tex[(i / 4) % 4].get()};
            fs.samplers = {&smp};
            return cb.draw(texp, pass, d, Bindings{}, fs, o);
        }
        if (i % 8 != 7) {
            d.vertexCount = 6;
            Bindings vs;
            vs.buffers = {Binding{vb.get(), uint64_t(i % quads) * 6 * 12}, Binding{ub.get(), 0}};
            return cb.draw(attr, pass, d, vs, Bindings{}, o);
        }
        d.vertexCount = 3;
        Bindings fs;
        fs.textures = {tex[(i / 16) % 4].get()};
        fs.samplers = {&smp};
        return cb.draw(texp, pass, d, Bindings{}, fs, o);
    }
};

double nowNs() { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

// the words one command buffer of N scene draws records (before the commit's own)
std::vector<uint32_t> sceneWords(uint32_t n, bool allTex = false) {
    const uint32_t from = 0;
    Scene s(64);
    s.allTex = allTex;
    auto cb = s.r.queue->commandBuffer();
    for (uint32_t i = 0; i < n; i++) CHECK(s.draw(*cb, from + i));
    std::vector<uint32_t> w = cb->streamForTesting();
    cb->commit();
    cb->waitUntilCompleted();
    CHECK(cb->status() == CommandBuffer::kCompleted);
    CHECK_EQ(s.r.fake->stats().faults, 0u);
    return w;
}

struct SkipEnv {
    std::string old;
    bool had;
    explicit SkipEnv(const char* v) {
        const char* o = getenv("HM_DRAW_SKIP");
        had = o != nullptr;
        if (had) old = o;
        if (v != nullptr) setenv("HM_DRAW_SKIP", v, 1);
        else unsetenv("HM_DRAW_SKIP");
        render::reloadDrawSkipForTesting();
    }
    ~SkipEnv() {
        if (had) setenv("HM_DRAW_SKIP", old.c_str(), 1);
        else unsetenv("HM_DRAW_SKIP");
        render::reloadDrawSkipForTesting();
    }
};
}  // namespace

// HMETAL_BENCH: ns a draw to encode (CommandBuffer::draw), and a commit's, for the scene, HM_DRAW_SKIP as the
// environment has it; without HMETAL_BENCH a short run that checks the scene draws (no timing printed)
TEST(drawcpu_bench) {
    const char* e = getenv("HMETAL_BENCH");
    uint32_t n = e != nullptr ? uint32_t(atoi(e)) : 64;
    if (n == 1) n = 2000;
    const int rounds = e != nullptr ? 5 : 1;
    render::reloadDrawSkipForTesting();
    for (int scene = 0; scene < 2; scene++) {
    Scene s(64);
    s.allTex = scene == 1;
    double best = 1e30, bestCommit = 1e30;
    size_t words = 0;
    for (int k = 0; k < rounds; k++) {
        auto cb = s.r.queue->commandBuffer();
        const double t0 = nowNs();
        for (uint32_t i = 0; i < n; i++) {
            if (!s.draw(*cb, i)) {
                CHECK(false);
                std::printf("       %s\n", cb->error().c_str());
                break;
            }
        }
        const double t1 = nowNs();
        words = cb->streamWords();
        cb->commit();
        const double t2 = nowNs();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        best = std::min(best, (t1 - t0) / n);
        bestCommit = std::min(bestCommit, t2 - t1);
    }
    CHECK_EQ(s.r.fake->stats().faults, 0u);
    if (e != nullptr) {
        const char* skip = getenv("HM_DRAW_SKIP");
        std::printf("bench: %s HM_DRAW_SKIP=%s %u draws: %.0f ns/draw, %.1f words/draw, commit %.0f us (fake GPU runs in it)\n",
                    scene == 0 ? "mixed" : "onepipe", skip ? skip : "(unset)", n, best, double(words) / n, bestCommit / 1e3);
    }
    }
}

// HM_DRAW_SKIP unset and HM_DRAW_SKIP=0 record the same words as each other (the switch's default is today's stream)
TEST(drawcpu_skip_off_is_byte_identical) {
    std::vector<uint32_t> a, b, a2, b2;
    {
        SkipEnv e(nullptr);
        a = sceneWords(200);
        a2 = sceneWords(200, true);
    }
    {
        SkipEnv e("0");
        b = sceneWords(200);
        b2 = sceneWords(200, true);
    }
    CHECK(!a.empty() && !a2.empty());
    CHECK(a == b);
    CHECK(a2 == b2);
}

// HM_DRAW_SKIP=all: fewer words, the fake runs every draw with no fault, the target holds the same pixels as with
// nothing skipped
TEST(drawcpu_skip_draws_the_same) {
    auto pixels = [](const char* v, size_t* words) {
        SkipEnv e(v);
        Scene s(64);
        auto cb = s.r.queue->commandBuffer();
        for (uint32_t i = 0; i < 200; i++) CHECK(s.draw(*cb, i));
        *words = cb->streamWords();
        const uint32_t before = s.r.fake->stats().draws;
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(s.r.fake->stats().faults, 0u);
        CHECK_EQ(s.r.fake->stats().draws - before, 200u);
        const uint8_t* p = static_cast<const uint8_t*>(s.rt->buffer()->contents());
        return std::vector<uint8_t>(p, p + s.rt->buffer()->length());
    };
    size_t w0 = 0, w1 = 0;
    const auto a = pixels("0", &w0), b = pixels("all", &w1);
    CHECK(a == b);
    CHECK(w1 < w0);
}

namespace {
// in a stream: the shader preloads (ST6_SHADER into SB6_VS_SHADER / SB6_FS_SHADER) and the fragment texture
// descriptor loads (ST6_CONSTANTS into SB6_FS_TEX), and the draws that are not CP_NOPs
struct Loads {
    uint32_t preloads = 0, fsTex = 0, draws = 0;
};
Loads loadsIn(const std::vector<uint32_t>& w) {
    using namespace a6xx;
    Loads l;
    for (size_t i = 0; i < w.size();) {
        const pm4::Header h = pm4::decode(w[i]);
        if (h.kind == pm4::Header::kBad) break;
        if (h.kind == pm4::Header::kCommand && (h.opcode == CP_LOAD_STATE6_GEOM || h.opcode == CP_LOAD_STATE6_FRAG) && h.count >= 1) {
            const uint32_t type = (w[i + 1] >> 14) & 3, block = (w[i + 1] >> 18) & 0xf;
            if (type == ST6_SHADER && (block == SB6_VS_SHADER || block == SB6_FS_SHADER)) l.preloads++;
            if (type == ST6_CONSTANTS && block == SB6_FS_TEX) l.fsTex++;
        }
        if (h.kind == pm4::Header::kCommand && h.opcode == CP_DRAW_INDX_OFFSET) l.draws++;
        i += 1 + h.count;
    }
    return l;
}

struct LoadRig : Scene {
    LoadRig() : Scene(64) {}
    bool texDraw(CommandBuffer& cb, uint32_t t, uint32_t start, const RenderPass* p = nullptr) {
        Draw d;
        d.viewport = Viewport{0, 0, 64, 64, 0, 1};
        d.vertexCount = 3;
        d.vertexStart = start;
        Bindings fs;
        fs.textures = {tex[t].get()};
        fs.samplers = {&smp};
        return cb.draw(texp, p != nullptr ? *p : pass, d, Bindings{}, fs, o);
    }
    Loads finish(std::shared_ptr<CommandBuffer> cb) {
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(r.fake->stats().faults, 0u);
        return loadsIn(cb->streamForTesting());
    }
};
}  // namespace

// HM_DRAW_SKIP=all: in one pass the program's preloads once (two: VS and FS) and a texture's descriptors loaded again
// only when they change (A A B A: three loads); off, every draw both
TEST(drawcpu_skip_loads_what_changed_in_a_pass) {
    for (const char* v : {"0", "all"}) {
        SkipEnv e(v);
        LoadRig s;
        auto cb = s.r.queue->commandBuffer();
        const uint32_t texs[4] = {0, 0, 1, 0};
        for (uint32_t i = 0; i < 4; i++) CHECK(s.texDraw(*cb, texs[i], 3 * i));
        const Loads l = s.finish(cb);
        const bool on = strcmp(v, "all") == 0;
        CHECK_EQ(l.draws, 4u);
        CHECK_EQ(l.preloads, on ? 2u : 8u);
        CHECK_EQ(l.fsTex, on ? 3u : 4u);
    }
}

// ...and a new pass (another target), a copy between draws (it ends the pass), another program in between: each
// loads everything again
TEST(drawcpu_skip_forgets_at_pass_boundaries) {
    SkipEnv e("all");
    LoadRig s;
    auto rt2 = dcTexture(s.r, 64, 64, kTextureUsageRenderTarget | kTextureUsageRead);
    RenderPass other = s.pass;
    other.color0 = rt2.get();
    {
        auto cb = s.r.queue->commandBuffer();
        CHECK(s.texDraw(*cb, 0, 0));
        CHECK(s.texDraw(*cb, 0, 3, &other));   // another pass
        CHECK(s.texDraw(*cb, 0, 6));            // and back: a third
        const Loads l = s.finish(cb);
        CHECK_EQ(l.preloads, 6u);
        CHECK_EQ(l.fsTex, 3u);
    }
    {
        auto cb = s.r.queue->commandBuffer();
        CHECK(s.texDraw(*cb, 0, 0));
        CHECK(cb->copy(*s.ub, 0, *s.ub, 128, 64));   // (a copy closes the pass)
        CHECK(s.texDraw(*cb, 0, 3));
        const Loads l = s.finish(cb);
        CHECK_EQ(l.preloads, 4u);
        CHECK_EQ(l.fsTex, 2u);
    }
    {
        auto cb = s.r.queue->commandBuffer();
        CHECK(s.texDraw(*cb, 0, 0));
        CHECK(s.draw(*cb, 0));                  // the attribute pipeline (no textures)
        CHECK(s.texDraw(*cb, 0, 3));
        const Loads l = s.finish(cb);
        CHECK_EQ(l.preloads, 6u);
        CHECK_EQ(l.fsTex, 2u);
    }
}

// HM_DEADFILL's take-back of the last draw (a like draw over it) takes back what it loaded too: the draw after it,
// the same program and texture, loads them again (with the cache left as the taken-back draw left it, it would load
// nothing and draw with whatever was there before)
TEST(drawcpu_skip_deadfill_takeback_restores_the_cache) {
    SkipEnv e("all");
    LoadRig s;
    auto cb = s.r.queue->commandBuffer();
    CHECK(s.texDraw(*cb, 0, 0));
    CHECK(s.texDraw(*cb, 0, 0));   // (the same geometry: the first taken back)
    const Loads l = s.finish(cb);
    CHECK_EQ(l.draws, 1u);
    CHECK_EQ(l.preloads, 2u);
    CHECK_EQ(l.fsTex, 1u);
}

// (ws-hm-int11) HM_CULL under HM_DRAW_SKIP: a culled draw keeps its state loads in the stream, but nothing draws them,
// so they wait in the GPU's queue, and the next draw's HLSQ_INVALIDATE_CMD drops them (the fake GPU queues the stages'
// loads as the A6xx does). SpringBoard's dock platter (HM_DRAW_SKIP=prog, HM_CULL=2): another program draws, then a
// draw of the platter's program that a later cover hides (culled: its program loads dropped), then a draw of the same
// program that left its program loads out -- run with the other program's preload and immediates. HM_SKIP_CULL=off
// shows it (the fake GPU's "without the program preloaded" fault); inv (default) zeroes the invalidates from the culled
// loader to the draw relying on it, so its loads reach that draw and the cull stands; pin keeps the loader (no cull)
TEST(drawcpu_skip_culled_loader_reaches_the_draw_relying_on_it) {
    for (const char* fix : std::vector<const char*>{"off", "inv", "pin", nullptr}) {
        for (const char* skip : {"0", "prog", "all"}) {
            SkipEnv e(skip);
            setenv("HM_CULL", "2", 1);
            if (fix != nullptr) setenv("HM_SKIP_CULL", fix, 1);
            else unsetenv("HM_SKIP_CULL");
            LoadRig s;
            Program lph = dcAttr();
            lph.vs.k.name = "fixed_vert_lph_spc";
            lph.blend.enabled = false;
            auto rect = [&](float x0, float y0, float x1, float y1, uint64_t at) {
                uint8_t* v = static_cast<uint8_t*>(s.vb->contents()) + at;
                const float xy[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
                const uint32_t colour = 0xff0000ffu;
                for (int i = 0; i < 6; i++) {
                    memcpy(v + 12 * i, xy[i], 8);
                    memcpy(v + 12 * i + 8, &colour, 4);
                }
                Bindings b;
                b.buffers = {Binding{s.vb.get(), at}, Binding{s.ub.get(), 0}};
                return b;
            };
            Draw d;
            d.viewport = Viewport{0, 0, 64, 64, 0, 1};
            d.vertexCount = 6;
            const uint32_t before = s.r.fake->stats().draws, faults0 = s.r.fake->stats().faults;
            auto cb = s.r.queue->commandBuffer();
            CHECK(s.texDraw(*cb, 0, 0));                                                           // another program (culled too)
            CHECK(cb->draw(lph, s.pass, d, rect(-0.5f, -0.5f, 0.5f, 0.5f, 0), Bindings{}, s.o));   // hidden by the next
            CHECK(cb->draw(lph, s.pass, d, rect(-1, -1, 1, 1, 6 * 12), Bindings{}, s.o));          // a cover, relying on it
            cb->commit();
            cb->waitUntilCompleted();
            const Loads l = loadsIn(cb->streamForTesting());   // (after the commit: the cull is resolved as the pass ends)
            const uint32_t faults = s.r.fake->stats().faults - faults0, ran = s.r.fake->stats().draws - before;
            unsetenv("HM_CULL");
            unsetenv("HM_SKIP_CULL");
            const bool skipping = strcmp(skip, "0") != 0;
            const bool off = fix != nullptr && strcmp(fix, "off") == 0, pin = fix != nullptr && strcmp(fix, "pin") == 0;
            if (skipping && off) {
                CHECK_EQ(faults, 1u);   // the defect: the cover ran without its program
                continue;
            }
            CHECK(cb->status() == CommandBuffer::kCompleted);
            CHECK_EQ(faults, 0u);
            CHECK_EQ(l.preloads, skipping ? 4u : 6u);
            // the two draws under the cover are culled (the other program's too), the platter-like one kept if pinned
            CHECK_EQ(l.draws, skipping && pin ? 2u : 1u);
            CHECK_EQ(ran, skipping && pin ? 2u : 1u);
        }
    }
}

// (ws-hm-int13b) HM_DRAW_SKIP=prog and the immediates. Every draw writes HLSQ_INVALIDATE_CMD (programConfig); on the
// A620 a draw after it that did not load its immediates again ran with stale constants (SpringBoard's dock platter
// with HM_DRAW_SKIP=prog alone, HM_CULL off too), the pushed constants being loaded every draw anyway. The stream read
// as that GPU does: an invalidate of the fragment stage drops its constants, and a draw of a program with immediates
// needs them loaded since. HM_SKIP_PROG=old (the drawcpu..int12 encoding) leaves draws 2..4 without; imm (default)
// loads them every draw, the preloads still left out; noinv leaves the invalidate out with them
TEST(drawcpu_skip_prog_keeps_the_immediates_after_the_invalidate) {
    for (const char* mode : {"old", "imm", "noinv"}) {
        setenv("HM_SKIP_PROG", mode, 1);
        SkipEnv e("prog");
        LoadRig s;
        Program p = dcAttr();
        static const uint32_t imm[4] = {0x3f800000u, 0x3f000000u, 0x3e800000u, 0x3f800000u};
        const uint32_t off = (p.fs.k.constlen + 3) & ~3u;
        if (p.fs.k.numImmediates == 0) {
            p.fs.k.immediates = imm;
            p.fs.k.numImmediates = 4;
            p.fs.k.immediatesOffsetVec4 = off;
            p.fs.k.constlen = off + 4;
        }
        const uint32_t immOff = uint32_t(p.fs.k.immediatesOffsetVec4);
        auto cb = s.r.queue->commandBuffer();
        for (uint32_t i = 0; i < 4; i++) {
            Draw d;
            d.viewport = Viewport{0, 0, 64, 64, 0, 1};
            d.vertexCount = 6;
            Bindings vs;
            vs.buffers = {Binding{s.vb.get(), uint64_t(i) * 6 * 12}, Binding{s.ub.get(), 0}};
            CHECK(cb->draw(p, s.pass, d, vs, Bindings{}, s.o));
        }
        cb->commit();
        cb->waitUntilCompleted();
        unsetenv("HM_SKIP_PROG");
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(s.r.fake->stats().faults, 0u);
        using namespace a6xx;
        const std::vector<uint32_t> w = cb->streamForTesting();
        bool have = false;
        uint32_t draws = 0, stale = 0, preloads = 0;
        for (size_t i = 0; i < w.size();) {
            const pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kBad) break;
            if (h.kind == pm4::Header::kRegisters && h.reg <= REG_A6XX_HLSQ_INVALIDATE_CMD && REG_A6XX_HLSQ_INVALIDATE_CMD < h.reg + h.count &&
                (w[i + 1 + (REG_A6XX_HLSQ_INVALIDATE_CMD - h.reg)] & A6XX_HLSQ_INVALIDATE_CMD_FS_STATE) != 0) {
                have = false;
            }
            if (h.kind == pm4::Header::kCommand && h.opcode == CP_LOAD_STATE6_FRAG && h.count >= 1) {
                const uint32_t w0 = w[i + 1];
                const uint32_t type = (w0 >> 14) & 3, block = (w0 >> 18) & 0xf, dst = w0 & 0x3fff, units = w0 >> 22;
                if (type == ST6_SHADER && block == SB6_FS_SHADER) preloads++;
                if (type == ST6_CONSTANTS && block == SB6_FS_SHADER && dst <= immOff && immOff < dst + units) have = true;
            }
            if (h.kind == pm4::Header::kCommand && h.opcode == CP_DRAW_INDX_OFFSET) {
                draws++;
                if (!have) stale++;
            }
            i += 1 + h.count;
        }
        CHECK_EQ(draws, 4u);
        CHECK_EQ(preloads, 1u);   // (the preloads stay left out in every mode)
        CHECK_EQ(stale, strcmp(mode, "old") == 0 ? 3u : 0u);
    }
}
