// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ResolvedTimingGeometry.hpp -- the single authority for host-facing timing
// and HAL geometry of one audio stream at one sample rate.
//
// Ownership model: documentation/TIMING_GEOMETRY_OWNERSHIP.md (FW-180).
//
//   wire facts        AmdtpRateGeometryForSampleRate(rate), AmdtpTransferDelayTicks
//   device policy     DeviceTimingPolicy (from the device profile, at the rate)
//   HAL policy        HalBufferProfileForRate(rate)
//        └───────────────┬─────────────────┘
//                ResolveTimingGeometry()  ->  ResolvedTimingGeometry
//
// Consumers (graph declarations, rate change, StartIO transfer delay, IO
// callback budget) read the resolved value. They do not re-derive any field
// from the rate, a profile, or a compile-time constant.
//
// Pure and host-testable: no DriverKit, no profile classes, no transport.

#pragma once

#include "../../Shared/Isoch/AudioHalBufferProfiles.hpp"
#include "../../Shared/Isoch/AudioTimingGeometry.hpp"
#include "../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../Wire/AMDTP/AmdtpTransferDelay.hpp"
#include "../Wire/AMDTP/AmdtpTypes.hpp"

#include <cstdint>
#include <expected>

namespace ASFW::Audio::Runtime {

/// What a device profile declares at one rate. Policy, not fact.
struct DeviceTimingPolicy final {
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};
};

enum class TimingGeometryError : uint8_t {
    kUnsupportedSampleRate = 1,  ///< no AMDTP rate geometry for the rate
    kInvalidHalProfile,          ///< zero or inconsistent HAL buffer geometry
    kInvalidSafetyOffset,        ///< zero output safety, or safety >= ring
    kExceedsAllocation,          ///< active ring larger than the shared allocation
};

[[nodiscard]] constexpr const char* TimingGeometryErrorName(TimingGeometryError e) noexcept {
    switch (e) {
        case TimingGeometryError::kUnsupportedSampleRate: return "unsupported-sample-rate";
        case TimingGeometryError::kInvalidHalProfile: return "invalid-hal-profile";
        case TimingGeometryError::kInvalidSafetyOffset: return "invalid-safety-offset";
        case TimingGeometryError::kExceedsAllocation: return "exceeds-allocation";
    }
    return "unknown";
}

struct ResolvedTimingGeometry final {
    // Wire facts, copied from AmdtpRateGeometryForSampleRate -- never re-derived.
    uint32_t sampleRateHz{0};
    uint8_t fdf{0};
    uint32_t sytIntervalFrames{0};

    // HAL buffer geometry. frameRingFrames is the ACTIVE ring at this rate (the
    // HAL wraps on the ZTS period); allocatedFrameRingFrames is the fixed
    // shared-memory size it lives in.
    uint32_t frameRingFrames{0};
    uint32_t allocatedFrameRingFrames{0};
    uint32_t zeroTimestampPeriodFrames{0};
    uint32_t clientIoBudgetFrames{0};

    // Declarations published to the HAL.
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};

    // Diagnostics for the resolution log line: the profile's own input safety
    // and the completion-batch floor it was raised to.
    uint32_t profileInputSafetyFrames{0};
    uint32_t inputSafetyFloorFrames{0};

    // IEC 61883-6 presentation delay applied on the wire, 24.576 MHz ticks.
    uint32_t rxTransferDelayTicks{0};
    uint32_t txTransferDelayTicks{0};

    friend constexpr bool operator==(const ResolvedTimingGeometry&,
                                     const ResolvedTimingGeometry&) = default;
};

/// Transfer delay applied on the wire (decision D1, TIMING_GEOMETRY_MIDI_
/// DISTILLATION.md): the Linux-derived formula in blocking mode for every
/// stream, as the midi branch applies it. 12800 ticks at 48/96/192 kHz,
/// 13162 in the 44.1 kHz family, 14848 at 32 kHz.
[[nodiscard]] constexpr uint32_t AppliedTransferDelayTicks(
    const Encoding::AmdtpRateGeometry& geometry) noexcept {
    return Encoding::AmdtpTransferDelayTicks(geometry, Encoding::StreamMode::kBlocking);
}

/// Frames carried by the largest possible completion group at this rate: the
/// most DATA packets a timing group can hold, times the SYT interval. A capture
/// reader can lag the writer by one whole group between completions, so the
/// input safety offset must cover it. 48 frames at 48 kHz, 96 at 96 kHz.
[[nodiscard]] constexpr uint32_t CompletionBatchFrames(
    const Encoding::AmdtpRateGeometry& geometry) noexcept {
    return Encoding::MaxBlockingDataPacketsInCycles(
               IsochTransport::AudioTimingGeometry::kTimingGroupPackets, geometry) *
           geometry.sytIntervalFrames;
}

/// Input safety: the data-visibility margin only (CoreAudio accounts for the
/// IO buffer separately). The larger of the profile's value and one completion
/// batch -- a reader can lag the writer by one whole group between
/// completions, so no declaration may cover less (decision D3). No jitter term
/// and no grid alignment: a hardware-calibrated profile value (Saffire RX
/// safety, 80 frames at 48 kHz) must stand exactly as declared.
[[nodiscard]] constexpr uint32_t ResolveInputSafetyFrames(
    uint32_t profileInputSafetyFrames, const Encoding::AmdtpRateGeometry& geometry) noexcept {
    const uint32_t batch = CompletionBatchFrames(geometry);
    return profileInputSafetyFrames > batch ? profileInputSafetyFrames : batch;
}

[[nodiscard]] constexpr std::expected<ResolvedTimingGeometry, TimingGeometryError>
ResolveTimingGeometry(uint32_t sampleRateHz,
                      Encoding::StreamMode streamMode,
                      const DeviceTimingPolicy& policy) noexcept {
    const auto wire = Encoding::AmdtpRateGeometryForSampleRate(sampleRateHz);
    if (!wire) {
        return std::unexpected(TimingGeometryError::kUnsupportedSampleRate);
    }

    const auto hal = IsochTransport::HalBufferProfileForRate(sampleRateHz);
    if (!IsochTransport::IsValidAudioHalBufferProfile(hal)) {
        return std::unexpected(TimingGeometryError::kInvalidHalProfile);
    }
    if (!IsochTransport::ProfileFitsAllocation(hal)) {
        return std::unexpected(TimingGeometryError::kExceedsAllocation);
    }

    const uint32_t floor = ResolveInputSafetyFrames(0, *wire);
    const uint32_t inputSafety = ResolveInputSafetyFrames(policy.inputSafetyOffsetFrames, *wire);
    if (policy.outputSafetyOffsetFrames == 0 ||
        policy.outputSafetyOffsetFrames >= hal.frameRingFrames ||
        inputSafety >= hal.frameRingFrames) {
        return std::unexpected(TimingGeometryError::kInvalidSafetyOffset);
    }

    // Stream mode does not change the applied delay (D1: blocking formula for
    // every stream, as midi does); kept as an input for the multi-rate epic.
    (void)streamMode;
    const uint32_t transferDelay = AppliedTransferDelayTicks(*wire);
    return ResolvedTimingGeometry{
        .sampleRateHz = sampleRateHz,
        .fdf = wire->fdf,
        .sytIntervalFrames = wire->sytIntervalFrames,
        .frameRingFrames = hal.frameRingFrames,
        .allocatedFrameRingFrames = IsochTransport::kAllocatedFrameRingFrames,
        .zeroTimestampPeriodFrames = hal.zeroTimestampPeriodFrames,
        .clientIoBudgetFrames = hal.clientIoBudgetFrames,
        .outputLatencyFrames = policy.outputLatencyFrames,
        .inputLatencyFrames = policy.inputLatencyFrames,
        .outputSafetyOffsetFrames = policy.outputSafetyOffsetFrames,
        .inputSafetyOffsetFrames = inputSafety,
        .profileInputSafetyFrames = policy.inputSafetyOffsetFrames,
        .inputSafetyFloorFrames = floor,
        .rxTransferDelayTicks = transferDelay,
        .txTransferDelayTicks = transferDelay,
    };
}

} // namespace ASFW::Audio::Runtime
