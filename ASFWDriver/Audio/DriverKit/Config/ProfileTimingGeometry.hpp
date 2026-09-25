// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ProfileTimingGeometry.hpp -- the one bridge from a device profile to the
// timing resolver (documentation/TIMING_GEOMETRY_OWNERSHIP.md).
//
// The audio driver resolves its profile once (graph construction) and calls
// this at graph time and on every accepted rate change. Nothing else asks the
// profile for timing, and nothing else calls ResolveTimingGeometry with
// profile-derived inputs.

#pragma once

#include "AudioDriverConfig.hpp"
#include "IAudioDeviceProfile.hpp"
#include "../../Runtime/ResolvedTimingGeometry.hpp"

#include <cstdint>
#include <expected>
#include <utility>

namespace ASFW::Audio::DriverKit {

[[nodiscard]] inline Runtime::DeviceTimingPolicy TimingPolicyFromProfile(
    const Isoch::Audio::IAudioDeviceProfile& profile, uint32_t sampleRateHz) noexcept {
    const double rate = sampleRateHz;
    return Runtime::DeviceTimingPolicy{
        .outputLatencyFrames = profile.TxReportedLatencyFrames(rate),
        .inputLatencyFrames = profile.RxReportedLatencyFrames(rate),
        .outputSafetyOffsetFrames = profile.TxSafetyOffsetFrames(rate),
        .inputSafetyOffsetFrames = profile.RxSafetyOffsetFrames(rate),
    };
}

/// The nub carries the stream mode as the DriverKit config enum's raw value.
/// Map by name, never by numeric cast: the Encoding enum orders them
/// differently in other modules (see AmdtpTypes.hpp).
[[nodiscard]] constexpr Encoding::StreamMode WireStreamModeFromRaw(uint32_t raw) noexcept {
    return raw == std::to_underlying(Isoch::Audio::StreamMode::kBlocking)
               ? Encoding::StreamMode::kBlocking
               : Encoding::StreamMode::kNonBlocking;
}

[[nodiscard]] inline std::expected<Runtime::ResolvedTimingGeometry, Runtime::TimingGeometryError>
ResolveProfileTimingGeometry(const Isoch::Audio::IAudioDeviceProfile& profile,
                             uint32_t sampleRateHz,
                             uint32_t streamModeRaw) noexcept {
    return Runtime::ResolveTimingGeometry(sampleRateHz, WireStreamModeFromRaw(streamModeRaw),
                                          TimingPolicyFromProfile(profile, sampleRateHz));
}

} // namespace ASFW::Audio::DriverKit
