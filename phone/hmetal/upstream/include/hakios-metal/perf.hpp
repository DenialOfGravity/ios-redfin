// (ws-hm-perf) HM_PERF: profiling events for the Perfetto timeline, as kdebug events (class 0x21 DBG_APPS, subclass
// 0xA6) that land in sweettrace's recordings beside the kernel's own. Default off: HM_PERF unset or 0 changes nothing
// (no stamp, no event, no extra GPU command). Bits (HM_PERF=0x0f for all):
//   1  timestamps: CP_ALWAYS_ON_COUNTER (19.2 MHz) at the start and end of every Metal encoder (the end after a wait
//      for idle), HM_ENC / HM_LABEL / HM_CB on completion, HM_CAL at each GPU clock calibration, and the plug-in's
//      GPUStartTime / GPUEndTime (and kernelStart/EndTime) from the command buffer's own stamps
//   2  render-pass decisions: HM_PASS as each draw pass closes (sysmem or GMEM, bins, samples, size, attachments)
//   4  shader stats: HM_SHADER for every stage or kernel compileStage / compileAir hand out (ir3's numbers when
//      compiled now, the Kernel's when read from the memo or the cache)
//   8  counter snapshots: the 64-bit counters kern.sweet_gpuperf_regs lists, read beside each encoder's stamps
//      (CP_REG_TO_MEM), HM_CTR with their deltas; a missing or empty sysctl reads no counter
// Header-only, as trace.hpp: every binary the core links into has it.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/sysctl.h>
extern "C" int kdebug_trace(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);
#endif

namespace hakios::metal::perf {

enum : unsigned { kTimestamps = 1, kPasses = 2, kShaders = 4, kCounters = 8 };
// the event codes (debugid = 0x21 << 24 | 0xA6 << 16 | code << 2, DBG_FUNC_NONE)
enum : uint32_t { kEnc = 1, kLabel = 2, kCb = 3, kPass = 4, kShader = 5, kCtr = 6, kCal = 7 };
constexpr uint32_t kClass = 0x21, kSubclass = 0xA6;
constexpr uint32_t kMaxCounters = 15;

inline unsigned bits() {
    static const unsigned b = [] {
        const char* e = getenv("HM_PERF");
        return e != nullptr && e[0] ? unsigned(strtoul(e, nullptr, 0)) : 0u;
    }();
    return b;
}
inline bool on(unsigned bit) { return (bits() & bit) != 0; }

// the host tests' sink (every event also goes here when set)
using Sink = void (*)(uint32_t code, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
inline std::atomic<Sink>& sink() {
    static std::atomic<Sink> s{nullptr};
    return s;
}
inline void emit(uint32_t code, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    if (Sink s = sink().load(std::memory_order_acquire)) s(code, a1, a2, a3, a4);
#if defined(__APPLE__)
    (void)kdebug_trace((kClass << 24) | (kSubclass << 16) | ((code & 0x3fffu) << 2), a1, a2, a3, a4);
#endif
}

// mach_absolute_time (0 off Darwin)
inline uint64_t machNow() {
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    return 0;
#endif
}

inline uint32_t fnv1a(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ uint8_t(s[i])) * 16777619u;
    return h;
}
// FNV-1a 32 of LABEL (0 for none or ""), with HM_LABEL the first time this process sees the hash: the hash, then the
// label's first 24 bytes packed little-endian into three args
inline uint32_t labelHash(const char* label) {
    if (label == nullptr || label[0] == '\0') return 0;
    const size_t n = strlen(label);
    const uint32_t h = fnv1a(label, n);
    static std::mutex lock;
    static std::unordered_set<uint32_t> seen;
    {
        std::lock_guard<std::mutex> g(lock);
        if (!seen.insert(h).second) return h;
    }
    uint8_t b[24] = {};
    memcpy(b, label, n < sizeof b ? n : sizeof b);
    uint64_t a[3];
    for (int i = 0; i < 3; i++) {
        a[i] = 0;
        for (int j = 7; j >= 0; j--) a[i] = a[i] << 8 | b[8 * i + j];
    }
    emit(kLabel, h, a[0], a[1], a[2]);
    return h;
}
inline uint32_t labelHash(const std::string& label) { return labelHash(label.c_str()); }

// a command buffer's sequence number in this process (1, 2, ...; 0 is none)
inline uint32_t nextSeq() {
    static std::atomic<uint32_t> seq{0};
    uint32_t s = seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (s == 0) s = seq.fetch_add(1, std::memory_order_relaxed) + 1;
    return s;
}

// "GROUP:COUNTABLE@slot=0x<LO> ..." -> the LO registers, in order (at most kMaxCounters; a token without "=" skipped)
inline std::vector<uint32_t> parseRegs(const char* text) {
    std::vector<uint32_t> out;
    if (text == nullptr) return out;
    const char* p = text;
    while (*p != '\0' && out.size() < kMaxCounters) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',' || *p == ';') p++;
        if (*p == '\0') break;
        const char* end = p;
        while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n' && *end != ',' && *end != ';') end++;
        for (const char* q = p; q < end; q++) {
            if (*q != '=') continue;
            char* stop = nullptr;
            const unsigned long v = strtoul(q + 1, &stop, 0);
            if (stop != q + 1 && v != 0 && v < 0x40000) out.push_back(uint32_t(v));
            break;
        }
        p = end;
    }
    return out;
}
// kern.sweet_gpuperf_regs, read again at most once a second (a kernel patch fills it; missing or empty: none)
inline std::vector<uint32_t> counterRegs() {
    static std::mutex lock;
    static std::vector<uint32_t> regs;
    static int64_t readAt = 0;
    static bool read = false;
    const int64_t now = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard<std::mutex> g(lock);
    if (!read || now - readAt >= 1000) {
        read = true;
        readAt = now;
#if defined(__APPLE__)
        char text[1024];
        size_t len = sizeof text - 1;
        if (sysctlbyname("kern.sweet_gpuperf_regs", text, &len, nullptr, 0) == 0 && len < sizeof text) {
            text[len] = '\0';
            regs = parseRegs(text);
        } else {
            regs.clear();
        }
#else
        regs = parseRegs(getenv("HM_PERF_REGS_FOR_TESTING"));
#endif
    }
    return regs;
}

}  // namespace hakios::metal::perf
