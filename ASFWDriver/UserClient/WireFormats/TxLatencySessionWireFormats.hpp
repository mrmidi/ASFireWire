// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kTxLatencyWireVersion = 5;
inline constexpr uint32_t kTxLatencyMaxSamplesPerPage = 32;

#pragma pack(push, 8)

/// Field validity bitmask flags for TxLatencySampleWire.
inline constexpr uint16_t kTxLatencyFlagPubValid               = 1U << 0;
inline constexpr uint16_t kTxLatencyFlagTxValid                = 1U << 1;
inline constexpr uint16_t kTxLatencyFlagWaitValid              = 1U << 2;
inline constexpr uint16_t kTxLatencyFlagProvenanceValid        = 1U << 3;
inline constexpr uint16_t kTxLatencyFlagCorrelationValid       = 1U << 4;
inline constexpr uint16_t kTxLatencyFlagProducerDecisionValid  = 1U << 5;
inline constexpr uint16_t kTxLatencyFlagTransportDecisionValid = 1U << 6;
inline constexpr uint16_t kTxLatencyFlagImageReadyValid        = 1U << 7;
inline constexpr uint16_t kTxLatencyFlagOfferValid             = 1U << 8;
inline constexpr uint16_t kTxLatencyFlagBindValid              = 1U << 9;
inline constexpr uint16_t kTxLatencyFlagDescriptorUpdateValid  = 1U << 10;
inline constexpr uint16_t kTxLatencyFlagSealValid              = 1U << 11;
inline constexpr uint16_t kTxLatencyFlagDecisionOverwritten    = 1U << 12;
/// The position reported on the terminal event was the pass's opening
/// snapshot, not a fresh controller read, so it bounds the position rather
/// than observing it.
inline constexpr uint16_t kTxLatencyFlagTerminalPosIsSnapshot  = 1U << 13;

/// The bits ComputeTxLatencyValidityFlags owns. Everything above is decision
/// evidence gathered elsewhere, and a late-resolving publication join must
/// replace only this half of the mask -- overwriting the whole word erased the
/// producer/transport/offer/bind/seal/overwrite flags already collected.
inline constexpr uint16_t kTxLatencyFlagsE0E2Mask =
    kTxLatencyFlagPubValid | kTxLatencyFlagTxValid | kTxLatencyFlagWaitValid |
    kTxLatencyFlagProvenanceValid | kTxLatencyFlagCorrelationValid;

/// Wire representation of one sampled transmission latency measurement.
///
/// Size is derived from the fields and then pinned by the assertions below;
/// the decoder fixtures in the Swift tests check the same offsets from the
/// other side of the seam.
struct TxLatencySampleWire final {
    uint64_t packetIndex{0};
    uint64_t pcmCommittedStartFrame{0};
    uint64_t pcmCommittedEndFrame{0};
    uint64_t pubEarliestHostTicks{0};
    uint64_t pubLatestHostTicks{0};
    uint64_t txCycleStartHostTicks{0};
    int64_t  waitMinNanos{0};
    int64_t  waitMaxNanos{0};

    // Decision timeline.
    uint64_t encodeCompleteHostTicks{0};
    uint64_t offerStartHostTicks{0};
    uint64_t offerEndHostTicks{0};
    /// When the terminal examination happened -- not when its pass started.
    uint64_t transExaminedHostTicks{0};
    uint64_t descriptorUpdateHostTicks{0};
    uint64_t sealStartHostTicks{0};
    uint64_t sealEndHostTicks{0};
    /// Last transport look that found nothing on offer, and the first that saw
    /// the offer. The gap between the offer and firstAfterOffer is the window
    /// in which ready content existed unserviced.
    uint64_t lastBeforeOfferHostTicks{0};
    uint64_t firstAfterOfferHostTicks{0};
    uint64_t lastBeforeOfferPassId{0};
    uint64_t firstAfterOfferPassId{0};
    uint64_t passId{0};
    uint64_t liveHwPos{0};

    // Intervals, in nanoseconds. Every interval derived from the offer is a
    // BOUND PAIR: the offer is a CAS bracket, so a single signed delta would
    // assert an ordering the measurement does not establish when the brackets
    // overlap. min < 0 < max means "not established", not "zero".
    int64_t  e0ToImageReadyNanos{0};
    int64_t  offerToExaminedNanosMin{0};
    int64_t  offerToExaminedNanosMax{0};
    int64_t  offerToFirstServiceNanosMin{0};
    int64_t  offerToFirstServiceNanosMax{0};
    int64_t  offerToDescriptorUpdateNanosMin{0};
    int64_t  offerToDescriptorUpdateNanosMax{0};
    int64_t  sealRelativeToOfferNanosMin{0};
    int64_t  sealRelativeToOfferNanosMax{0};

    uint32_t uncertaintyHostTicks{0};
    uint32_t correlationAgeTicks{0};
    uint32_t producerFlags{0};        // kTxProducerFlag*
    uint32_t transportFlags{0};       // kTxTransportFlag*
    uint32_t examinationCount{0};     // transport looks at this packet
    int32_t  hwDistancePackets{0};

    uint16_t validityFlags{0};        // Bitmask of kTxLatencyFlag*
    uint8_t  outcome{0};              // TxLatencyOutcome
    uint8_t  unresolvedReason{0};     // TxLatencyUnresolvedReason
    uint8_t  selectedImage{0};        // 0 or 1
    uint8_t  cyclePhaseMod8{0};       // cycle position (0..7)
    uint8_t  packetGeneration{0};
    uint8_t  pcmIdentityProven{0};
    uint8_t  acquireResult{0};        // LatePayloadAcquireResult
    uint8_t  offerResult{0};          // 0=none, 1=won, 2=lost
    uint8_t  bindResult{0};           // LatePayloadBindResult (terminal look)
    uint8_t  sealResult{0};           // SealResult
    uint8_t  sealReason{0};           // SealReason
    uint8_t  observedArbPhase{0};
    uint8_t  examinedArbPhase{0};
    /// TxDecisionJoinResult for each lane. These separate "capture began after
    /// this packet was prepared" from "a later packet overwrote the record"
    /// from "the writer was mid-update" -- findings that mean different things
    /// and that a single present/absent bit would blur into one.
    uint8_t  producerJoinResult{0};
    uint8_t  transportJoinResult{0};
    /// LatePayloadBindResult of the most recent concluded attempt. Distinct
    /// from bindResult, which comes from the terminal event and is absent when
    /// an attempt failed without deciding the packet.
    uint8_t  lastAttemptBindResult{0};
    uint8_t  reserved[6]{0};
};
static_assert(sizeof(TxLatencySampleWire) == 288, "TxLatencySampleWire layout changed");
static_assert(offsetof(TxLatencySampleWire, packetIndex) == 0);
static_assert(offsetof(TxLatencySampleWire, waitMaxNanos) == 56);
static_assert(offsetof(TxLatencySampleWire, encodeCompleteHostTicks) == 64);
static_assert(offsetof(TxLatencySampleWire, offerStartHostTicks) == 72);
static_assert(offsetof(TxLatencySampleWire, offerEndHostTicks) == 80);
static_assert(offsetof(TxLatencySampleWire, transExaminedHostTicks) == 88);
static_assert(offsetof(TxLatencySampleWire, descriptorUpdateHostTicks) == 96);
static_assert(offsetof(TxLatencySampleWire, sealStartHostTicks) == 104);
static_assert(offsetof(TxLatencySampleWire, sealEndHostTicks) == 112);
static_assert(offsetof(TxLatencySampleWire, lastBeforeOfferHostTicks) == 120);
static_assert(offsetof(TxLatencySampleWire, firstAfterOfferHostTicks) == 128);
static_assert(offsetof(TxLatencySampleWire, lastBeforeOfferPassId) == 136);
static_assert(offsetof(TxLatencySampleWire, firstAfterOfferPassId) == 144);
static_assert(offsetof(TxLatencySampleWire, passId) == 152);
static_assert(offsetof(TxLatencySampleWire, liveHwPos) == 160);
static_assert(offsetof(TxLatencySampleWire, e0ToImageReadyNanos) == 168);
static_assert(offsetof(TxLatencySampleWire, offerToExaminedNanosMin) == 176);
static_assert(offsetof(TxLatencySampleWire, offerToExaminedNanosMax) == 184);
static_assert(offsetof(TxLatencySampleWire, offerToFirstServiceNanosMin) == 192);
static_assert(offsetof(TxLatencySampleWire, offerToFirstServiceNanosMax) == 200);
static_assert(offsetof(TxLatencySampleWire, offerToDescriptorUpdateNanosMin) == 208);
static_assert(offsetof(TxLatencySampleWire, offerToDescriptorUpdateNanosMax) == 216);
static_assert(offsetof(TxLatencySampleWire, sealRelativeToOfferNanosMin) == 224);
static_assert(offsetof(TxLatencySampleWire, sealRelativeToOfferNanosMax) == 232);
static_assert(offsetof(TxLatencySampleWire, uncertaintyHostTicks) == 240);
static_assert(offsetof(TxLatencySampleWire, correlationAgeTicks) == 244);
static_assert(offsetof(TxLatencySampleWire, producerFlags) == 248);
static_assert(offsetof(TxLatencySampleWire, transportFlags) == 252);
static_assert(offsetof(TxLatencySampleWire, examinationCount) == 256);
static_assert(offsetof(TxLatencySampleWire, hwDistancePackets) == 260);
static_assert(offsetof(TxLatencySampleWire, validityFlags) == 264);
static_assert(offsetof(TxLatencySampleWire, outcome) == 266);
static_assert(offsetof(TxLatencySampleWire, unresolvedReason) == 267);
static_assert(offsetof(TxLatencySampleWire, selectedImage) == 268);
static_assert(offsetof(TxLatencySampleWire, cyclePhaseMod8) == 269);
static_assert(offsetof(TxLatencySampleWire, packetGeneration) == 270);
static_assert(offsetof(TxLatencySampleWire, pcmIdentityProven) == 271);
static_assert(offsetof(TxLatencySampleWire, acquireResult) == 272);
static_assert(offsetof(TxLatencySampleWire, offerResult) == 273);
static_assert(offsetof(TxLatencySampleWire, bindResult) == 274);
static_assert(offsetof(TxLatencySampleWire, sealResult) == 275);
static_assert(offsetof(TxLatencySampleWire, sealReason) == 276);
static_assert(offsetof(TxLatencySampleWire, observedArbPhase) == 277);
static_assert(offsetof(TxLatencySampleWire, examinedArbPhase) == 278);
static_assert(offsetof(TxLatencySampleWire, producerJoinResult) == 279);
static_assert(offsetof(TxLatencySampleWire, transportJoinResult) == 280);
static_assert(offsetof(TxLatencySampleWire, lastAttemptBindResult) == 281);

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
    uint32_t sampleRateHz{0};         // Effective sample rate (e.g. 48000, 44100)

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
    uint32_t geometryProvenance{0};   // [31:16]=hardwareRingPackets, [15:0]=leadPackets
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
static_assert(offsetof(TxLatencySessionWireHeader, sampleRateHz) == 108);
static_assert(offsetof(TxLatencySessionWireHeader, eligibleByPhase) == 112);
static_assert(offsetof(TxLatencySessionWireHeader, reasonStaleCorrelation) == 144);
static_assert(offsetof(TxLatencySessionWireHeader, totalRingRecords) == 176);
static_assert(offsetof(TxLatencySessionWireHeader, geometryProvenance) == 188);

/// Wire representation of one paged query result.
struct TxLatencyResultsPageWire final {
    TxLatencySessionWireHeader header{};
    uint32_t pageIndex{0};
    uint32_t totalPages{0};
    uint32_t samplesInPage{0};
    uint32_t reserved{0};
    TxLatencySampleWire samples[kTxLatencyMaxSamplesPerPage]{};
};
static_assert(sizeof(TxLatencyResultsPageWire) == 192 + 16 + 32 * 288, "TxLatencyResultsPageWire size mismatch");

#pragma pack(pop)

} // namespace ASFW::UserClient::Wire
