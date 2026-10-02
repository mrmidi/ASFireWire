// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include <vector>

#include "../../ASFWDriver/Audio/Protocols/BeBoB/BeBoBCaptureChannelMap.hpp"

namespace {

using ASFW::Audio::BeBoBProbe::CaptureChannelMapFromSections;
using ASFW::Audio::BeBoBProbe::ChannelMapFromSections;
using ASFW::Audio::BeBoB::ChannelPosition;
using ASFW::Audio::BeBoB::ChannelSection;

constexpr uint8_t kLineSectionType = 0x03;
constexpr uint8_t kMidiSectionType = 0x0a;

[[nodiscard]] ChannelSection Section(
    uint8_t type, std::initializer_list<ChannelPosition> positions) {
    return {.type = type, .positions = positions};
}

} // namespace

TEST(BeBoBCaptureChannelMapTests, ReordersPlanarWireSectionToLogicalChannels) {
    // The stream slots are planar L1, L2, R1, R2 while the advertised section
    // locations put them in CoreAudio order L1, R1, L2, R2.
    std::vector<ChannelSection> capture;
    capture.push_back(Section(kLineSectionType, {
        {0, 0}, {1, 2}, {2, 1}, {3, 3},
    }));

    const auto map = CaptureChannelMapFromSections(capture, 4, 5);

    ASSERT_EQ(map.slotCount, 4u);
    EXPECT_EQ(map.channelCount, 4u);
    EXPECT_EQ(map.SlotFor(0), 0u);
    EXPECT_EQ(map.SlotFor(1), 2u);
    EXPECT_EQ(map.SlotFor(2), 1u);
    EXPECT_EQ(map.SlotFor(3), 3u);
    EXPECT_TRUE(map.FitsWithin(4, 5));

    // The host-to-device plug uses the same BridgeCo channel-position contract.
    // Playback has no capture delay, but it must place host PCM in the same
    // advertised AM824 slots.
    const auto playbackMap = ChannelMapFromSections(capture, 4, 5);
    EXPECT_EQ(playbackMap.SlotFor(0), 0u);
    EXPECT_EQ(playbackMap.SlotFor(1), 2u);
    EXPECT_EQ(playbackMap.SlotFor(2), 1u);
    EXPECT_EQ(playbackMap.SlotFor(3), 3u);
}

TEST(BeBoBCaptureChannelMapTests, MidiSectionDoesNotConsumeAPcmChannel) {
    std::vector<ChannelSection> capture;
    capture.push_back(Section(kLineSectionType, {
        {0, 0}, {1, 1},
    }));
    capture.push_back(Section(kMidiSectionType, {
        {2, 0},
    }));
    capture.push_back(Section(kLineSectionType, {
        {3, 0}, {4, 1},
    }));

    const auto map = CaptureChannelMapFromSections(capture, 4, 5);

    ASSERT_EQ(map.slotCount, 4u);
    EXPECT_EQ(map.SlotFor(0), 0u);
    EXPECT_EQ(map.SlotFor(1), 1u);
    EXPECT_EQ(map.SlotFor(2), 3u);
    EXPECT_EQ(map.SlotFor(3), 4u);
}

TEST(BeBoBCaptureChannelMapTests, InvalidOrIncompleteEvidenceFailsClosedToIdentity) {
    std::vector<ChannelSection> duplicateLocation;
    duplicateLocation.push_back(Section(kLineSectionType, {
        {0, 0}, {1, 0},
    }));
    EXPECT_TRUE(CaptureChannelMapFromSections(duplicateLocation, 2, 2).IsIdentity());

    std::vector<ChannelSection> missingSectionType;
    missingSectionType.push_back({
        .positions = {{0, 0}, {1, 1}},
    });
    EXPECT_TRUE(CaptureChannelMapFromSections(missingSectionType, 2, 2).IsIdentity());

    std::vector<ChannelSection> outOfRangeSlot;
    outOfRangeSlot.push_back(Section(kLineSectionType, {
        {0, 0}, {2, 1},
    }));
    EXPECT_TRUE(CaptureChannelMapFromSections(outOfRangeSlot, 2, 2).IsIdentity());
}

TEST(BeBoBCaptureChannelMapTests, IdentityReplyKeepsTheDecoderFastPath) {
    std::vector<ChannelSection> capture;
    capture.push_back(Section(kLineSectionType, {
        {0, 0}, {1, 1},
    }));

    const auto map = CaptureChannelMapFromSections(capture, 2, 3);
    EXPECT_TRUE(map.IsIdentity());
    EXPECT_TRUE(map.FitsWithin(2, 3));
}
