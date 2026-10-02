// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicSubunitDescriptorTests.cpp - Unit tests for Music Subunit Status Descriptor
// parsing against captured hardware fixtures (Apogee Duet & Terratec Phase 88).
//

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Descriptors/MusicSubunitDescriptor.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Descriptors::Test {

namespace {

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        if (i + 1 < hex.size()) {
            uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
            bytes.push_back(byte);
        }
    }
    return bytes;
}

// An AV/C info block: compound_length, type, primary_fields_length, primary
// fields, nested blocks (TA 1999045).
std::vector<uint8_t> InfoBlock(uint16_t type, std::vector<uint8_t> primary,
                               const std::vector<uint8_t>& nested = {}) {
    std::vector<uint8_t> out;
    const size_t compound = 4 + primary.size() + nested.size();
    out.push_back(static_cast<uint8_t>(compound >> 8));
    out.push_back(static_cast<uint8_t>(compound));
    out.push_back(static_cast<uint8_t>(type >> 8));
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>(primary.size() >> 8));
    out.push_back(static_cast<uint8_t>(primary.size()));
    out.insert(out.end(), primary.begin(), primary.end());
    out.insert(out.end(), nested.begin(), nested.end());
    return out;
}

// Source plug 0 labelled with `labels` (one raw text block), and audio music
// plugs 12, 13, 14 routed from destination plug 1 to source plug 0.
Parsed<MusicSubunitStatus> ParseSourcePlugLabels(const std::string& labels) {
    const auto raw = InfoBlock(0x000A, std::vector<uint8_t>(labels.begin(), labels.end()));
    std::vector<uint8_t> namePrimary{0x00, 0x00, 0xFF, 0xFF};
    namePrimary.insert(namePrimary.end(), raw.begin(), raw.end());
    const auto name = InfoBlock(0x000B, namePrimary);
    const auto audioInfo = InfoBlock(0x8103, {3}, name);
    const auto sourceStatus = InfoBlock(0x8102, {0}, audioInfo);
    const auto outputArea = InfoBlock(0x8101, {1}, sourceStatus);
    std::vector<uint8_t> musicPlugs;
    for (uint8_t id = 12; id <= 14; ++id) {
        const auto block = InfoBlock(0x810B, {0x00, 0x00, id, 0x00,
                                              0xF0, 0x01, 0xFF, static_cast<uint8_t>(id - 12), 0x01,
                                              0xF1, 0x00, 0xFF, static_cast<uint8_t>(id - 12), 0x01});
        musicPlugs.insert(musicPlugs.end(), block.begin(), block.end());
    }
    const auto routing = InfoBlock(0x8108, {0x01, 0x01}, musicPlugs);
    std::vector<uint8_t> body = outputArea;
    body.insert(body.end(), routing.begin(), routing.end());
    std::vector<uint8_t> descriptor{static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size())};
    descriptor.insert(descriptor.end(), body.begin(), body.end());
    return MusicSubunitDescriptorParser::ParseStatusDescriptor(descriptor);
}

} // namespace

// =============================================================================
// Duet Music Subunit Status Descriptor Tests (464 bytes, fixtures/duet_descriptors.json)
// =============================================================================

TEST(MusicSubunitDescriptorTests, DuetMusicStatusDescriptorParsing) {
    const std::string duetHex =
        "01ce000a810000060101ffffffff01c08108000403030005002e8109000800900200000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f757400002d810900080190020500010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e0000248109000802900203000100010016810a0007400901000400ff0009000a000553796e6300002d810900080090020000010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e00002e8109000801900205000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f75740000248109000802900203000100010016810a0007400901000400ff0009000a000553796e63000025810b000e00000000f000ff00fff101ff00ff0011000a000d416e616c6f67204f75742031000025810b000e00000100f000ff01fff101ff01ff0011000a000d416e616c6f67204f75742032000024810b000e00000200f001ff00fff100ff00ff0010000a000c416e616c6f6720496e2031000024810b000e00000300f001ff01fff100ff01ff0010000a000c416e616c6f6720496e2032000012810b000e80000400f002ff00fff102ff00ff";

    const auto bytes = HexToBytes(duetHex);
    ASSERT_EQ(bytes.size(), 464u);

    auto result = MusicSubunitDescriptorParser::ParseStatusDescriptor(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->declaredLength, 462);

    // General Capabilities (0x8100)
    EXPECT_TRUE(result->capabilities.hasGeneralCapability);
    EXPECT_EQ(result->capabilities.transmitCapabilityFlags, 0x01);
    EXPECT_EQ(result->capabilities.receiveCapabilityFlags, 0x01);

    // Routing Status (0x8108)
    EXPECT_TRUE(result->hasRoutingStatus);
    EXPECT_EQ(result->numDestPlugs, 3);
    EXPECT_EQ(result->numSrcPlugs, 3);

    // Total plugs: 3 destination + 3 source = 6 plugs
    ASSERT_EQ(result->plugs.size(), 6u);

    // Destination Plug 0: Analog Out
    const auto* dest0 = result->FindPlug(0, true);
    ASSERT_NE(dest0, nullptr);
    EXPECT_EQ(dest0->usage, 0x00);
    ASSERT_EQ(dest0->clusters.size(), 1u);
    EXPECT_EQ(dest0->clusters[0].streamFormatCode, 0x06); // AM824
    EXPECT_EQ(dest0->clusters[0].channelCount, 2);
    EXPECT_EQ(dest0->clusters[0].name, "Analog Out");
    ASSERT_EQ(dest0->clusters[0].signals.size(), 2u);
    EXPECT_EQ(dest0->clusters[0].signals[0].musicPlugId, 0x0000);
    EXPECT_EQ(dest0->clusters[0].signals[1].musicPlugId, 0x0001);

    // Destination Plug 1: Analog In
    const auto* dest1 = result->FindPlug(1, true);
    ASSERT_NE(dest1, nullptr);
    EXPECT_EQ(dest1->usage, 0x05);
    ASSERT_EQ(dest1->clusters.size(), 1u);
    EXPECT_EQ(dest1->clusters[0].name, "Analog In");

    // Destination Plug 2: Sync
    const auto* dest2 = result->FindPlug(2, true);
    ASSERT_NE(dest2, nullptr);
    EXPECT_EQ(dest2->usage, 0x03);
    ASSERT_EQ(dest2->clusters.size(), 1u);
    EXPECT_EQ(dest2->clusters[0].streamFormatCode, 0x40); // Sync
    EXPECT_EQ(dest2->clusters[0].channelCount, 1);
    EXPECT_EQ(dest2->clusters[0].name, "Sync");

    // Music Plugs (0x810B) - 5 music plugs in Duet
    ASSERT_EQ(result->musicPlugs.size(), 5u);

    const auto* mp0 = result->FindMusicPlug(0x0000);
    ASSERT_NE(mp0, nullptr);
    EXPECT_EQ(mp0->name, "Analog Out 1");

    const auto* mp1 = result->FindMusicPlug(0x0001);
    ASSERT_NE(mp1, nullptr);
    EXPECT_EQ(mp1->name, "Analog Out 2");

    const auto* mp2 = result->FindMusicPlug(0x0002);
    ASSERT_NE(mp2, nullptr);
    EXPECT_EQ(mp2->name, "Analog In 1");

    const auto* mp3 = result->FindMusicPlug(0x0003);
    ASSERT_NE(mp3, nullptr);
    EXPECT_EQ(mp3->name, "Analog In 2");

    const auto* mp4 = result->FindMusicPlug(0x0004);
    ASSERT_NE(mp4, nullptr);
}

// =============================================================================
// Phase 88 Music Subunit Status Descriptor Tests (2410 bytes, fixtures/phase88_descriptors.json)
// =============================================================================

TEST(MusicSubunitDescriptorTests, Phase88MusicStatusDescriptorParsing) {
    const std::string phase88Hex =
        "0968000a810000060203ffffffff02bf8101000104015d81020001000119810300010a0112000b010e0000ffff0108000a01044c696e655f312f32206c65667420504841534538382046570d0a4c696e655f312f3220726967687420504841534538382046570d0a4c696e655f332f34206c65667420504841534538382046570d0a4c696e655f332f3420726967687420504841534538382046570d0a4c696e655f352f36206c65667420504841534538382046570d0a4c696e655f352f3620726967687420504841534538382046570d0a4c696e655f372f38206c65667420504841534538382046570d0a4c696e655f372f3820726967687420504841534538382046570d0a5350444946206c65667420504841534538382046570d0a535044494620726967687420504841534538382046570d0a00003b81040001020019000b00150000ffff000f000a000b4d696469506f72745f31000019000b00150000ffff000f000a000b4d696469506f72745f320000f5810200010100ee810300010800e7000b00e30000ffff00dd000a00d94d756c74696368616e6e656c203120504841534538382046570d0a4d756c74696368616e6e656c203220504841534538382046570d0a4d756c74696368616e6e656c203320504841534538382046570d0a4d756c74696368616e6e656c203420504841534538382046570d0a4d756c74696368616e6e656c203520504841534538382046570d0a4d756c74696368616e6e656c203620504841534538382046570d0a4d756c74696368616e6e656c203720504841534538382046570d0a4d756c74696368616e6e656c203820504841534538382046570d0a0000548102000102004d81030001020046000b00420000ffff003c000a003853504449462f414333206c65667420504841534538382046570d0a53504449462f41433320726967687420504841534538382046570d0a00000c8102000105000581070001030699810800040a04001900bf81090008009001000003000b0053810a00230603080000010100010602000202030003070400040305000508060006040700070908002a000b00260000ffff0020000a001c50484153453838204657204d756c74696368616e6e656c204f757400002d810a000b0004020008000100090502001c000b00180000ffff0012000a000e53504449462f414333204f757400002d810a000b0d0a02000a0a01000a0a02001c000b00180000ffff0012000a000e4d69646953656374696f6e2e30000039810900080190010000010002002b810a000b060302000c0001000d0102001a000b00160000ffff0010000a000c4c696e6520496e20312f32000039810900080290010000010002002b810a000b060302000e0001000f0102001a000b00160000ffff0010000a000c4c696e6520496e20332f34000039810900080390010000010002002b810a000b0603020010000100110102001a000b00160000ffff0010000a000c4c696e6520496e20352f36000039810900080490010000010002002b810a000b0603020012000100130102001a000b00160000ffff0010000a000c4c696e6520496e20372f380000368109000805900100000100020028810a000b06040200140001001501020017000b00130000ffff000d000a0009535044494620496e000039810900080690010200010002002b810a000b0d0a020016000100170102001a000b00160000ffff0010000a000c4d69646953656374696f6e0000318109000807900102000100020023810a00030d0a00001a000b00160000ffff0010000a000c4d69646953656374696f6e00002f8109000808900103000100020021810a0007400901001800010014000b00100000ffff000a000a000653796e636800002b810900080990010300010002001d810a00034009000014000b00100000ffff000a000a000653796e636800011981090008009001000006000b002b810a000b060302000c0101000d0602001a000b00160000ffff0010000a000c4c696e6520496e20312f3200002b810a000b060302000e0201000f0702001a000b00160000ffff0010000a000c4c696e6520496e20332f3400002b810a000b0603020010030100110802001a000b00160000ffff0010000a000c4c696e6520496e20352f3600002b810a000b0603020012040100130902001a000b00160000ffff0010000a000c4c696e6520496e20372f38000028810a000b06040200140001001505020017000b00130000ffff000d000a0009535044494620496e00002d810a000b0d0a0200160a0100160a02001c000b00180000ffff0012000a000e4d69646953656374696f6e2e300000618109000801900100000100080053810a00230603080000000100010402000201030003050400040205000506060006030700070708002a000b00260000ffff0020000a001c50484153453838204657204d756c74696368616e6e656c204f757400003b810900080290010000010002002d810a000b0004020008000100090102001c000b00180000ffff0012000a000e53504449462f414333204f757400002f8109000805900103000100020021810a0007400901001800010014000b00100000ffff000a000a000653796e6368000012810b000e00000000f000ff0102f101ff00010012810b000e00000100f000ff0601f101ff04010012810b000e00000200f000ff0201f101ff01020012810b000e00000300f000ff0702f101ff05020012810b000e00000400f000ff0302f101ff02010012810b000e00000500f000ff0801f101ff06010012810b000e00000600f000ff0401f101ff03020012810b000e00000700f000ff0902f101ff07020012810b000e00000800f000ff0001f102ff00010012810b000e00000900f000ff0502f102ff01020012810b000e01000a00f000ff0a01f1ffff00010012810b000e01000b00f000ff0a01f1ffff00010012810b000e00000c00f001ff0001f100ff01020012810b000e00000d00f001ff0102f100ff06010012810b000e00000e00f002ff0001f100ff02010012810b000e00000f00f002ff0102f100ff07020012810b000e00001000f003ff0001f100ff03020012810b000e00001100f003ff0102f100ff08010012810b000e00001200f004ff0001f100ff04010012810b000e00001300f004ff0102f100ff09020012810b000e00001400f005ff0001f100ff00010012810b000e00001500f005ff0102f100ff05020012810b000e01001600f006ff0001f100ff0a010012810b000e01001700f006ff0102f100ff0a010012810b000e80001800f008ff0001f105ff0001";

    const auto bytes = HexToBytes(phase88Hex);
    ASSERT_EQ(bytes.size(), 2410u);

    auto result = MusicSubunitDescriptorParser::ParseStatusDescriptor(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->declaredLength, 2408);
    const auto* capture = result->FindPlug(0, false);
    ASSERT_NE(capture, nullptr);
    std::vector<uint8_t> slots;
    for (const auto& cluster : capture->clusters)
        for (const auto& signal : cluster.signals) slots.push_back(signal.position);
    EXPECT_EQ(slots, (std::vector<uint8_t>{1, 6, 2, 7, 3, 8, 4, 9, 0, 5, 10, 10}));


    // General Capabilities (0x8100)
    EXPECT_TRUE(result->capabilities.hasGeneralCapability);
    EXPECT_EQ(result->capabilities.transmitCapabilityFlags, 0x02);
    EXPECT_EQ(result->capabilities.receiveCapabilityFlags, 0x03);

    // Routing Status (0x8108)
    EXPECT_TRUE(result->hasRoutingStatus);
    EXPECT_EQ(result->numDestPlugs, 10);
    EXPECT_EQ(result->numSrcPlugs, 4);

    // Total plugs: 10 dest + 4 src = 14 plugs
    ASSERT_EQ(result->plugs.size(), 14u);

    // Dest Plug 0: 8-channel multichannel out
    const auto* dest0 = result->FindPlug(0, true);
    ASSERT_NE(dest0, nullptr);
    ASSERT_FALSE(dest0->clusters.empty());
    EXPECT_EQ(dest0->clusters[0].streamFormatCode, 0x06); // AM824
    EXPECT_EQ(dest0->clusters[0].portType, 0x03);         // line
    EXPECT_EQ(dest0->clusters[0].channelCount, 8);
    EXPECT_EQ(dest0->clusters[0].signals.size(), 8u);

    // Music Plugs (0x810B) - 25 music plugs in Phase 88 (0x0000..0x0018)
    ASSERT_EQ(result->musicPlugs.size(), 25u);
    EXPECT_NE(result->FindMusicPlug(0x0000), nullptr);
    EXPECT_NE(result->FindMusicPlug(0x0007), nullptr);
    EXPECT_NE(result->FindMusicPlug(0x0018), nullptr);

    // Per-plug channel names from 0x8101 -> 0x8102 -> 0x8103
    ASSERT_TRUE(result->perPlugChannelNames.contains(0));
    EXPECT_EQ(result->perPlugChannelNames[0].size(), 10u);
    EXPECT_EQ(result->perPlugChannelNames[0][0], "Line_1/2 left PHASE88 FW");

    // Music plug routing (0x810B, 14-byte primary fields): music plug 0 runs
    // from destination plug 0 position 1 to source plug 1 position 0.
    const auto* mp0 = result->FindMusicPlug(0);
    ASSERT_NE(mp0, nullptr);
    ASSERT_TRUE(mp0->source.has_value());
    ASSERT_TRUE(mp0->destination.has_value());
    EXPECT_EQ(mp0->source->functionType, MusicPlugEndpoint::kSubunitDestinationPlug);
    EXPECT_EQ(mp0->source->streamPosition, 1);
    EXPECT_EQ(mp0->destination->functionType, MusicPlugEndpoint::kSubunitSourcePlug);
    EXPECT_EQ(mp0->destination->plugId, 1);

    // Labels per music plug (TA 2001007 Table 6.2): the k-th audio music plug
    // routed to a source plug gets that plug's k-th label.
    EXPECT_EQ(result->musicPlugLabels.at(0), "Multichannel 1 PHASE88 FW");
    EXPECT_EQ(result->musicPlugLabels.at(1), "Multichannel 2 PHASE88 FW");
    EXPECT_EQ(result->musicPlugLabels.at(8), "SPDIF/AC3 left PHASE88 FW");
    EXPECT_EQ(result->musicPlugLabels.at(12), "Line_1/2 left PHASE88 FW");
    EXPECT_EQ(result->musicPlugLabels.at(21), "SPDIF right PHASE88 FW");
    EXPECT_FALSE(result->musicPlugLabels.contains(10)); // MIDI plug: no audio label
}

TEST(MusicSubunitDescriptorTests, UnlabelledStreamKeepsItsPlaceInTheLabelList) {
    // TA 2001007 §6.2.3.1: "If a music plug does not have a label, then (CR)(LF/NL)
    // follow consecutively." A dropped empty entry would shift every later label.
    auto status = ParseSourcePlugLabels("A\r\n\r\nC\r\n");
    ASSERT_TRUE(status.has_value());
    const auto& names = status->perPlugChannelNames.at(0);
    ASSERT_GE(names.size(), 3U);
    EXPECT_EQ(names[0], "A");
    EXPECT_EQ(names[1], "");
    EXPECT_EQ(names[2], "C");
    EXPECT_EQ(status->musicPlugLabels.at(12), "A");
    EXPECT_FALSE(status->musicPlugLabels.contains(13));
    EXPECT_EQ(status->musicPlugLabels.at(14), "C");
}

} // namespace ASFW::Protocols::AVC::Descriptors::Test
