#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::FatalStreamReason;
using ASFW::Audio::Runtime::TxPreparationRequestState;

TEST(AudioTransportControlBlockTests, PreparationRequestsAreMonotonicAndCoalescible) {
    TxPreparationRequestState requests{};

    EXPECT_FALSE(requests.NeedsHandling());
    EXPECT_EQ(requests.PublishRequest(100), 1U);
    EXPECT_EQ(requests.PublishRequest(200), 2U);
    EXPECT_TRUE(requests.NeedsHandling());
    EXPECT_EQ(requests.RequestedGeneration(), 2U);
    EXPECT_EQ(requests.requestHostTicks.load(std::memory_order_relaxed), 200U);

    requests.MarkHandled(2, 250);
    EXPECT_FALSE(requests.NeedsHandling());
    EXPECT_EQ(requests.handledGeneration.load(std::memory_order_acquire), 2U);
    EXPECT_EQ(requests.handledHostTicks.load(std::memory_order_relaxed), 250U);

    EXPECT_EQ(requests.PublishRequest(300), 3U);
    EXPECT_TRUE(requests.NeedsHandling());
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
