//
// TimingUtils.hpp
// ASFWDriver
//
// FireWire ↔ Host time conversion utilities per IEC 61883-6
// Adapted from FWAIsoch/include/utils/TimingUtils.hpp
//

#pragma once

#include <cstdint>
#include <DriverKit/IOLib.h>  // Provides mach_absolute_time, mach_timebase_info in DriverKit

namespace ASFW::Timing {

//-----------------------------------------------------------------------------
// Host timebase (macOS only)
//-----------------------------------------------------------------------------

/// Cached mach timebase info for host ↔ nanoseconds conversion
inline mach_timebase_info_data_t gHostTimebaseInfo = {0, 0}; // NOSONAR(cpp:S5421): intentionally mutable — populated once by initializeHostTimebase()

/// Initialize host timebase (call once at driver start)
[[nodiscard]] inline bool initializeHostTimebase() noexcept {
    if (gHostTimebaseInfo.denom == 0) {
        kern_return_t kr = mach_timebase_info(&gHostTimebaseInfo);
        return (kr == KERN_SUCCESS && gHostTimebaseInfo.denom != 0);
    }
    return true;
}

//-----------------------------------------------------------------------------
// FireWire timing constants (IEC 61883-6)
//-----------------------------------------------------------------------------

constexpr uint32_t kTicksPerCycle = 3072;         // 24.576 MHz / 8000 Hz
constexpr uint32_t kCyclesPerSecond = 8000;
constexpr uint64_t kTicksPerSecond = 24'576'000ULL;
constexpr uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr uint64_t kNanosPerCycle = 125'000ULL;   // 125 µs per cycle

/// 128-second wrap period for FireWire cycle timer
constexpr uint32_t kFWTimeWrapSeconds = 128;
constexpr int64_t kFWTimeWrapNanos = int64_t(kFWTimeWrapSeconds) * int64_t(kNanosPerSecond);

/// Transfer delay per IEC 61883-6 §7.3 (matches Linux TRANSFER_DELAY_TICKS)
constexpr uint32_t kTransferDelayTicks = 0x2E00;  // ~479 µs
constexpr uint64_t kTransferDelayNanos = 
    (uint64_t(kTransferDelayTicks) * kNanosPerCycle) / kTicksPerCycle;

//-----------------------------------------------------------------------------
// Cycle timer register field extraction
//-----------------------------------------------------------------------------

/// Masks for 32-bit OHCI cycle timer register
constexpr uint32_t kCycleTimerSecondsMask = 0xFE000000;  // bits 31:25
constexpr uint32_t kCycleTimerSecondsShift = 25;
constexpr uint32_t kCycleTimerCyclesMask = 0x01FFF000;   // bits 24:12
constexpr uint32_t kCycleTimerCyclesShift = 12;
constexpr uint32_t kCycleTimerOffsetMask = 0x00000FFF;   // bits 11:0

//-----------------------------------------------------------------------------
// Conversion functions
//-----------------------------------------------------------------------------

/// Convert 32-bit FireWire cycle timer to nanoseconds
[[nodiscard]] inline uint64_t encodedFWTimeToNanos(uint32_t cycleTimer) noexcept {
    uint32_t sec = (cycleTimer & kCycleTimerSecondsMask) >> kCycleTimerSecondsShift;
    uint32_t cyc = (cycleTimer & kCycleTimerCyclesMask) >> kCycleTimerCyclesShift;
    uint32_t off = cycleTimer & kCycleTimerOffsetMask;
    
    // Total time in nanoseconds
    uint64_t ns = uint64_t(sec) * kNanosPerSecond;
    ns += uint64_t(cyc) * kNanosPerCycle;
    ns += (uint64_t(off) * kNanosPerCycle) / kTicksPerCycle;
    
    return ns;
}

/// Convert nanoseconds to 32-bit FireWire cycle timer format
[[nodiscard]] inline uint32_t nanosToEncodedFWTime(uint64_t nanos) noexcept {
    // Wrap to [0, 128s)
    nanos %= (kFWTimeWrapSeconds * kNanosPerSecond);
    
    uint32_t sec = static_cast<uint32_t>(nanos / kNanosPerSecond) & 0x7F;
    uint64_t remNs = nanos % kNanosPerSecond;
    
    uint32_t cyc = static_cast<uint32_t>(remNs / kNanosPerCycle);
    uint64_t offsetNs = remNs % kNanosPerCycle;
    uint32_t off = static_cast<uint32_t>((offsetNs * kTicksPerCycle) / kNanosPerCycle);
    
    return (sec << kCycleTimerSecondsShift) 
         | (cyc << kCycleTimerCyclesShift) 
         | off;
}

/// Convert mach_absolute_time ticks to nanoseconds
[[nodiscard]] inline uint64_t hostTicksToNanos(uint64_t ticks) noexcept {
    if (gHostTimebaseInfo.denom == 0) return 0;
    
    // ticks * numer / denom
    // Use 128-bit multiplication to avoid overflow on long uptimes
    __uint128_t tmp = __uint128_t(ticks) * gHostTimebaseInfo.numer;
    return static_cast<uint64_t>(tmp / gHostTimebaseInfo.denom);
}

/// Convert nanoseconds to mach_absolute_time ticks
[[nodiscard]] inline uint64_t nanosToHostTicks(uint64_t nanos) noexcept {
    if (gHostTimebaseInfo.numer == 0) return 0;
    
    __uint128_t tmp = __uint128_t(nanos) * gHostTimebaseInfo.denom;
    return static_cast<uint64_t>(tmp / gHostTimebaseInfo.numer);
}

/// Signed delta between two FireWire times (handles 128s wrap)
[[nodiscard]] inline int64_t deltaFWTimeNanos(uint32_t a, uint32_t b) noexcept {
    int64_t na = static_cast<int64_t>(encodedFWTimeToNanos(a));
    int64_t nb = static_cast<int64_t>(encodedFWTimeToNanos(b));
    int64_t d = na - nb;
    
    // If delta > 64s, wrap around (shortest path)
    constexpr int64_t halfWrap = kFWTimeWrapNanos / 2;
    if (d > halfWrap) d -= kFWTimeWrapNanos;
    if (d < -halfWrap) d += kFWTimeWrapNanos;
    
    return d;
}

/// Normalize nanoseconds to [0, 128s) handling negative values
[[nodiscard]] inline uint64_t normalizeToFWTimeRange(int64_t nanos) noexcept {
    // Handle negative values with proper modulo
    int64_t normalized = ((nanos % kFWTimeWrapNanos) + kFWTimeWrapNanos) % kFWTimeWrapNanos;
    return static_cast<uint64_t>(normalized);
}

} // namespace ASFW::Timing
