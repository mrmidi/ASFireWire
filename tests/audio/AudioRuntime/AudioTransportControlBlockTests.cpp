#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Audio/Runtime/AudioTelemetrySnapshot.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::FatalStreamReason;
using ASFW::Audio::Runtime::TxPreparationRequestState;
using ASFW::Audio::Runtime::TxProducerFaultReason;
using ASFW::Audio::Runtime::TxProducerFaultRecord;
using ASFW::Audio::Runtime::TxProducerFaultStage;
using ASFW::Audio::Runtime::AudioTelemetryEndpointSnapshot;

TEST(AudioTransportControlBlockTests, PreparationRequestsAreMonotonicAndCoalescible) {
    TxPreparationRequestState requests{};

    EXPECT_FALSE(requests.NeedsHandling());
    EXPECT_EQ(requests.PublishRequest(100, 600), 1U);
    EXPECT_EQ(requests.PublishRequest(200, 1200), 2U);
    EXPECT_TRUE(requests.NeedsHandling());
    EXPECT_EQ(requests.RequestedGeneration(), 2U);
    EXPECT_EQ(requests.requestHostTicks.load(std::memory_order_relaxed), 200U);
    EXPECT_EQ(requests.requestedTargetFrameEnd.load(std::memory_order_acquire), 1200U);
    EXPECT_TRUE(requests.TryScheduleWake());
    EXPECT_FALSE(requests.TryScheduleWake());

    requests.MarkHandled(2, 250);
    EXPECT_FALSE(requests.NeedsHandling());
    EXPECT_EQ(requests.handledGeneration.load(std::memory_order_acquire), 2U);
    EXPECT_EQ(requests.handledHostTicks.load(std::memory_order_relaxed), 250U);
    requests.FinishWake();
    EXPECT_TRUE(requests.TryScheduleWake());
    requests.FinishWake();

    EXPECT_EQ(requests.PublishRequest(300, 1800), 3U);
    EXPECT_TRUE(requests.NeedsHandling());
}

TEST(AudioTransportControlBlockTests, ProducerFaultDetailIsAudioOwnedAndResettable) {
    AudioTransportControlBlock control{};
    const TxProducerFaultRecord published{
        .stage = TxProducerFaultStage::kReplaySytValidation,
        .reason = TxProducerFaultReason::kInvalidReplaySyt,
        .packetIndex = 192,
        .rangeStart = 180,
        .rangeTarget = 216,
        .preparedCount = 12,
        .completionCursor = 144,
        .committedEnd = 192,
        .replayProducerCursor = 480,
        .replayEpoch = 4,
    };

    EXPECT_EQ(control.txProducerFault.Publish(published), 1U);
    TxProducerFaultRecord observed{};
    ASSERT_TRUE(control.txProducerFault.TryRead(observed));
    EXPECT_EQ(observed.packetIndex, 192U);
    EXPECT_EQ(observed.committedEnd, 192U);
    EXPECT_STREQ(ASFW::Audio::Runtime::TxProducerFaultStageName(observed.stage),
                 "replay-syt-validation");
    EXPECT_STREQ(ASFW::Audio::Runtime::TxProducerFaultReasonName(observed.reason),
                 "invalid-replay-syt");

    control.ResetForStart();
    EXPECT_FALSE(control.txProducerFault.TryRead(observed));
}

TEST(AudioTransportControlBlockTests, TelemetrySnapshotCopiesOnlyCompletedInterval) {
    AudioTransportControlBlock control{};
    control.generation.store(7, std::memory_order_relaxed);
    control.txCurrentCommittedMarginPackets.store(672, std::memory_order_relaxed);
    control.txMinimumCommittedMarginPackets.store(665, std::memory_order_relaxed);
    control.txLastPreparationLatencyTicks.store(32, std::memory_order_relaxed);
    control.txMaxPreparationLatencyTicks.store(2468, std::memory_order_relaxed);
    control.txPreparationLatencySamples.store(144550, std::memory_order_relaxed);
    control.txPreparationAtMost750Us.store(144549, std::memory_order_relaxed);
    control.txPreparationAtLeast1500Us.store(1, std::memory_order_relaxed);
    control.txCompletedIntervalSequence.store(2, std::memory_order_relaxed);
    control.txCompletedIntervalMarginMinPackets.store(665, std::memory_order_relaxed);
    control.txCompletedIntervalMarginMaxPackets.store(678, std::memory_order_relaxed);
    control.txCompletedIntervalPreparationLatencyMaxTicks.store(158, std::memory_order_relaxed);
    control.txCompletedIntervalPreparationLatencyHistogram[0].store(6650, std::memory_order_relaxed);
    control.txCompletedIntervalCommittedMarginHistogram[3].store(7119, std::memory_order_relaxed);

    AudioTelemetryEndpointSnapshot snapshot{};
    ASFW::Audio::Runtime::CopyAudioTelemetrySnapshot(control, snapshot);

    EXPECT_EQ(snapshot.controlGeneration, 7U);
    EXPECT_EQ(snapshot.currentCommittedMarginPackets, 672U);
    EXPECT_EQ(snapshot.completedIntervalMarginMinPackets, 665U);
    EXPECT_EQ(snapshot.completedIntervalMarginMaxPackets, 678U);
    EXPECT_EQ(snapshot.completedLatencyHistogram[0], 6650U);
    EXPECT_EQ(snapshot.completedMarginHistogram[3], 7119U);
    EXPECT_NE(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryHasCompletedInterval, 0U);
}

TEST(AudioTransportControlBlockTests,
     TelemetrySnapshotCopiesValueOwnedTxContentAndFirstFaultState) {
    AudioTransportControlBlock control{};
    control.playbackRingWriteFrame.store(14'096, std::memory_order_relaxed);
    control.playbackRingOldestValidFrame.store(12'560,
                                               std::memory_order_relaxed);
    control.txContentFinalizedFrameEnd.store(13'984,
                                             std::memory_order_relaxed);
    control.txPcmStagingTelemetry.oldestValidFrame.store(
        10'000, std::memory_order_relaxed);
    control.txPcmStagingTelemetry.writtenEndFrame.store(
        14'096, std::memory_order_release);
    control.txPcmStagingTelemetry.readsReady.store(
        1'748, std::memory_order_relaxed);
    control.txPcmStagingTelemetry.readsNotYetWritten.store(
        3, std::memory_order_relaxed);
    control.txTransportCompletionCursor.store(8'000,
                                              std::memory_order_relaxed);
    control.txTransportCommittedEnd.store(8'678,
                                          std::memory_order_relaxed);
    control.txTransportStatus.store(1, std::memory_order_relaxed);
    control.txContentDeferrals.store(3, std::memory_order_relaxed);
    control.txContentDeadlineNoData.store(1, std::memory_order_relaxed);
    control.txContentFaultEvents.store(1, std::memory_order_relaxed);
    control.txContentFirstFaultPacket.store(8'679,
                                            std::memory_order_relaxed);
    control.txContentFirstFaultAudioFrame.store(13'984,
                                                std::memory_order_relaxed);
    control.txContentFirstFaultOldestFrame.store(10'000,
                                                 std::memory_order_relaxed);
    control.txContentFirstFaultWrittenEndFrame.store(
        13'984, std::memory_order_relaxed);
    control.txContentFirstFaultCompletionCursor.store(
        8'000, std::memory_order_relaxed);
    control.txContentFirstFaultCommittedEnd.store(
        8'678, std::memory_order_relaxed);
    control.txContentFirstFaultReason.store(
        static_cast<uint32_t>(
            ASFW::Audio::Runtime::TxContentFaultReason::
                kNotYetWrittenAtDeadline),
        std::memory_order_release);
    control.rxEmptyCompletions.store(2, std::memory_order_relaxed);

    AudioTelemetryEndpointSnapshot snapshot{};
    ASFW::Audio::Runtime::CopyAudioTelemetrySnapshot(control, snapshot);

    EXPECT_EQ(snapshot.txPlaybackWriteFrame, 14'096U);
    EXPECT_EQ(snapshot.txPlaybackOldestValidFrame, 12'560U);
    EXPECT_EQ(snapshot.txContentFinalizedFrameEnd, 13'984U);
    EXPECT_EQ(snapshot.txStagingOldestValidFrame, 10'000U);
    EXPECT_EQ(snapshot.txStagingWrittenEndFrame, 14'096U);
    EXPECT_EQ(snapshot.txStagingReadsReady, 1'748U);
    EXPECT_EQ(snapshot.txStagingReadsNotYetWritten, 3U);
    EXPECT_EQ(snapshot.txTransportCompletionCursor, 8'000U);
    EXPECT_EQ(snapshot.txTransportCommittedEnd, 8'678U);
    EXPECT_EQ(snapshot.txTransportStatus, 1U);
    EXPECT_EQ(snapshot.txContentDeferrals, 3U);
    EXPECT_EQ(snapshot.txContentDeadlineNoData, 1U);
    EXPECT_EQ(snapshot.txContentFirstFaultPacket, 8'679U);
    EXPECT_EQ(snapshot.txContentFirstFaultReason, 1U);
    EXPECT_EQ(snapshot.rxEmptyCompletions, 2U);
}

TEST(AudioTransportControlBlockTests, RxCaptureTelemetryCapturesIntervalWatermarks) {
    AudioTransportControlBlock control{};
    auto& telemetry = control.rxCaptureBufferTelemetry;
    telemetry.Observe(800, 600, 1024);
    telemetry.Observe(1600, 800, 1024);
    telemetry.RecordOverrun(32);
    telemetry.RecordStarvation(16);
    telemetry.RecordReaderBeginRead();
    telemetry.CompleteInterval();

    AudioTelemetryEndpointSnapshot snapshot{};
    ASFW::Audio::Runtime::CopyAudioTelemetrySnapshot(control, snapshot);

    EXPECT_EQ(snapshot.rxCurrentAvailableFrames, 800U);
    EXPECT_EQ(snapshot.rxCompletedIntervalMinimumAvailableFrames, 200U);
    EXPECT_EQ(snapshot.rxCompletedIntervalMaximumAvailableFrames, 800U);
    EXPECT_EQ(snapshot.rxCompletedIntervalMinimumFreeHeadroomFrames, 224U);
    EXPECT_EQ(snapshot.rxCompletedIntervalOverrunEvents, 1U);
    EXPECT_EQ(snapshot.rxCompletedIntervalOverwrittenFrames, 32U);
    EXPECT_EQ(snapshot.rxCompletedIntervalStarvationEvents, 1U);
    EXPECT_EQ(snapshot.rxCompletedIntervalStarvedFrames, 16U);
    EXPECT_EQ(snapshot.rxTotalOverwrittenFrames, 32U);
    EXPECT_EQ(snapshot.rxTotalStarvedFrames, 16U);
    EXPECT_NE(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryHasCompletedRxInterval, 0U);
    EXPECT_NE(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryRxCaptureReaderActive, 0U);
}

TEST(AudioTransportControlBlockTests, RxCaptureReaderActivityIsScopedToTheCompletedInterval) {
    AudioTransportControlBlock control{};
    control.rxCaptureBufferTelemetry.CompleteInterval();

    AudioTelemetryEndpointSnapshot snapshot{};
    ASFW::Audio::Runtime::CopyAudioTelemetrySnapshot(control, snapshot);

    EXPECT_NE(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryHasCompletedRxInterval, 0U);
    EXPECT_EQ(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryRxCaptureReaderActive, 0U);
}

TEST(AudioTransportControlBlockTests, WirePayloadTelemetryStaysInAudioAndFlagsDropout) {
    AudioTransportControlBlock control{};
    const uint8_t dataPacket[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        0x40, 0x00, 0x00, 0x01,
    };
    const uint8_t zeroPacket[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0,
    };

    const auto first = control.txWirePayloadTelemetry.Observe(
        12, dataPacket, sizeof(dataPacket));
    EXPECT_TRUE(first.firstInfo);
    EXPECT_FALSE(first.dropout);
    EXPECT_EQ(first.infoQuads, 1U);

    const auto dropout = control.txWirePayloadTelemetry.Observe(
        13, zeroPacket, sizeof(zeroPacket));
    EXPECT_FALSE(dropout.firstInfo);
    EXPECT_TRUE(dropout.dropout);
    EXPECT_EQ(control.txWirePayloadTelemetry.firstInfoPacketIndex.load(), 12U);
    EXPECT_EQ(control.txWirePayloadTelemetry.pcmDropouts.load(), 1U);
}

TEST(AudioTransportControlBlockTests, ResetForStartClearsNestedStateAndIncrementsGeneration) {
    AudioTransportControlBlock control{};

    control.generation.store(41, std::memory_order_relaxed);
    control.client.PublishBeginRead(2000, 456, 64);
    control.client.PublishWriteEnd(1000, 123, 128);
    control.device.Publish(3000, 789, 55);
    control.counters.CountBeginRead();
    control.counters.CountWriteEnd();
    control.counters.CountZtsPublished();
    control.counters.CountRxZtsPublished();
    control.counters.CountRxAdkZtsPublished();
    control.counters.txPackets.store(9, std::memory_order_relaxed);
    control.counters.rxPackets.store(7, std::memory_order_relaxed);
    control.inputProducedEndFrame.store(111, std::memory_order_relaxed);
    control.outputConsumedEndFrame.store(222, std::memory_order_relaxed);
    control.inputOverruns.store(3, std::memory_order_relaxed);
    control.outputUnderruns.store(4, std::memory_order_relaxed);
    control.discontinuities.store(5, std::memory_order_relaxed);
    control.playbackRingOldestValidFrame.store(123, std::memory_order_relaxed);
    control.playbackRingDiscontinuityGeneration.store(6, std::memory_order_relaxed);
    control.txScheduledSampleFrame.store(456, std::memory_order_relaxed);
    control.txCompletedSampleFrame.store(400, std::memory_order_relaxed);
    control.txMinimumPreparationDistance.store(70, std::memory_order_relaxed);
    control.txMinimumCommittedMarginPackets.store(
        12, std::memory_order_relaxed);
    control.txLastPreparationLatencyTicks.store(20, std::memory_order_relaxed);
    control.txMaxPreparationLatencyTicks.store(30, std::memory_order_relaxed);
    control.txPreparationLatencySamples.store(10, std::memory_order_relaxed);
    control.txPreparationAtMost750Us.store(9, std::memory_order_relaxed);
    control.txPreparationAtLeast1500Us.store(1, std::memory_order_relaxed);
    control.txIntervalCommittedMarginMinPackets.store(
        64, std::memory_order_relaxed);
    control.txIntervalCommittedMarginMaxPackets.store(
        512, std::memory_order_relaxed);
    control.txIntervalPreparationLatencyMaxTicks.store(
        31, std::memory_order_relaxed);
    for (auto& bucket : control.txIntervalPreparationLatencyHistogram) {
        bucket.store(7, std::memory_order_relaxed);
    }
    for (auto& bucket : control.txIntervalCommittedMarginHistogram) {
        bucket.store(11, std::memory_order_relaxed);
    }
    control.txLastLeadTicks.store(40, std::memory_order_relaxed);
    control.txMinimumLeadTicks.store(10, std::memory_order_relaxed);
    control.txMaximumLeadTicks.store(70, std::memory_order_relaxed);
    control.counters.txPhaseRebases.store(1, std::memory_order_relaxed);
    control.counters.txSilenceFallback.store(2, std::memory_order_relaxed);
    control.counters.txStaleOverwrittenReads.store(3, std::memory_order_relaxed);
    control.counters.txProducerAheadUnderruns.store(4, std::memory_order_relaxed);
    control.counters.txPcmNonzeroPackets.store(6, std::memory_order_relaxed);
    control.counters.txPcmAllZeroPackets.store(7, std::memory_order_relaxed);
    control.counters.txPreparedPcmSlots.store(9, std::memory_order_relaxed);
    control.counters.txReadAheadFaults.store(11, std::memory_order_relaxed);
    (void)control.txPreparationRequests.PublishRequest(1000);
    control.txPreparationRequests.MarkHandled(1, 1100);
    control.counters.txPreparationWakeRequests.store(1, std::memory_order_relaxed);
    control.counters.txCompletedPayloadHashMatches.store(4, std::memory_order_relaxed);
    control.counters.txCompletedPayloadHashMismatches.store(5, std::memory_order_relaxed);
    control.counters.txPayloadMismatchFaults.store(6, std::memory_order_relaxed);
    control.counters.txPostLockNoDataPackets.store(8, std::memory_order_relaxed);
    control.txFatalSnapshot.packetIndex.store(12, std::memory_order_relaxed);
    control.fatalGeneration.store(13, std::memory_order_relaxed);
    control.fatalReason.store(FatalStreamReason::TxReadAhead, std::memory_order_relaxed);

    control.ResetForStart();

    EXPECT_EQ(control.generation.load(std::memory_order_acquire), 42U);

    EXPECT_EQ(control.client.inputBeginReadSampleFrame.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.inputBeginReadHostTicks.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.inputBeginReadFrames.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.InputReadEndFrame(), 0U);
    EXPECT_EQ(control.client.outputWriteEndSampleFrame.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.outputWriteEndHostTicks.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.outputWriteEndFrames.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.OutputWrittenEndFrame(), 0U);

    EXPECT_EQ(control.device.sampleFrame.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.device.hostTicks.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.device.hostNanosPerSampleQ8.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.device.generation.load(std::memory_order_acquire), 0U);

    EXPECT_EQ(control.counters.ioBeginReadCount.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.ioWriteEndCount.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txPackets.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.rxPackets.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.ztsPublished.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.ztsRxPublished.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.ztsRxAdkPublished.load(std::memory_order_relaxed), 0U);

    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.outputConsumedEndFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.inputOverruns.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.outputUnderruns.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.discontinuities.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.playbackRingOldestValidFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.playbackRingDiscontinuityGeneration.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txCompletedSampleFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txMinimumPreparationDistance.load(std::memory_order_acquire),
              UINT32_MAX);
    EXPECT_EQ(
        control.txMinimumCommittedMarginPackets.load(
            std::memory_order_acquire),
        UINT32_MAX);
    EXPECT_EQ(control.txLastPreparationLatencyTicks.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txMaxPreparationLatencyTicks.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txPreparationLatencySamples.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txPreparationAtMost750Us.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txPreparationAtLeast1500Us.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.txIntervalCommittedMarginMinPackets.load(
                  std::memory_order_acquire),
              UINT32_MAX);
    EXPECT_EQ(control.txIntervalCommittedMarginMaxPackets.load(
                  std::memory_order_acquire),
              0U);
    EXPECT_EQ(control.txIntervalPreparationLatencyMaxTicks.load(
                  std::memory_order_acquire),
              0U);
    for (const auto& bucket : control.txIntervalPreparationLatencyHistogram) {
        EXPECT_EQ(bucket.load(std::memory_order_acquire), 0U);
    }
    for (const auto& bucket : control.txIntervalCommittedMarginHistogram) {
        EXPECT_EQ(bucket.load(std::memory_order_acquire), 0U);
    }
    EXPECT_EQ(control.txLastLeadTicks.load(std::memory_order_acquire), 0);
    EXPECT_EQ(control.txMinimumLeadTicks.load(std::memory_order_acquire),
              INT64_MAX);
    EXPECT_EQ(control.txMaximumLeadTicks.load(std::memory_order_acquire),
              INT64_MIN);
    EXPECT_EQ(control.counters.txPhaseRebases.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txSilenceFallback.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txStaleOverwrittenReads.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txProducerAheadUnderruns.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txPcmNonzeroPackets.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txPcmAllZeroPackets.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txPreparedPcmSlots.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txReadAheadFaults.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.txPreparationRequests.RequestedGeneration(), 0U);
    EXPECT_EQ(control.txPreparationRequests.handledGeneration.load(
                  std::memory_order_acquire),
              0U);
    EXPECT_EQ(control.counters.txPreparationWakeRequests.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.counters.txCompletedPayloadHashMatches.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.counters.txCompletedPayloadHashMismatches.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.counters.txPayloadMismatchFaults.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.counters.txPostLockNoDataPackets.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.txFatalSnapshot.packetIndex.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.fatalGeneration.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::None);
}

} // namespace
