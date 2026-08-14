// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Wire contract for the M-Audio blank-slate clock sequence. These bytes are
// pinned against the vendor-kext FireBug trace; changing an operand can leave
// playback alive while the device-to-host stream emits NO-DATA forever.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioClockCommand.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

using namespace ASFW::Audio::BeBoB;

TEST(MAudioClockCommandTests, BlankSlateFrameMatchesTheVendorDriver) {
    constexpr auto frame = BuildMAudioClockCommand(
        MAudioClockSource::InternalDigitalMute,
        MAudioClockDigitalFormat::SPDIF,
        MAudioClockDigitalFormat::SPDIF,
        false);
    constexpr std::array<uint8_t, kMAudioClockCommandBytes> expected{
        0x00, 0xFF, 0x00, 0x04, 0x00, 0x04,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00};

    EXPECT_EQ(frame, expected);
}

TEST(MAudioClockCommandTests, PlainInternalRemainsADistinctLinuxVariant) {
    constexpr auto vendorBlankSlate = BuildMAudioClockCommand(
        MAudioClockSource::InternalDigitalMute,
        MAudioClockDigitalFormat::SPDIF,
        MAudioClockDigitalFormat::SPDIF,
        false);
    constexpr auto linuxDiscovery = BuildMAudioClockCommand(
        MAudioClockSource::Internal,
        MAudioClockDigitalFormat::SPDIF,
        MAudioClockDigitalFormat::SPDIF,
        false);

    EXPECT_EQ(vendorBlankSlate[6], 0x00U);
    EXPECT_EQ(linuxDiscovery[6], 0x03U);
    for (size_t index = 0; index < vendorBlankSlate.size(); ++index) {
        if (index == 6) continue;
        EXPECT_EQ(vendorBlankSlate[index], linuxDiscovery[index]) << index;
    }
}

TEST(MAudioClockCommandTests, BlankSlateTimingIncludesBothInnerInterlocks) {
    EXPECT_EQ(kMAudioClockToSelectorInterlockMs, 300U);
    EXPECT_EQ(kMAudioClockSelectorSettleMs, 300U);
    EXPECT_EQ(kMAudioClockSettleMs, 2500U);
    EXPECT_EQ(kMAudioPostSelectorSettleMs, 2800U);
}

TEST(MAudioClockCommandTests, BlankSlateSelectorIsFunctionBlockFourInputZero) {
    EXPECT_EQ(kMAudioDigitalInputSelectorBlockId, 0x04U);
    EXPECT_EQ(kMAudioDefaultDigitalInputInterface, 0x00U);
}
