// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Read-only, value-owned audio telemetry contract (selector 1013,
// kMethodDiagGetAudioTelemetry). Callers receive copied atomics while the
// endpoint still owns the direct-audio mapping; nothing here is a view of
// AudioTransportControlBlock.
//
// Wire v4 (FW-175). What it is for: a small STABLE per-endpoint health summary
// for the app, MCP and field reports. Research traces never go here -- they go
// to the log ring or an optional sidecar, so an experiment never enlarges this
// ABI (documentation/OBSERVABILITY_INVENTORY.md).
//
// Layout rules, each pinned by a static_assert below:
//  - a self-describing header: version, header size, total bytes, endpoint
//    count and record size, so a reader strides by the declared record size and
//    tolerates a LONGER record from a newer driver;
//  - only `endpointCount` records are serialised (SerializeAudioTelemetry), and
//    the full 8-endpoint worst case must fit the 4 KiB inline user-client reply:
//    a larger reply is not an error, it silently returns nothing;
//  - every field offset is asserted, and the host test suite compares a golden
//    byte fixture (tests/fixtures/audio_telemetry_v4.bin) that the Swift tests
//    decode too, so the two sides cannot drift apart unnoticed;
//  - completed-interval fields are read under the Seqlock.hpp protocol; a copy
//    that never stabilises is discarded (defaults, flag clear), never torn;
//  - no device-family fields: family diagnostics belong to their own paths.

#pragma once

#include "../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../Shared/Isoch/AudioTimingGeometry.hpp"
#include "Seqlock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace ASFW::Audio::Runtime {

constexpr uint16_t kAudioTelemetryWireVersion = 4;
constexpr uint32_t kAudioTelemetryMaxEndpoints = 8;
// IOConnectCallStructMethod inline reply limit: a reply above this returns no
// data at all rather than failing.
constexpr size_t kAudioTelemetryInlineReplyLimitBytes = 4096;

enum AudioTelemetryFlags : uint32_t {
    kAudioTelemetryBindingReady = 1U << 0,
    kAudioTelemetryStreaming = 1U << 1,
    kAudioTelemetryHasCompletedInterval = 1U << 2,
    kAudioTelemetryHasCompletedRxInterval = 1U << 3,
    // CoreAudio issued BeginRead during the completed RX interval. Without a
    // reader, a full capture ring is intentionally only a retained window.
    kAudioTelemetryRxCaptureReaderActive = 1U << 4,
    // The completed interval's duration is known (not the first interval
    // after a reset). Without it, interval counts cannot be turned into rates.
    kAudioTelemetryTxIntervalDurationKnown = 1U << 5,
    kAudioTelemetryRxIntervalDurationKnown = 1U << 6,
};

struct AudioTelemetryHeader final {
    uint16_t version{kAudioTelemetryWireVersion};
    uint16_t headerBytes{0};
    uint32_t totalBytes{0};          // header + endpointCount * endpointRecordBytes
    uint32_t endpointCount{0};
    uint32_t endpointRecordBytes{0};
    uint64_t captureHostTicks{0};    // mach_absolute_time() when copied
    uint32_t hostTimebaseNumer{0};   // ticks -> ns = ticks * numer / denom
    uint32_t hostTimebaseDenom{0};
};

static_assert(sizeof(AudioTelemetryHeader) == 32);
static_assert(offsetof(AudioTelemetryHeader, version) == 0);
static_assert(offsetof(AudioTelemetryHeader, headerBytes) == 2);
static_assert(offsetof(AudioTelemetryHeader, totalBytes) == 4);
static_assert(offsetof(AudioTelemetryHeader, endpointCount) == 8);
static_assert(offsetof(AudioTelemetryHeader, endpointRecordBytes) == 12);
static_assert(offsetof(AudioTelemetryHeader, captureHostTicks) == 16);
static_assert(offsetof(AudioTelemetryHeader, hostTimebaseNumer) == 24);
static_assert(offsetof(AudioTelemetryHeader, hostTimebaseDenom) == 28);

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
               IsochTransport::AudioTimingGeometry::
                   kRxCaptureOccupancyHistogramBuckets>
        rxCompletedOccupancyHistogram{};
    uint32_t inputFrameCapacityFrames{0};
    uint32_t reserved0{0};  // explicit; was implicit padding in v3
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
    // v4: when and over how long the completed intervals were measured.
    uint64_t txCompletedIntervalDurationTicks{0};
    uint64_t txCompletedIntervalEndHostTicks{0};
    uint64_t rxCompletedIntervalDurationTicks{0};
    uint64_t rxCompletedIntervalEndHostTicks{0};
};

// Every offset is part of the wire contract (Swift: DriverConnector+AudioTelemetry.swift).
#define ASFW_TELEMETRY_OFFSET(field, off) \
    static_assert(offsetof(AudioTelemetryEndpointSnapshot, field) == (off), #field)
ASFW_TELEMETRY_OFFSET(guid, 0);
ASFW_TELEMETRY_OFFSET(endpointGeneration, 8);
ASFW_TELEMETRY_OFFSET(controlGeneration, 16);
ASFW_TELEMETRY_OFFSET(completedIntervalSequence, 24);
ASFW_TELEMETRY_OFFSET(lastPreparationLatencyTicks, 32);
ASFW_TELEMETRY_OFFSET(completedIntervalMaxLatencyTicks, 40);
ASFW_TELEMETRY_OFFSET(maxPreparationLatencyTicks, 48);
ASFW_TELEMETRY_OFFSET(preparationWakeCount, 56);
ASFW_TELEMETRY_OFFSET(preparationAtMost750Us, 64);
ASFW_TELEMETRY_OFFSET(preparationAtLeast1500Us, 72);
ASFW_TELEMETRY_OFFSET(rxReplayEntries, 80);
ASFW_TELEMETRY_OFFSET(rxReplayEpochResets, 88);
ASFW_TELEMETRY_OFFSET(completedLatencyHistogram, 96);
ASFW_TELEMETRY_OFFSET(completedMarginHistogram, 144);
ASFW_TELEMETRY_OFFSET(flags, 184);
ASFW_TELEMETRY_OFFSET(sampleRateHz, 188);
ASFW_TELEMETRY_OFFSET(outputChannels, 192);
ASFW_TELEMETRY_OFFSET(inputChannels, 196);
ASFW_TELEMETRY_OFFSET(currentCommittedMarginPackets, 200);
ASFW_TELEMETRY_OFFSET(completedIntervalMarginMinPackets, 204);
ASFW_TELEMETRY_OFFSET(completedIntervalMarginMaxPackets, 208);
ASFW_TELEMETRY_OFFSET(minimumCommittedMarginPackets, 212);
ASFW_TELEMETRY_OFFSET(preparationLeadPackets, 216);
ASFW_TELEMETRY_OFFSET(hardwareFloorPackets, 220);
ASFW_TELEMETRY_OFFSET(rxCurrentAvailableFrames, 224);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalSequence, 232);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalMinimumAvailableFrames, 240);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalMaximumAvailableFrames, 248);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalMinimumFreeHeadroomFrames, 256);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalOverrunEvents, 264);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalOverwrittenFrames, 272);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalStarvationEvents, 280);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalStarvedFrames, 288);
ASFW_TELEMETRY_OFFSET(rxCaptureOverrunEvents, 296);
ASFW_TELEMETRY_OFFSET(rxCaptureStarvationEvents, 304);
ASFW_TELEMETRY_OFFSET(rxTotalOverwrittenFrames, 312);
ASFW_TELEMETRY_OFFSET(rxTotalStarvedFrames, 320);
ASFW_TELEMETRY_OFFSET(rxCompletedOccupancyHistogram, 328);
ASFW_TELEMETRY_OFFSET(inputFrameCapacityFrames, 368);
ASFW_TELEMETRY_OFFSET(reserved0, 372);
ASFW_TELEMETRY_OFFSET(rxPacketsSeen, 376);
ASFW_TELEMETRY_OFFSET(rxDataPackets, 384);
ASFW_TELEMETRY_OFFSET(rxNoDataPackets, 392);
ASFW_TELEMETRY_OFFSET(rxShortPackets, 400);
ASFW_TELEMETRY_OFFSET(rxInvalidCipHeaders, 408);
ASFW_TELEMETRY_OFFSET(rxZeroDataBlockSize, 416);
ASFW_TELEMETRY_OFFSET(rxGeometryMismatch, 424);
ASFW_TELEMETRY_OFFSET(txCompletedIntervalDurationTicks, 432);
ASFW_TELEMETRY_OFFSET(txCompletedIntervalEndHostTicks, 440);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalDurationTicks, 448);
ASFW_TELEMETRY_OFFSET(rxCompletedIntervalEndHostTicks, 456);
#undef ASFW_TELEMETRY_OFFSET

constexpr uint32_t kAudioTelemetryEndpointRecordBytes = 464;
static_assert(sizeof(AudioTelemetryEndpointSnapshot) == kAudioTelemetryEndpointRecordBytes);

struct AudioTelemetrySnapshot final {
    AudioTelemetryHeader header{};
    std::array<AudioTelemetryEndpointSnapshot, kAudioTelemetryMaxEndpoints> endpoints{};
};

// The worst case (every endpoint) must fit one inline reply.
static_assert(sizeof(AudioTelemetryHeader) +
                  kAudioTelemetryMaxEndpoints * kAudioTelemetryEndpointRecordBytes <=
              kAudioTelemetryInlineReplyLimitBytes,
              "audio telemetry no longer fits the 4 KiB inline reply; "
              "move it to an IOMemoryDescriptor before growing it");

/// Bytes SerializeAudioTelemetry writes for `endpointCount` endpoints.
[[nodiscard]] constexpr size_t AudioTelemetryWireBytes(uint32_t endpointCount) noexcept {
    const uint32_t count = endpointCount < kAudioTelemetryMaxEndpoints
                               ? endpointCount
                               : kAudioTelemetryMaxEndpoints;
    return sizeof(AudioTelemetryHeader) +
           static_cast<size_t>(count) * kAudioTelemetryEndpointRecordBytes;
}

/// Fills the header's size fields and writes header + the populated records.
/// Returns the byte count written, or 0 if `out` is too small.
[[nodiscard]] inline size_t SerializeAudioTelemetry(AudioTelemetrySnapshot& snapshot,
                                                    std::span<uint8_t> out) noexcept {
    auto& h = snapshot.header;
    if (h.endpointCount > kAudioTelemetryMaxEndpoints) {
        h.endpointCount = kAudioTelemetryMaxEndpoints;
    }
    h.version = kAudioTelemetryWireVersion;
    h.headerBytes = static_cast<uint16_t>(sizeof(AudioTelemetryHeader));
    h.endpointRecordBytes = kAudioTelemetryEndpointRecordBytes;
    const size_t bytes = AudioTelemetryWireBytes(h.endpointCount);
    h.totalBytes = static_cast<uint32_t>(bytes);
    if (out.size() < bytes) {
        return 0;
    }
    std::memcpy(out.data(), &h, sizeof(h));
    if (h.endpointCount != 0) {
        std::memcpy(out.data() + sizeof(h), snapshot.endpoints.data(),
                    static_cast<size_t>(h.endpointCount) * kAudioTelemetryEndpointRecordBytes);
    }
    return bytes;
}

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

    // Completed intervals: accept a copy only if its sequence was stable
    // (Seqlock.hpp). A copy that never stabilises is discarded, so a reader
    // sees either a whole interval or the defaults -- never a torn mix.
    const auto& rx = control.rxCaptureBufferTelemetry;
    AudioTelemetryEndpointSnapshot tx{};
    const auto txSequence = SeqlockTryRead(control.txCompletedIntervalSequence, [&] {
        tx.completedIntervalMarginMinPackets =
            control.txCompletedIntervalMarginMinPackets.load(memoryOrder);
        tx.completedIntervalMarginMaxPackets =
            control.txCompletedIntervalMarginMaxPackets.load(memoryOrder);
        tx.completedIntervalMaxLatencyTicks =
            control.txCompletedIntervalPreparationLatencyMaxTicks.load(memoryOrder);
        tx.txCompletedIntervalDurationTicks =
            control.txCompletedIntervalDurationTicks.load(memoryOrder);
        tx.txCompletedIntervalEndHostTicks =
            control.txCompletedIntervalEndHostTicks.load(memoryOrder);
        for (size_t index = 0; index < tx.completedLatencyHistogram.size(); ++index) {
            tx.completedLatencyHistogram[index] =
                control.txCompletedIntervalPreparationLatencyHistogram[index].load(memoryOrder);
        }
        for (size_t index = 0; index < tx.completedMarginHistogram.size(); ++index) {
            tx.completedMarginHistogram[index] =
                control.txCompletedIntervalCommittedMarginHistogram[index].load(memoryOrder);
        }
    });
    if (txSequence && *txSequence != 0) {
        out.completedIntervalSequence = *txSequence;
        out.completedIntervalMarginMinPackets = tx.completedIntervalMarginMinPackets;
        out.completedIntervalMarginMaxPackets = tx.completedIntervalMarginMaxPackets;
        out.completedIntervalMaxLatencyTicks = tx.completedIntervalMaxLatencyTicks;
        out.txCompletedIntervalDurationTicks = tx.txCompletedIntervalDurationTicks;
        out.txCompletedIntervalEndHostTicks = tx.txCompletedIntervalEndHostTicks;
        out.completedLatencyHistogram = tx.completedLatencyHistogram;
        out.completedMarginHistogram = tx.completedMarginHistogram;
        out.flags |= kAudioTelemetryHasCompletedInterval;
        if (tx.txCompletedIntervalDurationTicks != 0) {
            out.flags |= kAudioTelemetryTxIntervalDurationKnown;
        }
    }

    AudioTelemetryEndpointSnapshot rxCopy{};
    uint64_t readerCalls = 0;
    const auto rxSequence = SeqlockTryRead(rx.completedIntervalSequence, [&] {
        rxCopy.rxCompletedIntervalMinimumAvailableFrames =
            rx.completedMinimumAvailableFrames.load(memoryOrder);
        rxCopy.rxCompletedIntervalMaximumAvailableFrames =
            rx.completedMaximumAvailableFrames.load(memoryOrder);
        rxCopy.rxCompletedIntervalMinimumFreeHeadroomFrames =
            rx.completedMinimumFreeHeadroomFrames.load(memoryOrder);
        rxCopy.rxCompletedIntervalOverrunEvents = rx.completedOverrunEvents.load(memoryOrder);
        rxCopy.rxCompletedIntervalOverwrittenFrames =
            rx.completedOverwrittenFrames.load(memoryOrder);
        rxCopy.rxCompletedIntervalStarvationEvents =
            rx.completedStarvationEvents.load(memoryOrder);
        rxCopy.rxCompletedIntervalStarvedFrames = rx.completedStarvedFrames.load(memoryOrder);
        rxCopy.rxCompletedIntervalDurationTicks =
            rx.completedIntervalDurationTicks.load(memoryOrder);
        rxCopy.rxCompletedIntervalEndHostTicks =
            rx.completedIntervalEndHostTicks.load(memoryOrder);
        for (size_t index = 0; index < rxCopy.rxCompletedOccupancyHistogram.size(); ++index) {
            rxCopy.rxCompletedOccupancyHistogram[index] =
                rx.completedOccupancyHistogram[index].load(memoryOrder);
        }
        readerCalls = rx.completedReaderBeginReadCalls.load(memoryOrder);
    });
    if (rxSequence && *rxSequence != 0) {
        out.rxCompletedIntervalSequence = *rxSequence;
        out.rxCompletedIntervalMinimumAvailableFrames =
            rxCopy.rxCompletedIntervalMinimumAvailableFrames;
        out.rxCompletedIntervalMaximumAvailableFrames =
            rxCopy.rxCompletedIntervalMaximumAvailableFrames;
        out.rxCompletedIntervalMinimumFreeHeadroomFrames =
            rxCopy.rxCompletedIntervalMinimumFreeHeadroomFrames;
        out.rxCompletedIntervalOverrunEvents = rxCopy.rxCompletedIntervalOverrunEvents;
        out.rxCompletedIntervalOverwrittenFrames = rxCopy.rxCompletedIntervalOverwrittenFrames;
        out.rxCompletedIntervalStarvationEvents = rxCopy.rxCompletedIntervalStarvationEvents;
        out.rxCompletedIntervalStarvedFrames = rxCopy.rxCompletedIntervalStarvedFrames;
        out.rxCompletedIntervalDurationTicks = rxCopy.rxCompletedIntervalDurationTicks;
        out.rxCompletedIntervalEndHostTicks = rxCopy.rxCompletedIntervalEndHostTicks;
        out.rxCompletedOccupancyHistogram = rxCopy.rxCompletedOccupancyHistogram;
        out.flags |= kAudioTelemetryHasCompletedRxInterval;
        if (rxCopy.rxCompletedIntervalDurationTicks != 0) {
            out.flags |= kAudioTelemetryRxIntervalDurationKnown;
        }
        if (readerCalls != 0) {
            out.flags |= kAudioTelemetryRxCaptureReaderActive;
        }
    }
}

} // namespace ASFW::Audio::Runtime
