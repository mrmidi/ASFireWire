// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// HalTimingProjection.hpp
// ASFWDriver
//
// Unified pure HAL timing projection (latency, safety offsets, ZTS period)
// used across graph construction and dynamic rate changes.

#pragma once

#include "IAudioDeviceProfile.hpp"
#include "../../Shared/AudioGeometryResolver.hpp"
#include "../../Shared/AudioTimingGeometry.hpp"
#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <cstdint>
#include <optional>

namespace ASFW::Audio::DriverKit {

struct HalTimingProjection final {
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};
    uint32_t zeroTimestampPeriodFrames{0};
    uint32_t rawProfileOutputSafetyFrames{0};
    Shared::ResolvedAudioGeometry resolvedGeometry{};
};

[[nodiscard]] inline std::optional<HalTimingProjection> DeriveHalTimingProjection(
    const Isoch::Audio::IAudioDeviceProfile& profile,
    const double sampleRateHz,
    uint32_t inputChannels = 0,
    uint32_t outputChannels = 0,
    const Shared::AudioRuntimeTuning* tuningRequest = nullptr,
    const Shared::DirectAudioAllocationLimits* allocationLimits = nullptr,
    uint64_t topologyRevision = 0) noexcept {
    if (sampleRateHz <= 0.0) {
        return std::nullopt;
    }

    const uint32_t rateInt = static_cast<uint32_t>(sampleRateHz);
    const uint32_t inCh = inputChannels != 0 ? inputChannels : profile.RxChannelCount();
    const uint32_t outCh = outputChannels != 0 ? outputChannels : profile.TxChannelCount();

    const auto packetGeometry =
        Encoding::AmdtpRateGeometryForSampleRate(rateInt);
    const uint8_t fdf = packetGeometry ? packetGeometry->fdf : 0;
    const uint8_t syt = packetGeometry ? packetGeometry->sytIntervalFrames : 0;

    const Shared::AudioDeviceTimingPolicy timingPolicy{
        .txReportedLatencyFrames = profile.TxReportedLatencyFrames(sampleRateHz),
        .rxReportedLatencyFrames = profile.RxReportedLatencyFrames(sampleRateHz),
        .txSafetyOffsetFrames = profile.TxSafetyOffsetFrames(sampleRateHz),
        .rxSafetyOffsetFrames = profile.RxSafetyOffsetFrames(sampleRateHz),
        .txTransferDelayTicks = profile.TxTransferDelayTicks(sampleRateHz),
        .rxTransferDelayTicks = profile.RxTransferDelayTicks(sampleRateHz),
    };

    const Shared::AudioDeviceFormation formation{
        .sampleRateHz = rateInt,
        .inputChannels = inCh,
        .outputChannels = outCh,
        .fdf = fdf,
        .sytIntervalFrames = syt,
    };

    Shared::DirectAudioAllocationLimits defaultLimits{};
    if (allocationLimits) {
        defaultLimits = *allocationLimits;
    }

    const auto resolved = Shared::ResolveAudioGeometry(
        formation, timingPolicy, tuningRequest, defaultLimits, topologyRevision);
    if (!resolved) {
        return std::nullopt;
    }

    return HalTimingProjection{
        .outputLatencyFrames = resolved->outputLatencyFrames,
        .inputLatencyFrames = resolved->inputLatencyFrames,
        .outputSafetyOffsetFrames = resolved->outputSafetyOffsetFrames,
        .inputSafetyOffsetFrames = resolved->inputSafetyOffsetFrames,
        .zeroTimestampPeriodFrames = resolved->zeroTimestampPeriodFrames,
        .rawProfileOutputSafetyFrames = timingPolicy.txSafetyOffsetFrames,
        .resolvedGeometry = *resolved,
    };
}

} // namespace ASFW::Audio::DriverKit
