#include "Audio/Runtime/TimingCursorPolicy.hpp"
#include <gtest/gtest.h>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::TimingCursorPolicy;
using ASFW::Audio::AudioDirection;
using ASFW::Audio::AudioTimingMode;

TEST(TimingCursorPolicyTests, DICE48kBlockingValues) {
    const auto policy = TimingCursorPolicy::MakeDice48kBlocking();

    EXPECT_EQ(policy.SampleRateHz(), 48000U);
    EXPECT_EQ(policy.FramesPerPacketMax(), 8U);
    EXPECT_EQ(policy.CursorOffsetFrames(AudioDirection::Output), 48U);
    EXPECT_EQ(policy.CursorOffsetFrames(AudioDirection::Input), 128U);
    EXPECT_EQ(policy.ReportedLatencyFrames(AudioDirection::Output), 29U);
    EXPECT_EQ(policy.ReportedLatencyFrames(AudioDirection::Input), 0U);
    EXPECT_EQ(policy.SafetyOffsetFrames(AudioDirection::Output), 8U);
    EXPECT_EQ(policy.SafetyOffsetFrames(AudioDirection::Input), 8U);
    EXPECT_EQ(policy.PacketLeadFrames(AudioDirection::Output), 768U);
    EXPECT_EQ(policy.StartupLeadFrames(AudioDirection::Output), 768U);
    EXPECT_EQ(policy.PreparationDeadlineFrames(AudioDirection::Output), 384U);
    EXPECT_EQ(policy.PacketLeadFrames(AudioDirection::Input), 0U);
    EXPECT_EQ(policy.CursorResyncDeadbandFrames(), 256U);
    EXPECT_EQ(policy.ZtsPeriodFrames(), 8U);
}

TEST(TimingCursorPolicyTests, TimingCursorPolicy_Dice48kBlocking_SeparatesPacketAndHalZtsPeriods) {
    const auto policy = TimingCursorPolicy::MakeDice48kBlocking();

    EXPECT_EQ(policy.FramesPerPacketMax(), 8U);
    EXPECT_EQ(policy.RxAuthorityUpdatePeriodFrames(), 8U);
    EXPECT_EQ(policy.HalZeroTimestampPeriodFrames(), 512U);
    EXPECT_EQ(policy.HalIoPeriodFrames(), 512U);

    EXPECT_NE(policy.FramesPerPacketMax(), policy.HalZeroTimestampPeriodFrames());
}

TEST(TimingCursorPolicyTests, FrameConversions) {
    const auto policy = TimingCursorPolicy::MakeDice48kBlocking();

    // HardwareOutputFrameToReportedFrame
    EXPECT_EQ(policy.HardwareOutputFrameToReportedFrame(0), 0U);
    EXPECT_EQ(policy.HardwareOutputFrameToReportedFrame(47), 0U);
    EXPECT_EQ(policy.HardwareOutputFrameToReportedFrame(48), 0U);
    EXPECT_EQ(policy.HardwareOutputFrameToReportedFrame(100), 52U);

    // HostOutputFrameToPlaybackFrame
    EXPECT_EQ(policy.HostOutputFrameToPlaybackFrame(0), 48U);
    EXPECT_EQ(policy.HostOutputFrameToPlaybackFrame(100), 148U);

    // HardwareInputFrameToCaptureFrame
    EXPECT_EQ(policy.HardwareInputFrameToCaptureFrame(0), 0U);
    EXPECT_EQ(policy.HardwareInputFrameToCaptureFrame(127), 0U);
    EXPECT_EQ(policy.HardwareInputFrameToCaptureFrame(128), 0U);
    EXPECT_EQ(policy.HardwareInputFrameToCaptureFrame(200), 72U);
}

} // namespace ASFW::Tests::AudioRuntime
