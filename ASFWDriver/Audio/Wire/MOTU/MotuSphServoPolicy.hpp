// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuSphServoPolicy.hpp - the production configuration of the MOTU SPH
// regulator, for each of the six MOTU sample rates.
//
// Every value below was chosen and measured at 48 kHz. The other rates keep
// the same decision time and the same step limits relative to their own
// nominal rate.
// assumed: that scaling. The regulator has been measured at 48 kHz only.

#pragma once

#include "../AMDTP/AmdtpRateGeometry.hpp"
#include "MotuSphClockServo.hpp"

#include <cstdint>
#include <optional>

namespace ASFW::Encoding::Motu {

inline constexpr uint32_t kSphServoReferenceRateHz = 48000;

// Decision interval at 48 kHz: 512 frames, about 10.7 ms.
// measured: every lock margin of the regulator was measured with decisions
// at least 512 frames apart (533-543 in practice) on an 828mk3 at 48 kHz.
// linux: FFADO likewise updates its transmit timestamp DLL once per client
// period rather than per packet (libstreaming/generic/StreamProcessor.cpp:235).
// vendor driver: the only correction interval visible on the official
// driver's wire is 4096 frames, at stream start; its steady-state interval
// cannot be seen there, so this value is ours.
inline constexpr uint32_t kSphServoDecisionFramesAt48k = 512;

// Step limits at 48 kHz, expressed as the sample rate the transmit clock may
// run at.
// vendor driver: the official driver's step never goes past 50160 Hz. At
// stream start it holds exactly that rate (3919.6172 ticks per 8-frame
// packet) for one 4096-frame window; seen on the wire in two captures of the
// official driver at 48 kHz.
// assumed: the 41895 Hz lower limit. Never seen on the wire.
inline constexpr uint32_t kSphServoMaximumTrackingRateAt48k = 50160;
inline constexpr uint32_t kSphServoMinimumTrackingRateAt48k = 41895;

// Locked phase gain 1/4 (a shift of 2).
// measured: on an 828mk3 at 48 kHz the standard deviation of the locked
// phase error fell from 2.99 to 1.28 ticks going from a gain of about 1 to 1/4.
// observed: on a bus capture of our stream at 48 kHz, after a +68-tick seed
// step the per-decision phase movements shrank by about 3/4 from one
// decision to the next.
// measured: replaying 2.3 h of recorded 828mk3 RX clock (48 kHz) through this
// loop puts 1/4 at the knee: from gain 1 the locked error falls from 1.05 to
// 0.70 ticks (sd); 1/8 removes only 0.04 more and doubles seed settling.
inline constexpr uint32_t kSphServoLockedPhaseGainShift = 2;

// Acquisition phase gain 1/D (a shift of 0): until the first lock the whole
// measured error is asked for over the next interval, so even a full-cycle
// error clears in one decision instead of fourteen at the locked gain.
// measured: on an 828mk3 at 48 kHz the output unmuted 6.07 ms after start.
// linux: FFADO also runs a fast loop until it has synchronised and a slow one
// afterwards (DLL bandwidth 5 Hz, then 0.1 Hz; config.h.in:148-151).
inline constexpr uint32_t kSphServoAcquisitionPhaseGainShift = 0;

// Frames per DATA packet in MOTU's blocking cadence: the SYT interval, 8, 16
// or 32 (linux: amdtp-motu.c:440 is CIP_BLOCKING; amdtp-stream.c:138-145).
// Zero for a rate MOTU does not carry.
[[nodiscard]] constexpr uint32_t SphServoFramesPerDataPacket(uint32_t sampleRateHz) noexcept {
    if (RateToIndex(sampleRateHz) < 0) {
        return 0;
    }
    const auto geometry = AmdtpRateGeometryForSampleRate(sampleRateHz);
    return geometry ? geometry->sytIntervalFrames : 0;
}

// The 48 kHz decision time at this rate, rounded to whole DATA packets: the
// received frame count advances a packet at a time, so a decision can only
// ever fall on a packet boundary anyway.
[[nodiscard]] constexpr uint64_t SphServoDecisionFrames(uint32_t sampleRateHz) noexcept {
    const uint64_t framesPerPacket = SphServoFramesPerDataPacket(sampleRateHz);
    if (framesPerPacket == 0) {
        return 0;
    }
    const uint64_t numerator = uint64_t{kSphServoDecisionFramesAt48k} * sampleRateHz;
    const uint64_t denominator = uint64_t{kSphServoReferenceRateHz} * framesPerPacket;
    const uint64_t packets = (numerator + denominator / 2) / denominator;
    return packets * framesPerPacket;
}

// The step, Q32.32 ticks per frame, for a transmit clock running at
// `trackingRateAt48k` scaled to this rate: rate * tracking / 48000 Hz. Exact
// in one division, so at 48 kHz it is the step for `trackingRateAt48k` itself.
[[nodiscard]] constexpr int64_t SphServoStepLimitQ32(uint32_t sampleRateHz,
                                                     uint32_t trackingRateAt48k) noexcept {
    if (sampleRateHz == 0 || trackingRateAt48k == 0) {
        return 0;
    }
    const __int128 numerator =
        (static_cast<__int128>(kTicksPerSecond) * kSphServoReferenceRateHz)
        << MotuSphClockServo::kFractionBits;
    const __int128 denominator = static_cast<__int128>(trackingRateAt48k) * sampleRateHz;
    return static_cast<int64_t>(numerator / denominator);
}

// The production regulator configuration for one rate; nullopt for a rate MOTU
// does not carry. The absolute-phase setpoint is a per-model value and is not
// set here.
[[nodiscard]] constexpr std::optional<MotuSphServoConfig> MotuSphServoConfigForRate(
    uint32_t sampleRateHz) noexcept {
    const uint64_t decisionFrames = SphServoDecisionFrames(sampleRateHz);
    if (decisionFrames == 0 || MotuSphClockServo::NominalStepQ32(sampleRateHz) <= 0) {
        return std::nullopt;
    }
    return MotuSphServoConfig{
        .sampleRateHz = sampleRateHz,
        .phaseCorrectionHorizonFrames = decisionFrames,
        .phaseGainShift = kSphServoLockedPhaseGainShift,
        .acquisitionPhaseGainShift = kSphServoAcquisitionPhaseGainShift,
        // The highest tracking rate is the smallest step, and vice versa.
        .minimumStepQ32 = SphServoStepLimitQ32(sampleRateHz, kSphServoMaximumTrackingRateAt48k),
        .maximumStepQ32 = SphServoStepLimitQ32(sampleRateHz, kSphServoMinimumTrackingRateAt48k),
    };
}

static_assert(SphServoDecisionFrames(48000) == kSphServoDecisionFramesAt48k);
static_assert(SphServoStepLimitQ32(48000, 48000) == MotuSphClockServo::NominalStepQ32(48000));

} // namespace ASFW::Encoding::Motu
