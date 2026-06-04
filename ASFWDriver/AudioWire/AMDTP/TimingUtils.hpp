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
// AMDTP SYT presentation-timestamp helpers
//-----------------------------------------------------------------------------

/// 24.576 MHz ticks per audio sample at 48 kHz.
constexpr uint32_t kTicksPerSample48k = 512;

/// IEC 61883-6 48 kHz blocking-mode SYT interval, in data blocks.
constexpr uint32_t kSytInterval48k = 8;

/// Nominal 48 kHz blocking-mode SYT step: 8 samples * 512 ticks/sample.
constexpr uint32_t kSytPacketStepTicks48k = kTicksPerSample48k * kSytInterval48k;

/// 8-second 24.576 MHz offset domain used for shortest-path phase deltas.
constexpr int64_t kEightSecondTicks = int64_t(8) * int64_t(kTicksPerSecond);

/// 16-cycle SYT field domain: [cycle4:offset12].
constexpr int64_t kSytFieldDomainTicks = int64_t(16) * int64_t(kTicksPerCycle);

struct CycleTimerFields {
    uint32_t seconds{0};
    uint32_t cycle{0};
    uint32_t offset{0};
};

/// Collapse a FireWire (seconds, cycles, offset) timestamp to 24.576 MHz ticks.
[[nodiscard]] inline int64_t tstampToOffsets(uint32_t seconds,
                                             uint32_t cycle,
                                             uint32_t offset) noexcept {
    return int64_t(kTicksPerCycle) *
           (int64_t(cycle) + int64_t(kCyclesPerSecond) * int64_t(seconds)) +
           int64_t(offset);
}

/// Reconstruct a full offset-domain timestamp from a base cycle and 16-bit SYT.
[[nodiscard]] inline int64_t extendTstamp(uint32_t baseCycle, uint16_t syt) noexcept {
    const uint32_t sytCycle4 = (uint32_t(syt) >> 12) & 0x0Fu;
    const uint32_t sytOffset = uint32_t(syt) & 0x0FFFu;
    uint32_t cycle = (baseCycle & ~0x0Fu) | sytCycle4;
    if (cycle < baseCycle) {
        cycle += 16;
    }
    return tstampToOffsets(0, cycle, sytOffset);
}

/// Signed shortest-path delta (a - b) in the 8-second offset domain.
[[nodiscard]] inline int64_t extOffsetDiff(int64_t a, int64_t b) noexcept {
    int64_t d = (a - b) % kEightSecondTicks;
    if (d < 0) {
        d += kEightSecondTicks;
    }
    if (d > (kEightSecondTicks / 2)) {
        d -= kEightSecondTicks;
    }
    return d;
}

/// Normalize a 24.576 MHz tick value to the 8-second offset domain.
[[nodiscard]] inline int64_t normalizeOffsetDomain(int64_t ticks) noexcept {
    ticks %= kEightSecondTicks;
    if (ticks < 0) {
        ticks += kEightSecondTicks;
    }
    return ticks;
}

/// Collapse the 16-bit SYT [cycle4:offset12] field to its 16-cycle tick index.
[[nodiscard]] inline int64_t sytToFieldTicks(uint16_t syt) noexcept {
    return int64_t((uint32_t(syt) >> 12) & 0x0Fu) * int64_t(kTicksPerCycle) +
           int64_t(uint32_t(syt) & 0x0FFFu);
}

/// Signed delta between consecutive 16-bit SYT values in offset ticks.
[[nodiscard]] inline int64_t SYTDiffInOffsets(uint16_t sytNew, uint16_t sytOld) noexcept {
    int64_t d = (sytToFieldTicks(sytNew) - sytToFieldTicks(sytOld)) % kSytFieldDomainTicks;
    if (d < 0) {
        d += kSytFieldDomainTicks;
    }
    if (d > (kSytFieldDomainTicks / 2)) {
        d -= kSytFieldDomainTicks;
    }
    return d;
}

//-----------------------------------------------------------------------------
// Cycle timer register field extraction
//-----------------------------------------------------------------------------

/// Masks for 32-bit OHCI cycle timer register
constexpr uint32_t kCycleTimerSecondsMask = 0xFE000000;  // bits 31:25
constexpr uint32_t kCycleTimerSecondsShift = 25;
constexpr uint32_t kCycleTimerCyclesMask = 0x01FFF000;   // bits 24:12
constexpr uint32_t kCycleTimerCyclesShift = 12;
constexpr uint32_t kCycleTimerOffsetMask = 0x00000FFF;   // bits 11:0

[[nodiscard]] inline CycleTimerFields decodeCycleTimer(uint32_t cycleTimer) noexcept {
    return CycleTimerFields{
        .seconds = (cycleTimer & kCycleTimerSecondsMask) >> kCycleTimerSecondsShift,
        .cycle = (cycleTimer & kCycleTimerCyclesMask) >> kCycleTimerCyclesShift,
        .offset = cycleTimer & kCycleTimerOffsetMask,
    };
}

/// Collapse an already-decoded FireWire cycle timer to the 24.576 MHz offset domain.
[[nodiscard]] inline int64_t tstampToOffsets(CycleTimerFields timestamp) noexcept {
    return tstampToOffsets(timestamp.seconds, timestamp.cycle, timestamp.offset);
}

/// Collapse an encoded OHCI cycle timer value to the 24.576 MHz offset domain.
[[nodiscard]] inline int64_t encodedTstampToOffsets(uint32_t cycleTimer) noexcept {
    return tstampToOffsets(decodeCycleTimer(cycleTimer));
}

/// Reconstruct a full offset-domain presentation timestamp from an encoded OHCI
/// cycle timer and a 16-bit SYT. This is the full-cycle-timer counterpart to the
/// cycle-only `extendTstamp()`: it preserves the seconds field and carries across
/// the 8000-cycle boundary when the SYT's low cycle nibble lands in the next
/// 16-cycle window.
[[nodiscard]] inline int64_t extendTstampFromCycleTimer(uint32_t baseCycleTimer,
                                                        uint16_t syt) noexcept {
    const CycleTimerFields base = decodeCycleTimer(baseCycleTimer);
    const uint32_t sytCycle4 = (uint32_t(syt) >> 12) & 0x0Fu;
    const uint32_t sytOffset = uint32_t(syt) & 0x0FFFu;

    uint32_t seconds = base.seconds;
    uint32_t cycle = (base.cycle & ~0x0Fu) | sytCycle4;
    if (cycle < base.cycle) {
        cycle += 16;
        if (cycle >= kCyclesPerSecond) {
            cycle -= kCyclesPerSecond;
            seconds = (seconds + 1) & 0x7Fu;
        }
    }

    return tstampToOffsets(seconds, cycle, sytOffset);
}

//-----------------------------------------------------------------------------
// Conversion functions
//-----------------------------------------------------------------------------

/// Convert 32-bit FireWire cycle timer to nanoseconds
[[nodiscard]] inline uint64_t encodedFWTimeToNanos(uint32_t cycleTimer) noexcept {
    const CycleTimerFields t = decodeCycleTimer(cycleTimer);
    
    // Total time in nanoseconds
    uint64_t ns = uint64_t(t.seconds) * kNanosPerSecond;
    ns += uint64_t(t.cycle) * kNanosPerCycle;
    ns += (uint64_t(t.offset) * kNanosPerCycle) / kTicksPerCycle;
    
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
