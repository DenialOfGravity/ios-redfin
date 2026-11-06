// (redfin) HM_PROF=N: cheap per-phase timers of the command buffer's way to the GPU and back, printed to stderr every
// N command buffers (N <= 1: 300) as "hm-prof: <phase> <calls> <us per call> <us per buffer>". Off (one relaxed load
// per timer) when HM_PROF is not set. Phases are counted wherever they run (the caller's thread for submit, the
// queue's waiter thread for wait/finish), so a "per buffer" figure is the phase's total over the buffers counted.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace hakios::metal::prof {

enum Phase : unsigned {
    kSubmit,        // CommandQueue::submit, whole (the core's part of -commit)
    kClosePass,     //   the last pass's tail (GMEM: the tiles are emitted here)
    kCommandBo,     //   the command BO taken (from the idle list or made)
    kCopyWords,     //   the stream copied into it
    kFaultsCommit,  //   GET_PARAM FAULTS at commit
    kGpuSubmit,     //   Gpu::submit (BO list + GEM_SUBMIT)
    kFlushBefore,   //     transport: dc civac of imported BOs before
    kIoSubmit,      //     transport: the Submit method (IOKit)
    kWaitIo,        // waiter: the WaitFence method (IOKit), idle time included
    kFlushAfter,    // waiter: dc civac of written imports after the fence
    kFaultsDone,    // waiter: GET_PARAM FAULTS after the fence
    kFinish,        // waiter: CommandBuffer::finish (the completion callback: the plug-in's handlers)
    kCount
};

inline const char* name(unsigned p) {
    static const char* const names[kCount] = {"submit", " closePass", " commandBo", " copyWords", " faultsAtCommit", " gpuSubmit",
                                              "  flushBefore", "  ioSubmit", "waitIo", "flushAfter", "faultsDone", "finish"};
    return p < kCount ? names[p] : "?";
}

struct State {
    std::atomic<uint64_t> ns[kCount]{}, calls[kCount]{};
    std::atomic<uint64_t> buffers{0}, bytesBefore{0}, bytesAfter{0}, skipsBefore{0};
};
inline State& state() {
    static State s;
    return s;
}
inline unsigned every() {
    static const unsigned n = [] {
        const char* e = getenv("HM_PROF");
        if (e == nullptr || *e == 0 || *e == '0') return 0u;
        const long v = strtol(e, nullptr, 10);
        return v > 1 ? unsigned(v) : 300u;
    }();
    return n;
}
inline bool on() { return every() != 0; }
inline uint64_t now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void add(unsigned p, uint64_t ns) {
    state().ns[p].fetch_add(ns, std::memory_order_relaxed);
    state().calls[p].fetch_add(1, std::memory_order_relaxed);
}
inline void bytes(bool before, uint64_t n, bool skipped) {
    if (!on()) return;
    if (skipped) state().skipsBefore.fetch_add(1, std::memory_order_relaxed);
    else (before ? state().bytesBefore : state().bytesAfter).fetch_add(n, std::memory_order_relaxed);
}
// one command buffer completed: every N, the table, then from zero again
inline void buffer() {
    if (!on()) return;
    State& s = state();
    const uint64_t b = s.buffers.fetch_add(1) + 1;
    if (b < every()) return;
    s.buffers.store(0);
    char line[2048];
    int n = snprintf(line, sizeof line, "hm-prof: %llu buffers; flushed before %.1f MB/buffer (%llu skipped by seed), after %.1f MB/buffer\n",
                     (unsigned long long)b, double(s.bytesBefore.exchange(0)) / 1e6 / double(b), (unsigned long long)s.skipsBefore.exchange(0),
                     double(s.bytesAfter.exchange(0)) / 1e6 / double(b));
    for (unsigned p = 0; p < kCount && n > 0 && n < int(sizeof line); p++) {
        const uint64_t ns = s.ns[p].exchange(0), c = s.calls[p].exchange(0);
        n += snprintf(line + n, sizeof line - size_t(n), "hm-prof:   %-16s %7llu calls %9.1f us/call %9.1f us/buffer\n", name(p),
                      (unsigned long long)c, c ? double(ns) / 1e3 / double(c) : 0.0, double(ns) / 1e3 / double(b));
    }
    fputs(line, stderr);
}
struct Scope {
    unsigned p;
    uint64_t t;
    explicit Scope(unsigned phase) : p(phase), t(on() ? now() : 0) {}
    ~Scope() {
        if (t != 0) add(p, now() - t);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace hakios::metal::prof
