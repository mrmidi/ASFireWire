// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Read-only, value-owned audio telemetry contract.  This is deliberately not a
// view of AudioTransportControlBlock: callers receive copied atomics while the
// endpoint still owns the direct-audio mapping.

#pragma once

#include "../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../Shared/Isoch/AudioTimingGeometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ASFW::Audio::Runtime {

constexpr uint32_t kAudioTelemetryWireVersion = 1;
constexpr uint32_t kAudioTelemetryMaxEndpoints = 8;

enum AudioTelemetryFlags : uint32_t {
    kAudioTelemetryBindingReady = 1U << 0,
    kAudioTelemetryStreaming = 1U << 1,
    kAudioTelemetryHasCompletedInterval = 1U << 2,
};

struct AudioTelemetryEndpointSnapshot final {
    uint64_t guid{0};
    uint64_t endpointGeneration{0};
    uint64_t controlGeneration{0};
    uint64_t completedIntervalSequence{0};
    uint64_t lastPreparationLatencyTicks{0};
    uint64_t completedIntervalMaxLatencyTicks{0};
    uint64_t maxPreparationLatencyTicks{0};
    uint64_t preparationWakeCount{0};
    uint64_t preparationAtMost750Us{0};
    uint64_t preparationAtLeast1500Us{0};
    uint64_t rxReplayEntries{0};
    uint64_t rxReplayEpochResets{0};
    std::array<uint64_t,
               IsochTransport::AudioTimingGeometry::
                   kTxPreparationLatencyHistogramBuckets>
        completedLatencyHistogram{};
    std::array<uint64_t,
               IsochTransport::AudioTimingGeometry::
                   kTxCommittedMarginHistogramBuckets>
        completedMarginHistogram{};
    uint32_t flags{0};
    uint32_t sampleRateHz{0};
    uint32_t outputChannels{0};
    uint32_t inputChannels{0};
    uint32_t currentCommittedMarginPackets{0};
    uint32_t completedIntervalMarginMinPackets{std::numeric_limits<uint32_t>::max()};
    uint32_t completedIntervalMarginMaxPackets{0};
    uint32_t minimumCommittedMarginPackets{std::numeric_limits<uint32_t>::max()};
    uint32_t preparationLeadPackets{0};
    uint32_t hardwareFloorPackets{0};
};

static_assert(sizeof(AudioTelemetryEndpointSnapshot) == 224);

struct AudioTelemetrySnapshot final {
    uint32_t version{kAudioTelemetryWireVersion};
    uint32_t endpointCount{0};
    std::array<AudioTelemetryEndpointSnapshot, kAudioTelemetryMaxEndpoints> endpoints{};
};

static_assert(sizeof(AudioTelemetrySnapshot) == 1800);

inline void CopyAudioTelemetrySnapshot(
    const AudioTransportControlBlock& control,
    AudioTelemetryEndpointSnapshot& out) noexcept {
    constexpr auto memoryOrder = std::memory_order_relaxed;
    out.controlGeneration = control.generation.load(memoryOrder);
    out.lastPreparationLatencyTicks =
        control.txLastPreparationLatencyTicks.load(memoryOrder);
    out.maxPreparationLatencyTicks =
        control.txMaxPreparationLatencyTicks.load(memoryOrder);
    out.preparationWakeCount = control.txPreparationLatencySamples.load(memoryOrder);
    out.preparationAtMost750Us = control.txPreparationAtMost750Us.load(memoryOrder);
    out.preparationAtLeast1500Us = control.txPreparationAtLeast1500Us.load(memoryOrder);
    out.rxReplayEntries = control.rxReplayEntries.load(memoryOrder);
    out.rxReplayEpochResets = control.rxReplayEpochResets.load(memoryOrder);
    out.currentCommittedMarginPackets =
        control.txCurrentCommittedMarginPackets.load(memoryOrder);
    out.minimumCommittedMarginPackets =
        control.txMinimumCommittedMarginPackets.load(memoryOrder);

    // The heartbeat writer brackets this completed interval with an even
    // sequence. Retry a few times rather than blocking a real-time producer.
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = control.txCompletedIntervalSequence.load(memoryOrder);
        if ((before & 1U) != 0U) {
            continue;
        }
        out.completedIntervalMarginMinPackets =
            control.txCompletedIntervalMarginMinPackets.load(memoryOrder);
        out.completedIntervalMarginMaxPackets =
            control.txCompletedIntervalMarginMaxPackets.load(memoryOrder);
        out.completedIntervalMaxLatencyTicks =
            control.txCompletedIntervalPreparationLatencyMaxTicks.load(memoryOrder);
        for (size_t index = 0; index < out.completedLatencyHistogram.size(); ++index) {
            out.completedLatencyHistogram[index] =
                control.txCompletedIntervalPreparationLatencyHistogram[index].load(memoryOrder);
        }
        for (size_t index = 0; index < out.completedMarginHistogram.size(); ++index) {
            out.completedMarginHistogram[index] =
                control.txCompletedIntervalCommittedMarginHistogram[index].load(memoryOrder);
        }
        const uint64_t after = control.txCompletedIntervalSequence.load(memoryOrder);
        if (before == after && (after & 1U) == 0U) {
            out.completedIntervalSequence = after;
            if (after != 0) {
                out.flags |= kAudioTelemetryHasCompletedInterval;
            }
            return;
        }
    }
}

} // namespace ASFW::Audio::Runtime
