// (redfin hm-trace) HM_TRACE's first-use events from the core: a stage compiled, read from the disk cache or a seed, a
// read mask taken or read back, a program linked or specialised, a large GEM object made, an IOSurface mapped, a texture
// made or uploaded. Nothing is kept here: each event goes to the sink the plug-in set (hakios_metal_trace_set_sink), and
// with no sink every call below is one relaxed load. Header-only, so that every binary the core links into (the tests,
// hm-seed, the probe, the plug-in) has it without a source file of its own.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace hakios {
namespace trace {

using Sink = void (*)(const char* kind, const char* what, int64_t startNs, int64_t durNs, unsigned long long bytes, void* ctx);

struct State {
    std::atomic<Sink> sink{nullptr};
    std::atomic<void*> ctx{nullptr};
};
inline State& state() {
    static State s;
    return s;
}
inline bool on() { return state().sink.load(std::memory_order_relaxed) != nullptr; }
// the clock every trace time is in: steady_clock (mach_absolute_time's on Darwin = CLOCK_UPTIME_RAW in ns)
inline int64_t nowNs() {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void event(const char* kind, const char* what, int64_t startNs, int64_t durNs, uint64_t bytes) {
    if (Sink s = state().sink.load(std::memory_order_acquire)) s(kind, what != nullptr ? what : "", startNs, durNs, bytes, state().ctx.load());
}
inline void event(const char* kind, const std::string& what, int64_t startNs, int64_t durNs, uint64_t bytes) {
    event(kind, what.c_str(), startNs, durNs, bytes);
}
// the size from which a GEM object, a texture or an upload is an event (HM_TRACE_ALLOC_KB, default 64 KB)
inline uint64_t bigBytes() {
    static const uint64_t kb = [] { const char* e = getenv("HM_TRACE_ALLOC_KB"); return e != nullptr && e[0] ? uint64_t(strtoull(e, nullptr, 10)) : 64ull; }();
    return kb << 10;
}

}  // namespace trace
}  // namespace hakios
