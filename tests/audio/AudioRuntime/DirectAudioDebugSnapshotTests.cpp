#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/DriverKit/Runtime/DirectAudioDebugSnapshot.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;
using ASFW::Audio::Runtime::CaptureDirectAudioDebugSnapshot;
using ASFW::Audio::Runtime::DirectAudioDebugLogState;
using ASFW::Audio::Runtime::DirectAudioDebugSnapshot;
using ASFW::Audio::Runtime::FatalStreamReason;
using ASFW::Audio::Runtime::kDirectAudioDebugLogIntervalNs;
using ASFW::Audio::Runtime::ShouldLogDirectAudioDebugSnapshot;

TEST(DirectAudioDebugSnapshotTests, CapturesBindingCountersAndCursors) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> input{};
    std::array<int32_t, 16> output{};

    const AudioGraphBinding binding{
        .guid = 0x1122334455667788ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = input.data(),
            .outputBase = output.data(),
            .inputFrameCapacity = 8,
            .outputFrameCapacity = 8,
            .inputChannels = 2,
            .outputChannels = 2,
        },
        .control = &control,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .streamMode = AudioStreamMode::kBlocking,
        .hostToDeviceWireFormat = AudioWireFormat::kAM824,
        .audioDevice = &audioDevice,
    };

    control.client.PublishBeginRead(100, 10, 32);
    control.client.PublishWriteEnd(200, 20, 32);
    control.counters.CountBeginRead();
    control.counters.CountWriteEnd();
    control.counters.txPackets.store(7, std::memory_order_relaxed);
    control.counters.txUnderruns.store(3, std::memory_order_relaxed);
    control.counters.txSilenceSubstitutions.store(2, std::memory_order_relaxed);
    control.playbackRingOldestValidFrame.store(168, std::memory_order_relaxed);
    control.playbackRingWriteFrame.store(232, std::memory_order_relaxed);
    control.playbackRingReadFrame.store(184, std::memory_order_relaxed);
    control.txScheduledSampleFrame.store(4096, std::memory_order_relaxed);
    control.txCompletedSampleFrame.store(4000, std::memory_order_relaxed);
    control.txLastSourceFrame.store(4040, std::memory_order_relaxed);
    control.txPreparedSourceEndFrame.store(4048, std::memory_order_relaxed);
    control.counters.txForwardCursorCorrections.store(4, std::memory_order_relaxed);
    control.counters.txPreventedBackwardCorrections.store(5, std::memory_order_relaxed);
    control.counters.txStaleOverwrittenReads.store(6, std::memory_order_relaxed);
    control.counters.txProducerAheadUnderruns.store(7, std::memory_order_relaxed);
    control.counters.txTimelineDiscontinuities.store(8, std::memory_order_relaxed);
    control.counters.txPcmNonzeroPackets.store(9, std::memory_order_relaxed);
    control.counters.txPcmAllZeroPackets.store(10, std::memory_order_relaxed);
    control.counters.txTimelineInvariantFailures.store(11, std::memory_order_relaxed);
    control.counters.txPreparedPcmSlots.store(12, std::memory_order_relaxed);
    control.counters.txPendingSourceSlots.store(13, std::memory_order_relaxed);
    control.counters.txStartupSilenceSlots.store(14, std::memory_order_relaxed);
    control.counters.txRetiredEpochSilenceSlots.store(15, std::memory_order_relaxed);
    control.counters.txReadAheadFaults.store(16, std::memory_order_relaxed);
    control.counters.txSourceOverwrittenFaults.store(17, std::memory_order_relaxed);
    control.counters.txPreparationDeadlineFaults.store(18, std::memory_order_relaxed);
    control.counters.txSlotOwnershipFaults.store(19, std::memory_order_relaxed);
    control.counters.txImmediateStops.store(20, std::memory_order_relaxed);
    control.txFatalSnapshot.packetIndex.store(21, std::memory_order_relaxed);
    control.txFatalSnapshot.distanceToHardware.store(4, std::memory_order_relaxed);
    control.txFatalSnapshot.sourceFirstFrame.store(500, std::memory_order_relaxed);
    control.txFatalSnapshot.sourceEndFrame.store(508, std::memory_order_relaxed);
    control.txFatalSnapshot.oldestValidFrame.store(480, std::memory_order_relaxed);
    control.txFatalSnapshot.writtenEndFrame.store(504, std::memory_order_relaxed);
    control.fatalGeneration.store(22, std::memory_order_relaxed);
    control.fatalReason.store(FatalStreamReason::TxReadAhead, std::memory_order_release);

    const auto snapshot = CaptureDirectAudioDebugSnapshot(
        binding,
        true,
        32,
        64,
        32,
        1,
        2,
        true);

    EXPECT_TRUE(snapshot.bound);
    EXPECT_EQ(snapshot.inputBufferAddress,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(input.data())));
    EXPECT_EQ(snapshot.outputBufferAddress,
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(output.data())));
    EXPECT_EQ(snapshot.inputFrameCapacity, 8U);
    EXPECT_EQ(snapshot.outputFrameCapacity, 8U);
    EXPECT_EQ(snapshot.inputChannels, 2U);
    EXPECT_EQ(snapshot.outputChannels, 2U);
    EXPECT_EQ(snapshot.ioBeginReadCount, 1U);
    EXPECT_EQ(snapshot.ioWriteEndCount, 1U);
    EXPECT_EQ(snapshot.inputBeginReadSampleFrame, 100U);
    EXPECT_EQ(snapshot.inputClientReadEndFrame, 132U);
    EXPECT_EQ(snapshot.outputWriteEndSampleFrame, 200U);
    EXPECT_EQ(snapshot.outputClientWriteEndFrame, 232U);
    EXPECT_EQ(snapshot.inputBeginReadFrameCount, 32U);
    EXPECT_EQ(snapshot.outputWriteEndFrameCount, 32U);
    EXPECT_EQ(snapshot.ioBufferFrameSize, 32U);
    EXPECT_EQ(snapshot.expectedIoBufferFrameSize, 64U);
    EXPECT_EQ(snapshot.lastSampleDelta, 32);
    EXPECT_EQ(snapshot.sampleTimeRegressionCount, 1U);
    EXPECT_EQ(snapshot.ioBufferFrameSizeChangeCount, 2U);
    EXPECT_EQ(snapshot.directTxPackets, 7U);
    EXPECT_EQ(snapshot.directTxUnderruns, 3U);
    EXPECT_EQ(snapshot.directTxSilenceSubstitutions, 2U);
    EXPECT_EQ(snapshot.playbackRingOldestValidFrame, 168U);
    EXPECT_EQ(snapshot.playbackRingAvailableFrames, 48U);
    EXPECT_EQ(snapshot.txScheduledSampleFrame, 4096U);
    EXPECT_EQ(snapshot.txCompletedSampleFrame, 4000U);
    EXPECT_EQ(snapshot.txLastSourceFrame, 4040U);
    EXPECT_EQ(snapshot.txPreparedSourceEndFrame, 4048U);
    EXPECT_EQ(snapshot.txForwardCursorCorrections, 4U);
    EXPECT_EQ(snapshot.txPreventedBackwardCorrections, 5U);
    EXPECT_EQ(snapshot.txStaleOverwrittenReads, 6U);
    EXPECT_EQ(snapshot.txProducerAheadUnderruns, 7U);
    EXPECT_EQ(snapshot.txTimelineDiscontinuities, 8U);
    EXPECT_EQ(snapshot.txPcmNonzeroPackets, 9U);
    EXPECT_EQ(snapshot.txPcmAllZeroPackets, 10U);
    EXPECT_EQ(snapshot.txTimelineInvariantFailures, 11U);
    EXPECT_EQ(snapshot.txPreparedPcmSlots, 12U);
    EXPECT_EQ(snapshot.txPendingSourceSlots, 13U);
    EXPECT_EQ(snapshot.txStartupSilenceSlots, 14U);
    EXPECT_EQ(snapshot.txRetiredEpochSilenceSlots, 15U);
    EXPECT_EQ(snapshot.txReadAheadFaults, 16U);
    EXPECT_EQ(snapshot.txSourceOverwrittenFaults, 17U);
    EXPECT_EQ(snapshot.txPreparationDeadlineFaults, 18U);
    EXPECT_EQ(snapshot.txSlotOwnershipFaults, 19U);
    EXPECT_EQ(snapshot.txImmediateStops, 20U);
    EXPECT_EQ(snapshot.fatalReason, FatalStreamReason::TxReadAhead);
    EXPECT_EQ(snapshot.fatalGeneration, 22U);
    EXPECT_EQ(snapshot.fatalPacketIndex, 21U);
    EXPECT_EQ(snapshot.fatalDistanceToHardware, 4U);
    EXPECT_EQ(snapshot.fatalSourceFirstFrame, 500U);
    EXPECT_EQ(snapshot.fatalSourceEndFrame, 508U);
    EXPECT_EQ(snapshot.fatalOldestValidFrame, 480U);
    EXPECT_EQ(snapshot.fatalWrittenEndFrame, 504U);
    EXPECT_TRUE(snapshot.outputReaderAvailableAtWriteEnd);
}

TEST(DirectAudioDebugSnapshotTests, ThrottleLogsFirstBoundChangesAndBoundIntervals) {
    DirectAudioDebugLogState state{};
    DirectAudioDebugSnapshot snapshot{};
    snapshot.bound = true;

    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(state, snapshot, 100));
    EXPECT_FALSE(ShouldLogDirectAudioDebugSnapshot(state, snapshot, 101));
    EXPECT_FALSE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + kDirectAudioDebugLogIntervalNs - 1));
    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + kDirectAudioDebugLogIntervalNs));

    snapshot.bound = false;
    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + kDirectAudioDebugLogIntervalNs + 1));
    EXPECT_FALSE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + (2 * kDirectAudioDebugLogIntervalNs) + 1));

    snapshot.bound = true;
    EXPECT_TRUE(ShouldLogDirectAudioDebugSnapshot(
        state,
        snapshot,
        100 + (2 * kDirectAudioDebugLogIntervalNs) + 2));
}

} // namespace ASFW::Tests::AudioRuntime
