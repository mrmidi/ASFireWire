#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"

#include <gtest/gtest.h>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::HostClockAnchorSample;
using ASFW::Audio::Runtime::HostClockAnchorState;

TEST(HostClockAnchorTests, ResetState) {
    HostClockAnchorState state{};
    state.producerCursor.store(5);
    state.consumerCursor.store(3);
    state.notificationPending.store(true);
    state.generation.store(5);
    state.sampleFrame.store(512);
    state.hostTicks.store(200);
    state.hostNanosPerSampleQ8.store(300);
    state.anchorUpdates.store(1);
    state.mirrorPublications.store(3);
    state.staleUpdates.store(7);
    state.queueOverflows.store(2);

    state.Reset();

    EXPECT_EQ(state.producerCursor.load(), 0U);
    EXPECT_EQ(state.consumerCursor.load(), 0U);
    EXPECT_FALSE(state.notificationPending.load());
    EXPECT_EQ(state.generation.load(), 0U);
    EXPECT_EQ(state.sampleFrame.load(), 0U);
    EXPECT_EQ(state.hostTicks.load(), 0U);
    EXPECT_EQ(state.hostNanosPerSampleQ8.load(), 0U);
    EXPECT_EQ(state.anchorUpdates.load(), 0U);
    EXPECT_EQ(state.mirrorPublications.load(), 0U);
    EXPECT_EQ(state.staleUpdates.load(), 0U);
    EXPECT_EQ(state.queueOverflows.load(), 0U);
}

TEST(HostClockAnchorTests, QueuesMonotonicRxHostAnchorsInOrder) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    const auto first =
        control.PublishHostClockAnchor(512, 200, 300);
    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.notifyConsumer);
    EXPECT_EQ(control.hostClockAnchor.sampleFrame.load(), 512U);
    EXPECT_EQ(control.hostClockAnchor.hostTicks.load(), 200U);
    EXPECT_EQ(control.hostClockAnchor.hostNanosPerSampleQ8.load(), 300U);
    EXPECT_EQ(control.hostClockAnchor.anchorUpdates.load(), 1U);

    const auto second =
        control.PublishHostClockAnchor(1024, 400, 300);
    EXPECT_TRUE(second.accepted);
    EXPECT_FALSE(second.notifyConsumer);
    EXPECT_EQ(control.hostClockAnchor.generation.load(), 2U);

    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryPop(anchor));
    EXPECT_EQ(anchor.sampleFrame, 512U);
    EXPECT_EQ(anchor.hostTicks, 200U);
    ASSERT_TRUE(control.hostClockAnchor.TryPop(anchor));
    EXPECT_EQ(anchor.sampleFrame, 1024U);
    EXPECT_EQ(anchor.hostTicks, 400U);
    EXPECT_FALSE(control.hostClockAnchor.TryPop(anchor));
    EXPECT_FALSE(
        control.hostClockAnchor.FinishDrainAndNeedsAnotherPass());

    EXPECT_FALSE(
        control.PublishHostClockAnchor(1536, 0, 300).accepted);
    EXPECT_EQ(control.hostClockAnchor.staleUpdates.load(), 1U);
}

TEST(HostClockAnchorTests, RejectsForwardSampleBackwardHost) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_TRUE(control.PublishHostClockAnchor(
        512, 5528585432680ULL, 256).accepted);
    EXPECT_FALSE(control.PublishHostClockAnchor(
        1024, 5528585408482ULL, 256).accepted);

    EXPECT_EQ(control.hostClockAnchor.sampleFrame.load(), 512U);
    EXPECT_EQ(control.hostClockAnchor.hostTicks.load(), 5528585432680ULL);
    EXPECT_EQ(control.hostClockAnchor.staleUpdates.load(), 1U);
}

TEST(HostClockAnchorTests, RejectsSkippedGridPoint) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_TRUE(
        control.PublishHostClockAnchor(512, 100, 300).accepted);
    EXPECT_FALSE(
        control.PublishHostClockAnchor(1536, 200, 300).accepted);
    EXPECT_EQ(control.hostClockAnchor.staleUpdates.load(), 1U);
}

TEST(HostClockAnchorTests, ConsumerRechecksAfterCoalescedPublish) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_TRUE(
        control.PublishHostClockAnchor(512, 100, 300)
            .notifyConsumer);

    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryPop(anchor));
    EXPECT_FALSE(
        control.PublishHostClockAnchor(1024, 200, 300)
            .notifyConsumer);

    EXPECT_TRUE(
        control.hostClockAnchor.FinishDrainAndNeedsAnotherPass());
    ASSERT_TRUE(control.hostClockAnchor.TryPop(anchor));
    EXPECT_EQ(anchor.sampleFrame, 1024U);
    EXPECT_FALSE(
        control.hostClockAnchor.FinishDrainAndNeedsAnotherPass());
}

TEST(HostClockAnchorTests, RejectsOffGridPublications) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_FALSE(
        control.PublishHostClockAnchor(48, 100, 300).accepted);
    EXPECT_FALSE(
        control.PublishHostClockAnchor(511, 200, 300).accepted);
    EXPECT_TRUE(
        control.PublishHostClockAnchor(512, 300, 300).accepted);
    EXPECT_EQ(control.hostClockAnchor.anchorUpdates.load(), 1U);
}

TEST(HostClockAnchorTests, RejectsPublicationWhenQueueIsFull) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    for (uint64_t index = 1;
         index <= HostClockAnchorState::kQueueCapacity;
         ++index) {
        EXPECT_TRUE(control.PublishHostClockAnchor(
            index * 512, index * 100, 300).accepted);
    }

    EXPECT_FALSE(control.PublishHostClockAnchor(
        (HostClockAnchorState::kQueueCapacity + 1) * 512,
        (HostClockAnchorState::kQueueCapacity + 1) * 100,
        300).accepted);
    EXPECT_EQ(control.hostClockAnchor.queueOverflows.load(), 1U);
}

} // namespace ASFW::Tests::AudioRuntime
