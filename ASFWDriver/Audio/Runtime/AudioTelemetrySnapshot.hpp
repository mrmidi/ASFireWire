// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Read-only, value-owned audio telemetry contract.  This is deliberately not a
// view of AudioTransportControlBlock: callers receive copied atomics while the
// endpoint still owns the direct-audio mapping.

#pragma once

#include "../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../Shared/AudioTimingGeometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ASFW::Audio::Runtime {

constexpr uint32_t kAudioTelemetryWireVersion = 5;
constexpr uint32_t kAudioTelemetryMaxEndpoints = 8;
constexpr uint16_t kAudioTelemetryHeaderBytes = 16;
constexpr uint16_t kAudioTelemetryEndpointBytes = 688;

enum AudioTelemetryFlags : uint32_t {
    kAudioTelemetryBindingReady = 1U << 0,
    kAudioTelemetryStreaming = 1U << 1,
    kAudioTelemetryHasCompletedInterval = 1U << 2,
    kAudioTelemetryHasCompletedRxInterval = 1U << 3,
    // CoreAudio issued BeginRead during the completed RX interval. Without a
    // reader, a full capture ring is intentionally only a retained window.
    kAudioTelemetryRxCaptureReaderActive = 1U << 4,
};

struct AudioTelemetryEndpointSnapshot final {
    uint16_t version{kAudioTelemetryWireVersion};
    uint16_t byteSize{kAudioTelemetryEndpointBytes};
    uint32_t _reserved{0};
    uint64_t endpointId{0};
    uint64_t deviceInstanceId{0};
    uint64_t observedGuid{0}; // diagnostics only
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
               Shared::AudioTimingGeometry::
                   kTxPreparationLatencyHistogramBuckets>
        completedLatencyHistogram{};
    std::array<uint64_t,
               Shared::AudioTimingGeometry::
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
    uint64_t rxCurrentAvailableFrames{0};
    uint64_t rxCompletedIntervalSequence{0};
    uint64_t rxCompletedIntervalMinimumAvailableFrames{std::numeric_limits<uint64_t>::max()};
    uint64_t rxCompletedIntervalMaximumAvailableFrames{0};
    uint64_t rxCompletedIntervalMinimumFreeHeadroomFrames{std::numeric_limits<uint64_t>::max()};
    uint64_t rxCompletedIntervalOverrunEvents{0};
    uint64_t rxCompletedIntervalOverwrittenFrames{0};
    uint64_t rxCompletedIntervalStarvationEvents{0};
    uint64_t rxCompletedIntervalStarvedFrames{0};
    uint64_t rxCaptureOverrunEvents{0};
    uint64_t rxCaptureStarvationEvents{0};
    uint64_t rxTotalOverwrittenFrames{0};
    uint64_t rxTotalStarvedFrames{0};
    std::array<uint64_t,
               Shared::AudioTimingGeometry::
                   kRxCaptureOccupancyHistogramBuckets>
        rxCompletedOccupancyHistogram{};
    uint32_t inputFrameCapacityFrames{0};
    // Wire v3. Appended at the tail deliberately: the app parses this struct by
    // fixed byte offset (DriverConnector+AudioTelemetry.swift), so new fields go
    // last or every existing offset shifts.
    //
    // Bring-up attribution; see AudioTransportControlBlock for how to read the
    // combination. These make a never-establishing stream explainable without a
    // packet analyser: all-zero means nothing arrived, seen == noData means the
    // device really is sending only CIP NO-DATA, and a non-zero reject counter
    // means ASFW rejected packets the device did send.
    uint64_t rxPacketsSeen{0};
    uint64_t rxDataPackets{0};
    uint64_t rxNoDataPackets{0};
    uint64_t rxShortPackets{0};
    uint64_t rxInvalidCipHeaders{0};
    uint64_t rxZeroDataBlockSize{0};
    uint64_t rxGeometryMismatch{0};

    // Wire v4 TX content-ownership attribution. These are copied values, never
    // cross-service views: W is the latest staged CoreAudio frame, F is the
    // immutable content frontier, and [completion, committed) is the neutral
    // transport-owned packet range. The first-fault tuple is latched so a
    // transient Heisenbug remains diagnosable after the live cursors move on.
    uint64_t txPlaybackWriteFrame{0};
    uint64_t txPlaybackOldestValidFrame{0};
    uint64_t txContentFinalizedFrameEnd{0};
    uint64_t txStagingOldestValidFrame{0};
    uint64_t txStagingWrittenEndFrame{0};
    uint64_t txTransportCompletionCursor{0};
    uint64_t txTransportCommittedEnd{0};
    uint64_t txStagingWrites{0};
    uint64_t txStagingFrames{0};
    uint64_t txStagingDiscontinuities{0};
    uint64_t txStagingOverwrittenFrames{0};
    uint64_t txStagingReadsReady{0};
    uint64_t txStagingReadsNotYetWritten{0};
    uint64_t txStagingReadsStaleOverwritten{0};
    uint64_t txStagingReadsSnapshotBusy{0};
    uint64_t txStagingReadsInvalid{0};
    uint64_t txContentDeferrals{0};
    uint64_t txContentDeadlineNoData{0};
    uint64_t txContentStaleXruns{0};
    uint64_t txContentRebases{0};
    uint64_t txContentFaultEvents{0};
    uint64_t txContentFirstFaultPacket{0};
    uint64_t txContentFirstFaultAudioFrame{0};
    uint64_t txContentFirstFaultOldestFrame{0};
    uint64_t txContentFirstFaultWrittenEndFrame{0};
    uint64_t txContentFirstFaultCompletionCursor{0};
    uint64_t txContentFirstFaultCommittedEnd{0};
    uint32_t txContentFirstFaultReason{0};
    uint32_t txTransportStatus{0};
    uint64_t rxEmptyCompletions{0};
};

static_assert(sizeof(AudioTelemetryEndpointSnapshot) == kAudioTelemetryEndpointBytes);
// Swift decodes this ABI by fixed offsets. Lock the v5 strong-identity prefix
// and the existing diagnostic tails so a harmless-looking insertion fails the
// driver build instead of silently relabelling MCP diagnostics.
static_assert(offsetof(AudioTelemetryEndpointSnapshot, endpointId) == 8);
static_assert(offsetof(AudioTelemetryEndpointSnapshot, deviceInstanceId) == 16);
static_assert(offsetof(AudioTelemetryEndpointSnapshot, observedGuid) == 24);
static_assert(offsetof(AudioTelemetryEndpointSnapshot, rxPacketsSeen) == 400);
static_assert(offsetof(AudioTelemetryEndpointSnapshot, txPlaybackWriteFrame) ==
              456);
static_assert(offsetof(AudioTelemetryEndpointSnapshot,
                       txContentFirstFaultReason) == 672);
static_assert(offsetof(AudioTelemetryEndpointSnapshot, rxEmptyCompletions) ==
              680);

struct AudioTelemetrySnapshot final {
    uint16_t version{kAudioTelemetryWireVersion};
    uint16_t headerSize{kAudioTelemetryHeaderBytes};
    uint32_t byteSize{kAudioTelemetryHeaderBytes +
                      kAudioTelemetryMaxEndpoints * kAudioTelemetryEndpointBytes};
    uint32_t endpointCount{0};
    uint32_t endpointRecordSize{kAudioTelemetryEndpointBytes};
    std::array<AudioTelemetryEndpointSnapshot, kAudioTelemetryMaxEndpoints> endpoints{};
};

static_assert(sizeof(AudioTelemetrySnapshot) ==
              kAudioTelemetryHeaderBytes +
                  kAudioTelemetryMaxEndpoints * kAudioTelemetryEndpointBytes);
static_assert(offsetof(AudioTelemetrySnapshot, endpoints) == kAudioTelemetryHeaderBytes);

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
    out.rxPacketsSeen = control.rxPacketsSeen.load(memoryOrder);
    out.rxDataPackets = control.rxDataPackets.load(memoryOrder);
    out.rxNoDataPackets = control.rxNoDataPackets.load(memoryOrder);
    out.rxShortPackets = control.rxShortPackets.load(memoryOrder);
    out.rxInvalidCipHeaders = control.rxInvalidCipHeaders.load(memoryOrder);
    out.rxZeroDataBlockSize = control.rxZeroDataBlockSize.load(memoryOrder);
    out.rxGeometryMismatch = control.rxGeometryMismatch.load(memoryOrder);
    out.txPlaybackWriteFrame =
        control.playbackRingWriteFrame.load(memoryOrder);
    out.txPlaybackOldestValidFrame =
        control.playbackRingOldestValidFrame.load(memoryOrder);
    out.txContentFinalizedFrameEnd =
        control.txContentFinalizedFrameEnd.load(memoryOrder);
    // writtenEndFrame is the release publication for the paired staging range.
    out.txStagingWrittenEndFrame =
        control.txPcmStagingTelemetry.writtenEndFrame.load(
            std::memory_order_acquire);
    out.txStagingOldestValidFrame =
        control.txPcmStagingTelemetry.oldestValidFrame.load(memoryOrder);
    out.txTransportCompletionCursor =
        control.txTransportCompletionCursor.load(memoryOrder);
    out.txTransportCommittedEnd =
        control.txTransportCommittedEnd.load(memoryOrder);
    out.txTransportStatus = control.txTransportStatus.load(memoryOrder);
    out.txStagingWrites =
        control.txPcmStagingTelemetry.writes.load(memoryOrder);
    out.txStagingFrames =
        control.txPcmStagingTelemetry.framesStaged.load(memoryOrder);
    out.txStagingDiscontinuities =
        control.txPcmStagingTelemetry.discontinuities.load(memoryOrder);
    out.txStagingOverwrittenFrames =
        control.txPcmStagingTelemetry.overwrittenFrames.load(memoryOrder);
    out.txStagingReadsReady =
        control.txPcmStagingTelemetry.readsReady.load(memoryOrder);
    out.txStagingReadsNotYetWritten =
        control.txPcmStagingTelemetry.readsNotYetWritten.load(memoryOrder);
    out.txStagingReadsStaleOverwritten =
        control.txPcmStagingTelemetry.readsStaleOverwritten.load(memoryOrder);
    out.txStagingReadsSnapshotBusy =
        control.txPcmStagingTelemetry.readsSnapshotBusy.load(memoryOrder);
    out.txStagingReadsInvalid =
        control.txPcmStagingTelemetry.readsInvalid.load(memoryOrder);
    out.txContentDeferrals = control.txContentDeferrals.load(memoryOrder);
    out.txContentDeadlineNoData =
        control.txContentDeadlineNoData.load(memoryOrder);
    out.txContentStaleXruns = control.txContentStaleXruns.load(memoryOrder);
    out.txContentRebases = control.txContentRebases.load(memoryOrder);
    out.txContentFaultEvents = control.txContentFaultEvents.load(memoryOrder);
    out.txContentFirstFaultPacket =
        control.txContentFirstFaultPacket.load(memoryOrder);
    out.txContentFirstFaultAudioFrame =
        control.txContentFirstFaultAudioFrame.load(memoryOrder);
    out.txContentFirstFaultOldestFrame =
        control.txContentFirstFaultOldestFrame.load(memoryOrder);
    out.txContentFirstFaultWrittenEndFrame =
        control.txContentFirstFaultWrittenEndFrame.load(memoryOrder);
    out.txContentFirstFaultCompletionCursor =
        control.txContentFirstFaultCompletionCursor.load(memoryOrder);
    out.txContentFirstFaultCommittedEnd =
        control.txContentFirstFaultCommittedEnd.load(memoryOrder);
    // Reason is the release-published latch for the tuple above; load it last.
    out.txContentFirstFaultReason =
        control.txContentFirstFaultReason.load(std::memory_order_acquire);
    out.rxEmptyCompletions = control.rxEmptyCompletions.load(memoryOrder);
    out.currentCommittedMarginPackets =
        control.txCurrentCommittedMarginPackets.load(memoryOrder);
    out.minimumCommittedMarginPackets =
        control.txMinimumCommittedMarginPackets.load(memoryOrder);
    out.rxCurrentAvailableFrames =
        control.rxCaptureBufferTelemetry.currentAvailableFrames.load(memoryOrder);
    out.rxCaptureOverrunEvents =
        control.captureRingOverruns.load(memoryOrder);
    out.rxCaptureStarvationEvents =
        control.captureRingStarvations.load(memoryOrder);
    out.rxTotalOverwrittenFrames =
        control.rxCaptureBufferTelemetry.totalOverwrittenFrames.load(memoryOrder);
    out.rxTotalStarvedFrames =
        control.rxCaptureBufferTelemetry.totalStarvedFrames.load(memoryOrder);

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
            break;
        }
    }

    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before =
            control.rxCaptureBufferTelemetry.completedIntervalSequence.load(memoryOrder);
        if ((before & 1U) != 0U) {
            continue;
        }
        out.rxCompletedIntervalMinimumAvailableFrames =
            control.rxCaptureBufferTelemetry.completedMinimumAvailableFrames.load(memoryOrder);
        out.rxCompletedIntervalMaximumAvailableFrames =
            control.rxCaptureBufferTelemetry.completedMaximumAvailableFrames.load(memoryOrder);
        out.rxCompletedIntervalMinimumFreeHeadroomFrames =
            control.rxCaptureBufferTelemetry.completedMinimumFreeHeadroomFrames.load(memoryOrder);
        out.rxCompletedIntervalOverrunEvents =
            control.rxCaptureBufferTelemetry.completedOverrunEvents.load(memoryOrder);
        out.rxCompletedIntervalOverwrittenFrames =
            control.rxCaptureBufferTelemetry.completedOverwrittenFrames.load(memoryOrder);
        out.rxCompletedIntervalStarvationEvents =
            control.rxCaptureBufferTelemetry.completedStarvationEvents.load(memoryOrder);
        out.rxCompletedIntervalStarvedFrames =
            control.rxCaptureBufferTelemetry.completedStarvedFrames.load(memoryOrder);
        for (size_t index = 0; index < out.rxCompletedOccupancyHistogram.size(); ++index) {
            out.rxCompletedOccupancyHistogram[index] =
                control.rxCaptureBufferTelemetry.completedOccupancyHistogram[index].load(memoryOrder);
        }
        const uint64_t after =
            control.rxCaptureBufferTelemetry.completedIntervalSequence.load(memoryOrder);
        if (before == after && (after & 1U) == 0U) {
            out.rxCompletedIntervalSequence = after;
            if (after != 0) {
                out.flags |= kAudioTelemetryHasCompletedRxInterval;
                if (control.rxCaptureBufferTelemetry.completedReaderBeginReadCalls.load(
                        memoryOrder) != 0) {
                    out.flags |= kAudioTelemetryRxCaptureReaderActive;
                }
            }
            return;
        }
    }
}

} // namespace ASFW::Audio::Runtime
