// mach/mach_time.h stub for host testing on Linux
#pragma once

#include <cstdint>
#include <chrono>

// Mach timebase info structure
struct mach_timebase_info_data_t {
    uint32_t numer;
    uint32_t denom;
};

// Stub implementation - returns 1:1 timebase (nanoseconds)
inline int mach_timebase_info(mach_timebase_info_data_t* info) {
    if (info) {
        info->numer = 1;
        info->denom = 1;
    }
    return 0;  // KERN_SUCCESS
}

// Stub implementation - returns steady clock time in nanoseconds
inline uint64_t mach_absolute_time() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}
