// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/AVCCommandFilter.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/MAudioSpecialClock.hpp"

#include <span>

using namespace ASFW::AVC::MAudio;
using ASFW::AVC::CommandType;

TEST(MAudioSpecialCommandTests, InitialClockWriteFitsThePersonaAllowlist) {
    const auto command = SpecialInitialClockCommand();
    ASSERT_TRUE(command.has_value());
    const auto frame = command->Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    const auto wire = frame->WireBytes();
    ASSERT_EQ(wire.size(), 16U);
    EXPECT_EQ(wire[0], 0x00); // CONTROL
    EXPECT_EQ(wire[1], 0xFF); // unit
    EXPECT_EQ(wire[2], 0x00); // vendor-dependent opcode
    EXPECT_EQ(wire[3], 0x04);
    EXPECT_EQ(wire[4], 0x00);
    EXPECT_EQ(wire[5], 0x04);
    EXPECT_EQ(wire[6], 0x03); // internal clock
    EXPECT_EQ(wire[7], 0x00); // SPDIF input
    EXPECT_EQ(wire[8], 0x00); // SPDIF output
    EXPECT_EQ(wire[9], 0x00); // unlocked
    for (size_t i = 10; i < wire.size(); ++i) {
        EXPECT_EQ(wire[i], 0x00) << "padding byte " << i;
    }

    const auto allowed = ASFW::Protocols::AVC::PermittedFramesFor(
        ASFW::Discovery::AvcCommandFilterId::MAudioSpecialBeBoB);
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(allowed, wire));
}

TEST(MAudioSpecialCommandTests, BuildsOnlySupportedClockAndFormatValues) {
    for (uint8_t source = 0; source <= 3; ++source) {
        for (uint8_t input = 0; input <= 1; ++input) {
            for (uint8_t output = 0; output <= 1; ++output) {
                EXPECT_TRUE(SpecialClockCommand(static_cast<SpecialClockSource>(source),
                                                static_cast<SpecialDigitalFormat>(input),
                                                static_cast<SpecialDigitalFormat>(output), false));
            }
        }
    }
    EXPECT_FALSE(SpecialClockCommand(static_cast<SpecialClockSource>(4), SpecialDigitalFormat::Spdif,
                                     SpecialDigitalFormat::Spdif, false));
    EXPECT_FALSE(SpecialClockCommand(SpecialClockSource::Internal, static_cast<SpecialDigitalFormat>(2),
                                     SpecialDigitalFormat::Spdif, false));
}

TEST(MAudioSpecialCommandTests, PermittedFramesMatchTheAllowlistContract) {
    const auto table = ASFW::Protocols::AVC::PermittedFramesFor(ASFW::Discovery::AvcCommandFilterId::MAudioSpecialBeBoB);
    ASSERT_EQ(table.size(), 7U);

    // 1. sig-fmt STATUS input plug
    const uint8_t inSigFmtStatus[] = {0x01, 0xFF, 0x19, 0x00, 0x90, 0x00, 0x00, 0x00};
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, inSigFmtStatus));

    // 2. sig-fmt STATUS output plug
    const uint8_t outSigFmtStatus[] = {0x01, 0xFF, 0x18, 0x00, 0x90, 0x00, 0x00, 0x00};
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, outSigFmtStatus));

    // 3. sig-fmt CONTROL input plug
    const uint8_t inSigFmtCtrl[] = {0x00, 0xFF, 0x19, 0x00, 0x90, 0x02, 0x00, 0x00};
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, inSigFmtCtrl));

    // 4. sig-fmt CONTROL output plug
    const uint8_t outSigFmtCtrl[] = {0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0x00, 0x00};
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, outSigFmtCtrl));

    // 5. M-Audio vendor clock/format (16 bytes, lock = 0x00)
    const uint8_t clkCmd[] = {
        0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, clkCmd));

    // 6. M-Audio blank-slate input selector (12 bytes)
    const uint8_t selCmd[] = {
        0x00, 0x08, 0xB8, 0x80, 0x04, 0x10, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, selCmd));

    // 7. M-Audio LED (8 bytes)
    const uint8_t ledCmd[] = {0x00, 0xFF, 0x00, 0x03, 0x00, 0x01, 0x01, 0x00};
    EXPECT_TRUE(ASFW::Protocols::AVC::FrameIsPermitted(table, ledCmd));

    // Ensure hazardous frames are blocked:
    // SUBUNIT_INFO (0x31)
    const uint8_t subInfo[] = {0x01, 0xFF, 0x31, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(ASFW::Protocols::AVC::FrameIsPermitted(table, subInfo));

    // PLUG_INFO (0x02)
    const uint8_t plugInfo[] = {0x01, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(ASFW::Protocols::AVC::FrameIsPermitted(table, plugInfo));

    // Extended stream format (0x2F / 0xBF)
    const uint8_t extFmt[] = {0x01, 0xFF, 0x2F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    EXPECT_FALSE(ASFW::Protocols::AVC::FrameIsPermitted(table, extFmt));

    // UNIT_INFO (0x30)
    const uint8_t unitInfo[] = {0x01, 0xFF, 0x30, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(ASFW::Protocols::AVC::FrameIsPermitted(table, unitInfo));
}
