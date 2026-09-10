// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Audio-owned TX latency measurement session: state machine, stratified
// pseudo-random sampling, sample storage, and quiescent drain protocol.

#pragma once

#include "PublicationRangeRing.hpp"
#include "TxLatencyMeasurement.hpp"
#include "TxLatencySessionResult.hpp"
#include "../Ports/ITxPcmSource.hpp"
#include "../Shared/AudioTimingGeometry.hpp"
#include "../Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "../../UserClient/WireFormats/TxLatencySessionWireFormats.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW::Audio::Runtime {

class SpinLockGuard final {
public:
    explicit SpinLockGuard(std::atomic_flag& flag) noexcept : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
            #elif defined(__aarch64__)
            asm volatile("yield");
            #endif
        }
    }
    ~SpinLockGuard() noexcept {
        flag_.clear(std::memory_order_release);
    }
    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;
private:
    std::atomic_flag& flag_;
};

class SeqlockWriteGuard final {
public:
    explicit SeqlockWriteGuard(std::atomic<uint32_t>& seq) noexcept
        : seq_(seq) {
        seq_.fetch_add(1, std::memory_order_release);
    }
    ~SeqlockWriteGuard() noexcept {
        seq_.fetch_add(1, std::memory_order_release);
    }
    SeqlockWriteGuard(const SeqlockWriteGuard&) = delete;
    SeqlockWriteGuard& operator=(const SeqlockWriteGuard&) = delete;
    SeqlockWriteGuard(SeqlockWriteGuard&&) = delete;
    SeqlockWriteGuard& operator=(SeqlockWriteGuard&&) = delete;
private:
    std::atomic<uint32_t>& seq_;
};

class TxLatencySession final {
public:
    [[nodiscard]] static constexpr uint64_t PackLifecycle(
        uint32_t generation,
        TxLatencySessionState state,
        TxLatencyTerminationReason reason) noexcept {
        return (static_cast<uint64_t>(generation) << 32) |
               (static_cast<uint64_t>(static_cast<uint16_t>(state)) << 16) |
               static_cast<uint64_t>(static_cast<uint16_t>(reason));
    }

    [[nodiscard]] static constexpr uint32_t LifecycleGeneration(uint64_t lc) noexcept {
        return static_cast<uint32_t>(lc >> 32);
    }

    [[nodiscard]] static constexpr TxLatencySessionState LifecycleState(uint64_t lc) noexcept {
        return static_cast<TxLatencySessionState>(static_cast<uint16_t>((lc >> 16) & 0xFFFFU));
    }

    [[nodiscard]] static constexpr TxLatencyTerminationReason LifecycleTerminationReason(uint64_t lc) noexcept {
        return static_cast<TxLatencyTerminationReason>(static_cast<uint16_t>(lc & 0xFFFFU));
    }

    TxLatencySession() noexcept = default;

    TxLatencySession(const TxLatencySession&) = delete;
    TxLatencySession& operator=(const TxLatencySession&) = delete;

    /// Check if session deadline has passed without completions arriving.
    bool CheckExpiration(uint64_t currentHostTicks = 0) noexcept {
        const uint64_t cur = lifecycle_.load(std::memory_order_acquire);
        if (LifecycleState(cur) != TxLatencySessionState::Capturing) {
            return false;
        }
        if (currentHostTicks == 0) {
            currentHostTicks = mach_absolute_time();
        }
        if (currentHostTicks >= deadlineHostTicks_) {
            RequestStop(TxLatencyTerminationReason::DeadlineExpired, LifecycleGeneration(cur));
            PollQuiescence();
            return true;
        }
        return false;
    }

    /// Graceful stream reset: freeze in-flight session with StreamReset reason instead of wiping to Idle.
    void HandleStreamReset() noexcept {
        const uint64_t cur = lifecycle_.load(std::memory_order_acquire);
        const auto st = LifecycleState(cur);
        if (st == TxLatencySessionState::Capturing || st == TxLatencySessionState::StopRequested) {
            RequestStop(TxLatencyTerminationReason::StreamReset, LifecycleGeneration(cur));
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
                           uint32_t assumedDriftPpm = 100,
                           uint32_t preparationLeadPackets = Shared::AudioTimingGeometry::kTxPreparationLeadPackets,
                           uint32_t hardwareRingPackets = Shared::AudioTimingGeometry::kTxHardwareRingPackets,
                           const PublicationRangeRing* pubRing = nullptr) noexcept {
        uint64_t cur = lifecycle_.load(std::memory_order_acquire);
        const auto current = LifecycleState(cur);
        if (current != TxLatencySessionState::Idle && current != TxLatencySessionState::Frozen) {
            return false;
        }

        // Wait until all writers from previous session are completely drained.
        if (activeWriters_.load(std::memory_order_seq_cst) != 0) {
            return false;
        }

        const uint32_t nextGen = LifecycleGeneration(cur) + 1;
        const uint64_t armingLifecycle = PackLifecycle(nextGen, TxLatencySessionState::Arming, TxLatencyTerminationReason::None);

        if (!lifecycle_.compare_exchange_strong(cur, armingLifecycle, std::memory_order_seq_cst)) {
            return false;
        }

        // Re-verify no writers registered while transitioning to Arming.
        if (activeWriters_.load(std::memory_order_seq_cst) != 0) {
            lifecycle_.store(cur, std::memory_order_seq_cst);
            return false;
        }

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
        preparationLeadPackets_ = (preparationLeadPackets > 0) ? preparationLeadPackets : Shared::AudioTimingGeometry::kTxPreparationLeadPackets;
        hardwareRingPackets_ = (hardwareRingPackets > 0) ? hardwareRingPackets : Shared::AudioTimingGeometry::kTxHardwareRingPackets;
        publicationRing_.store(pubRing, std::memory_order_release);
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
        {
            SpinLockGuard guard(completedResultLock_);
            completedResult_.reset();
        }
        {
            SpinLockGuard guard(pendingJoinsLock_);
            for (auto& pj : pendingJoins_) {
                pj.active = false;
            }
        }

        startHostTicks_ = mach_absolute_time();
        const uint64_t durationNs = static_cast<uint64_t>(durationSeconds_) * 1'000'000'000ULL;
        deadlineHostTicks_ = startHostTicks_ + Timing::nanosToHostTicks(durationNs);
        frozenHostTicks_ = 0;

        lifecycle_.store(PackLifecycle(nextGen, TxLatencySessionState::Capturing, TxLatencyTerminationReason::None),
                         std::memory_order_seq_cst);
        return true;
    }

    /// Request session stop. Atomically transitions both generation and state to StopRequested.
    void RequestStop(TxLatencyTerminationReason reason = TxLatencyTerminationReason::UserStopped,
                     uint32_t expectedGeneration = 0) noexcept {
        uint64_t cur = lifecycle_.load(std::memory_order_acquire);
        const uint32_t targetGen = (expectedGeneration != 0)
                                       ? expectedGeneration
                                       : LifecycleGeneration(cur);
        if (targetGen == 0) {
            return;
        }

        while (LifecycleGeneration(cur) == targetGen &&
               LifecycleState(cur) == TxLatencySessionState::Capturing) {
            const uint64_t desired = PackLifecycle(targetGen, TxLatencySessionState::StopRequested, reason);
            if (lifecycle_.compare_exchange_weak(cur, desired,
                                                 std::memory_order_seq_cst,
                                                 std::memory_order_acquire)) {
                (void)TryFinalize(targetGen);
                return;
            }
        }

        // If already in StopRequested for targetGen, attempt finalization if writers have now drained.
        if (LifecycleGeneration(cur) == targetGen &&
            LifecycleState(cur) == TxLatencySessionState::StopRequested) {
            (void)TryFinalize(targetGen);
        }
    }

    /// Synchronize and finalize session into Frozen state once writers have drained.
    /// Uses an atomic claim latch on finalizedGeneration_ so exactly one caller
    /// records termination metadata and publishes Frozen for this session generation.
    bool TryFinalize(uint32_t expectedGeneration = 0) noexcept {
        uint64_t cur = lifecycle_.load(std::memory_order_acquire);
        const uint32_t targetGen = (expectedGeneration != 0)
                                       ? expectedGeneration
                                       : LifecycleGeneration(cur);
        if (targetGen == 0) {
            return false;
        }
        if (LifecycleGeneration(cur) != targetGen) {
            return false;
        }
        if (LifecycleState(cur) != TxLatencySessionState::StopRequested) {
            return false;
        }
        if (activeWriters_.load(std::memory_order_acquire) != 0) {
            return false;
        }

        uint32_t currentFinalized = finalizedGeneration_.load(std::memory_order_acquire);
        while (currentFinalized < targetGen) {
            if (finalizedGeneration_.compare_exchange_weak(
                    currentFinalized, targetGen,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
        if (currentFinalized >= targetGen) {
            return false; // Another caller already claimed/finalized for targetGen or newer.
        }

        // Re-verify that lifecycle_ has not moved and state is still StopRequested for targetGen.
        cur = lifecycle_.load(std::memory_order_acquire);
        if (LifecycleGeneration(cur) != targetGen ||
            LifecycleState(cur) != TxLatencySessionState::StopRequested) {
            return false;
        }

        // Perform final retry of pending joins before taking immutable snapshot
        const auto* ring = publicationRing_.load(std::memory_order_acquire);
        if (ring) {
            RetryPendingJoins(*ring);
        }

        // Exclusive owner of finalization for targetGen:
        TxLatencyTerminationReason reason = LifecycleTerminationReason(cur);
        if (reason == TxLatencyTerminationReason::None) {
            reason = TxLatencyTerminationReason::UserStopped;
        }
        terminationReason_ = reason;
        frozenHostTicks_ = mach_absolute_time();

        // Create immutable result snapshot BEFORE exposing Frozen!
        auto res = std::make_shared<TxLatencySessionResult>();
        PopulateHeaderLocked(res->header);
        res->header.state = TxLatencySessionState::Frozen;
        res->header.terminationReason = reason;
        res->header.sessionFrozenHostTicks = frozenHostTicks_;
        res->header.assumedDriftPpm = assumedDriftPpm_;
        const uint32_t cnt = std::min(recordCount_.load(std::memory_order_acquire), kTxLatencyMaxSamples);
        res->records.assign(records_.begin(), records_.begin() + cnt);
        res->preparationLeadPackets = preparationLeadPackets_;
        res->hardwareRingPackets = hardwareRingPackets_;

        {
            SpinLockGuard guard(completedResultLock_);
            completedResult_ = std::move(res);
        }

        {
            SeqlockWriteGuard seqGuard(statusSeq_);
            // Publish Frozen AFTER completedResult_ is published and metadata is fully written.
            lifecycle_.store(PackLifecycle(targetGen, TxLatencySessionState::Frozen, reason),
                             std::memory_order_release);
        }
        return true;
    }

    [[nodiscard]] std::shared_ptr<const TxLatencySessionResult> GetCompletedResult() const noexcept {
        SpinLockGuard guard(completedResultLock_);
        return completedResult_;
    }

    /// Check if in-flight writer has exited and publish Frozen.
    void PollQuiescence(const PublicationRangeRing* pubRing = nullptr) noexcept {
        if (pubRing != nullptr) {
            publicationRing_.store(pubRing, std::memory_order_release);
            RetryPendingJoins(*pubRing);
        }
        (void)CheckExpiration();
        (void)TryFinalize();
    }

    /// Reset session to idle on explicit teardown or rearm.
    void Reset() noexcept {
        const uint32_t curGen = LifecycleGeneration(lifecycle_.load(std::memory_order_relaxed));
        lifecycle_.store(PackLifecycle(curGen, TxLatencySessionState::Idle, TxLatencyTerminationReason::None),
                         std::memory_order_seq_cst);
        activeWriters_.store(0, std::memory_order_seq_cst);
        recordCount_.store(0, std::memory_order_relaxed);
        terminationReason_ = TxLatencyTerminationReason::None;
        frozenHostTicks_ = 0;
        {
            SpinLockGuard guard(completedResultLock_);
            completedResult_.reset();
        }
        for (auto& pj : pendingJoins_) {
            pj.active = false;
        }
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
        const uint64_t admissionLc = lifecycle_.load(std::memory_order_acquire);
        const uint32_t myGen = LifecycleGeneration(admissionLc);
        struct WriterGuard {
            TxLatencySession& session;
            ~WriterGuard() {
                session.activeWriters_.fetch_sub(1, std::memory_order_seq_cst);
            }
        } guard{*this};

        // 2. Check state.
        if (myGen == 0 || LifecycleState(admissionLc) != TxLatencySessionState::Capturing) {
            return;
        }

        // 3. Discard any old completions predating this session's start time.
        if (currentHostNow < startHostTicks_) {
            return;
        }

        // 4. Deadline check: stop automatically if session deadline has expired.
        if (currentHostNow >= deadlineHostTicks_) {
            publicationRing_.store(&publicationRing, std::memory_order_release);
            RetryPendingJoins(publicationRing);
            RequestStop(TxLatencyTerminationReason::DeadlineExpired, myGen);
            return;
        }

        const auto* slot = timeline.SlotByIndex(static_cast<uint32_t>(packetIndex));
        if (!slot || !slot->isData || slot->framesInPacket == 0) {
            return;
        }

        // Epoch mismatch causes immediate stop to prevent mixing coordinates.
        if (slot->epoch != epoch_) {
            RequestStop(TxLatencyTerminationReason::EpochChanged, myGen);
            return;
        }

        const uint8_t phase = static_cast<uint8_t>(slot->cycleOrdinal % 8);

        publicationRing_.store(&publicationRing, std::memory_order_release);
        RetryPendingJoins(publicationRing);

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
            return;
        }

        // Verify generation and state are still valid before claiming a slot.
        const uint64_t preSlotLc = lifecycle_.load(std::memory_order_acquire);
        if (LifecycleGeneration(preSlotLc) != myGen ||
            LifecycleState(preSlotLc) != TxLatencySessionState::Capturing) {
            return;
        }

        const uint32_t curRecords = recordCount_.load(std::memory_order_relaxed);
        if (curRecords >= sampleBudget_ || curRecords >= kTxLatencyMaxSamples) {
            RequestStop(TxLatencyTerminationReason::CapacityReached, myGen);
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
        const uint64_t preCommitLc = lifecycle_.load(std::memory_order_acquire);
        if (LifecycleGeneration(preCommitLc) != myGen ||
            LifecycleState(preCommitLc) != TxLatencySessionState::Capturing) {
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
        rec.packetGeneration = static_cast<uint8_t>(provenance.commitGeneration & 0xFF);
        rec.pcmIdentityProven = haveProvenance ? 1 : 0;
        rec.validityFlags = ComputeTxLatencyValidityFlags(pubEarliest, pubLatest, txBounds, haveProvenance);

        if (outcome == TxLatencyOutcome::Unresolved &&
            reason == TxLatencyUnresolvedReason::CoveragePending) {
            SpinLockGuard guard(pendingJoinsLock_);
            for (auto& pj : pendingJoins_) {
                if (!pj.active) {
                    pj.packetIndex = packetIndex;
                    pj.recordIndex = curRecords;
                    pj.epoch = epoch_;
                    pj.firstAudioFrame = slot->firstAudioFrame;
                    pj.frameCount = slot->framesInPacket;
                    pj.txBounds = txBounds;
                    pj.eventCode = eventCode;
                    pj.selectedImage = selectedImage;
                    pj.groupPhase = phase;
                    pj.packetGeneration = rec.packetGeneration;
                    pj.pcmIdentityProven = rec.pcmIdentityProven;
                    pj.isSubstitution = isSubstitution ? 1 : 0;
                    pj.active = true;
                    break;
                }
            }
        }

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
    }

    void NoteStampsMissed(uint64_t count) noexcept {
        stampsMissedCount_.fetch_add(count, std::memory_order_relaxed);
    }

    [[nodiscard]] TxLatencySessionState State() const noexcept {
        return LifecycleState(lifecycle_.load(std::memory_order_acquire));
    }

    [[nodiscard]] uint32_t SessionId() const noexcept {
        return sessionId_;
    }

    [[nodiscard]] uint32_t SessionGeneration() const noexcept {
        return LifecycleGeneration(lifecycle_.load(std::memory_order_relaxed));
    }

    [[nodiscard]] uint32_t StatusSequence() const noexcept {
        return statusSeq_.load(std::memory_order_relaxed);
    }

    /// Populate session header without triggering quiescence polling or side effects.
    void PopulateHeaderLocked(TxLatencySessionHeader& outHeader) const noexcept {
        const auto snap = ReadCountersSnapshot();
        outHeader.version = kTxLatencySessionWireVersion;
        outHeader.sessionId = sessionId_;
        const uint64_t lc = lifecycle_.load(std::memory_order_acquire);
        const auto st = LifecycleState(lc);
        outHeader.state = st;
        if (st == TxLatencySessionState::Frozen) {
            outHeader.terminationReason = terminationReason_;
            outHeader.sessionFrozenHostTicks = frozenHostTicks_;
        } else {
            outHeader.terminationReason = TxLatencyTerminationReason::None;
            outHeader.sessionFrozenHostTicks = 0;
        }
        outHeader.epoch = epoch_;
        outHeader.sampleRateHz = sampleRateHz_;
        outHeader.samplingSeed = samplingSeed_;
        outHeader.strataSize = strataSize_;
        outHeader.assumedDriftPpm = assumedDriftPpm_;
        outHeader.sessionStartHostTicks = startHostTicks_;
        outHeader.sessionDeadlineHostTicks = deadlineHostTicks_;

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

    /// Read session header with synchronized seqlock snapshot. Valid in any state.
    void ReadHeader(TxLatencySessionHeader& outHeader) const noexcept {
        PopulateHeaderLocked(outHeader);
    }

    /// Paged record reader. Permitted ONLY when session is Frozen.
    [[nodiscard]] uint32_t ReadRecordsPage(uint32_t cursor,
                                           uint32_t maxRecords,
                                           TxLatencyRecord* outRecords) const noexcept {
        if (State() != TxLatencySessionState::Frozen) {
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
                                    UserClient::Wire::TxLatencyResultsPageWire& out) const noexcept {
        if (requestedSessionId != 0 && sessionId_ != requestedSessionId) {
            return false;
        }

        const uint64_t lc = lifecycle_.load(std::memory_order_acquire);
        const auto st = LifecycleState(lc);

        if (st == TxLatencySessionState::Frozen) {
            auto completed = GetCompletedResult();
            if (completed) {
                return completed->CopyWirePage(pageIndex, samplesPerPage, requestedSessionId, endpointId, out);
            }
        }

        const auto snap = ReadCountersSnapshot();
        out = {};
        out.header.version = UserClient::Wire::kTxLatencyWireVersion;
        out.header.sessionState = static_cast<uint32_t>(st);
        if (st == TxLatencySessionState::Frozen) {
            out.header.terminationReason = static_cast<uint32_t>(terminationReason_);
            out.header.frozenHostTicks = frozenHostTicks_;
        } else {
            out.header.terminationReason = static_cast<uint32_t>(TxLatencyTerminationReason::None);
            out.header.frozenHostTicks = 0;
        }
        out.header.sessionId = sessionId_;
        out.header.endpointId = endpointId;
        out.header.epoch = epoch_;
        out.header.startHostTicks = startHostTicks_;
        out.header.deadlineHostTicks = deadlineHostTicks_;
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
        out.header.sampleRateHz = sampleRateHz_;

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
        out.header.geometryProvenance = (hardwareRingPackets_ << 16) | (preparationLeadPackets_ & 0xFFFF);

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

        if (st != TxLatencySessionState::Frozen || pageIndex >= totalPages) {
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

    std::atomic<uint64_t> lifecycle_{PackLifecycle(0, TxLatencySessionState::Idle, TxLatencyTerminationReason::None)};
    std::atomic<uint32_t> activeWriters_{0};
    std::atomic<uint32_t> finalizedGeneration_{0};
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

    struct PendingJoinEntry {
        uint64_t packetIndex{0};
        uint32_t recordIndex{0};
        uint64_t epoch{0};
        uint64_t firstAudioFrame{0};
        uint32_t frameCount{0};
        TransmitBounds txBounds{};
        uint16_t eventCode{0};
        uint8_t selectedImage{0};
        uint8_t groupPhase{0};
        uint8_t packetGeneration{0};
        uint8_t pcmIdentityProven{0};
        uint8_t isSubstitution{0};
        bool active{false};
    };

    void RetryPendingJoins(const PublicationRangeRing& publicationRing) noexcept {
        SpinLockGuard guard(pendingJoinsLock_);
        SeqlockWriteGuard seqGuard(statusSeq_);
        for (auto& pj : pendingJoins_) {
            if (!pj.active) {
                continue;
            }
            uint64_t pubEarliest = 0;
            uint64_t pubLatest = 0;
            const auto coverage = publicationRing.LookupPacketCoverage(
                pj.epoch, pj.firstAudioFrame, pj.frameCount, pubEarliest, pubLatest);
            if (coverage == PublicationCoverageResult::Pending) {
                continue;
            }

            TxLatencyUnresolvedReason reason = TxLatencyUnresolvedReason::None;
            const auto outcome = ClassifyTxLatencySample(
                pj.eventCode, pj.pcmIdentityProven != 0, pj.isSubstitution != 0,
                coverage, pj.txBounds, pubEarliest, pubLatest, reason);

            if (pj.recordIndex < kTxLatencyMaxSamples) {
                auto& rec = records_[pj.recordIndex];
                rec.payloadReadyHostTicksEarliest = pubEarliest;
                rec.payloadReadyHostTicksLatest = pubLatest;
                rec.outcome = outcome;
                rec.unresolvedReason = reason;
                rec.validityFlags = ComputeTxLatencyValidityFlags(
                    pubEarliest, pubLatest, pj.txBounds, pj.pcmIdentityProven != 0);

                unresolvedCount_.fetch_sub(1, std::memory_order_relaxed);
                reasonCoveragePending_.fetch_sub(1, std::memory_order_relaxed);

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
            }
            pj.active = false;
        }
    }

    std::atomic<uint32_t> recordCount_{0};
    std::array<TxLatencyRecord, kTxLatencyMaxSamples> records_{};
    mutable std::atomic_flag completedResultLock_{};
    std::shared_ptr<const TxLatencySessionResult> completedResult_{};
    mutable std::atomic_flag pendingJoinsLock_{};
    std::atomic<const PublicationRangeRing*> publicationRing_{nullptr};
    uint32_t preparationLeadPackets_{Shared::AudioTimingGeometry::kTxPreparationLeadPackets};
    uint32_t hardwareRingPackets_{Shared::AudioTimingGeometry::kTxHardwareRingPackets};
    std::array<PendingJoinEntry, 16> pendingJoins_{};
};

} // namespace ASFW::Audio::Runtime
