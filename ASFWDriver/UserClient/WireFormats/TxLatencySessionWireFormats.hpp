// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kTxLatencyWireVersion = 2;
inline constexpr uint32_t kTxLatencyMaxSamplesPerPage = 32;

#pragma pack(push, 8)

/// Wire representation of one sampled transmission latency measurement (80 bytes).
struct TxLatencySampleWire final {
    uint64_t packetIndex{0};
    uint64_t pcmCommittedStartFrame{0};
    uint64_t pcmCommittedEndFrame{0};
    uint64_t pubEarliestHostTicks{0};
    uint64_t pubLatestHostTicks{0};
    uint64_t txCycleStartHostTicks{0};
    int64_t  waitMinNanos{0};
    int64_t  waitMaxNanos{0};
    uint32_t uncertaintyHostTicks{0};
    uint8_t  outcome{0};          // TxLatencyOutcome
    uint8_t  unresolvedReason{0}; // TxLatencyUnresolvedReason
    uint8_t  selectedImage{0};    // 0 or 1
    uint8_t  arbitrationPhase{0}; // 0..7
    uint8_t  pcmIdentityProven{0};// 0 or 1
    uint8_t  padding[7]{0};
};
static_assert(sizeof(TxLatencySampleWire) == 80, "TxLatencySampleWire must be exactly 80 bytes");
static_assert(offsetof(TxLatencySampleWire, packetIndex) == 0);
static_assert(offsetof(TxLatencySampleWire, pcmCommittedStartFrame) == 8);
static_assert(offsetof(TxLatencySampleWire, pcmCommittedEndFrame) == 16);
static_assert(offsetof(TxLatencySampleWire, pubEarliestHostTicks) == 24);
static_assert(offsetof(TxLatencySampleWire, pubLatestHostTicks) == 32);
static_assert(offsetof(TxLatencySampleWire, txCycleStartHostTicks) == 40);
static_assert(offsetof(TxLatencySampleWire, waitMinNanos) == 48);
static_assert(offsetof(TxLatencySampleWire, waitMaxNanos) == 56);
static_assert(offsetof(TxLatencySampleWire, uncertaintyHostTicks) == 64);
static_assert(offsetof(TxLatencySampleWire, outcome) == 68);
static_assert(offsetof(TxLatencySampleWire, unresolvedReason) == 69);
static_assert(offsetof(TxLatencySampleWire, selectedImage) == 70);
static_assert(offsetof(TxLatencySampleWire, arbitrationPhase) == 71);
static_assert(offsetof(TxLatencySampleWire, pcmIdentityProven) == 72);
static_assert(offsetof(TxLatencySampleWire, padding) == 73);

/// Wire representation of the session state, parameters, and counters.
struct TxLatencySessionWireHeader final {
    uint32_t version{kTxLatencyWireVersion};
    uint32_t sessionState{0};         // TxLatencySessionState
    uint32_t terminationReason{0};    // TxLatencyTerminationReason
    uint32_t sessionId{0};

    uint64_t endpointId{0};
    uint64_t epoch{0};
    uint64_t startHostTicks{0};
    uint64_t deadlineHostTicks{0};
    uint64_t frozenHostTicks{0};

    uint32_t durationSeconds{0};
    uint32_t strataSize{0};
    uint32_t seed{0};
    uint32_t assumedDriftPpm{0};

    uint64_t dataPacketsSeen{0};
    uint32_t samplesCaptured{0};
    uint32_t stampsMissedCount{0};

    uint32_t resolvedCount{0};
    uint32_t unresolvedCount{0};
    uint32_t transmitFailedCount{0};
    uint32_t substitutionCount{0};
    uint32_t invalidCount{0};
    uint32_t reserved1{0};

    uint32_t eligibleByPhase[8]{0};

    uint32_t reasonStaleCorrelation{0};
    uint32_t reasonCoveragePending{0};
    uint32_t reasonCoverageGap{0};
    uint32_t reasonEpochMismatch{0};
    uint32_t reasonPublicationAgedOut{0};
    uint32_t reasonProvenanceAgedOut{0};
    uint32_t reasonUnrecognizedEventCode{0};
    uint32_t reasonImageUnavailable{0};

    uint32_t totalRingRecords{0};
    uint32_t ringHead{0};
    uint32_t ringTail{0};
    uint32_t reserved2{0};
};
static_assert(sizeof(TxLatencySessionWireHeader) == 192, "TxLatencySessionWireHeader must be 192 bytes");
static_assert(offsetof(TxLatencySessionWireHeader, version) == 0);
static_assert(offsetof(TxLatencySessionWireHeader, sessionId) == 12);
static_assert(offsetof(TxLatencySessionWireHeader, endpointId) == 16);
static_assert(offsetof(TxLatencySessionWireHeader, epoch) == 24);
static_assert(offsetof(TxLatencySessionWireHeader, startHostTicks) == 32);
static_assert(offsetof(TxLatencySessionWireHeader, deadlineHostTicks) == 40);
static_assert(offsetof(TxLatencySessionWireHeader, frozenHostTicks) == 48);
static_assert(offsetof(TxLatencySessionWireHeader, durationSeconds) == 56);
static_assert(offsetof(TxLatencySessionWireHeader, strataSize) == 60);
static_assert(offsetof(TxLatencySessionWireHeader, seed) == 64);
static_assert(offsetof(TxLatencySessionWireHeader, assumedDriftPpm) == 68);
static_assert(offsetof(TxLatencySessionWireHeader, dataPacketsSeen) == 72);
static_assert(offsetof(TxLatencySessionWireHeader, samplesCaptured) == 80);
static_assert(offsetof(TxLatencySessionWireHeader, stampsMissedCount) == 84);
static_assert(offsetof(TxLatencySessionWireHeader, resolvedCount) == 88);
static_assert(offsetof(TxLatencySessionWireHeader, unresolvedCount) == 92);
static_assert(offsetof(TxLatencySessionWireHeader, transmitFailedCount) == 96);
static_assert(offsetof(TxLatencySessionWireHeader, substitutionCount) == 100);
static_assert(offsetof(TxLatencySessionWireHeader, invalidCount) == 104);
static_assert(offsetof(TxLatencySessionWireHeader, eligibleByPhase) == 112);
static_assert(offsetof(TxLatencySessionWireHeader, reasonStaleCorrelation) == 144);
static_assert(offsetof(TxLatencySessionWireHeader, totalRingRecords) == 176);

/// Wire representation of one paged query result.
struct TxLatencyResultsPageWire final {
    TxLatencySessionWireHeader header{};
    uint32_t pageIndex{0};
    uint32_t totalPages{0};
    uint32_t samplesInPage{0};
    uint32_t reserved{0};
    TxLatencySampleWire samples[kTxLatencyMaxSamplesPerPage]{};
};
static_assert(sizeof(TxLatencyResultsPageWire) == 192 + 16 + 32 * 80, "TxLatencyResultsPageWire size mismatch");

#pragma pack(pop)

} // namespace ASFW::UserClient::Wire
