//
// Signposts.hpp
// ASFWDriver
//
// Timing utilities for performance measurement
// Note: os_signpost is NOT available in DriverKit, using mach_absolute_time instead
//

#pragma once

#include <DriverKit/IOLib.h>
#include <cstdint>

namespace ASFW::Diagnostics {

/// Convert mach ticks to microseconds
inline uint64_t MachTicksToMicroseconds(uint64_t ticks) {
    static mach_timebase_info_data_t timebase{0, 0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    // ticks * numer / denom = nanoseconds; divide by 1000 for microseconds
    return (ticks * timebase.numer) / (timebase.denom * 1000);
}

/// RAII timer for measuring code section latency
class ScopedTimer {
public:
    explicit ScopedTimer(uint64_t& resultMicroseconds)
        : result_(resultMicroseconds), start_(mach_absolute_time()) {}
    
    ~ScopedTimer() {
        uint64_t elapsed = mach_absolute_time() - start_;
        result_ = MachTicksToMicroseconds(elapsed);
    }
    
    // Non-copyable
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    
private:
    uint64_t& result_;
    uint64_t start_;
};

/// Simple timer for manual start/stop
class ManualTimer {
public:
    void Start() { start_ = mach_absolute_time(); }
    
    uint64_t ElapsedMicroseconds() const {
        return MachTicksToMicroseconds(mach_absolute_time() - start_);
    }
    
private:
    uint64_t start_{0};
};

} // namespace ASFW::Diagnostics
