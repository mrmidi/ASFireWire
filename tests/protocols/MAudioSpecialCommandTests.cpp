// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/AVCCommandFilter.hpp"
#include "ASFWDriver/Protocols/AVC/MAudioSpecialCommand.hpp"

#include <span>

using namespace ASFW::Protocols::AVC;

TEST(MAudioSpecialCommandTests, InitialClockWriteFitsThePersonaAllowlist) {
    const auto cdb = BuildMAudioSpecialInitialClockCommand();
    ASSERT_TRUE(cdb.has_value());
    const auto frame = cdb->Encode();
    ASSERT_EQ(frame.length, 16U);
    EXPECT_EQ(frame.data[0], 0x00); // CONTROL
    EXPECT_EQ(frame.data[1], 0xFF); // unit
    EXPECT_EQ(frame.data[2], 0x00); // vendor-dependent opcode
    EXPECT_EQ(frame.data[3], 0x04);
    EXPECT_EQ(frame.data[4], 0x00);
    EXPECT_EQ(frame.data[5], 0x04);
    EXPECT_EQ(frame.data[6], 0x03); // internal clock
    EXPECT_EQ(frame.data[7], 0x00); // SPDIF input
    EXPECT_EQ(frame.data[8], 0x00); // SPDIF output
    EXPECT_EQ(frame.data[9], 0x00); // unlocked
    for (size_t i = 10; i < frame.length; ++i) {
        EXPECT_EQ(frame.data[i], 0x00) << "padding byte " << i;
    }

    const auto allowed = PermittedFramesFor(
        ASFW::Discovery::AvcCommandFilterId::MAudioSpecialBeBoB);
    EXPECT_TRUE(FrameIsPermitted(
        allowed, std::span<const uint8_t>(frame.data.data(), frame.length)));
}

TEST(MAudioSpecialCommandTests, BuildsOnlySupportedClockAndFormatValues) {
    for (uint8_t source = 0; source <= 3; ++source) {
        for (uint8_t input = 0; input <= 1; ++input) {
            for (uint8_t output = 0; output <= 1; ++output) {
                EXPECT_TRUE(BuildMAudioSpecialClockCommand(
                    static_cast<MAudioSpecialClockSource>(source),
                    static_cast<MAudioSpecialDigitalFormat>(input),
                    static_cast<MAudioSpecialDigitalFormat>(output), false));
            }
        }
    }
    EXPECT_FALSE(BuildMAudioSpecialClockCommand(
        static_cast<MAudioSpecialClockSource>(4), MAudioSpecialDigitalFormat::Spdif,
        MAudioSpecialDigitalFormat::Spdif, false));
    EXPECT_FALSE(BuildMAudioSpecialClockCommand(
        MAudioSpecialClockSource::Internal,
        static_cast<MAudioSpecialDigitalFormat>(2),
        MAudioSpecialDigitalFormat::Spdif, false));
}
