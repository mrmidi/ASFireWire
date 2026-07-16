// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "Protocols/AVC/BridgeCo/BridgeCoReadOnlyProbe.hpp"

namespace {

using ASFW::Protocols::AVC::BridgeCo::IsTerraTecPhase88RackFw;
using ASFW::Protocols::AVC::BridgeCo::ParseStreamFormation;
using ASFW::Protocols::AVC::BridgeCo::ParseExtendedStreamFormatListResponse;

TEST(BridgeCoReadOnlyProbeTests, MatchesOnlyExactPhase88Identity) {
    EXPECT_TRUE(IsTerraTecPhase88RackFw(0x000aac, 0x000003));
    EXPECT_FALSE(IsTerraTecPhase88RackFw(0x000aac, 0x000004));
    EXPECT_FALSE(IsTerraTecPhase88RackFw(0x000a92, 0x000003));
}

TEST(BridgeCoReadOnlyProbeTests, ParsesPcmAndMidiSlotsWithoutGuessingPorts) {
    // AM824 compound, BridgeCo 48k rate code, 10 PCM slots and one MIDI slot.
    const uint8_t payload[]{0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d};
    const auto formation = ParseStreamFormation(payload);
    ASSERT_TRUE(formation.has_value());
    EXPECT_EQ(formation->rateCode, 0x04);
    EXPECT_EQ(formation->pcmChannels, 10);
    EXPECT_EQ(formation->midiSlots, 1);
}

TEST(BridgeCoReadOnlyProbeTests, RejectsTruncatedAndUnsupportedFormations) {
    const uint8_t truncated[]{0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06};
    const uint8_t unsupported[]{0x90, 0x40, 0x04, 0x00, 0x01, 0x02, 0x40};
    EXPECT_FALSE(ParseStreamFormation(truncated).has_value());
    EXPECT_FALSE(ParseStreamFormation(unsupported).has_value());
}

TEST(BridgeCoReadOnlyProbeTests, ParsesFormatListAtTheBridgeCoResponseOffset) {
    // The returned list index is operand 7, with the compound formation at 8.
    // This shape is a clean-room fixture derived from the ALSA BeBoB codec tests.
    const uint8_t operands[]{0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x03,
                             0x90, 0x40, 0x04, 0x00, 0x01, 0x0a, 0x06};
    const auto formation = ParseExtendedStreamFormatListResponse(3, operands);
    ASSERT_TRUE(formation.has_value());
    EXPECT_EQ(formation->rateCode, 0x04);
    EXPECT_EQ(formation->pcmChannels, 10);
    EXPECT_EQ(formation->midiSlots, 0);
    EXPECT_FALSE(ParseExtendedStreamFormatListResponse(2, operands).has_value());
}

} // namespace
