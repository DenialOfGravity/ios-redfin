// (ws-hm-int13) the draw path's CPU work cut (RegShadow's take-back journal, fixed-size staticKey, reused deadFillKey
// buffer, one cullNote a draw, stack vectors in encodeDraw, Stream appends): pure refactors. A CoreAnimation-like
// frame (opaque rectangles, one in eight over the whole target, blended ones between, textured draws, a draw repeated
// now and then for HM_DEADFILL's keyed take-back, indexed draws, setBytes-style shadowed buffers and plain ones, three
// passes over two targets) recorded under HM_CULL 0/2/3 x HM_DEADFILL 1/2 x HM_DRAW_SKIP 0/tex,table/all x HM_NOUC 0/1:
// each stream's FNV-1a digest. HM13_DIGEST_OUT=FILE writes them; HM13_DIGEST_GOLDEN=FILE compares them with a file the
// base build (ws-hm-int12) wrote with this same test. HMETAL_BENCH=N: ns a draw for the frame, HM_CULL=2 HM_DEADFILL=2
// HM_DRAW_SKIP=tex,table.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"
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
struct P13Rig {
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
    FakeGpu* fake = nullptr;
    P13Rig() {
        auto f = std::make_unique<FakeGpu>();
        fake = f.get();
        gpu = std::make_unique<Gpu>(HMETAL_TRANSPORT_OF(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
};

Program p13Lph(bool blended, const char* label) {
    Program p{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, label};
    p.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
    p.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
    p.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
    p.vs.k.name = "fixed_vert_lph_spc";
    if (blended) {
        p.blend.enabled = true;
        p.blend.srcRgb = p.blend.srcAlpha = 1;
        p.blend.dstRgb = p.blend.dstAlpha = 5;
    }
    return p;
}

struct P13Env {
    std::string name, old;
    bool had;
    P13Env(const char* n, const char* v) : name(n) {
        const char* o = getenv(n);
        had = o != nullptr;
        if (had) old = o;
        if (v != nullptr) setenv(n, v, 1);
        else unsetenv(n);
    }
    ~P13Env() {
        if (had) setenv(name.c_str(), old.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

constexpr float kP13Identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

struct P13Scene {
    P13Rig r;
    std::shared_ptr<Texture> rt[2], tex[4];
    std::vector<std::shared_ptr<Buffer>> keep;
    std::shared_ptr<Buffer> bigVb, mat, ib;
    Sampler smp;
    Program cover = p13Lph(false, "cover"), blend = p13Lph(true, "blend");
    Program texp{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
    render::Options o;
    bool shadowed;
    explicit P13Scene(bool sh) : shadowed(sh) {
        std::string why;
        for (int i = 0; i < 2; i++) {
            TextureDesc d;
            d.format = PixelFormat::kRGBA8Unorm;
            d.width = d.height = 64;
            d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
            rt[i] = Texture::make(*r.gpu, d, &why);
            CHECK(rt[i] != nullptr);
        }
        for (auto& t : tex) {
            TextureDesc d;
            d.format = PixelFormat::kRGBA8Unorm;
            d.width = d.height = 8;
            d.usage = kTextureUsageRead;
            t = Texture::make(*r.gpu, d, &why);
            CHECK(t != nullptr);
        }
        CHECK(makeSampler(SamplerDesc{}, &smp, &why));
        o.gmem = false;
        bigVb = newBuffer(*r.gpu, 64 * 6 * 12 + 8192, &why);
        mat = newBuffer(*r.gpu, 64, &why);
        ib = newBuffer(*r.gpu, 64 * 2, &why);
        CHECK(bigVb != nullptr && mat != nullptr && ib != nullptr);
        for (uint32_t q = 0; q < 64; q++) quad(static_cast<uint8_t*>(bigVb->contents()) + q * 72, q);
        memcpy(mat->contents(), kP13Identity, 64);
        const uint16_t ix[6] = {0, 1, 2, 3, 4, 5};
        for (int k = 0; k < 10; k++) memcpy(static_cast<uint8_t*>(ib->contents()) + 12 * k, ix, 12);
        if (shadowed) ib->setShadow(ib->contents(), ib->length());
        // the setBytes-style buffers, one pair a draw, made before (the bench times the draws alone)
        if (shadowed) {
            for (uint32_t i = 0; i < kDraws; i++) {
                uint8_t v[72];
                quad(v, (i % 7 == 6 ? i - 1 : i) % 64);
                sv.push_back(scratch(v, sizeof v));
                sm.push_back(scratch(kP13Identity, 64));
            }
        }
    }
    static constexpr uint32_t kDraws = 130;
    std::vector<Buffer*> sv, sm;
    static void quad(uint8_t* v, uint32_t q) {
        float x0 = -1 + 2.0f * (q % 4) / 4, y0 = -1 + 2.0f * ((q / 4) % 4) / 4, x1 = x0 + 0.75f, y1 = y0 + 0.75f;
        if (q % 8 == 7) x0 = y0 = -1, x1 = y1 = 1;
        const float xy[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
        const uint32_t colour = 0xff000000u | (q * 2654435761u >> 8);
        for (int i = 0; i < 6; i++) {
            memcpy(v + 12 * i, xy[i], 8);
            memcpy(v + 12 * i + 8, &colour, 4);
        }
    }
    Buffer* scratch(const void* bytes, uint64_t n) {
        std::string why;
        auto b = newBuffer(*r.gpu, n, &why);
        CHECK(b != nullptr);
        memcpy(b->contents(), bytes, n);
        b->setShadow(bytes, n);
        keep.push_back(b);
        return b.get();
    }
    bool draw(CommandBuffer& cb, uint32_t i) {
        // three passes: target 0 (load), target 1 (clear), target 0 again (load)
        RenderPass pass;
        const uint32_t ph = i < 50 ? 0 : i < 90 ? 1 : 2;
        pass.color0 = rt[ph == 1 ? 1 : 0].get();
        pass.load = ph == 1 ? LoadAction::kClear : LoadAction::kLoad;
        // a draw now and then the same as the one before it (HM_DEADFILL's keyed take-back)
        const uint32_t j = i % 7 == 6 ? i - 1 : i;
        const uint32_t q = j % 64;
        Draw d;
        d.viewport = Viewport{0, 0, 64, 64, 0, 1};
        if (j % 11 == 5) {
            d.vertexCount = 3;
            d.vertexStart = 0;
            Bindings fs;
            fs.textures = {tex[(j / 3) % 4].get()};
            fs.samplers = {&smp};
            return cb.draw(texp, pass, d, Bindings{}, fs, o);
        }
        d.vertexCount = 6;
        Bindings vs;
        if (shadowed) {
            vs.buffers = {Binding{sv[i], 0}, Binding{sm[i], 0}};
        } else {
            vs.buffers = {Binding{bigVb.get(), uint64_t(q) * 72}, Binding{mat.get(), 0}};
        }
        if (j % 3 == 2) {
            d.indexBuffer = ib.get();
            d.indexOffset = 12 * (j % 10);
        }
        return cb.draw(j % 2 == 0 || q % 8 == 7 ? cover : blend, pass, d, vs, Bindings{}, o);
    }
    std::vector<uint32_t> words(uint32_t n) {
        auto cb = r.queue->commandBuffer();
        for (uint32_t i = 0; i < n; i++) CHECK(draw(*cb, i));
        std::vector<uint32_t> w = cb->streamForTesting();
        cb->commit();
        cb->waitUntilCompleted();
        CHECK(cb->status() == CommandBuffer::kCompleted);
        CHECK_EQ(r.fake->stats().faults, 0u);
        return w;
    }
};

uint64_t p13Digest(const std::vector<uint32_t>& w) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t x : w) {
        for (int k = 0; k < 4; k++) {
            h ^= (x >> (8 * k)) & 0xff;
            h *= 1099511628211ull;
        }
    }
    return h;
}

// every configuration's line: "<cfg> <digest> <words> <culled>"
std::vector<std::string> p13Lines() {
    std::vector<std::string> out;
    for (const char* cull : {"0", "2", "3"}) {
        for (const char* df : {"1", "2"}) {
            for (const char* skip : {"0", "tex,table", "all"}) {
                for (int nouc : {0, 1}) {
                    for (bool sh : {true, false}) {
                        P13Env c("HM_CULL", cull), f("HM_DEADFILL", df), s("HM_DRAW_SKIP", skip);
                        render::reloadDrawSkipForTesting();
                        render::noUcSetForTesting(nouc);
                        uint64_t c0[8], c1[8];
                        cullStats(c0);
                        const uint64_t fd0 = fillsDropped();
                        std::vector<uint32_t> w;
                        {
                            P13Scene scene(sh);
                            w = scene.words(P13Scene::kDraws);
                        }
                        cullStats(c1);
                        const uint64_t fd1 = fillsDropped();
                        render::noUcSetForTesting(0);
                        char line[256];
                        std::snprintf(line, sizeof line, "cull=%s df=%s skip=%s nouc=%d %s %016llx %zu %llu %llu", cull, df, skip, nouc,
                                      sh ? "shadowed" : "plain", (unsigned long long)p13Digest(w), w.size(), (unsigned long long)(c1[0] - c0[0]),
                                      (unsigned long long)(fd1 - fd0));
                        out.push_back(line);
                    }
                }
            }
        }
    }
    render::reloadDrawSkipForTesting();
    return out;
}
}  // namespace

TEST(perf13_stream_digests) {
    const std::vector<std::string> lines = p13Lines();
    CHECK_EQ(lines.size(), size_t(72));
    // the frame does cull and take back somewhere (the switches reach the paths changed)
    bool culled = false, dropped = false;
    for (const std::string& l : lines) {
        unsigned long long c = 0, fd = 0;
        const char* p = strrchr(l.c_str(), ' ');
        fd = strtoull(p + 1, nullptr, 10);
        const std::string head = l.substr(0, size_t(p - l.c_str()));
        c = strtoull(strrchr(head.c_str(), ' ') + 1, nullptr, 10);
        if (c != 0) culled = true;
        if (fd != 0) dropped = true;
    }
    CHECK(culled);
    CHECK(dropped);
    if (const char* o = getenv("HM13_DIGEST_OUT")) {
        FILE* f = fopen(o, "w");
        CHECK(f != nullptr);
        if (f != nullptr) {
            for (const std::string& l : lines) std::fprintf(f, "%s\n", l.c_str());
            fclose(f);
        }
    }
    if (const char* g = getenv("HM13_DIGEST_GOLDEN")) {
        FILE* f = fopen(g, "r");
        CHECK(f != nullptr);
        if (f != nullptr) {
            char buf[256];
            size_t i = 0, bad = 0;
            while (fgets(buf, sizeof buf, f) != nullptr) {
                std::string s(buf);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (i >= lines.size() || lines[i] != s) {
                    bad++;
                    std::printf("       golden %s\n       now    %s\n", s.c_str(), i < lines.size() ? lines[i].c_str() : "(none)");
                }
                i++;
            }
            fclose(f);
            CHECK_EQ(i, lines.size());
            CHECK_EQ(bad, size_t(0));
            std::printf("       %zu configurations compared with %s, %zu differ\n", i, g, bad);
        }
    }
}

// HMETAL_BENCH=N: ns a draw (CommandBuffer::draw) for the frame with HM_CULL=2 HM_DEADFILL=2 HM_DRAW_SKIP=tex,table
// (HM_NOUC 0 and 1; shadowed and plain buffers); without it a short run
TEST(perf13_bench) {
    const char* e = getenv("HMETAL_BENCH");
    const uint32_t n = P13Scene::kDraws;
    const int rounds = e != nullptr ? std::max(3, atoi(e)) : 1;
    P13Env c("HM_CULL", "2"), f("HM_DEADFILL", "2"), s("HM_DRAW_SKIP", "tex,table");
    render::reloadDrawSkipForTesting();
    for (int nouc : {0, 1}) {
        for (bool sh : {true, false}) {
            render::noUcSetForTesting(nouc);
            P13Scene scene(sh);
            double best = 1e30;
            for (int k = 0; k < rounds; k++) {
                auto cb = scene.r.queue->commandBuffer();
                const auto t0 = std::chrono::steady_clock::now();
                for (uint32_t i = 0; i < n; i++) CHECK(scene.draw(*cb, i));
                const auto t1 = std::chrono::steady_clock::now();
                cb->commit();
                cb->waitUntilCompleted();
                CHECK(cb->status() == CommandBuffer::kCompleted);
                best = std::min(best, double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / n);
            }
            if (e != nullptr) std::printf("bench13: nouc=%d %s: %.0f ns/draw (best of %d)\n", nouc, sh ? "shadowed" : "plain", best, rounds);
            render::noUcSetForTesting(0);
        }
    }
    render::reloadDrawSkipForTesting();
}

// RegShadow::mark / rollback (HM_DEADFILL's take-back, was a copy of the table): after any holds, a growth of the
// table and a clear() past the mark, rollback leaves what each register holds as it was at the mark
TEST(perf13_regshadow_rollback) {
    uint32_t seed = 12345;
    auto rnd = [&seed]() { return seed = seed * 1664525u + 1013904223u; };
    auto same = [](const CommandBuffer::RegShadow& a, const std::map<uint32_t, uint32_t>& m, uint32_t universe) {
        CommandBuffer::RegShadow c = a;   // (hold changes the table: a copy is checked)
        for (uint32_t r = 0; r < universe; r++) {
            auto it = m.find(r);
            const uint32_t v = it != m.end() ? it->second : 0xdeadbeefu;
            if (c.hold(r, v) != (it != m.end())) return false;
        }
        return true;
    };
    for (int round = 0; round < 200; round++) {
        CommandBuffer::RegShadow s;
        std::map<uint32_t, uint32_t> m;
        const uint32_t universe = round % 3 == 0 ? 3000 : 700;   // (past 512 entries the table grows)
        for (int k = 0; k < int(rnd() % 600); k++) {
            const uint32_t r = rnd() % universe, v = rnd() % 4;
            CHECK_EQ(s.hold(r, v), m.count(r) != 0 && m[r] == v);
            m[r] = v;
        }
        const std::map<uint32_t, uint32_t> at = m;
        s.mark();
        const int n = int(rnd() % 1500);
        for (int k = 0; k < n; k++) {
            if (round % 5 == 4 && k == n / 2) {
                s.clear();
                m.clear();
                continue;
            }
            const uint32_t r = rnd() % universe, v = rnd() % 4;
            CHECK_EQ(s.hold(r, v), m.count(r) != 0 && m[r] == v);
            m[r] = v;
        }
        CHECK(same(s, m, universe));
        s.rollback();
        CHECK(same(s, at, universe));
        // marked again and unmarked: the changes stay
        s.mark();
        s.hold(1, 77);
        s.unmark();
        std::map<uint32_t, uint32_t> after = at;
        after[1] = 77;
        CHECK(same(s, after, universe));
    }
}
