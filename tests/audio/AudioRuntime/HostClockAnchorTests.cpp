#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Audio/Engine/Direct/AudioClockPublisher.hpp"

#include <gtest/gtest.h>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::HostClockAnchorSample;
using ASFW::Audio::Runtime::HostClockAnchorState;
using ASFW::Audio::Runtime::MotuPhaseTraceSample;

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

// Epic 4 (HARDWARE_TIMELINE_OWNERSHIP.md): an anchor carries the timeline
// epoch it was projected in, so the HAL publish point can refuse one whose
// epoch has ended.
TEST(HostClockAnchorTests, AnchorCarriesItsTimelineEpoch) {
    AudioTransportControlBlock control{};
    ASSERT_TRUE(control.PublishHostClockAnchor(12288, 1000, 300, 7).accepted);
    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(0, anchor));
    EXPECT_EQ(anchor.timelineEpoch, 7U);
    control.ResetForStart();
    EXPECT_EQ(control.hostClockAnchor.timelineEpoch.load(), 0U);
    EXPECT_EQ(control.hardwareTimeline.Epoch(), 0U);
}

// One authority per device: while the timeline is in a Transmit epoch (M-Audio
// special firmware), the RX publisher stays silent; in a Receive epoch it
// publishes, tagged; with no epoch (a rate the timeline does not model) it
// publishes untagged as before.
TEST(HostClockAnchorTests, RxPublisherFollowsTheTimelineSource) {
    using ASFW::Audio::Runtime::HardwareTimelineDiscontinuity;
    using ASFW::Audio::Runtime::HardwareTimelineSource;
    AudioTransportControlBlock control{};
    ASFW::Audio::Runtime::AudioGraphBinding binding{};
    binding.control = &control;
    ASFW::AudioEngine::Direct::AudioClockPublisher publisher;
    publisher.Bind(&binding);

    EXPECT_TRUE(publisher.Publish(12288, 1000, 300).accepted);
    HostClockAnchorSample anchor{};
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(0, anchor));
    EXPECT_EQ(anchor.timelineEpoch, 0U);

    const uint64_t transmit = control.hardwareTimeline.BeginEpoch(
        HardwareTimelineSource::Transmit, HardwareTimelineDiscontinuity::StartIO, 48000, 0);
    ASSERT_NE(transmit, 0U);
    EXPECT_FALSE(publisher.Publish(24576, 2000, 300).accepted);
    EXPECT_EQ(control.hostClockAnchor.generation.load(), anchor.generation);

    const uint64_t receive = control.hardwareTimeline.BeginEpoch(
        HardwareTimelineSource::Receive, HardwareTimelineDiscontinuity::StartIO, 48000, 0);
    ASSERT_NE(receive, 0U);
    EXPECT_TRUE(publisher.Publish(36864, 3000, 300).accepted);
    ASSERT_TRUE(control.hostClockAnchor.TryReadLatest(anchor.generation, anchor));
    EXPECT_EQ(anchor.timelineEpoch, receive);
}

TEST(HostClockAnchorTests, MotuPhaseTracePublishesLatestPacketAndOutputLastReference) {
    AudioTransportControlBlock control{};
    control.ResetForStart();

    ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample period{};
    uint64_t updates = 0;
    EXPECT_FALSE(control.motuPhaseTrace.ReadLatest(period, updates));

    // One publication is a whole cadence period, so the bridge
    // round-trip has to carry every bucket, not just the newest.
    ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample published{};
    ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket(published, 410, 0x0f123454u);
    ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket(published, 411, 0x0f123455u);
    ASFW::Audio::Runtime::PushMotuPhaseTraceCadencePacket(published, 412, 0x0f123456u);
    published.outputLastPacketIndex = 400;
    published.outputLastCycleTimer = 0x04234000u;
    control.motuPhaseTrace.Publish(published);

    ASSERT_TRUE(control.motuPhaseTrace.ReadLatest(period, updates));
    EXPECT_EQ(updates, 1U);
    EXPECT_EQ(period.count, 3U);
    EXPECT_EQ(period.packetIndex[0], 410U);
    EXPECT_EQ(period.packetIndex[2], 412U);
    EXPECT_EQ(period.firstSph[0], 0x0f123454u);
    EXPECT_EQ(period.firstSph[2], 0x0f123456u);
    EXPECT_EQ(period.outputLastPacketIndex, 400U);
    EXPECT_EQ(period.outputLastCycleTimer, 0x04234000u);
    EXPECT_FALSE(period.hasOutputLast);

    // The per-packet view the `[MotuPhase]` line uses is still available.
    const auto newest = ASFW::Audio::Runtime::NewestMotuPhaseTraceSample(period);
    EXPECT_EQ(newest.packetIndex, 412U);
    EXPECT_EQ(newest.firstSph, 0x0f123456u);
}

TEST(HostClockAnchorTests, MotuPhaseResidualUsesOutputLastPacketDistanceAndLead) {
    constexpr uint32_t kOutputLast = 0x04234000u;
    constexpr uint64_t kCompletedPacket = 400;
    constexpr uint64_t kPreparedPacket = 412;
    const int64_t expectedTicks = ASFW::Audio::Runtime::NormalizeMotuTicks(
        ASFW::Timing::encodedTstampToOffsets(kOutputLast) +
        static_cast<int64_t>(kPreparedPacket - kCompletedPacket) *
            ASFW::Timing::kTicksPerCycle +
        ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kPresentationLeadTicks);
    const auto residual = ASFW::Audio::Runtime::ComputeMotuTxPhaseResidual({
        .packetIndex = kPreparedPacket,
        .outputLastPacketIndex = kCompletedPacket,
        .firstSph = ASFW::Protocols::Audio::AMDTP::MotuV3Wire::EncodeSph(expectedTicks + 512),
        .outputLastCycleTimer = kOutputLast,
        .hasOutputLast = true,
    });

    ASSERT_TRUE(residual.valid);
    EXPECT_EQ(residual.packetLead, 12U);
    EXPECT_EQ(residual.expectedSphTicks, expectedTicks);
    EXPECT_EQ(residual.residualTicks, 512);
}

// The SPH carries no seconds and the cycle timer counts to 128 s, so the two
// only compare once both are folded into the SPH's one-second domain. Read
// straight, this pair would differ by the host clock's seven whole seconds.
TEST(HostClockAnchorTests, MotuRxSphCycleDifferenceFoldsHostSecondsAway) {
    const auto sph = ASFW::Protocols::Audio::AMDTP::MotuV3Wire::EncodeSph(1024);
    const auto cycle = ASFW::Timing::encodeCycleTimer(7, 7999, 2048);
    EXPECT_EQ(ASFW::Audio::Runtime::MotuRxSphMinusCycleTimerTicks(sph, cycle), 2048);

    // Same instant within the second, a different host second: the phase this
    // field reports must not move. Before the domain fix it moved by seconds.
    const auto laterCycle = ASFW::Timing::encodeCycleTimer(93, 7999, 2048);
    EXPECT_EQ(ASFW::Audio::Runtime::MotuRxSphMinusCycleTimerTicks(sph, laterCycle),
              2048);
}

} // namespace ASFW::Tests::AudioRuntime
