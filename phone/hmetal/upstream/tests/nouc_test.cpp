// (ws-hm-int12) HM_NOUC: the CPU reads of buffer memory a draw makes (HM_DEADFILL's key, HM_CULL's CoreAnimation
// geometry, constant copies, argument-buffer words), counted through the core's one accessor (render::cpuRead /
// cpuView / ucCount: the fake GPU's buffers are host memory, so the count is what tells, not the time). With HM_NOUC=1 a
// CoreAnimation-like frame whose small per-draw data comes through setBytes (the plug-in's shadowed scratch) reads no
// buffer memory at all; one whose vertices live in a big buffer CA writes itself reads it only in bulk copies; and the
// words recorded are the same as with HM_NOUC=0, byte for byte, with HM_CULL / HM_DEADFILL on and off.
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "test.hpp"
#include "transport_hook.hpp"

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

namespace {
struct NuRig {
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<CommandQueue> queue;
    FakeGpu* fake = nullptr;
    NuRig() {
        auto f = std::make_unique<FakeGpu>();
        fake = f.get();
        gpu = std::make_unique<Gpu>(HMETAL_TRANSPORT_OF(f));
        std::string why;
        queue = CommandQueue::make(*gpu, &why);
    }
};

// CoreAnimation's fixed_vert_lph_spc as cullNote knows it (tri_attr's 12-byte vertices, the transform at buffer 1)
Program lph(bool blended, const char* label) {
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

struct Env {
    const char* name;
    explicit Env(const char* n, const char* v) : name(n) {
        if (v != nullptr) setenv(n, v, 1);
        else unsetenv(n);
    }
    ~Env() { unsetenv(name); }
};
struct NoUc {
    explicit NoUc(int on) { render::noUcSetForTesting(on); }
    ~NoUc() { render::noUcSetForTesting(0); }
};

// SHADOWED: each draw's vertices and transform a fresh small buffer written once and shadowed (the plug-in's
// -setVertexBytes: scratch under HM_NOUC); else the vertices in one big buffer and the transform in one 64-byte
// buffer CA writes itself (no shadow). Every third draw indexed (16-bit indices, shadowed or not the same way).
struct CaScene {
    NuRig r;
    std::shared_ptr<Texture> rt;
    std::vector<std::shared_ptr<Buffer>> keep;
    std::shared_ptr<Buffer> bigVb, mat, ib;
    Program cover = lph(false, "cover"), blend = lph(true, "blend");
    RenderPass pass;
    render::Options o;
    bool shadowed;
    explicit CaScene(bool sh) : shadowed(sh) {
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        d.width = d.height = 64;
        d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
        std::string why;
        rt = Texture::make(*r.gpu, d, &why);
        CHECK(rt != nullptr);
        pass.color0 = rt.get();
        pass.load = LoadAction::kLoad;
        o.gmem = false;
        bigVb = newBuffer(*r.gpu, 64 * 6 * 12 + 8192, &why);
        mat = newBuffer(*r.gpu, 64, &why);
        ib = newBuffer(*r.gpu, 64 * 2, &why);
        CHECK(bigVb != nullptr && mat != nullptr && ib != nullptr);
        for (uint32_t q = 0; q < 64; q++) quad(static_cast<uint8_t*>(bigVb->contents()) + q * 72, q);
        memcpy(mat->contents(), kIdentity, 64);
        const uint16_t ix[6] = {0, 1, 2, 3, 4, 5};
        for (int k = 0; k < 10; k++) memcpy(static_cast<uint8_t*>(ib->contents()) + 12 * k, ix, 12);
        if (shadowed) ib->setShadow(ib->contents(), ib->length());
    }
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // quad Q: a rectangle somewhere in the target; every eighth the whole target (a full cover)
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
    // the plug-in's -setVertexBytes: a new buffer, the bytes written once, shadowed
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
        const uint32_t q = i % 64;
        Draw d;
        d.viewport = Viewport{0, 0, 64, 64, 0, 1};
        d.vertexCount = 6;
        Bindings vs;
        if (shadowed) {
            uint8_t v[72];
            quad(v, q);
            vs.buffers = {Binding{scratch(v, sizeof v), 0}, Binding{scratch(kIdentity, 64), 0}};
        } else {
            vs.buffers = {Binding{bigVb.get(), uint64_t(q) * 72}, Binding{mat.get(), 0}};
        }
        if (i % 3 == 2) {
            d.indexBuffer = ib.get();
            d.indexOffset = 12 * (i % 10);
        }
        // opaque covers (one in eight over the whole target) and blended quads between them
        return cb.draw(i % 2 == 0 || q % 8 == 7 ? cover : blend, pass, d, vs, Bindings{}, o);
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
constexpr float CaScene::kIdentity[16];

// the scene's words and its buffer reads (ucReadStats), with HM_NOUC=ON
std::vector<uint32_t> run(bool shadowed, int on, uint32_t n, uint64_t uc[6], uint64_t* culled = nullptr) {
    NoUc u(on);
    CaScene s(shadowed);
    uint64_t c0[8], c1[8];
    cullStats(c0);
    render::ucResetForTesting();
    std::vector<uint32_t> w = s.words(n);
    render::ucReadStats(uc);
    cullStats(c1);
    if (culled != nullptr) *culled = (c1[0] - c0[0]) + (c1[6] - c0[6]);
    return w;
}
}  // namespace

// HM_NOUC unset/0 and 1 record the same words, with culling and dead fills on (they inspect the geometry) and off
TEST(nouc_streams_are_byte_identical) {
    for (const char* level : {(const char*)nullptr, "2", "3"}) {
        Env c("HM_CULL", level), f("HM_DEADFILL", level != nullptr ? "2" : nullptr);
        for (bool shadowed : {true, false}) {
            uint64_t uc[6], culled0 = 0, culled1 = 0;
            const std::vector<uint32_t> a = run(shadowed, 0, 120, uc, &culled0);
            const std::vector<uint32_t> b = run(shadowed, 1, 120, uc, &culled1);
            CHECK(!a.empty());
            CHECK(a == b);
            CHECK_EQ(culled0, culled1);
            if (level != nullptr) CHECK(culled1 > 0);   // (the scene does cull: the geometry was read and used)
            if (a != b) std::printf("       HM_CULL=%s %s: %zu vs %zu words\n", level ? level : "-", shadowed ? "shadowed" : "plain", a.size(), b.size());
        }
    }
}

// The CA-like scene with setBytes data, HM_CULL=2 HM_DEADFILL=2: HM_NOUC=1 reads no buffer memory (every read from a
// shadow); HM_NOUC=0 reads the mapping as before
TEST(nouc_ca_scene_reads_no_buffer_memory) {
    Env c("HM_CULL", "2"), f("HM_DEADFILL", "2");
    const uint32_t n = 120;
    uint64_t off[6], on[6];
    run(true, 0, n, off);
    run(true, 1, n, on);
    CHECK_EQ(on[0], 0u);
    CHECK_EQ(on[2], 0u);
    CHECK(on[4] > 0);
    CHECK(off[0] > 0);
    std::printf("       nouc setBytes scene, per draw: HM_NOUC=0 %.1f in-place reads (%.0f B) + %.1f bulk (%.0f B); "
                "HM_NOUC=1 %.1f + %.1f, %.1f shadow reads (%.0f B)\n",
                double(off[0]) / n, double(off[1]) / n, double(off[2]) / n, double(off[3]) / n, double(on[0]) / n, double(on[2]) / n,
                double(on[4]) / n, double(on[5]) / n);
}

// Vertices in one big buffer CA writes itself (no shadow): HM_NOUC=1 reads it only in bulk copies (the indices, the
// vertices' span, the transform: at most 3 a draw for each inspection), never load by load
TEST(nouc_unshadowed_reads_only_in_bulk) {
    Env c("HM_CULL", "2"), f("HM_DEADFILL", "2");
    const uint32_t n = 120;
    uint64_t off[6], on[6];
    run(false, 0, n, off);
    run(false, 1, n, on);
    CHECK_EQ(on[0], 0u);
    CHECK(on[2] > 0);
    CHECK(on[2] <= 4ull * n);
    CHECK(off[0] > on[2]);
    std::printf("       nouc plain scene, per draw: HM_NOUC=0 %.1f in-place reads (%.0f B) + %.1f bulk (%.0f B); HM_NOUC=1 %.1f + %.1f bulk (%.0f B)\n",
                double(off[0]) / n, double(off[1]) / n, double(off[2]) / n, double(off[3]) / n, double(on[0]) / n, double(on[2]) / n,
                double(on[3]) / n);
}

// The C interface the plug-in uses: a shadow, and reads through it or (HM_NOUC=1) into the caller's scratch
TEST(nouc_c_api_shadow_and_read) {
    hakios_metal_device* dev = hakios_metal_open_fake();
    CHECK(dev != nullptr);
    if (dev == nullptr) return;
    hakios_metal_buffer* b = hakios_metal_buffer_new(dev, 64);
    CHECK(b != nullptr);
    uint8_t bytes[64];
    for (int i = 0; i < 64; i++) bytes[i] = uint8_t(i * 7);
    memcpy(hakios_metal_buffer_contents(b), bytes, 64);
    uint8_t scratch[64] = {};
    {
        NoUc u(0);
        CHECK(hakios_metal_nouc() == 0);
        CHECK(hakios_metal_buffer_read(b, 8, 16, scratch) == static_cast<uint8_t*>(hakios_metal_buffer_contents(b)) + 8);
    }
    {
        NoUc u(1);
        CHECK(hakios_metal_nouc() == 1);
        const void* p = hakios_metal_buffer_read(b, 8, 16, scratch);
        CHECK(p == scratch);
        CHECK(memcmp(p, bytes + 8, 16) == 0);
        CHECK(hakios_metal_buffer_read(b, 60, 8, scratch) == nullptr);   // (past the end)
        CHECK_EQ(hakios_metal_buffer_set_shadow(b, bytes, 65), -1);
        CHECK_EQ(hakios_metal_buffer_set_shadow(b, bytes, 64), 0);
        const uint8_t* s = static_cast<const uint8_t*>(hakios_metal_buffer_read(b, 8, 16, scratch));
        CHECK(s != nullptr && s != scratch && s != static_cast<uint8_t*>(hakios_metal_buffer_contents(b)) + 8);
        CHECK(s != nullptr && memcmp(s, bytes + 8, 16) == 0);
    }
    hakios_metal_buffer_free(b);
    hakios_metal_close(dev);
}
