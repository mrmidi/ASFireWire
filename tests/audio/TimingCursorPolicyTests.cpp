// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Config/TimingCursorPolicy.hpp"

#include <gtest/gtest.h>

namespace {

TEST(TimingCursorPolicyTests, Dice1xSnapshotKeepsTheActive44100Rate) {
    constexpr auto policy =
        ASFW::Audio::TimingCursorPolicy::MakeDice1xBlocking(44100);
    constexpr auto snapshot = policy.Snapshot();

    EXPECT_EQ(snapshot.sampleRateHz, 44100U);
    EXPECT_EQ(snapshot.framesPerPacketMax, 8U);
    EXPECT_EQ(snapshot.outputCursorOffsetFrames, 48U);
    EXPECT_EQ(snapshot.inputCursorOffsetFrames, 128U);
    EXPECT_EQ(snapshot.ztsPeriodFrames, 1536U);
}

TEST(TimingCursorPolicyTests, Dice1xSnapshotPreserves48kGeometry) {
    constexpr auto policy =
        ASFW::Audio::TimingCursorPolicy::MakeDice1xBlocking(48000);
    constexpr auto snapshot = policy.Snapshot();

    EXPECT_EQ(snapshot.sampleRateHz, 48000U);
    EXPECT_EQ(snapshot.framesPerPacketMax, 8U);
    EXPECT_EQ(snapshot.outputCursorOffsetFrames, 48U);
    EXPECT_EQ(snapshot.inputCursorOffsetFrames, 128U);
}

} // namespace
