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

    TxLatencySession(const TxLatencySession&) = delete;
    TxLatencySession& operator=(const TxLatencySession&) = delete;

    /// Check if session deadline has passed without completions arriving.
    bool CheckExpiration(uint64_t currentHostTicks = 0) noexcept {
        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::Capturing) {
            return false;
        }
        if (currentHostTicks == 0) {
            currentHostTicks = mach_absolute_time();
        }
        if (currentHostTicks >= deadlineHostTicks_) {
            RequestStop(TxLatencyTerminationReason::DeadlineExpired);
            PollQuiescence();
            return true;
        }
        return false;
    }

    /// Graceful stream reset: freeze in-flight session with StreamReset reason instead of wiping to Idle.
    void HandleStreamReset() noexcept {
        const auto st = state_.load(std::memory_order_acquire);
        if (st == TxLatencySessionState::Capturing || st == TxLatencySessionState::StopRequested) {
            RequestStop(TxLatencyTerminationReason::StreamReset);
            PollQuiescence();
        }
    }

    /// Arm a new measurement session. Returns false if not in Idle or Frozen state,
    /// or if in-flight writers from a prior session are still active.
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

        // Wait until all writers from previous session are completely drained.
        if (activeWriters_.load(std::memory_order_seq_cst) != 0) {
            return false;
        }

        state_.store(TxLatencySessionState::Arming, std::memory_order_seq_cst);

        // Re-verify no writers registered while transitioning to Arming.
        if (activeWriters_.load(std::memory_order_seq_cst) != 0) {
            state_.store(current, std::memory_order_seq_cst);
            return false;
        }

        // Advance session generation. Any stale writer that enters will detect this bump.
        sessionGeneration_.fetch_add(1, std::memory_order_seq_cst);

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

        statusSeq_.store(0, std::memory_order_release);
        dataPacketsSeen_.store(0, std::memory_order_relaxed);
        for (auto& e : eligibleByPhase_) e.store(0, std::memory_order_relaxed);
        for (auto& s : sampledByPhase_) s.store(0, std::memory_order_relaxed);

        sampledCount_.store(0, std::memory_order_relaxed);
        matchedCount_.store(0, std::memory_order_relaxed);
        substitutedCount_.store(0, std::memory_order_relaxed);
        unresolvedCount_.store(0, std::memory_order_relaxed);
        transmitFailedCount_.store(0, std::memory_order_relaxed);
        invalidCount_.store(0, std::memory_order_relaxed);
        stampsMissedCount_.store(0, std::memory_order_relaxed);

        reasonStaleCorrelation_.store(0, std::memory_order_relaxed);
        reasonCoveragePending_.store(0, std::memory_order_relaxed);
        reasonCoverageGap_.store(0, std::memory_order_relaxed);
        reasonEpochMismatch_.store(0, std::memory_order_relaxed);
        reasonPublicationAgedOut_.store(0, std::memory_order_relaxed);
        reasonProvenanceAgedOut_.store(0, std::memory_order_relaxed);
        reasonUnrecognizedEventCode_.store(0, std::memory_order_relaxed);
        reasonImageUnavailable_.store(0, std::memory_order_relaxed);

        recordCount_.store(0, std::memory_order_relaxed);
        terminationReason_ = TxLatencyTerminationReason::None;
        pendingTerminationReason_.store(TxLatencyTerminationReason::None, std::memory_order_release);
        finalizerClaimed_.store(false, std::memory_order_release);

        startHostTicks_ = mach_absolute_time();
        const uint64_t durationNs = static_cast<uint64_t>(durationSeconds_) * 1'000'000'000ULL;
        deadlineHostTicks_ = startHostTicks_ + Timing::nanosToHostTicks(durationNs);
        frozenHostTicks_ = 0;

        state_.store(TxLatencySessionState::Capturing, std::memory_order_seq_cst);
        return true;
    }

    /// Request session stop.
    void RequestStop(TxLatencyTerminationReason reason = TxLatencyTerminationReason::UserStopped) noexcept {
        // 1. Atomically store the pending termination reason if not already set.
        TxLatencyTerminationReason expectedReason = TxLatencyTerminationReason::None;
        pendingTerminationReason_.compare_exchange_strong(
            expectedReason, reason, std::memory_order_acq_rel);

        // 2. Transition state from Capturing to StopRequested.
        auto expectedState = TxLatencySessionState::Capturing;
        if (state_.compare_exchange_strong(expectedState, TxLatencySessionState::StopRequested,
                                           std::memory_order_seq_cst)) {
            (void)TryFinalize();
        } else if (expectedState == TxLatencySessionState::StopRequested) {
            // Already in StopRequested; attempt finalization if writers have now drained.
            (void)TryFinalize();
        }
    }

    /// Synchronize and finalize session into Frozen state once writers have drained.
    /// Uses an atomic claim latch to serialize finalization so exactly one caller
    /// records termination metadata and publishes Frozen.
    bool TryFinalize() noexcept {
        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::StopRequested) {
            return false;
        }
        if (activeWriters_.load(std::memory_order_acquire) != 0) {
            return false;
        }

        bool expectedClaim = false;
        if (!finalizerClaimed_.compare_exchange_strong(expectedClaim, true,
                                                      std::memory_order_acq_rel)) {
            return false; // Another caller is already finalizing or has finalized.
        }

        // Exclusive owner of finalization:
        terminationReason_ = pendingTerminationReason_.load(std::memory_order_acquire);
        if (terminationReason_ == TxLatencyTerminationReason::None) {
            terminationReason_ = TxLatencyTerminationReason::UserStopped;
        }
        frozenHostTicks_ = mach_absolute_time();

        statusSeq_.fetch_add(1, std::memory_order_release);
        // Publish Frozen AFTER metadata (terminationReason_, frozenHostTicks_) is fully written.
        state_.store(TxLatencySessionState::Frozen, std::memory_order_release);
        statusSeq_.fetch_add(1, std::memory_order_release);
        return true;
    }

    /// Check if in-flight writer has exited and publish Frozen.
    void PollQuiescence() noexcept {
        (void)CheckExpiration();
        (void)TryFinalize();
    }

    /// Reset session to idle on explicit teardown or rearm.
    void Reset() noexcept {
        state_.store(TxLatencySessionState::Idle, std::memory_order_seq_cst);
        activeWriters_.store(0, std::memory_order_seq_cst);
        recordCount_.store(0, std::memory_order_relaxed);
        finalizerClaimed_.store(false, std::memory_order_relaxed);
        pendingTerminationReason_.store(TxLatencyTerminationReason::None, std::memory_order_relaxed);
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

        // 1. Register active writer BEFORE checking state or generation.
        activeWriters_.fetch_add(1, std::memory_order_seq_cst);
        struct WriterGuard {
            TxLatencySession& session;
            ~WriterGuard() {
                if (session.activeWriters_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
                    // Last active writer drained; finalize if stop was requested.
                    (void)session.TryFinalize();
                }
            }
        } guard{*this};

        // 2. Load generation and state.
        const uint32_t myGen = sessionGeneration_.load(std::memory_order_acquire);
        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::Capturing) {
            return;
        }

        // 3. Discard any old completions predating this session's start time.
        if (currentHostNow < startHostTicks_) {
            return;
        }

        // 4. Deadline check: stop automatically if session deadline has expired.
        if (currentHostNow >= deadlineHostTicks_) {
            RequestStop(TxLatencyTerminationReason::DeadlineExpired);
            return;
        }

        const auto* slot = timeline.SlotByIndex(static_cast<uint32_t>(packetIndex));
        if (!slot || !slot->isData || slot->framesInPacket == 0) {
            return;
        }

        // Epoch mismatch causes immediate stop to prevent mixing coordinates.
        if (slot->epoch != epoch_) {
            RequestStop(TxLatencyTerminationReason::EpochChanged);
            return;
        }

        const uint8_t phase = static_cast<uint8_t>(slot->cycleOrdinal % 8);

        statusSeq_.fetch_add(1, std::memory_order_release);

        eligibleByPhase_[phase].fetch_add(1, std::memory_order_relaxed);
        dataPacketsSeen_.fetch_add(1, std::memory_order_relaxed);

        // Stratified selection check.
        const bool selectThisPacket = (currentStratumOffset_ == targetOffsetInStratum_);
        ++currentStratumOffset_;
        if (currentStratumOffset_ >= strataSize_) {
            currentStratumOffset_ = 0;
            targetOffsetInStratum_ = NextPrng() % strataSize_;
        }

        if (!selectThisPacket) {
            statusSeq_.fetch_add(1, std::memory_order_release);
            return;
        }

        // Verify generation and state are still valid before claiming a slot.
        if (sessionGeneration_.load(std::memory_order_acquire) != myGen ||
            state_.load(std::memory_order_acquire) != TxLatencySessionState::Capturing) {
            return;
        }

        const uint32_t curRecords = recordCount_.load(std::memory_order_relaxed);
        if (curRecords >= sampleBudget_ || curRecords >= kTxLatencyMaxSamples) {
            statusSeq_.fetch_add(1, std::memory_order_release);
            RequestStop(TxLatencyTerminationReason::CapacityReached);
            return;
        }

        sampledByPhase_[phase].fetch_add(1, std::memory_order_relaxed);

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
            eventCode, haveProvenance, isSubstitution, coverage, txBounds, pubEarliest, pubLatest, reason);

        // Re-verify generation before committing the sample.
        if (sessionGeneration_.load(std::memory_order_acquire) != myGen ||
            state_.load(std::memory_order_acquire) != TxLatencySessionState::Capturing) {
            return;
        }

        // Record the sample.
        auto& rec = records_[curRecords];
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

        recordCount_.store(curRecords + 1, std::memory_order_relaxed);
        sampledCount_.fetch_add(1, std::memory_order_relaxed);
        switch (outcome) {
            case TxLatencyOutcome::Matched:
                matchedCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyOutcome::Substituted:
                substitutedCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyOutcome::Unresolved:
                unresolvedCount_.fetch_add(1, std::memory_order_relaxed);
                CountUnresolvedReason(reason);
                break;
            case TxLatencyOutcome::TransmitFailed:
                transmitFailedCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyOutcome::Invalid:
                invalidCount_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyOutcome::Unknown:
                break;
        }

        statusSeq_.fetch_add(1, std::memory_order_release);
    }

    void NoteStampsMissed(uint64_t count) noexcept {
        stampsMissedCount_.fetch_add(count, std::memory_order_relaxed);
    }

    [[nodiscard]] TxLatencySessionState State() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t SessionId() const noexcept {
        return sessionId_;
    }

    /// Read session header with synchronized seqlock snapshot. Valid in any state.
    void ReadHeader(TxLatencySessionHeader& outHeader) noexcept {
        CheckExpiration();
        PollQuiescence();

        const auto snap = ReadCountersSnapshot();
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

        outHeader.dataPacketsSeen = snap.dataPacketsSeen;
        for (size_t i = 0; i < 8; ++i) {
            outHeader.eligibleByPhase[i] = snap.eligibleByPhase[i];
            outHeader.sampledByPhase[i] = sampledByPhase_[i].load(std::memory_order_relaxed);
        }

        outHeader.sampledCount = snap.samplesCaptured;
        outHeader.matchedCount = snap.resolvedCount;
        outHeader.substitutedCount = snap.substitutionCount;
        outHeader.unresolvedCount = snap.unresolvedCount;
        outHeader.agedOutCount = snap.reasonPublicationAgedOut + snap.reasonProvenanceAgedOut;
        outHeader.transmitFailedCount = snap.transmitFailedCount;
        outHeader.invalidCount = snap.invalidCount;
        outHeader.stampsMissedCount = snap.stampsMissedCount;

        outHeader.reasonStaleCorrelation = snap.reasonStaleCorrelation;
        outHeader.reasonCoveragePending = snap.reasonCoveragePending;
        outHeader.reasonCoverageGap = snap.reasonCoverageGap;
        outHeader.reasonEpochMismatch = snap.reasonEpochMismatch;
        outHeader.reasonPublicationAgedOut = snap.reasonPublicationAgedOut;
        outHeader.reasonProvenanceAgedOut = snap.reasonProvenanceAgedOut;
        outHeader.reasonUnrecognizedEventCode = snap.reasonUnrecognizedEventCode;
        outHeader.reasonImageUnavailable = snap.reasonImageUnavailable;

        outHeader.recordCount = snap.samplesCaptured;
    }

    /// Paged record reader. Permitted ONLY when session is Frozen.
    [[nodiscard]] uint32_t ReadRecordsPage(uint32_t cursor,
                                           uint32_t maxRecords,
                                           TxLatencyRecord* outRecords) const noexcept {
        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::Frozen) {
            return 0;
        }
        const uint32_t total = recordCount_.load(std::memory_order_acquire);
        if (!outRecords || cursor >= total) return 0;
        const uint32_t available = total - cursor;
        const uint32_t toCopy = std::min(maxRecords, available);
        for (uint32_t i = 0; i < toCopy; ++i) {
            outRecords[i] = records_[cursor + i];
        }
        return toCopy;
    }

    /// Paged wire serializer with synchronized status snapshot and session ID protection.
    [[nodiscard]] bool CopyWirePage(uint32_t pageIndex,
                                    uint32_t samplesPerPage,
                                    uint32_t requestedSessionId,
                                    uint64_t endpointId,
                                    UserClient::Wire::TxLatencyResultsPageWire& out) noexcept {
        CheckExpiration();
        PollQuiescence();

        if (requestedSessionId != 0 && sessionId_ != requestedSessionId) {
            return false;
        }

        const auto snap = ReadCountersSnapshot();
        out = {};
        out.header.version = UserClient::Wire::kTxLatencyWireVersion;
        out.header.sessionState = static_cast<uint32_t>(state_.load(std::memory_order_acquire));
        out.header.terminationReason = static_cast<uint32_t>(terminationReason_);
        out.header.sessionId = sessionId_;
        out.header.endpointId = endpointId;
        out.header.epoch = epoch_;
        out.header.startHostTicks = startHostTicks_;
        out.header.deadlineHostTicks = deadlineHostTicks_;
        out.header.frozenHostTicks = frozenHostTicks_;
        out.header.durationSeconds = durationSeconds_;
        out.header.strataSize = strataSize_;
        out.header.seed = samplingSeed_;
        out.header.assumedDriftPpm = assumedDriftPpm_;
        out.header.dataPacketsSeen = snap.dataPacketsSeen;
        out.header.samplesCaptured = snap.samplesCaptured;
        out.header.stampsMissedCount = static_cast<uint32_t>(snap.stampsMissedCount);
        out.header.resolvedCount = static_cast<uint32_t>(snap.resolvedCount);
        out.header.unresolvedCount = static_cast<uint32_t>(snap.unresolvedCount);
        out.header.transmitFailedCount = static_cast<uint32_t>(snap.transmitFailedCount);
        out.header.substitutionCount = static_cast<uint32_t>(snap.substitutionCount);
        out.header.invalidCount = static_cast<uint32_t>(snap.invalidCount);

        for (size_t i = 0; i < 8; ++i) {
            out.header.eligibleByPhase[i] = static_cast<uint32_t>(snap.eligibleByPhase[i]);
        }

        out.header.reasonStaleCorrelation = static_cast<uint32_t>(snap.reasonStaleCorrelation);
        out.header.reasonCoveragePending = static_cast<uint32_t>(snap.reasonCoveragePending);
        out.header.reasonCoverageGap = static_cast<uint32_t>(snap.reasonCoverageGap);
        out.header.reasonEpochMismatch = static_cast<uint32_t>(snap.reasonEpochMismatch);
        out.header.reasonPublicationAgedOut = static_cast<uint32_t>(snap.reasonPublicationAgedOut);
        out.header.reasonProvenanceAgedOut = static_cast<uint32_t>(snap.reasonProvenanceAgedOut);
        out.header.reasonUnrecognizedEventCode = static_cast<uint32_t>(snap.reasonUnrecognizedEventCode);
        out.header.reasonImageUnavailable = static_cast<uint32_t>(snap.reasonImageUnavailable);

        out.header.totalRingRecords = snap.samplesCaptured;
        out.header.ringHead = snap.samplesCaptured;
        out.header.ringTail = 0;

        const uint32_t perPage = std::min(samplesPerPage == 0 ? UserClient::Wire::kTxLatencyMaxSamplesPerPage : samplesPerPage,
                                          UserClient::Wire::kTxLatencyMaxSamplesPerPage);
        if (snap.samplesCaptured == 0) {
            out.pageIndex = 0;
            out.totalPages = 0;
            out.samplesInPage = 0;
            return true;
        }

        const uint32_t totalPages = (snap.samplesCaptured + perPage - 1) / perPage;
        out.pageIndex = pageIndex;
        out.totalPages = totalPages;

        if (state_.load(std::memory_order_acquire) != TxLatencySessionState::Frozen || pageIndex >= totalPages) {
            out.samplesInPage = 0;
            return true;
        }

        const uint32_t cursor = pageIndex * perPage;
        const uint32_t count = std::min(perPage, snap.samplesCaptured - cursor);
        out.samplesInPage = count;

        for (uint32_t i = 0; i < count; ++i) {
            const auto& rec = records_[cursor + i];
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
            sw.pcmIdentityProven = (rec.outcome == TxLatencyOutcome::Matched) ? 1 : 0;
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

    struct CountersSnapshot {
        uint64_t dataPacketsSeen{0};
        uint32_t samplesCaptured{0};
        uint64_t stampsMissedCount{0};
        uint64_t resolvedCount{0};
        uint64_t unresolvedCount{0};
        uint64_t transmitFailedCount{0};
        uint64_t substitutionCount{0};
        uint64_t invalidCount{0};
        uint64_t eligibleByPhase[8]{};
        uint64_t reasonStaleCorrelation{0};
        uint64_t reasonCoveragePending{0};
        uint64_t reasonCoverageGap{0};
        uint64_t reasonEpochMismatch{0};
        uint64_t reasonPublicationAgedOut{0};
        uint64_t reasonProvenanceAgedOut{0};
        uint64_t reasonUnrecognizedEventCode{0};
        uint64_t reasonImageUnavailable{0};
    };

    CountersSnapshot ReadCountersSnapshot() const noexcept {
        CountersSnapshot snap{};
        for (int spin = 0; spin < 10; ++spin) {
            const uint32_t seq1 = statusSeq_.load(std::memory_order_acquire);
            if (seq1 & 1) {
                continue;
            }
            snap.dataPacketsSeen = dataPacketsSeen_.load(std::memory_order_relaxed);
            snap.samplesCaptured = recordCount_.load(std::memory_order_relaxed);
            snap.stampsMissedCount = stampsMissedCount_.load(std::memory_order_relaxed);
            snap.resolvedCount = matchedCount_.load(std::memory_order_relaxed);
            snap.unresolvedCount = unresolvedCount_.load(std::memory_order_relaxed);
            snap.transmitFailedCount = transmitFailedCount_.load(std::memory_order_relaxed);
            snap.substitutionCount = substitutedCount_.load(std::memory_order_relaxed);
            snap.invalidCount = invalidCount_.load(std::memory_order_relaxed);
            for (size_t i = 0; i < 8; ++i) {
                snap.eligibleByPhase[i] = eligibleByPhase_[i].load(std::memory_order_relaxed);
            }
            snap.reasonStaleCorrelation = reasonStaleCorrelation_.load(std::memory_order_relaxed);
            snap.reasonCoveragePending = reasonCoveragePending_.load(std::memory_order_relaxed);
            snap.reasonCoverageGap = reasonCoverageGap_.load(std::memory_order_relaxed);
            snap.reasonEpochMismatch = reasonEpochMismatch_.load(std::memory_order_relaxed);
            snap.reasonPublicationAgedOut = reasonPublicationAgedOut_.load(std::memory_order_relaxed);
            snap.reasonProvenanceAgedOut = reasonProvenanceAgedOut_.load(std::memory_order_relaxed);
            snap.reasonUnrecognizedEventCode = reasonUnrecognizedEventCode_.load(std::memory_order_relaxed);
            snap.reasonImageUnavailable = reasonImageUnavailable_.load(std::memory_order_relaxed);

            const uint32_t seq2 = statusSeq_.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                return snap;
            }
        }
        return snap;
    }

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
                reasonStaleCorrelation_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::CoveragePending:
                reasonCoveragePending_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::CoverageGap:
                reasonCoverageGap_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::EpochMismatch:
                reasonEpochMismatch_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::PublicationAgedOut:
                reasonPublicationAgedOut_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::ProvenanceAgedOut:
                reasonProvenanceAgedOut_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::UnrecognizedEventCode:
                reasonUnrecognizedEventCode_.fetch_add(1, std::memory_order_relaxed);
                break;
            case TxLatencyUnresolvedReason::ImageUnavailable:
                reasonImageUnavailable_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    std::atomic<TxLatencySessionState> state_{TxLatencySessionState::Idle};
    std::atomic<uint32_t> sessionGeneration_{0};
    std::atomic<uint32_t> activeWriters_{0};
    std::atomic<TxLatencyTerminationReason> pendingTerminationReason_{TxLatencyTerminationReason::None};
    std::atomic<bool> finalizerClaimed_{false};
    mutable std::atomic<uint32_t> statusSeq_{0};

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

    std::atomic<uint64_t> dataPacketsSeen_{0};
    std::array<std::atomic<uint64_t>, 8> eligibleByPhase_{};
    std::array<std::atomic<uint64_t>, 8> sampledByPhase_{};

    std::atomic<uint64_t> sampledCount_{0};
    std::atomic<uint64_t> matchedCount_{0};
    std::atomic<uint64_t> substitutedCount_{0};
    std::atomic<uint64_t> unresolvedCount_{0};
    std::atomic<uint64_t> transmitFailedCount_{0};
    std::atomic<uint64_t> invalidCount_{0};
    std::atomic<uint64_t> stampsMissedCount_{0};

    std::atomic<uint64_t> reasonStaleCorrelation_{0};
    std::atomic<uint64_t> reasonCoveragePending_{0};
    std::atomic<uint64_t> reasonCoverageGap_{0};
    std::atomic<uint64_t> reasonEpochMismatch_{0};
    std::atomic<uint64_t> reasonPublicationAgedOut_{0};
    std::atomic<uint64_t> reasonProvenanceAgedOut_{0};
    std::atomic<uint64_t> reasonUnrecognizedEventCode_{0};
    std::atomic<uint64_t> reasonImageUnavailable_{0};

    std::atomic<uint32_t> recordCount_{0};
    std::array<TxLatencyRecord, kTxLatencyMaxSamples> records_{};
};

} // namespace ASFW::Audio::Runtime
