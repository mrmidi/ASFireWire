#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Audio/DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "Audio/DriverKit/Runtime/MotuServoStallDetector.hpp"
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
using ASFW::Audio::Runtime::MotuRxSphClockSample;
using ASFW::Audio::Runtime::MotuRxSphRateMeter;
using ASFW::Audio::Runtime::MotuServoStallDetector;
using ASFW::Audio::Runtime::MotuServoStallEvent;
namespace MotuV3Wire = ASFW::Protocols::Audio::AMDTP::MotuV3Wire;


TEST(AudioTransportControlBlockTests, MotuRxSphClockBridgePublishesOnlyMonotonicCurrentGeneration) {
    AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t streamGeneration =
        control.generation.load(std::memory_order_acquire);

    MotuRxSphClockSample observed{};
    uint64_t updates = 0;
    EXPECT_FALSE(control.motuRxSphClock.ReadLatest(observed, updates));

    ASSERT_TRUE(control.motuRxSphClock.Publish({
        .streamGeneration = streamGeneration,
        .rxFrames = 192000,
        .rxTicks = 98304000,
    }));
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_EQ(updates, 1U);
    EXPECT_EQ(observed.streamGeneration, streamGeneration);
    EXPECT_EQ(observed.rxFrames, 192000U);
    EXPECT_EQ(observed.rxTicks, 98304000);

    EXPECT_FALSE(control.motuRxSphClock.Publish({
        .streamGeneration = streamGeneration,
        .rxFrames = 191992,
        .rxTicks = 98299904,
    }));
    EXPECT_FALSE(control.motuRxSphClock.Publish({
        .streamGeneration = streamGeneration + 1,
        .rxFrames = 192008,
        .rxTicks = 98308096,
    }));

    ASSERT_TRUE(control.motuRxSphClock.Publish({
        .streamGeneration = streamGeneration,
        .rxFrames = 192008,
        .rxTicks = 98308096,
    }));
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_EQ(updates, 2U);
    EXPECT_EQ(observed.rxFrames, 192008U);
    EXPECT_EQ(observed.rxTicks, 98308096);
}

TEST(AudioTransportControlBlockTests, MotuRxSphClockBridgeRebasesOnStartGeneration) {
    AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t firstGeneration =
        control.generation.load(std::memory_order_acquire);
    ASSERT_TRUE(control.motuRxSphClock.Publish({
        .streamGeneration = firstGeneration,
        .rxFrames = 192000,
        .rxTicks = 98304000,
    }));

    control.ResetForStart();
    const uint64_t secondGeneration =
        control.generation.load(std::memory_order_acquire);
    ASSERT_EQ(secondGeneration, firstGeneration + 1);

    MotuRxSphClockSample observed{};
    uint64_t updates = 0;
    EXPECT_FALSE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_FALSE(control.motuRxSphClock.Publish({
        .streamGeneration = firstGeneration,
        .rxFrames = 192008,
        .rxTicks = 98308096,
    }));

    ASSERT_TRUE(control.motuRxSphClock.Publish({
        .streamGeneration = secondGeneration,
        .rxFrames = 8,
        .rxTicks = 4096,
    }));
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_EQ(updates, 1U);
    EXPECT_EQ(observed.streamGeneration, secondGeneration);
    EXPECT_EQ(observed.rxFrames, 8U);
    EXPECT_EQ(observed.rxTicks, 4096);
}

TEST(AudioTransportControlBlockTests,
     ReceiveReactivationWithinSameGenerationPreservesMotuClockBridgeProgress) {
    AudioTransportControlBlock control{};
    control.ResetForStart();
    const uint64_t streamGeneration =
        control.generation.load(std::memory_order_acquire);

    // First model the measurement accumulated before a receive-context restart
    // and publish it normally.
    MotuRxSphRateMeter meter{};
    meter.Observe(MotuV3Wire::EncodeSph(0), 8U, 48000U);
    meter.Observe(MotuV3Wire::EncodeSph(4096), 8U, 48000U);
    meter.Observe(MotuV3Wire::EncodeSph(8192), 8U, 48000U);
    const auto beforeRestart = meter.CumulativeSnapshot();
    ASSERT_TRUE(beforeRestart.valid);
    ASSERT_EQ(beforeRestart.frames, 16U);
    ASSERT_EQ(beforeRestart.ticks, 8192);
    ASSERT_TRUE(control.motuRxSphClock.Publish({
        .streamGeneration = streamGeneration,
        .rxFrames = beforeRestart.frames,
        .rxTicks = beforeRestart.ticks,
    }));

    // An internal IR reactivation does not run AudioTransportControlBlock's
    // ResetForStart(). Reanchor only breaks the step across the inactive gap,
    // leaving cumulative high-water marks monotonic in the same generation.
    meter.Reanchor();
    meter.Observe(MotuV3Wire::EncodeSph(1048576), 8U, 48000U);
    meter.Observe(MotuV3Wire::EncodeSph(1052672), 8U, 48000U);
    const auto afterReactivation = meter.CumulativeSnapshot();
    ASSERT_TRUE(afterReactivation.valid);
    ASSERT_EQ(afterReactivation.frames, 24U);
    ASSERT_EQ(afterReactivation.ticks, 12288);

    EXPECT_TRUE(control.motuRxSphClock.Publish({
        .streamGeneration = streamGeneration,
        .rxFrames = afterReactivation.frames,
        .rxTicks = afterReactivation.ticks,
    }));

    MotuRxSphClockSample observed{};
    uint64_t updates = 0;
    ASSERT_TRUE(control.motuRxSphClock.ReadLatest(observed, updates));
    EXPECT_EQ(updates, 2U);
    EXPECT_EQ(observed.streamGeneration, streamGeneration);
    EXPECT_EQ(observed.rxFrames, afterReactivation.frames);
    EXPECT_EQ(observed.rxTicks, afterReactivation.ticks);
}

TEST(AudioTransportControlBlockTests,
     MotuServoStallDetectorReportsMeterRegressionOnlyOncePerActivation) {
    MotuServoStallDetector detector{};
    MotuServoStallEvent event{};
    const MotuRxSphClockSample bridge{
        .streamGeneration = 7,
        .rxFrames = 16000,
        .rxTicks = 8192000,
    };

    EXPECT_FALSE(detector.Observe(
        7, {.valid = true, .frames = 16000, .ticks = 8192000},
        bridge, 2000, event));
    ASSERT_TRUE(detector.Observe(
        7, {.valid = true, .frames = 8000, .ticks = 4096000},
        bridge, 2000, event));
    EXPECT_EQ(event.streamGeneration, 7U);
    EXPECT_EQ(event.bridgeUpdates, 2000U);
    EXPECT_EQ(event.bridgeFrames, 16000U);
    EXPECT_EQ(event.bridgeTicks, 8192000);
    EXPECT_EQ(event.meterFrames, 8000U);
    EXPECT_EQ(event.meterTicks, 4096000);

    EXPECT_FALSE(detector.Observe(
        7, {.valid = true, .frames = 8016, .ticks = 4104192},
        bridge, 2000, event));

    detector.Reset();
    EXPECT_TRUE(detector.Observe(
        7, {.valid = true, .frames = 8, .ticks = 4096},
        bridge, 2000, event));
}

TEST(AudioTransportControlBlockTests,
     MotuServoStallDetectorIgnoresIncompleteOrDifferentGenerationSnapshots) {
    MotuServoStallDetector detector{};
    MotuServoStallEvent event{};
    const MotuRxSphClockSample bridge{
        .streamGeneration = 9,
        .rxFrames = 16000,
        .rxTicks = 8192000,
    };

    EXPECT_FALSE(detector.Observe(
        9, {.valid = false, .frames = 0, .ticks = 0}, bridge, 2000, event));
    EXPECT_FALSE(detector.Observe(
        8, {.valid = true, .frames = 8000, .ticks = 4096000},
        bridge, 2000, event));
    EXPECT_FALSE(detector.Observe(
        9, {.valid = true, .frames = 8000, .ticks = 4096000},
        bridge, 0, event));
}
using ASFW::Audio::Runtime::AudioTelemetryEndpointSnapshot;

TEST(AudioTransportControlBlockTests, PreparationRequestsAreMonotonicAndCoalescible) {
    TxPreparationRequestState requests{};

    EXPECT_FALSE(requests.NeedsHandling());
    EXPECT_EQ(requests.PublishRequest(100), 1U);
    EXPECT_EQ(requests.PublishRequest(200), 2U);
    EXPECT_TRUE(requests.NeedsHandling());
    EXPECT_EQ(requests.RequestedGeneration(), 2U);
    EXPECT_EQ(requests.requestHostTicks.load(std::memory_order_relaxed), 200U);
    EXPECT_TRUE(requests.TryScheduleWake());
    EXPECT_FALSE(requests.TryScheduleWake());

    requests.MarkHandled(2, 250);
    EXPECT_FALSE(requests.NeedsHandling());
    EXPECT_EQ(requests.handledGeneration.load(std::memory_order_acquire), 2U);
    EXPECT_EQ(requests.handledHostTicks.load(std::memory_order_relaxed), 250U);
    requests.FinishWake();
    EXPECT_TRUE(requests.TryScheduleWake());
    requests.FinishWake();

    EXPECT_EQ(requests.PublishRequest(300), 3U);
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

TEST(AudioTransportControlBlockTests, RxCaptureTelemetryCapturesIntervalWatermarks) {
    AudioTransportControlBlock control{};
    auto& telemetry = control.rxCaptureBufferTelemetry;
    telemetry.Observe(800, 600, 1024);
    telemetry.Observe(1600, 800, 1024);
    telemetry.RecordOverrun(32);
    telemetry.RecordStarvation(16);
    telemetry.RecordReaderBeginRead();
    telemetry.CompleteInterval(1000);

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
    control.rxCaptureBufferTelemetry.CompleteInterval(1000);

    AudioTelemetryEndpointSnapshot snapshot{};
    ASFW::Audio::Runtime::CopyAudioTelemetrySnapshot(control, snapshot);

    EXPECT_NE(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryHasCompletedRxInterval, 0U);
    EXPECT_EQ(snapshot.flags & ASFW::Audio::Runtime::kAudioTelemetryRxCaptureReaderActive, 0U);
}

TEST(AudioTransportControlBlockTests, ResetForStartClearsNestedStateAndIncrementsGeneration) {
    AudioTransportControlBlock control{};

    control.generation.store(41, std::memory_order_relaxed);
    control.client.PublishBeginRead(2000, 456, 64);
    control.client.PublishWriteEnd(1000, 123, 128);
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
    control.counters.txPayloadMismatchFaults.store(6, std::memory_order_relaxed);
    control.counters.txPostLockNoDataPackets.store(8, std::memory_order_relaxed);
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
    EXPECT_EQ(control.counters.txPayloadMismatchFaults.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.counters.txPostLockNoDataPackets.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.fatalGeneration.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::None);
}

} // namespace
