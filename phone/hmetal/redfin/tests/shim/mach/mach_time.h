// (tests) mach_continuous_time as CLOCK_MONOTONIC in nanoseconds (timebase 1/1)
#pragma once
#include <cstdint>
#include <ctime>
struct mach_timebase_info_data_t { uint32_t numer, denom; };
inline int mach_timebase_info(mach_timebase_info_data_t* t) { t->numer = 1; t->denom = 1; return 0; }
inline uint64_t mach_continuous_time() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
