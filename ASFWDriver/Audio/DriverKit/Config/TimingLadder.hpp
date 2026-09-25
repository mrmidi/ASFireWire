// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// TimingLadder.hpp -- the Saffire-style declaration ladder, written once.
//
// Several DICE profiles declare safety offsets as "delay packets x frames per
// packet" and latency as 29/59/119 frames by rate tier. Each used to carry its
// own copy of the 8/16/32 frames-per-packet ladder keyed on `rate > 48000.0`.
// Frames per packet is a wire fact (the SYT interval), so it is read from
// AmdtpRateGeometryForSampleRate here; only the delay-packet counts and the
// latency ladder are device policy (documentation/TIMING_GEOMETRY_OWNERSHIP.md,
// row G-02).
//
// For an AMDTP rate this reproduces the old per-profile ladders exactly (the
// SYT interval is 8/16/32 for the 1x/2x/4x tiers of both rate families). For a
// rate with no wire geometry it returns 0, which the timing resolver rejects;
// the old ladders silently answered with the 1x values.

#pragma once

#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <cstdint>

namespace ASFW::Isoch::Audio::TimingLadder {

[[nodiscard]] constexpr uint32_t FramesPerPacket(double sampleRateHz) noexcept {
    const auto geometry =
        Encoding::AmdtpRateGeometryForSampleRate(static_cast<uint32_t>(sampleRateHz));
    return geometry ? geometry->sytIntervalFrames : 0;
}

/// 0 at 1x (8 frames per packet), 1 at 2x (16), 2 at 4x (32).
[[nodiscard]] constexpr uint32_t RateTier(double sampleRateHz) noexcept {
    const uint32_t fpp = FramesPerPacket(sampleRateHz);
    return fpp >= 32 ? 2U : (fpp >= 16 ? 1U : 0U);
}

enum class RateAddend : uint8_t {
    kNone,      ///< delayPackets x fpp at every rate (Weiss INT)
    kPerTier,   ///< (delayPackets + 2 x tier) x fpp (Saffire latencyMode 1)
};

[[nodiscard]] constexpr uint32_t SafetyOffsetFrames(uint32_t delayPackets,
                                                    double sampleRateHz,
                                                    RateAddend addend) noexcept {
    const uint32_t extra = addend == RateAddend::kPerTier ? 2U * RateTier(sampleRateHz) : 0U;
    return (delayPackets + extra) * FramesPerPacket(sampleRateHz);
}

/// Reported presentation latency of the Saffire kext ladder.
[[nodiscard]] constexpr uint32_t ReportedLatencyFrames(double sampleRateHz) noexcept {
    if (FramesPerPacket(sampleRateHz) == 0) {
        return 0;
    }
    constexpr uint32_t kByTier[] = {29, 59, 119};
    return kByTier[RateTier(sampleRateHz)];
}

/// Delay-packet counts of the Saffire ladder (latencyMode 1).
inline constexpr uint32_t kTxDelayPackets = 6;
inline constexpr uint32_t kRxDelayPackets = 16;

static_assert(SafetyOffsetFrames(kTxDelayPackets, 48000.0, RateAddend::kPerTier) == 48);
static_assert(SafetyOffsetFrames(kRxDelayPackets, 48000.0, RateAddend::kPerTier) == 128);
static_assert(SafetyOffsetFrames(kTxDelayPackets, 96000.0, RateAddend::kPerTier) == 128);
static_assert(SafetyOffsetFrames(kRxDelayPackets, 192000.0, RateAddend::kPerTier) == 640);
static_assert(ReportedLatencyFrames(48000.0) == 29);
static_assert(ReportedLatencyFrames(88200.0) == 59);
static_assert(ReportedLatencyFrames(176400.0) == 119);

} // namespace ASFW::Isoch::Audio::TimingLadder
