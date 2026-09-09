// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioGeometryResolver.hpp
// ASFWDriver
//
// Single pure geometry resolver for audio endpoints, rate changes, and runtime tuning.
// Free of DriverKit and transport dependencies.

#pragma once

#include "AudioGeometryPolicy.hpp"
#include "AudioHalBufferProfiles.hpp"
#include "AudioRuntimeTuning.hpp"
#include "AudioTimingGeometry.hpp"

#include <cstdint>
#include <expected>

namespace ASFW::Audio::Shared {

struct AudioDeviceTimingPolicy final {
    uint32_t txReportedLatencyFrames{0};
    uint32_t rxReportedLatencyFrames{0};
    uint32_t txSafetyOffsetFrames{0};
    uint32_t rxSafetyOffsetFrames{0};
    uint32_t txTransferDelayTicks{0};
    uint32_t rxTransferDelayTicks{0};
};

struct AudioDeviceFormation final {
    uint32_t sampleRateHz{0};
    uint32_t inputChannels{0};
    uint32_t outputChannels{0};
    uint8_t fdf{0};
    uint8_t sytIntervalFrames{0};
};

struct DirectAudioAllocationLimits final {
    uint64_t allocatedOutputBytes{0};
    uint64_t allocatedInputBytes{0};
    uint32_t maxOutputChannels{0};
    uint32_t maxInputChannels{0};
    uint32_t maxAllocatedFrames{0};
};

enum class GeometryError : uint8_t {
    None = 0,
    UnsupportedSampleRate,
    OutputBytesExceedAllocation,
    InputBytesExceedAllocation,
    FramesExceedAllocation,
    InvalidChannelCount,
    InvalidZtsPeriod,
    InvalidIoBudget,
    InvalidSafetyOffset,
};

struct ResolvedAudioGeometry final {
    // Identity & Revision
    uint32_t sampleRateHz{0};
    uint64_t topologyRevision{0};

    // Active HAL stream & memory geometry
    uint32_t activeOutputRingFrames{0};
    uint32_t activeInputRingFrames{0};
    uint32_t outputChannels{0};
    uint32_t inputChannels{0};
    uint64_t requiredOutputBytes{0};
    uint64_t requiredInputBytes{0};

    // HAL cadence & timing
    uint32_t zeroTimestampPeriodFrames{0};
    uint32_t clientIoBudgetFrames{0};

    // Cache retention (independent from active HAL ring)
    uint32_t pcmCacheCapacityFrames{0};

    // Effective Latency & Safety declarations
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};

    // Wire & Packet geometry
    uint8_t fdf{0};
    uint8_t sytIntervalFrames{0};
    uint32_t rxTransferDelayTicks{0};
    uint32_t txTransferDelayTicks{0};
};

[[nodiscard]] inline std::expected<ResolvedAudioGeometry, GeometryError>
ResolveAudioGeometry(
    const AudioDeviceFormation& formation,
    const AudioDeviceTimingPolicy& timingPolicy,
    const AudioRuntimeTuning* tuningRequest,
    const DirectAudioAllocationLimits& allocationLimits,
    uint64_t topologyRevision) noexcept {
    if (!AudioTimingGeometry::IsV3SampleRate(formation.sampleRateHz)) {
        return std::unexpected(GeometryError::UnsupportedSampleRate);
    }
    if (formation.inputChannels == 0 && formation.outputChannels == 0) {
        return std::unexpected(GeometryError::InvalidChannelCount);
    }

    const auto profile = AudioHalBufferProfileForRate(formation.sampleRateHz);
    const uint32_t activeRingFrames = AudioTimingGeometry::FrameRingFrames(formation.sampleRateHz);
    const uint32_t ztsPeriod = AudioTimingGeometry::ZeroTimestampPeriodFrames(formation.sampleRateHz);
    const uint32_t ioBudget = profile.clientIoBudgetFrames;

    if (allocationLimits.maxAllocatedFrames != 0 &&
        activeRingFrames > allocationLimits.maxAllocatedFrames) {
        return std::unexpected(GeometryError::FramesExceedAllocation);
    }

    const uint64_t reqOutBytes = static_cast<uint64_t>(activeRingFrames) *
        formation.outputChannels * sizeof(float);
    const uint64_t reqInBytes = static_cast<uint64_t>(activeRingFrames) *
        formation.inputChannels * sizeof(float);

    if (allocationLimits.allocatedOutputBytes != 0 &&
        reqOutBytes > allocationLimits.allocatedOutputBytes) {
        return std::unexpected(GeometryError::OutputBytesExceedAllocation);
    }
    if (allocationLimits.allocatedInputBytes != 0 &&
        reqInBytes > allocationLimits.allocatedInputBytes) {
        return std::unexpected(GeometryError::InputBytesExceedAllocation);
    }

    const uint32_t outSafety = AudioGeometryPolicy::RequiredOutputSafetyFrames(
        timingPolicy.txSafetyOffsetFrames, formation.sampleRateHz);
    if (outSafety == 0) {
        return std::unexpected(GeometryError::InvalidSafetyOffset);
    }

    const uint32_t pcmCacheFrames = (tuningRequest && tuningRequest->frameRingFrames != 0)
        ? tuningRequest->frameRingFrames
        : AudioTimingGeometry::PcmPublicationCacheFrames(formation.sampleRateHz);

    ResolvedAudioGeometry resolved{
        .sampleRateHz = formation.sampleRateHz,
        .topologyRevision = topologyRevision,
        .activeOutputRingFrames = activeRingFrames,
        .activeInputRingFrames = activeRingFrames,
        .outputChannels = formation.outputChannels,
        .inputChannels = formation.inputChannels,
        .requiredOutputBytes = reqOutBytes,
        .requiredInputBytes = reqInBytes,
        .zeroTimestampPeriodFrames = ztsPeriod,
        .clientIoBudgetFrames = ioBudget,
        .pcmCacheCapacityFrames = pcmCacheFrames,
        .outputLatencyFrames = timingPolicy.txReportedLatencyFrames,
        .inputLatencyFrames = timingPolicy.rxReportedLatencyFrames,
        .outputSafetyOffsetFrames = outSafety,
        .inputSafetyOffsetFrames = timingPolicy.rxSafetyOffsetFrames,
        .fdf = formation.fdf,
        .sytIntervalFrames = formation.sytIntervalFrames,
        .rxTransferDelayTicks = timingPolicy.rxTransferDelayTicks,
        .txTransferDelayTicks = timingPolicy.txTransferDelayTicks,
    };

    if (tuningRequest) {
        if (tuningRequest->outputLatencyFrames != 0) {
            resolved.outputLatencyFrames = tuningRequest->outputLatencyFrames;
        }
        if (tuningRequest->inputLatencyFrames != 0) {
            resolved.inputLatencyFrames = tuningRequest->inputLatencyFrames;
        }
        if (tuningRequest->outputSafetyOffsetFrames != 0) {
            resolved.outputSafetyOffsetFrames = tuningRequest->outputSafetyOffsetFrames;
        }
        if (tuningRequest->inputSafetyOffsetFrames != 0) {
            resolved.inputSafetyOffsetFrames = tuningRequest->inputSafetyOffsetFrames;
        }
    }

    return resolved;
}

} // namespace ASFW::Audio::Shared
