// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AmdtpTiming.hpp
// ASFWDriver
//
// AMDTP/audio-specific presentation timestamp timing helpers.
//

#pragma once

#include "../../../Common/TimingUtils.hpp"

namespace ASFW::Timing {

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

/// Signed shortest-path delta (a - b) in the 1-second (8000 cycle) offset domain.
[[nodiscard]] inline int64_t extOffsetDiff1s(int64_t a, int64_t b) noexcept {
    constexpr int64_t kOneSecondTicks = int64_t(kTicksPerSecond);
    int64_t d = (a - b) % kOneSecondTicks;
    if (d < 0) {
        d += kOneSecondTicks;
    }
    if (d > (kOneSecondTicks / 2)) {
        d -= kOneSecondTicks;
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

} // namespace ASFW::Timing
