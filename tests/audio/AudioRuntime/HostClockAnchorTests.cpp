#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"

#include <gtest/gtest.h>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::HostClockAnchorSample;
using ASFW::Audio::Runtime::HostClockAnchorState;

TEST(HostClockAnchorTests, ResetState) {
    HostClockAnchorState state{};
    state.sequence.store(6);
    state.generation.store(3);
    state.sampleFrame.store(512);
    state.hostTicks.store(200);
    state.hostNanosPerSampleQ8.store(300);
    state.anchorUpdates.store(3);
    state.mirrorPublications.store(2);
    state.invalidUpdates.store(1);

    state.Reset();

    EXPECT_EQ(state.sequence.load(), 0U);
    EXPECT_EQ(state.generation.load(), 0U);
    EXPECT_EQ(state.sampleFrame.load(), 0U);
    EXPECT_EQ(state.hostTicks.load(), 0U);
    EXPECT_EQ(state.hostNanosPerSampleQ8.load(), 0U);
    EXPECT_EQ(state.anchorUpdates.load(), 0U);
    EXPECT_EQ(state.mirrorPublications.load(), 0U);
    EXPECT_EQ(state.invalidUpdates.load(), 0U);
}

TEST(HostClockAnchorTests, PublishesAndReadsLatestAnchor) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    const auto result =
        control.PublishHostClockAnchor(512, 200, 300);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.notifyConsumer);
    EXPECT_EQ(result.notificationGeneration, 1U);

    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(0, anchor));
    EXPECT_EQ(anchor.generation, 1U);
    EXPECT_EQ(anchor.sampleFrame, 512U);
    EXPECT_EQ(anchor.hostTicks, 200U);
    EXPECT_EQ(anchor.hostNanosPerSampleQ8, 300U);
    EXPECT_EQ(control.hostClockAnchor.anchorUpdates.load(), 1U);
}

TEST(HostClockAnchorTests, NewPublicationReplacesIntermediateAnchors) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_TRUE(control.PublishHostClockAnchor(512, 100, 300).accepted);
    EXPECT_TRUE(control.PublishHostClockAnchor(1024, 200, 300).accepted);
    const auto latest =
        control.PublishHostClockAnchor(1536, 300, 300);
    EXPECT_EQ(latest.notificationGeneration, 3U);

    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(0, anchor));
    EXPECT_EQ(anchor.generation, 3U);
    EXPECT_EQ(anchor.sampleFrame, 1536U);
    EXPECT_EQ(anchor.hostTicks, 300U);
}

TEST(HostClockAnchorTests, LastSeenGenerationSuppressesDuplicates) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_TRUE(control.PublishHostClockAnchor(512, 100, 300).accepted);

    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(0, anchor));
    EXPECT_FALSE(control.hostClockAnchor.TryReadLatest(
        anchor.generation, anchor));
}

TEST(HostClockAnchorTests, MailboxDoesNotEnforceGridOrContinuity) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_TRUE(control.PublishHostClockAnchor(37, 500, 300).accepted);
    EXPECT_TRUE(control.PublishHostClockAnchor(4099, 400, 300).accepted);

    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(0, anchor));
    EXPECT_EQ(anchor.generation, 2U);
    EXPECT_EQ(anchor.sampleFrame, 4099U);
    EXPECT_EQ(anchor.hostTicks, 400U);
}

TEST(HostClockAnchorTests, RejectsOnlyInvalidHostMetadata) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    EXPECT_FALSE(control.PublishHostClockAnchor(512, 0, 300).accepted);
    EXPECT_FALSE(control.PublishHostClockAnchor(512, 100, 0).accepted);
    EXPECT_EQ(control.hostClockAnchor.invalidUpdates.load(), 2U);
    EXPECT_EQ(control.hostClockAnchor.generation.load(), 0U);

    EXPECT_TRUE(control.PublishHostClockAnchor(0, 100, 300).accepted);
    EXPECT_EQ(control.hostClockAnchor.generation.load(), 1U);
}

} // namespace ASFW::Tests::AudioRuntime
