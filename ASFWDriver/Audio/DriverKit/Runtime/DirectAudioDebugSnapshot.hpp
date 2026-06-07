#pragma once

#include "AudioGraphBinding.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

constexpr uint64_t kDirectAudioDebugLogIntervalNs = 5'000'000'000ULL;

struct DirectAudioDebugSnapshot final {
    bool bound{false};

    uint64_t inputBufferAddress{0};
    uint64_t outputBufferAddress{0};
    uint32_t inputFrameCapacity{0};
    uint32_t outputFrameCapacity{0};
    uint32_t inputChannels{0};
    uint32_t outputChannels{0};

    uint64_t ioBeginReadCount{0};
    uint64_t ioWriteEndCount{0};

    uint64_t inputBeginReadSampleFrame{0};
    uint64_t inputClientReadEndFrame{0};

    uint64_t outputWriteEndSampleFrame{0};
    uint64_t outputClientWriteEndFrame{0};

    uint32_t inputBeginReadFrameCount{0};
    uint32_t outputWriteEndFrameCount{0};
    uint32_t ioBufferFrameSize{0};
    uint32_t expectedIoBufferFrameSize{0};

    int64_t lastSampleDelta{0};
    uint64_t sampleTimeRegressionCount{0};
    uint64_t ioBufferFrameSizeChangeCount{0};

    uint64_t directTxPackets{0};
    uint64_t directTxUnderruns{0};
    uint64_t directTxSilenceSubstitutions{0};
    bool outputReaderAvailableAtWriteEnd{false};

    uint64_t playbackRingWriteFrame{0};
    uint64_t playbackRingReadFrame{0};
    uint64_t playbackRingOldestValidFrame{0};
    uint64_t playbackRingAvailableFrames{0};
    uint64_t playbackRingUnderruns{0};
    uint64_t playbackRingOverruns{0};
    uint64_t txScheduledSampleFrame{0};
    uint64_t txCompletedSampleFrame{0};
    uint64_t txLastSourceFrame{0};
    uint64_t txPreparedSourceEndFrame{0};
    uint64_t txStartupAvailableFrames{0};
    uint64_t txAnchorSourceFrame{0};
    uint64_t txAnchorTimelineFrame{0};
    uint32_t txAnchorPacketIndex{0};
    uint32_t txAnchorDistance{0};
    uint32_t txMinimumPreparationDistance{UINT32_MAX};
    uint64_t txLastPreparationLatencyTicks{0};
    uint64_t txMaxPreparationLatencyTicks{0};
    uint64_t txForwardCursorCorrections{0};
    uint64_t txPreventedBackwardCorrections{0};
    uint64_t txStaleOverwrittenReads{0};
    uint64_t txProducerAheadUnderruns{0};
    uint64_t txTimelineDiscontinuities{0};
    uint64_t txPcmNonzeroPackets{0};
    uint64_t txPcmAllZeroPackets{0};
    uint64_t txTimelineInvariantFailures{0};
    uint64_t txPreparedPcmSlots{0};
    uint64_t txPendingSourceSlots{0};
    uint64_t txStartupSilenceSlots{0};
    uint64_t txRetiredEpochSilenceSlots{0};
    uint64_t txReadAheadFaults{0};
    uint64_t txSourceOverwrittenFaults{0};
    uint64_t txPreparationDeadlineFaults{0};
    uint64_t txSlotOwnershipFaults{0};
    uint64_t txImmediateStops{0};
    uint64_t txPreparationRequestedGeneration{0};
    uint64_t txPreparationHandledGeneration{0};
    uint64_t txPreparationRequestHostTicks{0};
    uint64_t txPreparationHandledHostTicks{0};
    uint64_t txPreparationWakeRequests{0};
    uint64_t txPreparationWakeDispatches{0};
    uint64_t txPreparationWakeCoalesced{0};
    uint64_t txPreparationDrainPasses{0};
    uint64_t txDeferredStartupWrites{0};
    uint64_t txCompletedPayloadHashMatches{0};
    uint64_t txCompletedPayloadHashMismatches{0};
    uint64_t txCompletedPcmSlots{0};
    uint64_t txCompletedStartupSilenceSlots{0};
    uint64_t txCompletedRetiredSilenceSlots{0};
    uint64_t txPayloadMismatchFaults{0};
    FatalStreamReason fatalReason{FatalStreamReason::None};
    uint64_t fatalGeneration{0};
    uint32_t fatalPacketIndex{0};
    uint32_t fatalDistanceToHardware{0};
    uint64_t fatalSourceFirstFrame{0};
    uint64_t fatalSourceEndFrame{0};
    uint64_t fatalOldestValidFrame{0};
    uint64_t fatalWrittenEndFrame{0};
    uint64_t fatalPreparedPayloadHash{0};
    uint64_t fatalCompletedPayloadHash{0};

    uint64_t captureRingWriteFrame{0};
    uint64_t captureRingReadFrame{0};
    uint64_t captureRingAvailableFrames{0};
    uint64_t captureRingOverruns{0};
    uint64_t captureRingStarvations{0};
    uint64_t rxDecodedFrames{0};

    // Phase A counters
    uint64_t txValidPhasePcmPackets{0};
    uint64_t txValidPhaseSilencePackets{0};
    uint64_t txNoPhaseSilencePackets{0};
    uint64_t txUnderrunSilencePackets{0};
    uint64_t txStaleSyncPackets{0};
    uint64_t txInvalidGeometryPackets{0};
};

struct DirectAudioDebugLogState final {
    uint64_t lastLogTimeNs{0};
    bool hasLogged{false};
    bool lastBound{false};

    void Reset() noexcept {
        lastLogTimeNs = 0;
        hasLogged = false;
        lastBound = false;
    }
};

[[nodiscard]] inline DirectAudioDebugSnapshot CaptureDirectAudioDebugSnapshot(
    const AudioGraphBinding& binding,
    bool bound,
    uint32_t ioBufferFrameSize,
    uint32_t expectedIoBufferFrameSize,
    int64_t lastSampleDelta,
    uint64_t sampleTimeRegressionCount,
    uint64_t ioBufferFrameSizeChangeCount,
    bool outputReaderAvailableAtWriteEnd) noexcept {
    DirectAudioDebugSnapshot snapshot{};

    snapshot.bound = bound && binding.IsValid();
    snapshot.inputBufferAddress =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding.memory.inputBase));
    snapshot.outputBufferAddress =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(binding.memory.outputBase));
    snapshot.inputFrameCapacity = binding.memory.inputFrameCapacity;
    snapshot.outputFrameCapacity = binding.memory.outputFrameCapacity;
    snapshot.inputChannels = binding.memory.inputChannels;
    snapshot.outputChannels = binding.memory.outputChannels;
    snapshot.ioBufferFrameSize = ioBufferFrameSize;
    snapshot.expectedIoBufferFrameSize = expectedIoBufferFrameSize;
    snapshot.lastSampleDelta = lastSampleDelta;
    snapshot.sampleTimeRegressionCount = sampleTimeRegressionCount;
    snapshot.ioBufferFrameSizeChangeCount = ioBufferFrameSizeChangeCount;
    snapshot.outputReaderAvailableAtWriteEnd = outputReaderAvailableAtWriteEnd;

    if (!binding.control) {
        return snapshot;
    }

    const auto& control = *binding.control;
    snapshot.ioBeginReadCount = control.counters.ioBeginReadCount.load(std::memory_order_relaxed);
    snapshot.ioWriteEndCount = control.counters.ioWriteEndCount.load(std::memory_order_relaxed);

    snapshot.inputBeginReadSampleFrame =
        control.client.inputBeginReadSampleFrame.load(std::memory_order_relaxed);
    snapshot.inputClientReadEndFrame =
        control.client.inputClientReadEndFrame.load(std::memory_order_acquire);

    snapshot.outputWriteEndSampleFrame =
        control.client.outputWriteEndSampleFrame.load(std::memory_order_relaxed);
    snapshot.outputClientWriteEndFrame =
        control.client.outputClientWriteEndFrame.load(std::memory_order_acquire);

    snapshot.inputBeginReadFrameCount =
        control.client.inputBeginReadFrames.load(std::memory_order_relaxed);
    snapshot.outputWriteEndFrameCount =
        control.client.outputWriteEndFrames.load(std::memory_order_relaxed);

    snapshot.directTxPackets = control.counters.txPackets.load(std::memory_order_relaxed);
    snapshot.directTxUnderruns = control.counters.txUnderruns.load(std::memory_order_relaxed);
    snapshot.directTxSilenceSubstitutions =
        control.counters.txSilenceSubstitutions.load(std::memory_order_relaxed);

    snapshot.playbackRingWriteFrame =
        control.playbackRingWriteFrame.load(std::memory_order_acquire);
    snapshot.playbackRingReadFrame =
        control.playbackRingReadFrame.load(std::memory_order_acquire);
    snapshot.playbackRingOldestValidFrame =
        control.playbackRingOldestValidFrame.load(std::memory_order_acquire);
    snapshot.playbackRingAvailableFrames =
        (snapshot.playbackRingWriteFrame >= snapshot.playbackRingReadFrame)
            ? (snapshot.playbackRingWriteFrame - snapshot.playbackRingReadFrame)
            : 0;
    snapshot.playbackRingUnderruns =
        control.playbackRingUnderruns.load(std::memory_order_relaxed);
    snapshot.playbackRingOverruns =
        control.playbackRingOverruns.load(std::memory_order_relaxed);
    snapshot.txScheduledSampleFrame =
        control.txScheduledSampleFrame.load(std::memory_order_acquire);
    snapshot.txCompletedSampleFrame =
        control.txCompletedSampleFrame.load(std::memory_order_acquire);
    snapshot.txLastSourceFrame =
        control.txLastSourceFrame.load(std::memory_order_acquire);
    snapshot.txPreparedSourceEndFrame =
        control.txPreparedSourceEndFrame.load(std::memory_order_acquire);
    snapshot.txStartupAvailableFrames =
        control.txStartupAvailableFrames.load(std::memory_order_acquire);
    snapshot.txAnchorSourceFrame =
        control.txAnchorSourceFrame.load(std::memory_order_acquire);
    snapshot.txAnchorTimelineFrame =
        control.txAnchorTimelineFrame.load(std::memory_order_acquire);
    snapshot.txAnchorPacketIndex =
        control.txAnchorPacketIndex.load(std::memory_order_acquire);
    snapshot.txAnchorDistance =
        control.txAnchorDistance.load(std::memory_order_acquire);
    snapshot.txMinimumPreparationDistance =
        control.txMinimumPreparationDistance.load(std::memory_order_acquire);
    snapshot.txLastPreparationLatencyTicks =
        control.txLastPreparationLatencyTicks.load(std::memory_order_relaxed);
    snapshot.txMaxPreparationLatencyTicks =
        control.txMaxPreparationLatencyTicks.load(std::memory_order_relaxed);
    snapshot.txForwardCursorCorrections =
        control.counters.txForwardCursorCorrections.load(std::memory_order_relaxed);
    snapshot.txPreventedBackwardCorrections =
        control.counters.txPreventedBackwardCorrections.load(std::memory_order_relaxed);
    snapshot.txStaleOverwrittenReads =
        control.counters.txStaleOverwrittenReads.load(std::memory_order_relaxed);
    snapshot.txProducerAheadUnderruns =
        control.counters.txProducerAheadUnderruns.load(std::memory_order_relaxed);
    snapshot.txTimelineDiscontinuities =
        control.counters.txTimelineDiscontinuities.load(std::memory_order_relaxed);
    snapshot.txPcmNonzeroPackets =
        control.counters.txPcmNonzeroPackets.load(std::memory_order_relaxed);
    snapshot.txPcmAllZeroPackets =
        control.counters.txPcmAllZeroPackets.load(std::memory_order_relaxed);
    snapshot.txTimelineInvariantFailures =
        control.counters.txTimelineInvariantFailures.load(std::memory_order_relaxed);
    snapshot.txPreparedPcmSlots =
        control.counters.txPreparedPcmSlots.load(std::memory_order_relaxed);
    snapshot.txPendingSourceSlots =
        control.counters.txPendingSourceSlots.load(std::memory_order_relaxed);
    snapshot.txStartupSilenceSlots =
        control.counters.txStartupSilenceSlots.load(std::memory_order_relaxed);
    snapshot.txRetiredEpochSilenceSlots =
        control.counters.txRetiredEpochSilenceSlots.load(std::memory_order_relaxed);
    snapshot.txReadAheadFaults =
        control.counters.txReadAheadFaults.load(std::memory_order_relaxed);
    snapshot.txSourceOverwrittenFaults =
        control.counters.txSourceOverwrittenFaults.load(std::memory_order_relaxed);
    snapshot.txPreparationDeadlineFaults =
        control.counters.txPreparationDeadlineFaults.load(std::memory_order_relaxed);
    snapshot.txSlotOwnershipFaults =
        control.counters.txSlotOwnershipFaults.load(std::memory_order_relaxed);
    snapshot.txImmediateStops =
        control.counters.txImmediateStops.load(std::memory_order_relaxed);
    snapshot.txPreparationRequestedGeneration =
        control.txPreparationRequests.requestedGeneration.load(std::memory_order_acquire);
    snapshot.txPreparationHandledGeneration =
        control.txPreparationRequests.handledGeneration.load(std::memory_order_acquire);
    snapshot.txPreparationRequestHostTicks =
        control.txPreparationRequests.requestHostTicks.load(std::memory_order_relaxed);
    snapshot.txPreparationHandledHostTicks =
        control.txPreparationRequests.handledHostTicks.load(std::memory_order_relaxed);
    snapshot.txPreparationWakeRequests =
        control.counters.txPreparationWakeRequests.load(std::memory_order_relaxed);
    snapshot.txPreparationWakeDispatches =
        control.counters.txPreparationWakeDispatches.load(std::memory_order_relaxed);
    snapshot.txPreparationWakeCoalesced =
        control.counters.txPreparationWakeCoalesced.load(std::memory_order_relaxed);
    snapshot.txPreparationDrainPasses =
        control.counters.txPreparationDrainPasses.load(std::memory_order_relaxed);
    snapshot.txDeferredStartupWrites =
        control.counters.txDeferredStartupWrites.load(std::memory_order_relaxed);
    snapshot.txCompletedPayloadHashMatches =
        control.counters.txCompletedPayloadHashMatches.load(std::memory_order_relaxed);
    snapshot.txCompletedPayloadHashMismatches =
        control.counters.txCompletedPayloadHashMismatches.load(std::memory_order_relaxed);
    snapshot.txCompletedPcmSlots =
        control.counters.txCompletedPcmSlots.load(std::memory_order_relaxed);
    snapshot.txCompletedStartupSilenceSlots =
        control.counters.txCompletedStartupSilenceSlots.load(std::memory_order_relaxed);
    snapshot.txCompletedRetiredSilenceSlots =
        control.counters.txCompletedRetiredSilenceSlots.load(std::memory_order_relaxed);
    snapshot.txPayloadMismatchFaults =
        control.counters.txPayloadMismatchFaults.load(std::memory_order_relaxed);
    snapshot.fatalReason = control.fatalReason.load(std::memory_order_acquire);
    snapshot.fatalGeneration = control.fatalGeneration.load(std::memory_order_acquire);
    snapshot.fatalPacketIndex =
        control.txFatalSnapshot.packetIndex.load(std::memory_order_relaxed);
    snapshot.fatalDistanceToHardware =
        control.txFatalSnapshot.distanceToHardware.load(std::memory_order_relaxed);
    snapshot.fatalSourceFirstFrame =
        control.txFatalSnapshot.sourceFirstFrame.load(std::memory_order_relaxed);
    snapshot.fatalSourceEndFrame =
        control.txFatalSnapshot.sourceEndFrame.load(std::memory_order_relaxed);
    snapshot.fatalOldestValidFrame =
        control.txFatalSnapshot.oldestValidFrame.load(std::memory_order_relaxed);
    snapshot.fatalWrittenEndFrame =
        control.txFatalSnapshot.writtenEndFrame.load(std::memory_order_relaxed);
    snapshot.fatalPreparedPayloadHash =
        control.txFatalSnapshot.preparedPayloadHash.load(std::memory_order_relaxed);
    snapshot.fatalCompletedPayloadHash =
        control.txFatalSnapshot.completedPayloadHash.load(std::memory_order_relaxed);

    snapshot.captureRingWriteFrame =
        control.captureRingWriteFrame.load(std::memory_order_acquire);
    snapshot.captureRingReadFrame =
        control.captureRingReadFrame.load(std::memory_order_acquire);
    snapshot.captureRingAvailableFrames =
        (snapshot.captureRingWriteFrame >= snapshot.captureRingReadFrame)
            ? (snapshot.captureRingWriteFrame - snapshot.captureRingReadFrame)
            : 0;
    snapshot.captureRingOverruns =
        control.captureRingOverruns.load(std::memory_order_relaxed);
    snapshot.captureRingStarvations =
        control.captureRingStarvations.load(std::memory_order_relaxed);
    snapshot.rxDecodedFrames =
        control.counters.rxDecodedFrames.load(std::memory_order_relaxed);

    snapshot.txValidPhasePcmPackets = control.counters.txValidPhasePcmPackets.load(std::memory_order_relaxed);
    snapshot.txValidPhaseSilencePackets = control.counters.txValidPhaseSilencePackets.load(std::memory_order_relaxed);
    snapshot.txNoPhaseSilencePackets = control.counters.txNoPhaseSilencePackets.load(std::memory_order_relaxed);
    snapshot.txUnderrunSilencePackets = control.counters.txUnderrunSilencePackets.load(std::memory_order_relaxed);
    snapshot.txStaleSyncPackets = control.counters.txStaleSyncPackets.load(std::memory_order_relaxed);
    snapshot.txInvalidGeometryPackets = control.counters.txInvalidGeometryPackets.load(std::memory_order_relaxed);

    return snapshot;
}

[[nodiscard]] inline bool ShouldLogDirectAudioDebugSnapshot(
    DirectAudioDebugLogState& state,
    const DirectAudioDebugSnapshot& snapshot,
    uint64_t nowNs,
    uint64_t intervalNs = kDirectAudioDebugLogIntervalNs) noexcept {
    const bool first = !state.hasLogged;
    const bool boundChanged = state.hasLogged && state.lastBound != snapshot.bound;
    const bool intervalElapsed =
        state.hasLogged &&
        intervalNs > 0 &&
        nowNs >= state.lastLogTimeNs &&
        (nowNs - state.lastLogTimeNs) >= intervalNs;

    if (!first && !boundChanged && !(snapshot.bound && intervalElapsed)) {
        return false;
    }

    state.lastLogTimeNs = nowNs;
    state.hasLogged = true;
    state.lastBound = snapshot.bound;
    return true;
}

} // namespace ASFW::Audio::Runtime
