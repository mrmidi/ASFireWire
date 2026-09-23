// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "../../ASFWDriver/Audio/Families/BeBoB/MAudio/MAudioCaptureChannelMap.hpp"

#include <array>

namespace {
using ASFW::Audio::Families::BeBoB::MAudio::CaptureChannelMapFor;
using ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap;
using ProfileBuilderId = ASFW::DeviceProfiles::Audio::ProfileBuilderId;
constexpr std::array<uint8_t, 10> kExpectedSlots10{0, 4, 1, 5, 2, 6, 3, 7, 8, 9};
}

TEST(MAudioCaptureChannelMapTests, TenChannelCaptureUsesPlanarAnalogPermutation) {
    const auto map = CaptureChannelMapFor(ProfileBuilderId::MAudioFireWire1814, 10);
    ASSERT_EQ(map.slotCount, 10U);
    for (uint32_t channel = 0; channel < kExpectedSlots10.size(); ++channel) {
        EXPECT_EQ(map.SlotFor(channel), kExpectedSlots10[channel]) << "channel " << channel;
    }
}

TEST(MAudioCaptureChannelMapTests, MidRateAdatCaptureKeepsFourDigitalSlotsInOrder) {
    const auto map = CaptureChannelMapFor(ProfileBuilderId::MAudioProjectMix, 12);
    ASSERT_EQ(map.slotCount, 12U);
    for (uint32_t channel = 0; channel < 8; ++channel) {
        EXPECT_EQ(map.SlotFor(channel), kExpectedSlots10[channel]);
    }
    for (uint32_t channel = 8; channel < 12; ++channel) {
        EXPECT_EQ(map.SlotFor(channel), channel);
    }
}

TEST(MAudioCaptureChannelMapTests, FullRateAdatCaptureKeepsEightDigitalSlotsInOrder) {
    const auto map = CaptureChannelMapFor(ProfileBuilderId::MAudioProjectMix, 16);
    ASSERT_EQ(map.slotCount, 16U);
    for (uint32_t channel = 0; channel < 10; ++channel) {
        EXPECT_EQ(map.SlotFor(channel), kExpectedSlots10[channel]);
    }
    for (uint32_t channel = 10; channel < 16; ++channel) {
        EXPECT_EQ(map.SlotFor(channel), channel);
    }
}

TEST(MAudioCaptureChannelMapTests, SixteenFrameSkewAppliesOnlyTo1814LineChannels) {
    const auto map = CaptureChannelMapFor(ProfileBuilderId::MAudioFireWire1814, 10);
    EXPECT_EQ(map.delayFrames, 16U);
    for (uint32_t channel = 0; channel < 10; ++channel) {
        EXPECT_EQ(map.IsDelayed(channel), channel >= 2 && channel <= 7);
    }
    const auto projectMix = CaptureChannelMapFor(ProfileBuilderId::MAudioProjectMix, 10);
    EXPECT_FALSE(projectMix.HasDelay());
    EXPECT_EQ(projectMix.delayFrames, 0U);
}

TEST(MAudioCaptureChannelMapTests, UnknownProfilesAndWidthsUseIdentity) {
    EXPECT_TRUE(CaptureChannelMapFor(ProfileBuilderId::TerraTecPhase88, 10).IsIdentity());
    EXPECT_TRUE(CaptureChannelMapFor(ProfileBuilderId::MAudioFireWire1814, 14).IsIdentity());
}
