// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Audio-owned TX latency measurement session: state machine, stratified
// pseudo-random sampling, sample storage, and quiescent drain protocol.

#pragma once

#include "PublicationRangeRing.hpp"
#include "TxLatencyMeasurement.hpp"
#include "../Ports/ITxPcmSource.hpp"
#include "../Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "../../UserClient/WireFormats/TxLatencySessionWireFormats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

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
    uint64_t reserved{0};
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

class TxLatencySession final {
public:
    TxLatencySession() noexcept = default;

    /// Arm a new measurement session. Returns false if not in Idle or Frozen state.
    [[nodiscard]] bool Arm(uint32_t sessionId,
                           uint64_t epoch,
                           uint32_t sampleRateHz,
                           uint32_t durationSeconds,
                           uint32_t sampleBudget,
                           uint32_t seed = 0,
                           uint32_t strataSize = 0,
                           uint32_t assumedDriftPpm = 100) noexcept {
        auto current = state_.load(std::memory_order_acquire);
        if (current != TxLatencySessionState::Idle && current != TxLatencySessionState::Frozen) {
            return false;
        }

        // Wait until writer is completely quiescent before rearming.
        if (writerActive_.load(std::memory_order_acquire) != 0) {
            return false;
        }

        state_.store(TxLatencySessionState::Arming, std::memory_order_seq_cst);

        sessionId_ = sessionId;
        epoch_ = epoch;
        sampleRateHz_ = sampleRateHz;
        durationSeconds_ = std::max(1u, std::min(durationSeconds, 60u));
        sampleBudget_ = std::min(sampleBudget, kTxLatencyMaxSamples);
        if (sampleBudget_ == 0) sampleBudget_ = kTxLatencyMaxSamples;

        samplingSeed_ = (seed != 0) ? seed : 0x1394BEEF;
        prngState_ = samplingSeed_;

        if (strataSize > 0) {
            strataSize_ = strataSize;
        } else {
            // Conservative estimate of eligible DATA packets in session duration.
            // At 8000 packets/sec, total packets = duration * 8000.
            const uint64_t expectedPackets = static_cast<uint64_t>(durationSeconds_) * 8000ULL;
            strataSize_ = static_cast<uint32_t>(std::max<uint64_t>(1, expectedPackets / sampleBudget_));
        }
        assumedDriftPpm_ = (assumedDriftPpm > 0) ? assumedDriftPpm : 100;
        currentStratumOffset_ = 0;
        targetOffsetInStratum_ = prngState_ % strataSize_;

        dataPacketsSeen_ = 0;
        eligibleByPhase_.fill(0);
        sampledByPhase_.fill(0);

        sampledCount_ = 0;
        matchedCount_ = 0;
        substitutedCount_ = 0;
        unresolvedCount_ = 0;
        agedOutCount_ = 0;
        transmitFailedCount_ = 0;
        invalidCount_ = 0;
        stampsMissedCount_ = 0;

        reasonStaleCorrelation_ = 0;
        reasonCoveragePending_ = 0;
        reasonCoverageGap_ = 0;
        reasonEpochMismatch_ = 0;
        reasonPublicationAgedOut_ = 0;
        reasonProvenanceAgedOut_ = 0;
        reasonUnrecognizedEventCode_ = 0;
        reasonImageUnavailable_ = 0;

        recordCount_ = 0;
        terminationReason_ = TxLatencyTerminationReason::None;

        startHostTicks_ = mach_absolute_time();
        const uint64_t durationNs = static_cast<uint64_t>(durationSeconds_) * 1'000'000'000ULL;
        deadlineHostTicks_ = startHostTicks_ + Timing::nanosToHostTicks(durationNs);
        frozenHostTicks_ = 0;

        state_.store(TxLatencySessionState::Capturing, std::memory_order_seq_cst);
        return true;
    }

    /// Request session stop.
    void RequestStop(TxLatencyTerminationReason reason = TxLatencyTerminationReason::UserStopped) noexcept {
        auto expected = TxLatencySessionState::Capturing;
        if (state_.compare_exchange_strong(expected, TxLatencySessionState::StopRequested,
                                           std::memory_order_seq_cst)) {
            terminationReason_ = reason;
            PollQuiescence();
        }
    }

    /// Check if in-flight writer has exited and publish Frozen.
    void PollQuiescence() noexcept {
        if (state_.load(std::memory_order_seq_cst) != TxLatencySessionState::StopRequested) {
            return;
        }

        if (writerActive_.load(std::memory_order_acquire) == 0) {
            frozenHostTicks_ = mach_absolute_time();
            state_.store(TxLatencySessionState::Frozen, std::memory_order_seq_cst);
        }
    }

    /// Reset session to idle on audio stream reset.
    void Reset() noexcept {
        state_.store(TxLatencySessionState::Idle, std::memory_order_seq_cst);
        writerActive_.store(0, std::memory_order_seq_cst);
        recordCount_ = 0;
    }

    /// Process a completed completion stamp from the audio observer loop.
    void ObserveCompletion(
        uint64_t packetIndex,
        uint32_t completionCycleTimer,
        uint32_t completionMetadata,
        const Isoch::IsochTxClockPairSample& pair,
        const Protocols::Audio::AMDTP::AmdtpPacketTimeline& timeline,
        const PublicationRangeRing& publicationRing,
        uint64_t currentHostNow) noexcept {
        // Fast pre-check before setting writerActive.
        if (state_.load(std::memory_order_relaxed) != TxLatencySessionState::Capturing) {
            return;
        }

        // Deadline check: stop automatically if session deadline has expired.
        if (currentHostNow >= deadlineHostTicks_) {
            RequestStop(TxLatencyTerminationReason::DeadlineExpired);
            return;
        }

        writerActive_.store(1, std::memory_order_seq_cst);
        if (state_.load(std::memory_order_seq_cst) != TxLatencySessionState::Capturing) {
            writerActive_.store(0, std::memory_order_release);
            return;
        }

        const auto* slot = timeline.SlotByIndex(static_cast<uint32_t>(packetIndex));
        if (!slot || !slot->isData || slot->framesInPacket == 0) {
            writerActive_.store(0, std::memory_order_release);
            return;
        }

        // Epoch mismatch causes immediate stop to prevent mixing coordinates.
        if (slot->epoch != epoch_) {
            writerActive_.store(0, std::memory_order_release);
            RequestStop(TxLatencyTerminationReason::EpochChanged);
            return;
        }

        const uint8_t phase = static_cast<uint8_t>(slot->cycleOrdinal % 8);
        ++eligibleByPhase_[phase];
        const uint64_t seq = dataPacketsSeen_++;

        // Stratified selection check.
        const bool selectThisPacket = (currentStratumOffset_ == targetOffsetInStratum_);
        ++currentStratumOffset_;
        if (currentStratumOffset_ >= strataSize_) {
            currentStratumOffset_ = 0;
            targetOffsetInStratum_ = NextPrng() % strataSize_;
        }

        if (!selectThisPacket) {
            writerActive_.store(0, std::memory_order_release);
            return;
        }

        if (recordCount_ >= sampleBudget_ || recordCount_ >= kTxLatencyMaxSamples) {
            writerActive_.store(0, std::memory_order_release);
            RequestStop(TxLatencyTerminationReason::CapacityReached);
            return;
        }

        ++sampledByPhase_[phase];

        // Extract metadata reported by transport.
        const uint8_t selectedImage = Isoch::CompletionSelectedImage(completionMetadata);
        const uint16_t eventCode = Isoch::CompletionEventCode(completionMetadata);

        // Compute transmission bounds.
        const auto txBounds = ComputeTransmitBounds(completionCycleTimer, pair, assumedDriftPpm_);

        // Read audio provenance for the transmitted image.
        Protocols::Audio::AMDTP::ImageProvenance provenance{};
        const bool haveProvenance = timeline.ReadImageProvenance(
            static_cast<uint32_t>(packetIndex), selectedImage, 0, provenance);

        const bool pcmReady = haveProvenance &&
                              (provenance.pcmCopyResult ==
                               static_cast<uint8_t>(Ports::PcmCopyResult::Ready));
        const bool isSubstitution = (selectedImage == 0 && !pcmReady) ||
                                    (selectedImage == 1 && haveProvenance && !pcmReady);

        // Coverage walk in publication history.
        uint64_t pubEarliest = 0;
        uint64_t pubLatest = 0;
        const auto coverage = publicationRing.LookupPacketCoverage(
            epoch_, slot->firstAudioFrame, slot->framesInPacket, pubEarliest, pubLatest);

        TxLatencyUnresolvedReason reason = TxLatencyUnresolvedReason::None;
        const auto outcome = ClassifyTxLatencySample(
            eventCode, haveProvenance, isSubstitution, coverage, txBounds, pubLatest, reason);

        // Record the sample.
        auto& rec = records_[recordCount_++];
        rec.packetIndex = packetIndex;
        rec.firstAudioFrame = slot->firstAudioFrame;
        rec.frameCount = slot->framesInPacket;
        rec.payloadReadyHostTicksEarliest = pubEarliest;
        rec.payloadReadyHostTicksLatest = pubLatest;
        rec.txEarliestHostTicks = txBounds.txEarliestHost;
        rec.txLatestHostTicks = txBounds.txLatestHost;
        rec.outcome = outcome;
        rec.unresolvedReason = reason;
        rec.correlationAgeBusTicks = txBounds.correlationAgeBusTicks;
        rec.eventCode = eventCode;
        rec.selectedImage = selectedImage;
        rec.groupPhase = phase;

        ++sampledCount_;
        switch (outcome) {
            case TxLatencyOutcome::Matched:
                ++matchedCount_;
                break;
            case TxLatencyOutcome::Substituted:
                ++substitutedCount_;
                break;
            case TxLatencyOutcome::Unresolved:
                ++unresolvedCount_;
                CountUnresolvedReason(reason);
                break;
            case TxLatencyOutcome::AgedOut:
                ++agedOutCount_;
                CountUnresolvedReason(reason);
                break;
            case TxLatencyOutcome::TransmitFailed:
                ++transmitFailedCount_;
                break;
            case TxLatencyOutcome::Invalid:
                ++invalidCount_;
                break;
        }

        writerActive_.store(0, std::memory_order_release);
    }

    void NoteStampsMissed(uint64_t count) noexcept {
        stampsMissedCount_ += count;
    }

    [[nodiscard]] TxLatencySessionState State() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t SessionId() const noexcept {
        return sessionId_;
    }

    /// Read session header. Valid in any state.
    void ReadHeader(TxLatencySessionHeader& outHeader) const noexcept {
        outHeader.version = kTxLatencySessionWireVersion;
        outHeader.sessionId = sessionId_;
        outHeader.state = state_.load(std::memory_order_acquire);
        outHeader.terminationReason = terminationReason_;
        outHeader.epoch = epoch_;
        outHeader.sampleRateHz = sampleRateHz_;
        outHeader.samplingSeed = samplingSeed_;
        outHeader.strataSize = strataSize_;
        outHeader.sessionStartHostTicks = startHostTicks_;
        outHeader.sessionDeadlineHostTicks = deadlineHostTicks_;
        outHeader.sessionFrozenHostTicks = frozenHostTicks_;

        outHeader.dataPacketsSeen = dataPacketsSeen_;
        outHeader.eligibleByPhase = eligibleByPhase_;
        outHeader.sampledByPhase = sampledByPhase_;

        outHeader.sampledCount = sampledCount_;
        outHeader.matchedCount = matchedCount_;
        outHeader.substitutedCount = substitutedCount_;
        outHeader.unresolvedCount = unresolvedCount_;
        outHeader.agedOutCount = agedOutCount_;
        outHeader.transmitFailedCount = transmitFailedCount_;
        outHeader.invalidCount = invalidCount_;
        outHeader.stampsMissedCount = stampsMissedCount_;

        outHeader.reasonStaleCorrelation = reasonStaleCorrelation_;
        outHeader.reasonCoveragePending = reasonCoveragePending_;
        outHeader.reasonCoverageGap = reasonCoverageGap_;
        outHeader.reasonEpochMismatch = reasonEpochMismatch_;
        outHeader.reasonPublicationAgedOut = reasonPublicationAgedOut_;
        outHeader.reasonProvenanceAgedOut = reasonProvenanceAgedOut_;
        outHeader.reasonUnrecognizedEventCode = reasonUnrecognizedEventCode_;
        outHeader.reasonImageUnavailable = reasonImageUnavailable_;

        outHeader.recordCount = recordCount_;
    }

    /// Paged record reader. Permitted ONLY when session is Frozen.
    [[nodiscard]] uint32_t ReadRecordsPage(uint32_t cursor,
                                           uint32_t maxRecords,
                                           TxLatencyRecord* outRecords) const noexcept {
        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::Frozen) {
            return 0;
        }
        if (!outRecords || cursor >= recordCount_) return 0;
        const uint32_t available = recordCount_ - cursor;
        const uint32_t toCopy = std::min(maxRecords, available);
        for (uint32_t i = 0; i < toCopy; ++i) {
            outRecords[i] = records_[cursor + i];
        }
        return toCopy;
    }

    /// Paged wire serializer.
    [[nodiscard]] bool CopyWirePage(uint32_t pageIndex,
                                    uint32_t samplesPerPage,
                                    uint64_t endpointId,
                                    UserClient::Wire::TxLatencyResultsPageWire& out) const noexcept {
        out = {};
        out.header.version = UserClient::Wire::kTxLatencyWireVersion;
        out.header.sessionState = static_cast<uint32_t>(state_.load(std::memory_order_acquire));
        out.header.terminationReason = static_cast<uint32_t>(terminationReason_);
        out.header.endpointId = endpointId;
        out.header.epoch = epoch_;
        out.header.startHostTicks = startHostTicks_;
        out.header.deadlineHostTicks = deadlineHostTicks_;
        out.header.frozenHostTicks = frozenHostTicks_;
        out.header.durationSeconds = durationSeconds_;
        out.header.strataSize = strataSize_;
        out.header.seed = samplingSeed_;
        out.header.assumedDriftPpm = assumedDriftPpm_;
        out.header.dataPacketsSeen = dataPacketsSeen_;
        out.header.samplesCaptured = recordCount_;
        out.header.stampsMissedCount = static_cast<uint32_t>(stampsMissedCount_);
        out.header.resolvedCount = static_cast<uint32_t>(matchedCount_);
        out.header.unresolvedCount = static_cast<uint32_t>(unresolvedCount_);
        out.header.transmitFailedCount = static_cast<uint32_t>(transmitFailedCount_);
        out.header.substitutionCount = static_cast<uint32_t>(substitutedCount_);
        out.header.invalidCount = static_cast<uint32_t>(invalidCount_);

        for (size_t i = 0; i < 8; ++i) {
            out.header.eligibleByPhase[i] = static_cast<uint32_t>(eligibleByPhase_[i]);
        }

        out.header.reasonStaleCorrelation = static_cast<uint32_t>(reasonStaleCorrelation_);
        out.header.reasonCoveragePending = static_cast<uint32_t>(reasonCoveragePending_);
        out.header.reasonCoverageGap = static_cast<uint32_t>(reasonCoverageGap_);
        out.header.reasonEpochMismatch = static_cast<uint32_t>(reasonEpochMismatch_);
        out.header.reasonPublicationAgedOut = static_cast<uint32_t>(reasonPublicationAgedOut_);
        out.header.reasonProvenanceAgedOut = static_cast<uint32_t>(reasonProvenanceAgedOut_);
        out.header.reasonUnrecognizedEventCode = static_cast<uint32_t>(reasonUnrecognizedEventCode_);
        out.header.reasonImageUnavailable = static_cast<uint32_t>(reasonImageUnavailable_);

        out.header.totalRingRecords = recordCount_;
        out.header.ringHead = recordCount_;
        out.header.ringTail = 0;

        const uint32_t perPage = std::min(samplesPerPage == 0 ? UserClient::Wire::kTxLatencyMaxSamplesPerPage : samplesPerPage,
                                          UserClient::Wire::kTxLatencyMaxSamplesPerPage);
        if (recordCount_ == 0) {
            out.pageIndex = 0;
            out.totalPages = 0;
            out.samplesInPage = 0;
            return true;
        }

        const uint32_t totalPages = (recordCount_ + perPage - 1) / perPage;
        out.pageIndex = pageIndex;
        out.totalPages = totalPages;

        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::Frozen || pageIndex >= totalPages) {
            out.samplesInPage = 0;
            return true;
        }

        const uint32_t cursor = pageIndex * perPage;
        const uint32_t count = std::min(perPage, recordCount_ - cursor);
        out.samplesInPage = count;

        for (uint32_t i = 0; i < count; ++i) {
            const auto& rec = records_[cursor + i];
            auto& sw = out.samples[i];
            sw.packetIndex = rec.packetIndex;
            sw.pcmCommittedStartFrame = rec.firstAudioFrame;
            sw.pcmCommittedEndFrame = rec.firstAudioFrame + rec.frameCount;
            sw.pubLatestHostTicks = rec.payloadReadyHostTicksLatest;
            sw.txCycleStartHostTicks = rec.txEarliestHostTicks;
            sw.uncertaintyHostTicks = (rec.txLatestHostTicks > rec.txEarliestHostTicks)
                                          ? static_cast<uint32_t>(rec.txLatestHostTicks - rec.txEarliestHostTicks)
                                          : 0;
            sw.waitMinNanos = (rec.txEarliestHostTicks >= rec.payloadReadyHostTicksLatest && rec.payloadReadyHostTicksLatest != 0)
                                  ? static_cast<uint32_t>(Timing::hostTicksToNanos(rec.txEarliestHostTicks - rec.payloadReadyHostTicksLatest))
                                  : 0;
            sw.waitMaxNanos = (rec.txLatestHostTicks >= rec.payloadReadyHostTicksLatest && rec.payloadReadyHostTicksLatest != 0)
                                  ? static_cast<uint32_t>(Timing::hostTicksToNanos(rec.txLatestHostTicks - rec.payloadReadyHostTicksLatest))
                                  : 0;
            sw.outcome = static_cast<uint8_t>(rec.outcome);
            sw.unresolvedReason = static_cast<uint8_t>(rec.unresolvedReason);
            sw.selectedImage = rec.selectedImage;
            sw.arbitrationPhase = rec.groupPhase;
            sw.pcmIdentityProven = (rec.outcome == TxLatencyOutcome::Matched) ? 1 : 0;
        }
        return true;
    }

private:
    uint32_t NextPrng() noexcept {
        uint32_t x = prngState_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        prngState_ = x;
        return x;
    }

    void CountUnresolvedReason(TxLatencyUnresolvedReason reason) noexcept {
        switch (reason) {
            case TxLatencyUnresolvedReason::None:
                break;
            case TxLatencyUnresolvedReason::StaleCorrelation:
                ++reasonStaleCorrelation_;
                break;
            case TxLatencyUnresolvedReason::CoveragePending:
                ++reasonCoveragePending_;
                break;
            case TxLatencyUnresolvedReason::CoverageGap:
                ++reasonCoverageGap_;
                break;
            case TxLatencyUnresolvedReason::EpochMismatch:
                ++reasonEpochMismatch_;
                break;
            case TxLatencyUnresolvedReason::PublicationAgedOut:
                ++reasonPublicationAgedOut_;
                break;
            case TxLatencyUnresolvedReason::ProvenanceAgedOut:
                ++reasonProvenanceAgedOut_;
                break;
            case TxLatencyUnresolvedReason::UnrecognizedEventCode:
                ++reasonUnrecognizedEventCode_;
                break;
            case TxLatencyUnresolvedReason::ImageUnavailable:
                ++reasonImageUnavailable_;
                break;
        }
    }

    std::atomic<TxLatencySessionState> state_{TxLatencySessionState::Idle};
    std::atomic<uint32_t> writerActive_{0};

    uint32_t sessionId_{0};
    uint64_t epoch_{0};
    uint32_t sampleRateHz_{0};
    uint32_t durationSeconds_{5};
    uint32_t sampleBudget_{kTxLatencyMaxSamples};
    uint32_t samplingSeed_{0};
    uint32_t prngState_{0};
    uint32_t strataSize_{1};
    uint32_t currentStratumOffset_{0};
    uint32_t targetOffsetInStratum_{0};
    uint32_t assumedDriftPpm_{100};

    uint64_t startHostTicks_{0};
    uint64_t deadlineHostTicks_{0};
    uint64_t frozenHostTicks_{0};
    TxLatencyTerminationReason terminationReason_{TxLatencyTerminationReason::None};

    uint64_t dataPacketsSeen_{0};
    std::array<uint64_t, 8> eligibleByPhase_{};
    std::array<uint64_t, 8> sampledByPhase_{};

    uint64_t sampledCount_{0};
    uint64_t matchedCount_{0};
    uint64_t substitutedCount_{0};
    uint64_t unresolvedCount_{0};
    uint64_t agedOutCount_{0};
    uint64_t transmitFailedCount_{0};
    uint64_t invalidCount_{0};
    uint64_t stampsMissedCount_{0};

    uint64_t reasonStaleCorrelation_{0};
    uint64_t reasonCoveragePending_{0};
    uint64_t reasonCoverageGap_{0};
    uint64_t reasonEpochMismatch_{0};
    uint64_t reasonPublicationAgedOut_{0};
    uint64_t reasonProvenanceAgedOut_{0};
    uint64_t reasonUnrecognizedEventCode_{0};
    uint64_t reasonImageUnavailable_{0};

    uint32_t recordCount_{0};
    std::array<TxLatencyRecord, kTxLatencyMaxSamples> records_{};
};

} // namespace ASFW::Audio::Runtime
