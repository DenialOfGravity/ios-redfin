// (redfin ws-hm-defer) deferred recording (hakios_metal_queue_set_deferred2) through the C interface, on the fake GPU:
// the orders the plug-in relies on when HM_DEFER moves CoreAnimation's encoding onto the queue's own thread.
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hakios-metal/hakios_metal.h"
#include "test.hpp"

namespace {
hakios_metal_render_pass clearOf(hakios_metal_texture* rt, double r, double g, double b, double a) {
    hakios_metal_render_pass p{};
    p.color0 = rt;
    p.load_action = 2;   // Clear
    p.store_action = 1;  // Store
    p.clear_color[0] = r, p.clear_color[1] = g, p.clear_color[2] = b, p.clear_color[3] = a;
    p.clear_depth = 1.0;
    return p;
}

bool everywhere(hakios_metal_texture* rt, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* bytes = static_cast<const uint8_t*>(hakios_metal_texture_contents(rt));
    const uint64_t pitch = hakios_metal_texture_bytes_per_row(rt);
    for (uint32_t y = 0; y < hakios_metal_texture_height(rt); y++)
        for (uint32_t x = 0; x < hakios_metal_texture_width(rt); x++) {
            const uint8_t* px = bytes + y * pitch + x * 4;
            if (px[0] != r || px[1] != g || px[2] != b || px[3] != a) return false;
        }
    return true;
}

struct Log {
    std::mutex m;
    std::vector<std::string> events;
    hakios_metal_texture* rt = nullptr;
    void add(const std::string& e) {
        std::lock_guard<std::mutex> g(m);
        events.push_back(e);
    }
};

struct Done {
    Log* log;
    const char* name;
    hakios_metal_texture* own = nullptr;   // the buffer's own target, read in its callback (else the log's)
};

// a completion callback: what the GPU wrote is there when it runs (the plug-in's write-backs and handlers rely on it)
void completed(hakios_metal_cb* cb, void* context) {
    Done* d = static_cast<Done*>(context);
    hakios_metal_texture* rt = d->own != nullptr ? d->own : d->log->rt;
    const char* colour = rt == nullptr ? "" : everywhere(rt, 0, 255, 0, 255) ? " green" : everywhere(rt, 255, 0, 0, 255) ? " red" : " other";
    d->log->add(std::string("done ") + d->name + (hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED ? " ok" : " error") + colour);
}

struct Posted {
    Log* log;
    hakios_metal_cb* cb;
    const char* name;
};

// a hakios_metal_cb_post callback (the plug-in's "sent" mark after a deferred commit): runs after the commit's submit
void posted(void* context) {
    Posted* p = static_cast<Posted*>(context);
    char thread[32] = "";
    pthread_getname_np(pthread_self(), thread, sizeof thread);
    const int s = hakios_metal_cb_status(p->cb);   // (on the thread itself: no wait)
    p->log->add(std::string("posted ") + p->name + (s >= HAKIOS_METAL_COMMITTED ? " sent" : " NOT SENT") + " on " + thread);
}

std::atomic<int> gNotices{0};
std::string gLastNotice;
void notice(const char* text) {
    gLastNotice = text;
    gNotices++;
}

void freeItself(hakios_metal_cb* cb, void* context) {
    hakios_metal_cb_free(cb);   // the last reference, let go inside its own callback -- on the deferred thread when refused
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
}
}  // namespace

// commit, then waitUntilCompleted: the wait covers the deferred jobs (the clear, the commit) and the callback
TEST(defer_commit_then_wait) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_queue_set_deferred2(q, 1, "hakios.metal.defer.test");
    CHECK_EQ(hakios_metal_queue_deferred(q), 1);
    Log log;
    log.rt = rt;
    memset(hakios_metal_texture_contents(rt), 0xa5, hakios_metal_texture_bytes_per_row(rt) * 64);
    for (int round = 0; round < 20; round++) {
        hakios_metal_cb* cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_deferred(cb), 1);
        hakios_metal_render_pass green = clearOf(rt, 0, 1, 0, 1);
        CHECK_EQ(hakios_metal_cb_clear(cb, &green), 0);
        Done done{&log, "a"};
        hakios_metal_cb_commit(cb, completed, &done);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        CHECK_EQ(hakios_metal_cb_pending(cb), 0);
        CHECK(everywhere(rt, 0, 255, 0, 255));
        {
            std::lock_guard<std::mutex> g(log.m);
            CHECK_EQ(log.events.size(), size_t(1));   // the callback has run by the time the wait returns
            if (!log.events.empty()) CHECK(log.events[0] == "done a ok green");
            log.events.clear();
        }
        int64_t work = 0;
        unsigned jobs = 0, depth = 0;
        CHECK_EQ(hakios_metal_cb_defer_stats(cb, &work, &jobs, &depth), 0);
        CHECK_EQ(jobs, 2u);   // the clear, the commit
        CHECK(depth >= 1u);
        CHECK(work > 0);
        hakios_metal_cb_free(cb);
        memset(hakios_metal_texture_contents(rt), 0xa5, hakios_metal_texture_bytes_per_row(rt) * 64);
    }
    // the thread is named, and follows the scheduling of the thread posting to it
    char name[64] = "";
    int policy = -2, priority = -2, qos = -2, applied = -2;
    CHECK_EQ(hakios_metal_queue_defer_thread(q, name, sizeof name, &policy, &priority, &qos, &applied), 0);
    CHECK(strcmp(name, "hakios.metal.defer.test") == 0);
    {
        int myPolicy = -1;
        sched_param p{};
        pthread_getschedparam(pthread_self(), &myPolicy, &p);
        CHECK_EQ(policy, myPolicy);
        CHECK_EQ(priority, p.sched_priority);
        CHECK(applied != 0);   // taken (or refused by the system: a host without the privilege), not left pending
    }
    hakios_metal_queue_free(q);
    hakios_metal_texture_free(rt);
    hakios_metal_close(d);
}

// a completion handler after the GPU's writes; a hakios_metal_cb_post mark after the commit's submit, on the named thread,
// before the wait for completion is needed; the callbacks of two buffers in their commit order; the second's clear last
TEST(defer_order_of_two_buffers_and_their_handlers) {
    hakios_metal_device* d = hakios_metal_open_fake();
    const unsigned rt = HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1;
    hakios_metal_texture* shared = hakios_metal_texture_new(d, 2, 70, 32, 32, rt);
    hakios_metal_texture* ownA = hakios_metal_texture_new(d, 2, 70, 32, 32, rt);
    hakios_metal_texture* ownB = hakios_metal_texture_new(d, 2, 70, 32, 32, rt);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_queue_set_deferred2(q, 1, nullptr);
    Log log;
    for (int round = 0; round < 30; round++) {
        hakios_metal_cb* a = hakios_metal_cb_new(q);
        hakios_metal_cb* b = hakios_metal_cb_new(q);
        hakios_metal_render_pass red = clearOf(shared, 1, 0, 0, 1), green = clearOf(shared, 0, 1, 0, 1);
        hakios_metal_render_pass redA = clearOf(ownA, 1, 0, 0, 1), greenB = clearOf(ownB, 0, 1, 0, 1);
        memset(hakios_metal_texture_contents(ownA), 0xa5, hakios_metal_texture_bytes_per_row(ownA) * 32);
        memset(hakios_metal_texture_contents(ownB), 0xa5, hakios_metal_texture_bytes_per_row(ownB) * 32);
        // B's work is told first, A is committed first: the queue's order is the commit order, as Metal's
        for (int i = 0; i < 8; i++) CHECK_EQ(hakios_metal_cb_clear(b, &green), 0);
        CHECK_EQ(hakios_metal_cb_clear(b, &greenB), 0);
        for (int i = 0; i < 8; i++) CHECK_EQ(hakios_metal_cb_clear(a, &red), 0);
        CHECK_EQ(hakios_metal_cb_clear(a, &redA), 0);
        Done da{&log, "a", ownA}, db{&log, "b", ownB};
        Posted pa{&log, a, "a"}, pb{&log, b, "b"};
        hakios_metal_cb_commit(a, completed, &da);
        hakios_metal_cb_post(a, posted, &pa);
        hakios_metal_cb_commit(b, completed, &db);
        hakios_metal_cb_post(b, posted, &pb);
        hakios_metal_cb_wait(b);
        hakios_metal_cb_wait(a);
        CHECK(everywhere(shared, 0, 255, 0, 255));   // B ran last
        {
            std::lock_guard<std::mutex> g(log.m);
            std::vector<std::string> done, sent;
            for (const auto& e : log.events) (e.rfind("done", 0) == 0 ? done : sent).push_back(e);
            CHECK_EQ(done.size(), size_t(2));
            CHECK_EQ(sent.size(), size_t(2));
            if (done.size() == 2) {
                CHECK(done[0] == "done a ok red");     // A's callback first, A's writes there when it runs
                CHECK(done[1] == "done b ok green");
            }
            if (sent.size() == 2) {
                CHECK(sent[0] == "posted a sent on hakios.metal.de");   // (Linux keeps 15 bytes of the name)
                CHECK(sent[1] == "posted b sent on hakios.metal.de");
            }
            log.events.clear();
        }
        hakios_metal_cb_free(a);
        hakios_metal_cb_free(b);
    }
    hakios_metal_queue_free(q);
    hakios_metal_texture_free(shared);
    hakios_metal_texture_free(ownA);
    hakios_metal_texture_free(ownB);
    hakios_metal_close(d);
}

// a query after deferred work waits for it (and only for that buffer's); a copy after deferred work is recorded after it
TEST(defer_query_after_deferred_work) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 16, 16, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    hakios_metal_buffer* src = hakios_metal_buffer_new(d, 4096);
    hakios_metal_buffer* dst = hakios_metal_buffer_new(d, 4096);
    memset(hakios_metal_buffer_contents(src), 0x5a, 4096);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_queue_set_deferred2(q, 1, nullptr);
    unsigned long long c0[6], c1[6];
    hakios_metal_defer_counts(c0);
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    hakios_metal_render_pass green = clearOf(rt, 0, 1, 0, 1);
    for (int i = 0; i < 5; i++) CHECK_EQ(hakios_metal_cb_clear(cb, &green), 0);
    unsigned passes = 0, draws = 0, dispatches = 0, copies = 0, clears = 0;
    hakios_metal_cb_counts(cb, &passes, &draws, &dispatches, &copies, &clears);   // waits for the five
    CHECK_EQ(clears, 5u);
    CHECK_EQ(hakios_metal_cb_pending(cb), 0);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_NOT_ENQUEUED);   // told, not committed
    CHECK_EQ(hakios_metal_cb_copy(cb, src, 0, dst, 0, 4096), 0);       // recorded here, after the clears
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    CHECK_EQ(static_cast<uint8_t*>(hakios_metal_buffer_contents(dst))[4095], 0x5a);
    CHECK(everywhere(rt, 0, 255, 0, 255));
    hakios_metal_cb_counts(cb, &passes, &draws, &dispatches, &copies, &clears);
    CHECK_EQ(clears, 5u);
    hakios_metal_cb_free(cb);
    hakios_metal_defer_counts(c1);
    CHECK(c1[1] - c0[1] >= 6);   // five clears and the commit run on the thread
    CHECK(c1[4] - c0[4] >= 6);
    CHECK(c1[0] > c0[0]);
    hakios_metal_queue_free(q);
    hakios_metal_buffer_free(src);
    hakios_metal_buffer_free(dst);
    hakios_metal_texture_free(rt);
    hakios_metal_close(d);
}

// a clear refused on the deferred thread ends its buffer in error with the reason (and says it through the notice sink);
// a buffer refused there and freed inside its own callback, on that thread, leaves nothing behind; the queue goes on
TEST(defer_refusal_and_free_on_the_thread) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_set_notice(notice);
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 16, 16, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_queue_set_deferred2(q, 1, nullptr);
    hakios_metal_render_pass load = clearOf(rt, 0, 0, 0, 0);
    load.load_action = 1;   // a pass with nothing drawn that does not clear: refused
    const int n0 = gNotices.load();
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_clear(cb, &load), 0);   // told: the refusal comes on the thread
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_ERROR);
    CHECK(hakios_metal_cb_error(cb)[0] != '\0');
    CHECK(gNotices.load() > n0);
    CHECK(gLastNotice.find("refused on the deferred thread") != std::string::npos);
    hakios_metal_cb_free(cb);
    std::atomic<int> freed{0};
    for (int i = 0; i < 40; i++) {
        hakios_metal_cb* c = hakios_metal_cb_new(q);
        if (i % 2) hakios_metal_cb_fail(c, "refused by the test");   // finished inside the commit, on the thread
        else hakios_metal_cb_clear(c, &load);
        hakios_metal_cb_commit(c, freeItself, &freed);
    }
    hakios_metal_cb* last = hakios_metal_cb_new(q);
    hakios_metal_render_pass green = clearOf(rt, 0, 1, 0, 1);
    hakios_metal_cb_clear(last, &green);
    hakios_metal_cb_commit(last, nullptr, nullptr);
    hakios_metal_cb_wait(last);
    CHECK(hakios_metal_cb_status(last) == HAKIOS_METAL_COMPLETED);
    CHECK(everywhere(rt, 0, 255, 0, 255));
    hakios_metal_cb_free(last);
    CHECK_EQ(freed.load(), 40);
    hakios_metal_set_notice(nullptr);
    hakios_metal_queue_free(q);   // the thread drains and ends
    hakios_metal_texture_free(rt);
    hakios_metal_close(d);
}

// off (the default): hakios_metal_cb_post runs at once, nothing is pending, the stats say "not deferred"
TEST(defer_off_is_as_before) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    CHECK_EQ(hakios_metal_queue_deferred(q), 0);
    CHECK_EQ(hakios_metal_queue_defer_thread(q, nullptr, 0, nullptr, nullptr, nullptr, nullptr), -1);
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_deferred(cb), 0);
    Log log;
    Posted p{&log, cb, "x"};
    hakios_metal_cb_post(cb, posted, &p);
    CHECK_EQ(log.events.size(), size_t(1));   // here and now (before the commit: "NOT SENT")
    if (!log.events.empty()) CHECK(log.events[0].rfind("posted x NOT SENT", 0) == 0);
    CHECK_EQ(hakios_metal_cb_pending(cb), 0);
    CHECK_EQ(hakios_metal_cb_defer_stats(cb, nullptr, nullptr, nullptr), -1);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    hakios_metal_cb_free(cb);
    // and switched on and off again with no buffer being recorded
    hakios_metal_queue_set_deferred2(q, 1, "x");
    CHECK_EQ(hakios_metal_queue_deferred(q), 1);
    hakios_metal_queue_set_deferred2(q, 0, nullptr);
    CHECK_EQ(hakios_metal_queue_deferred(q), 0);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}
