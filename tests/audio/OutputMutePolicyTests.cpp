// SPDX-License-Identifier: Apache-2.0
//
// OutputMutePolicy tests: mute on a device that has no mute register, and what the driver
// does when that device reports the level of its own volume knob.

#include "Audio/DriverKit/Runtime/OutputMutePolicy.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Audio::DriverKit::DecideDeviceReport;
using ASFW::Audio::DriverKit::DeviceReportDecision;
using ASFW::Audio::DriverKit::LevelForMute;

constexpr float kMinDb = -64.0f;

TEST(OutputMutePolicyTests, MuteWritesSilenceAndUnmuteReturnsToTheControlLevel) {
    EXPECT_FLOAT_EQ(LevelForMute(/*muted=*/true, -12.0f, kMinDb), kMinDb);
    // The control keeps its value through the mute, so it is the level to come back to.
    EXPECT_FLOAT_EQ(LevelForMute(/*muted=*/false, -12.0f, kMinDb), -12.0f);
}

TEST(OutputMutePolicyTests, OurOwnMuteEchoIsIgnored) {
    // While muted the device reports the silence we wrote; that is not the knob moving.
    EXPECT_EQ(DecideDeviceReport(/*muted=*/true, kMinDb, -12.0f, kMinDb), DeviceReportDecision{});
}

TEST(OutputMutePolicyTests, TurningTheKnobWhileMutedClearsMute) {
    const auto decision = DecideDeviceReport(/*muted=*/true, -30.0f, -12.0f, kMinDb);
    EXPECT_TRUE(decision.clearMute) << "the device is audible again, so it is not muted";
    EXPECT_TRUE(decision.applyToControl);
}

TEST(OutputMutePolicyTests, UnmutedReportsFollowTheKnobOnlyWhenItMoved) {
    EXPECT_TRUE(DecideDeviceReport(/*muted=*/false, -30.0f, -12.0f, kMinDb).applyToControl);
    // Within half a step: the device is confirming what the host already set.
    EXPECT_FALSE(DecideDeviceReport(/*muted=*/false, -12.1f, -12.0f, kMinDb).applyToControl);
    EXPECT_FALSE(DecideDeviceReport(/*muted=*/false, -12.0f, -12.0f, kMinDb).clearMute);
}

TEST(OutputMutePolicyTests, AKnobLeftAtSilenceDoesNotClearMute) {
    // Knob at the bottom and muted: still silence, so there is nothing to correct.
    EXPECT_EQ(DecideDeviceReport(/*muted=*/true, kMinDb + 0.1f, -12.0f, kMinDb),
              DeviceReportDecision{});
}

} // namespace
