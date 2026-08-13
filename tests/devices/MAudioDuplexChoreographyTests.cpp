// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioDuplexChoreographyTests.cpp -- hardware-free contract for the special
// 1814 / ProjectMix start path. These tests deliberately exercise policy only;
// no FireWire transaction, timer, DMA descriptor, or AudioDriverKit object is
// involved here.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioDuplexChoreography.hpp"

#include <cstdint>
#include <variant>

namespace {

using namespace ASFW::Audio::Families::BeBoB::MAudio;

constexpr StartEpoch kEpoch{41};
constexpr StartEpoch kStaleEpoch{40};

[[nodiscard]] constexpr TxClockAnchor Anchor(uint32_t cycleTime,
                                              uint64_t hostTicks) {
    return TxClockAnchor{cycleTime, hostTicks};
}

[[nodiscard]] ChoreographyState ReachRunning() {
    auto step = Begin(kEpoch);
    step = Advance(step.state, PreStreamConfigurationSucceeded{kEpoch});
    step = Advance(step.state, DeviceStreamsConnected{kEpoch});
    step = Advance(step.state, HostTransportArmed{kEpoch});
    return step.state;
}

[[nodiscard]] ChoreographyState ReachPlaybackRunning() {
    auto state = ReachRunning();
    auto step = Advance(
        state, TxGroupTimestampObserved{kEpoch, kCaptureReferenceGroup,
                                        Anchor(0x01'0000U, 1000U)});
    step = Advance(
        step.state, TxGroupTimestampObserved{kEpoch, kFirstPlaybackClockGroup,
                                             Anchor(0x02'0000U, 2000U)});
    return step.state;
}

TEST(MAudioDuplexChoreographyTests, BeginsWithOnlyPreStreamConfiguration) {
    const auto step = Begin(kEpoch);

    const auto* state = std::get_if<Configuring>(&step.state);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->epoch.value, kEpoch.value);
    EXPECT_TRUE(std::holds_alternative<ApplyPreStreamConfiguration>(step.action));
    EXPECT_FALSE(IsTerminal(step.state));
}

TEST(MAudioDuplexChoreographyTests,
     EncodesVendorAsymmetricStartBeforeMAudioDeviceConfirmation) {
    auto step = Begin(kEpoch);
    step = Advance(step.state, PreStreamConfigurationSucceeded{kEpoch});
    ASSERT_TRUE(std::holds_alternative<Connecting>(step.state));
    EXPECT_TRUE(std::holds_alternative<ConnectDeviceStreams>(step.action));

    step = Advance(step.state, DeviceStreamsConnected{kEpoch});
    ASSERT_TRUE(std::holds_alternative<ArmingHostTransport>(step.state));
    const auto* start = std::get_if<StartAsymmetricHostTransport>(&step.action);
    ASSERT_NE(start, nullptr);
    // Vendor StartAudio: TX starts at now, RX at now + 160 cycles, after four
    // output segments have been populated (START_CHOREOGRAPHY.md section 2).
    EXPECT_EQ(start->transmitStartLeadCycles, 0U);
    EXPECT_EQ(start->receiveStartLeadCycles, kReceiveStartLeadCycles);
    EXPECT_EQ(start->transmitPrefillSegments, kTransmitPrefillSegments);

    step = Advance(step.state, HostTransportArmed{kEpoch});
    const auto* running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<DeviceStartConfirming>(running->deviceStart));
    EXPECT_TRUE(std::holds_alternative<AwaitingCaptureReference>(running->playback));
    EXPECT_TRUE(std::holds_alternative<CaptureAwaitingData>(running->capture));
    EXPECT_TRUE(std::holds_alternative<ConfirmDeviceStart>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     TxWarmupPlantsReferenceAtGroupTwoThenPublishesAtGroupThree) {
    auto state = ReachRunning();

    auto step = Advance(state, TxGroupTimestampObserved{kEpoch, 1U,
                                                         Anchor(0x00'1000U, 100U)});
    const auto* running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<AwaitingCaptureReference>(running->playback));
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));

    const TxClockAnchor reference = Anchor(0x00'2000U, 200U);
    step = Advance(step.state,
                   TxGroupTimestampObserved{kEpoch, kCaptureReferenceGroup, reference});
    running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    const auto* awaiting =
        std::get_if<AwaitingPlaybackAnchor>(&running->playback);
    ASSERT_NE(awaiting, nullptr);
    EXPECT_EQ(awaiting->captureReference.cycleTime, reference.cycleTime);
    EXPECT_EQ(awaiting->captureReference.hostTicks, reference.hostTicks);
    const auto* plant = std::get_if<PlantCaptureReference>(&step.action);
    ASSERT_NE(plant, nullptr);
    EXPECT_EQ(plant->anchor.cycleTime, reference.cycleTime);
    EXPECT_EQ(plant->anchor.hostTicks, reference.hostTicks);

    const TxClockAnchor firstPlayback = Anchor(0x00'3000U, 300U);
    step = Advance(
        step.state,
        TxGroupTimestampObserved{kEpoch, kFirstPlaybackClockGroup, firstPlayback});
    running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    const auto* playback = std::get_if<PlaybackRunning>(&running->playback);
    ASSERT_NE(playback, nullptr);
    EXPECT_EQ(playback->lastAnchor.cycleTime, firstPlayback.cycleTime);
    EXPECT_EQ(playback->lastAnchor.hostTicks, firstPlayback.hostTicks);
    const auto* publish = std::get_if<PublishPlaybackClock>(&step.action);
    ASSERT_NE(publish, nullptr);
    EXPECT_EQ(publish->anchor.cycleTime, firstPlayback.cycleTime);
    EXPECT_EQ(publish->anchor.hostTicks, firstPlayback.hostTicks);
    EXPECT_TRUE(IsPlaybackRunning(step.state));
}

TEST(MAudioDuplexChoreographyTests,
     TxWarmupObservedDuringHostArmingSurvivesTheArmCompletionRace) {
    auto step = Begin(kEpoch);
    step = Advance(step.state, PreStreamConfigurationSucceeded{kEpoch});
    step = Advance(step.state, DeviceStreamsConnected{kEpoch});
    ASSERT_TRUE(std::holds_alternative<ArmingHostTransport>(step.state));

    const TxClockAnchor reference = Anchor(0x00'2000U, 200U);
    step = Advance(step.state,
                   TxGroupTimestampObserved{kEpoch, kCaptureReferenceGroup, reference});
    const auto* arming = std::get_if<ArmingHostTransport>(&step.state);
    ASSERT_NE(arming, nullptr);
    EXPECT_TRUE(std::holds_alternative<AwaitingPlaybackAnchor>(arming->playback));
    EXPECT_TRUE(std::holds_alternative<PlantCaptureReference>(step.action));

    step = Advance(step.state, HostTransportArmed{kEpoch});
    const auto* running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    const auto* awaiting =
        std::get_if<AwaitingPlaybackAnchor>(&running->playback);
    ASSERT_NE(awaiting, nullptr);
    EXPECT_EQ(awaiting->captureReference.cycleTime, reference.cycleTime);
    EXPECT_EQ(awaiting->captureReference.hostTicks, reference.hostTicks);
    EXPECT_TRUE(std::holds_alternative<ConfirmDeviceStart>(step.action));

    step = Advance(
        step.state,
        TxGroupTimestampObserved{kEpoch, kFirstPlaybackClockGroup,
                                 Anchor(0x00'3000U, 300U)});
    EXPECT_TRUE(IsPlaybackRunning(step.state));
    EXPECT_TRUE(std::holds_alternative<PublishPlaybackClock>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     PlaybackWarmupDoesNotWaitForMAudioDeviceStartResponse) {
    auto state = ReachPlaybackRunning();
    auto* running = std::get_if<RunningTransport>(&state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<DeviceStartConfirming>(running->deviceStart));
    EXPECT_TRUE(std::holds_alternative<PlaybackRunning>(running->playback));

    auto step = Advance(state, DeviceStartConfirmed{kEpoch});
    running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<DeviceStartReady>(running->deviceStart));
    EXPECT_TRUE(std::holds_alternative<PlaybackRunning>(running->playback));
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     HeaderOnlyCaptureTransitionCannotResetOrGatePlaybackClock) {
    auto state = ReachRunning();

    auto step = Advance(state, RxHeaderOnlyNoDataObserved{kEpoch});
    auto* running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<CaptureInNoDataTransition>(running->capture));
    EXPECT_TRUE(std::holds_alternative<AwaitingCaptureReference>(running->playback));
    EXPECT_TRUE(std::holds_alternative<NoteCaptureNoDataTransition>(step.action));

    step = Advance(
        step.state,
        TxGroupTimestampObserved{kEpoch, kCaptureReferenceGroup,
                                 Anchor(0x00'2000U, 200U)});
    step = Advance(
        step.state,
        TxGroupTimestampObserved{kEpoch, kFirstPlaybackClockGroup,
                                 Anchor(0x00'3000U, 300U)});
    running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<CaptureInNoDataTransition>(running->capture));
    EXPECT_TRUE(std::holds_alternative<PlaybackRunning>(running->playback));
    EXPECT_TRUE(std::holds_alternative<PublishPlaybackClock>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     CaptureTimeoutDegradesOnlyCaptureAndItCanLaterRecover) {
    auto state = ReachPlaybackRunning();

    auto step = Advance(state, CaptureStartExpired{kEpoch});
    auto* running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<CaptureDegraded>(running->capture));
    EXPECT_TRUE(std::holds_alternative<PlaybackRunning>(running->playback));
    EXPECT_TRUE(std::holds_alternative<MarkCaptureDegraded>(step.action));
    EXPECT_FALSE(std::holds_alternative<StopHostTransport>(step.action));

    step = Advance(step.state, RxBecameUsable{kEpoch});
    running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<CaptureReady>(running->capture));
    EXPECT_TRUE(std::holds_alternative<PlaybackRunning>(running->playback));
    EXPECT_TRUE(std::holds_alternative<MarkCaptureReady>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     LateCaptureTimeoutCannotDegradeAlreadyUsableCapture) {
    auto state = ReachRunning();
    auto step = Advance(state, RxBecameUsable{kEpoch});
    ASSERT_TRUE(std::holds_alternative<MarkCaptureReady>(step.action));

    step = Advance(step.state, CaptureStartExpired{kEpoch});
    const auto* running = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(running, nullptr);
    EXPECT_TRUE(std::holds_alternative<CaptureReady>(running->capture));
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));
}

TEST(MAudioDuplexChoreographyTests, StaleCallbacksAreInert) {
    auto step = Begin(kEpoch);
    step = Advance(step.state, PreStreamConfigurationSucceeded{kStaleEpoch});
    ASSERT_TRUE(std::holds_alternative<Configuring>(step.state));
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));

    const auto running = ReachRunning();
    step = Advance(running,
                   TxGroupTimestampObserved{kStaleEpoch, kCaptureReferenceGroup,
                                            Anchor(0x00'2000U, 200U)});
    const auto* state = std::get_if<RunningTransport>(&step.state);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(std::holds_alternative<AwaitingCaptureReference>(state->playback));
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     GenerationInvalidationStopsLiveTransportAndMakesLaterEventsInert) {
    const auto running = ReachRunning();
    auto step = Advance(running, GenerationInvalidated{});
    const auto* stopped = std::get_if<Stopped>(&step.state);
    ASSERT_NE(stopped, nullptr);
    EXPECT_EQ(stopped->epoch.value, kEpoch.value);
    EXPECT_TRUE(std::holds_alternative<StopHostTransport>(step.action));
    EXPECT_TRUE(IsTerminal(step.state));

    step = Advance(step.state, DeviceStartConfirmed{kEpoch});
    EXPECT_TRUE(std::holds_alternative<Stopped>(step.state));
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));
}

TEST(MAudioDuplexChoreographyTests,
     DeviceStartFailureAfterHostStartQuiescesTransport) {
    const auto running = ReachRunning();
    const auto step = Advance(running, DeviceStartRejected{kEpoch});
    EXPECT_TRUE(std::holds_alternative<Failed>(step.state));
    EXPECT_TRUE(std::holds_alternative<StopHostTransport>(step.action));
    EXPECT_TRUE(IsTerminal(step.state));
}

} // namespace
