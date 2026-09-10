// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "TxLatencyMeasurement.hpp"
#include "../../UserClient/WireFormats/TxLatencySessionWireFormats.hpp"
#include "../../Common/TimingUtils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ASFW::Audio::Runtime {

inline constexpr uint32_t kTxLatencyMaxSamples = 4096;
inline constexpr uint32_t kTxLatencySessionWireVersion = 1;

enum class TxLatencySessionState : uint32_t {
    Idle = 0,
    Arming = 1,
    Capturing = 2,
    StopRequested = 3,
    Frozen = 4,
};

enum class TxLatencyTerminationReason : uint32_t {
    None = 0,
    UserStopped = 1,
    DeadlineExpired = 2,
    EpochChanged = 3,
    CapacityReached = 4,
    StreamReset = 5,
    DriverTeardown = 6,
};

struct TxLatencyRecord final {
    uint64_t packetIndex{0};
    uint64_t firstAudioFrame{0};
    uint64_t payloadReadyHostTicksEarliest{0};
    uint64_t payloadReadyHostTicksLatest{0};
    uint64_t txEarliestHostTicks{0};
    uint64_t txLatestHostTicks{0};
    uint32_t frameCount{0};
    TxLatencyOutcome outcome{TxLatencyOutcome::Unresolved};
    TxLatencyUnresolvedReason unresolvedReason{TxLatencyUnresolvedReason::None};
    uint32_t correlationAgeBusTicks{0};
    uint16_t eventCode{0};
    uint8_t selectedImage{0};
    uint8_t groupPhase{0};
    uint8_t packetGeneration{0};
    uint8_t pcmIdentityProven{0};
    uint16_t validityFlags{0};
    uint32_t reserved{0};
};

struct TxLatencySessionHeader final {
    uint32_t version{kTxLatencySessionWireVersion};
    uint32_t sessionId{0};
    TxLatencySessionState state{TxLatencySessionState::Idle};
    TxLatencyTerminationReason terminationReason{TxLatencyTerminationReason::None};
    uint64_t epoch{0};
    uint32_t sampleRateHz{0};
    uint32_t samplingSeed{0};
    uint32_t strataSize{1};
    uint64_t sessionStartHostTicks{0};
    uint64_t sessionDeadlineHostTicks{0};
    uint64_t sessionFrozenHostTicks{0};

    uint64_t dataPacketsSeen{0};
    std::array<uint64_t, 8> eligibleByPhase{};
    std::array<uint64_t, 8> sampledByPhase{};

    uint64_t sampledCount{0};
    uint64_t matchedCount{0};
    uint64_t substitutedCount{0};
    uint64_t unresolvedCount{0};
    uint64_t agedOutCount{0};
    uint64_t transmitFailedCount{0};
    uint64_t invalidCount{0};
    uint64_t stampsMissedCount{0};

    // Unresolved reason breakdown
    uint64_t reasonStaleCorrelation{0};
    uint64_t reasonCoveragePending{0};
    uint64_t reasonCoverageGap{0};
    uint64_t reasonEpochMismatch{0};
    uint64_t reasonPublicationAgedOut{0};
    uint64_t reasonProvenanceAgedOut{0};
    uint64_t reasonUnrecognizedEventCode{0};
    uint64_t reasonImageUnavailable{0};

    uint32_t recordCount{0};
};

/// Immutable snapshot of a finalized TX latency metering session.
/// Outstanding page readers retain this object via std::shared_ptr, completely
/// isolating pagination from any subsequent rearm or active capture in the driver.
struct TxLatencySessionResult final {
    TxLatencySessionHeader header{};
    std::vector<TxLatencyRecord> records{};
    uint64_t buildId{0};
    uint32_t preparationLeadPackets{0};
    uint32_t hardwareRingPackets{0};

    [[nodiscard]] bool CopyWirePage(
        uint32_t pageIndex,
        uint32_t samplesPerPage,
        uint32_t requestedSessionId,
        uint64_t endpointId,
        UserClient::Wire::TxLatencyResultsPageWire& out) const noexcept {
        if (requestedSessionId != 0 && header.sessionId != requestedSessionId) {
            return false;
        }

        out = {};
        out.header.version = UserClient::Wire::kTxLatencyWireVersion;
        out.header.sessionState = static_cast<uint32_t>(header.state);
        out.header.terminationReason = static_cast<uint32_t>(header.terminationReason);
        out.header.sessionId = header.sessionId;
        out.header.endpointId = endpointId;
        out.header.epoch = header.epoch;
        out.header.startHostTicks = header.sessionStartHostTicks;
        out.header.deadlineHostTicks = header.sessionDeadlineHostTicks;
        out.header.frozenHostTicks = header.sessionFrozenHostTicks;
        out.header.durationSeconds = static_cast<uint32_t>(
            (header.sessionDeadlineHostTicks > header.sessionStartHostTicks)
                ? Timing::hostTicksToNanos(header.sessionDeadlineHostTicks - header.sessionStartHostTicks) / 1'000'000'000ULL
                : 5);
        out.header.strataSize = header.strataSize;
        out.header.seed = header.samplingSeed;
        out.header.assumedDriftPpm = 100;
        out.header.dataPacketsSeen = header.dataPacketsSeen;
        out.header.samplesCaptured = static_cast<uint32_t>(records.size());
        out.header.stampsMissedCount = static_cast<uint32_t>(header.stampsMissedCount);
        out.header.resolvedCount = static_cast<uint32_t>(header.matchedCount);
        out.header.unresolvedCount = static_cast<uint32_t>(header.unresolvedCount);
        out.header.transmitFailedCount = static_cast<uint32_t>(header.transmitFailedCount);
        out.header.substitutionCount = static_cast<uint32_t>(header.substitutedCount);
        out.header.invalidCount = static_cast<uint32_t>(header.invalidCount);
        out.header.sampleRateHz = header.sampleRateHz;

        for (size_t i = 0; i < 8; ++i) {
            out.header.eligibleByPhase[i] = static_cast<uint32_t>(header.eligibleByPhase[i]);
        }

        out.header.reasonStaleCorrelation = static_cast<uint32_t>(header.reasonStaleCorrelation);
        out.header.reasonCoveragePending = static_cast<uint32_t>(header.reasonCoveragePending);
        out.header.reasonCoverageGap = static_cast<uint32_t>(header.reasonCoverageGap);
        out.header.reasonEpochMismatch = static_cast<uint32_t>(header.reasonEpochMismatch);
        out.header.reasonPublicationAgedOut = static_cast<uint32_t>(header.reasonPublicationAgedOut);
        out.header.reasonProvenanceAgedOut = static_cast<uint32_t>(header.reasonProvenanceAgedOut);
        out.header.reasonUnrecognizedEventCode = static_cast<uint32_t>(header.reasonUnrecognizedEventCode);
        out.header.reasonImageUnavailable = static_cast<uint32_t>(header.reasonImageUnavailable);

        out.header.totalRingRecords = static_cast<uint32_t>(records.size());
        out.header.ringHead = static_cast<uint32_t>(records.size());
        out.header.ringTail = 0;
        out.header.geometryProvenance = (hardwareRingPackets << 16) | (preparationLeadPackets & 0xFFFF);

        const uint32_t perPage = std::min(
            samplesPerPage == 0 ? UserClient::Wire::kTxLatencyMaxSamplesPerPage : samplesPerPage,
            UserClient::Wire::kTxLatencyMaxSamplesPerPage);

        const uint32_t totalRecords = static_cast<uint32_t>(records.size());
        if (totalRecords == 0) {
            out.pageIndex = 0;
            out.totalPages = 0;
            out.samplesInPage = 0;
            return true;
        }

        const uint32_t totalPages = (totalRecords + perPage - 1) / perPage;
        out.pageIndex = pageIndex;
        out.totalPages = totalPages;

        if (pageIndex >= totalPages) {
            out.samplesInPage = 0;
            return true;
        }

        const uint32_t cursor = pageIndex * perPage;
        const uint32_t count = std::min(perPage, totalRecords - cursor);
        out.samplesInPage = count;

        for (uint32_t i = 0; i < count; ++i) {
            const auto& rec = records[cursor + i];
            auto& sw = out.samples[i];
            sw.packetIndex = rec.packetIndex;
            sw.pcmCommittedStartFrame = rec.firstAudioFrame;
            sw.pcmCommittedEndFrame = rec.firstAudioFrame + rec.frameCount;
            sw.pubEarliestHostTicks = rec.payloadReadyHostTicksEarliest;
            sw.pubLatestHostTicks = rec.payloadReadyHostTicksLatest;
            sw.txCycleStartHostTicks = rec.txEarliestHostTicks;
            sw.uncertaintyHostTicks = (rec.txLatestHostTicks > rec.txEarliestHostTicks)
                                          ? static_cast<uint32_t>(rec.txLatestHostTicks - rec.txEarliestHostTicks)
                                          : 0;
            sw.correlationAgeTicks = rec.correlationAgeBusTicks;
            sw.waitMinNanos = (rec.payloadReadyHostTicksLatest != 0 && rec.txEarliestHostTicks != 0)
                                  ? DiffNanos(rec.txEarliestHostTicks, rec.payloadReadyHostTicksLatest)
                                  : 0;
            sw.waitMaxNanos = (rec.payloadReadyHostTicksEarliest != 0 && rec.txLatestHostTicks != 0)
                                  ? DiffNanos(rec.txLatestHostTicks, rec.payloadReadyHostTicksEarliest)
                                  : 0;
            sw.outcome = static_cast<uint8_t>(rec.outcome);
            sw.unresolvedReason = static_cast<uint8_t>(rec.unresolvedReason);
            sw.selectedImage = rec.selectedImage;
            sw.arbitrationPhase = rec.groupPhase;
            sw.packetGeneration = rec.packetGeneration;
            sw.pcmIdentityProven = rec.pcmIdentityProven;
            sw.validityFlags = rec.validityFlags;
        }
        return true;
    }

private:
    [[nodiscard]] static constexpr int64_t DiffNanos(uint64_t to, uint64_t from) noexcept {
        if (to >= from) {
            return static_cast<int64_t>(Timing::hostTicksToNanos(to - from));
        } else {
            return -static_cast<int64_t>(Timing::hostTicksToNanos(from - to));
        }
    }
};

} // namespace ASFW::Audio::Runtime
