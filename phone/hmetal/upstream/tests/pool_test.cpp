// (ws-hm-pool) HM_RT_CACHE: released render targets' GEM objects reused by the next render target of their size; never
// one a command buffer still holds; the least recently released let go past the limit; all of it on a refused
// allocation (then the allocation is retried) and on a forced trim. HM_POOL_KEEP_MB: an idle trim keeps free pieces.
#include <cerrno>
#include <cstring>
#include <map>

#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"
#include "hakios-metal/texture.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
constexpr unsigned kRt = HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1;   // render target, shader read
constexpr unsigned kBgra = 80;                                          // BGRA8Unorm

struct Stats {
    unsigned long long s[12];
    Stats() { hakios_metal_rt_cache_stats(s); }
    unsigned long long hits() const { return s[0]; }
    unsigned long long misses() const { return s[1]; }
    unsigned long long held() const { return s[3]; }
    unsigned long long entries() const { return s[4]; }
    unsigned long long evicted() const { return s[5]; }
    unsigned long long dropped() const { return s[6]; }
    unsigned long long gemNew() const { return s[8]; }
    unsigned long long retries() const { return s[10]; }
};

hakios_metal_texture* rt(hakios_metal_device* d, unsigned w, unsigned h, unsigned usage = kRt) {
    hakios_metal_texture* t = hakios_metal_texture_new_levels(d, 2, kBgra, w, h, 1, usage);
    CHECK(t != nullptr);
    return t;
}

bool allBytes(hakios_metal_texture* t, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    const uint8_t* p = static_cast<const uint8_t*>(hakios_metal_texture_contents(t));
    const uint64_t pitch = hakios_metal_texture_bytes_per_row(t);
    for (uint32_t y = 0; y < hakios_metal_texture_height(t); y++)
        for (uint32_t x = 0; x < hakios_metal_texture_width(t); x++) {
            const uint8_t* px = p + y * pitch + x * 4;
            if (px[0] != b || px[1] != g || px[2] != r || px[3] != a) return false;
        }
    return true;
}

hakios_metal_render_pass clearOf(hakios_metal_texture* t, double r, double g, double b, double a) {
    hakios_metal_render_pass p{};
    p.color0 = t;
    p.load_action = 2;   // Clear
    p.store_action = 1;  // Store
    p.clear_color[0] = r, p.clear_color[1] = g, p.clear_color[2] = b, p.clear_color[3] = a;
    p.clear_depth = 1.0;
    return p;
}
}  // namespace

// off (the default): nothing kept, every render target a GEM object of its own
TEST(rt_cache_off_by_default_keeps_nothing) {
    hakios_metal_device* d = hakios_metal_open_fake();
    const Stats a;
    CHECK_EQ(a.s[11], 0ull);
    hakios_metal_texture_free(rt(d, 64, 64));
    hakios_metal_texture* t = rt(d, 64, 64);
    const Stats b;
    CHECK_EQ(b.hits() - a.hits(), 0ull);
    CHECK_EQ(b.entries(), 0ull);
    CHECK_EQ(b.gemNew() - a.gemNew(), 2ull);
    hakios_metal_texture_free(t);
    hakios_metal_close(d);
}

// a released render target's memory serves the next one of its size (or a little smaller): no GEM object asked for;
// a bigger one, a texture that is no render target, and one past the slack are not served from it
TEST(rt_cache_reuses_a_released_render_target) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_rt_cache_set(d, 8ull << 20, 25, 0);
    hakios_metal_texture* t = rt(d, 64, 64);
    void* first = hakios_metal_texture_contents(t);
    hakios_metal_texture_free(t);
    const Stats a;
    CHECK_EQ(a.entries(), 1ull);
    CHECK(a.held() >= 64ull * 64 * 4);
    t = rt(d, 64, 60);   // 15 KB: the same 16 KB object
    const Stats b;
    CHECK_EQ(b.hits() - a.hits(), 1ull);
    CHECK_EQ(b.gemNew() - a.gemNew(), 0ull);
    CHECK_EQ(b.entries(), 0ull);
    CHECK(hakios_metal_texture_contents(t) == first);
    hakios_metal_texture_free(t);
    // bigger than anything held: a GEM object of its own (and the 16 KB one stays)
    hakios_metal_texture* big = rt(d, 128, 64);
    const Stats c;
    CHECK_EQ(c.misses() - b.misses(), 1ull);
    CHECK_EQ(c.gemNew() - b.gemNew(), 1ull);
    CHECK_EQ(c.entries(), 1ull);
    // a sampled texture is never made from the cache, nor given to it
    hakios_metal_texture* plain = rt(d, 64, 64, 1);
    CHECK(hakios_metal_texture_contents(plain) != first);
    hakios_metal_texture_free(plain);
    const Stats e;
    CHECK_EQ(e.entries(), 1ull);
    CHECK_EQ(e.hits(), c.hits());
    // with the 32 KB object released too: a 16 KB ask takes the 16 KB one, and a second is not served from the 32 KB
    // one (past 25 % bigger)
    void* bigAt = hakios_metal_texture_contents(big);
    hakios_metal_texture_free(big);
    CHECK_EQ(Stats().entries(), 2ull);
    hakios_metal_texture* s1 = rt(d, 64, 64);
    hakios_metal_texture* s2 = rt(d, 64, 64);
    CHECK(hakios_metal_texture_contents(s1) == first);
    CHECK(hakios_metal_texture_contents(s2) != bigAt);
    CHECK_EQ(Stats().entries(), 1ull);
    hakios_metal_texture* b2 = rt(d, 112, 64);   // 28 KB: the 32 KB one, inside the slack
    CHECK(hakios_metal_texture_contents(b2) == bigAt);
    hakios_metal_texture_free(b2);
    hakios_metal_texture_free(s1);
    hakios_metal_texture_free(s2);
    hakios_metal_close(d);   // (the cached objects closed with the device)
}

// one a command buffer still holds is not handed out: the next render target gets memory of its own until the
// buffer is done; then that memory is there to reuse, and a pass that clears it draws what it should
TEST(rt_cache_never_hands_out_one_in_flight) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_rt_cache_set(d, 8ull << 20, 25, 0);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_texture* t = rt(d, 64, 64);
    void* first = hakios_metal_texture_contents(t);
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    hakios_metal_render_pass red = clearOf(t, 1, 0, 0, 1);
    CHECK_EQ(hakios_metal_cb_clear(cb, &red), 0);
    hakios_metal_texture_free(t);   // (CoreAnimation lets go of it while the buffer holds it)
    CHECK_EQ(Stats().entries(), 0ull);
    hakios_metal_texture* u = rt(d, 64, 64);
    CHECK(hakios_metal_texture_contents(u) != first);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    CHECK_EQ(Stats().entries(), 1ull);   // done with: kept now
    hakios_metal_texture* v = rt(d, 64, 64);
    CHECK(hakios_metal_texture_contents(v) == first);
    CHECK(allBytes(v, 0, 0, 255, 255));   // (the old contents: undefined to Metal, not zeroed with ZERO_KB 0)
    hakios_metal_cb* cb2 = hakios_metal_cb_new(q);
    hakios_metal_render_pass green = clearOf(v, 0, 1, 0, 1);
    CHECK_EQ(hakios_metal_cb_clear(cb2, &green), 0);
    hakios_metal_cb_commit(cb2, nullptr, nullptr);
    hakios_metal_cb_wait(cb2);
    CHECK(allBytes(v, 0, 255, 0, 255));
    hakios_metal_cb_free(cb2);
    hakios_metal_texture_free(v);
    hakios_metal_texture_free(u);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// HM_RT_CACHE_ZERO_KB: a reused one that small comes back zeroed
TEST(rt_cache_zeroes_small_reused_ones_when_asked) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_rt_cache_set(d, 8ull << 20, 25, 16 << 10);
    hakios_metal_texture* t = rt(d, 64, 64);
    memset(hakios_metal_texture_contents(t), 0x5a, 64 * 64 * 4);
    hakios_metal_texture_free(t);
    t = rt(d, 64, 64);
    CHECK(allBytes(t, 0, 0, 0, 0));
    hakios_metal_texture_free(t);
    hakios_metal_close(d);
}

// past the limit the least recently released go; one bigger than the limit is not kept at all
TEST(rt_cache_evicts_the_least_recent_past_its_limit) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_rt_cache_set(d, 40 << 10, 0, 0);   // two 16 KB objects
    hakios_metal_texture* a = rt(d, 64, 64);
    hakios_metal_texture* b = rt(d, 64, 63);
    hakios_metal_texture* c = rt(d, 64, 62);
    void* pa = hakios_metal_texture_contents(a);
    const Stats s0;
    hakios_metal_texture_free(a);
    hakios_metal_texture_free(b);
    hakios_metal_texture_free(c);
    const Stats s1;
    CHECK_EQ(s1.entries(), 2ull);
    CHECK(s1.held() <= (40ull << 10));
    CHECK_EQ(s1.evicted() - s0.evicted(), 1ull);
    // a (released first) is the one gone: neither of the next two is it
    hakios_metal_texture* x = rt(d, 64, 64);
    hakios_metal_texture* y = rt(d, 64, 64);
    CHECK(hakios_metal_texture_contents(x) != pa);
    CHECK(hakios_metal_texture_contents(y) != pa);
    CHECK_EQ(Stats().hits() - s1.hits(), 2ull);
    hakios_metal_texture_free(x);
    hakios_metal_texture_free(y);
    // 128x128 (64 KB) is bigger than the whole cache
    hakios_metal_texture_free(rt(d, 128, 128));
    const Stats s2;
    CHECK_EQ(s2.s[7] - s1.s[7], 1ull);
    CHECK_EQ(s2.entries(), 2ull);
    hakios_metal_close(d);
}

// a forced trim (hakios_metal_pool_trim(1): what HM_POOL_SOFT_MB and a refused allocation do) lets all of it go
TEST(rt_cache_is_let_go_by_a_forced_trim) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_rt_cache_set(d, 8ull << 20, 25, 0);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_texture_free(rt(d, 64, 64));
    hakios_metal_texture_free(rt(d, 256, 256));
    CHECK_EQ(Stats().entries(), 2ull);
    const unsigned long long freed = hakios_metal_pool_trim(1);
    CHECK(freed >= (256ull * 256 * 4 + 64 * 64 * 4));
    CHECK_EQ(Stats().entries(), 0ull);
    CHECK_EQ(Stats().held(), 0ull);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

namespace {
// the fake behind a pool of BUDGET bytes: GEM_NEW past it refused with ENOMEM, as the kernel's pool refuses
class Budget : public Transport {
public:
    Budget(std::unique_ptr<FakeGpu> f, uint64_t budget) : f_(std::move(f)), budget_(budget) {}
    int call(uint32_t request, void* data, size_t size) override {
        if (request == kGemNew) {
            auto* n = static_cast<MsmGemNew*>(data);
            if (used_ + n->size > budget_) {
                refused++;
                return ENOMEM;
            }
            const int e = f_->call(request, data, size);
            if (e == 0) {
                sizes_[n->handle] = n->size;
                used_ += n->size;
            }
            return e;
        }
        if (request == kGemClose) {
            auto* c = static_cast<DrmGemClose*>(data);
            auto it = sizes_.find(c->handle);
            if (it != sizes_.end()) {
                used_ -= it->second;
                sizes_.erase(it);
            }
        }
        return f_->call(request, data, size);
    }
    void* map(uint32_t handle, uint64_t size) override { return f_->map(handle, size); }
    void unmap(uint32_t handle, void* address) override { f_->unmap(handle, address); }
    int64_t nowNs() override { return f_->nowNs(); }
    int refused = 0;

private:
    std::unique_ptr<FakeGpu> f_;
    uint64_t budget_, used_ = 0;
    std::map<uint32_t, uint64_t> sizes_;
};
}  // namespace

// the kernel's pool short: what the cache holds is given back, and the allocation asked for once more -- it succeeds
// where without the cache's memory it would not
TEST(rt_cache_is_given_back_when_an_allocation_is_refused) {
    auto t = std::make_unique<Budget>(std::make_unique<FakeGpu>(), 1ull << 20);
    Budget* budget = t.get();
    Gpu gpu(std::move(t));
    gpu.setBoCache(Gpu::BoCacheConfig{8ull << 20, 25, 0});
    std::string why;
    TextureDesc desc;
    desc.format = PixelFormat::kBGRA8Unorm;
    desc.width = 256;
    desc.height = 256;   // 256 KB
    desc.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    {
        std::shared_ptr<Texture> a = Texture::make(gpu, desc, &why), b = Texture::make(gpu, desc, &why), c = Texture::make(gpu, desc, &why);
        CHECK(a != nullptr && b != nullptr && c != nullptr);
    }
    CHECK_EQ(gpu.cacheStats().entries, 3ull);   // 768 KB held of the 1 MB
    // 512 KB of something else: refused at first, then made once the cache is let go
    std::shared_ptr<Buffer> other = newBuffer(gpu, 512 << 10, &why);
    CHECK(other != nullptr);
    CHECK(budget->refused >= 1);
    CHECK_EQ(gpu.cacheStats().retries, 1ull);
    CHECK_EQ(gpu.cacheStats().entries, 0ull);
    CHECK_EQ(gpu.cacheStats().held, 0ull);
    other.reset();
}

// HM_POOL_KEEP_MB is read once, from the environment; with it 0 (the tests' environment) an idle trim keeps nothing
TEST(pool_keep_is_off_by_default) { CHECK_EQ(poolKeepBytes(), 0ull); }
