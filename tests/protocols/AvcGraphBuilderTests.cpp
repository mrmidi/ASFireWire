// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcGraphBuilderTests.cpp - Unit tests for AppleFWAudio graph rules and
// AvcGraphBuilder against captured hardware fixtures (TerraTec Phase 88 and Apogee Duet).
//

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Descriptors/AudioSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/MusicSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Graph/AvcGraphBuilder.hpp"

#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Graph::Test {

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

// -----------------------------------------------------------------------------
// Fixture Hex Dumps
// -----------------------------------------------------------------------------

// Apogee Duet Music Status Descriptor (0x80, 464 bytes)
const std::string kDuetMusicStatusHex =
    "01ce000a810000060101ffffffff01c08108000403030005002e8109000800900200000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f757400002d810900080190020500010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e0000248109000802900203000100010016810a0007400901000400ff0009000a000553796e6300002d810900080090020000010002001f810a000b060302000200ff000301ff000e000a000a416e616c6f6720496e00002e8109000801900205000100020020810a000b060302000000ff000101ff000f000a000b416e616c6f67204f75740000248109000802900203000100010016810a0007400901000400ff0009000a000553796e63000025810b000e00000000f000ff00fff101ff00ff0011000a000d416e616c6f67204f75742031000025810b000e00000100f000ff01fff101ff01ff0011000a000d416e616c6f67204f75742032000024810b000e00000200f001ff00fff100ff00ff0010000a000c416e616c6f6720496e2031000024810b000e00000300f001ff01fff100ff01ff0010000a000c416e616c6f6720496e2032000012810b000e80000400f002ff00fff102ff00ff";

// Apogee Duet Audio Subunit Identifier (0x00, 56 bytes)
const std::string kDuetAudioIdHex =
    "0036000200020000002c002a00010026000000040202c0000181000100188101ffff01f00000040202c00000090802000003000200020000";

// Phase 88 Music Status Descriptor (0x80, 2410 bytes)
const std::string kPhase88MusicStatusHex =
    "0968000a810000060203ffffffff02bf8101000104015d81020001000119810300010a0112000b010e0000ffff0108000a01044c696e655f312f32206c65667420504841534538382046570d0a4c696e655f312f3220726967687420504841534538382046570d0a4c696e655f332f34206c65667420504841534538382046570d0a4c696e655f332f3420726967687420504841534538382046570d0a4c696e655f352f36206c65667420504841534538382046570d0a4c696e655f352f3620726967687420504841534538382046570d0a4c696e655f372f38206c65667420504841534538382046570d0a4c696e655f372f3820726967687420504841534538382046570d0a5350444946206c65667420504841534538382046570d0a535044494620726967687420504841534538382046570d0a00003b81040001020019000b00150000ffff000f000a000b4d696469506f72745f31000019000b00150000ffff000f000a000b4d696469506f72745f320000f5810200010100ee810300010800e7000b00e30000ffff00dd000a00d94d756c74696368616e6e656c203120504841534538382046570d0a4d756c74696368616e6e656c203220504841534538382046570d0a4d756c74696368616e6e656c203320504841534538382046570d0a4d756c74696368616e6e656c203420504841534538382046570d0a4d756c74696368616e6e656c203520504841534538382046570d0a4d756c74696368616e6e656c203620504841534538382046570d0a4d756c74696368616e6e656c203720504841534538382046570d0a4d756c74696368616e6e656c203820504841534538382046570d0a0000548102000102004d81030001020046000b00420000ffff003c000a003853504449462f414333206c65667420504841534538382046570d0a53504449462f41433320726967687420504841534538382046570d0a00000c8102000105000581070001030699810800040a04001900bf81090008009001000003000b0053810a00230603080000010100010602000202030003070400040305000508060006040700070908002a000b00260000ffff0020000a001c50484153453838204657204d756c74696368616e6e656c204f757400002d810a000b0004020008000100090502001c000b00180000ffff0012000a000e53504449462f414333204f757400002d810a000b0d0a02000a0a01000a0a02001c000b00180000ffff0012000a000e4d69646953656374696f6e2e30000039810900080190010000010002002b810a000b060302000c0001000d0102001a000b00160000ffff0010000a000c4c696e6520496e20312f32000039810900080290010000010002002b810a000b060302000e0001000f0102001a000b00160000ffff0010000a000c4c696e6520496e20332f34000039810900080390010000010002002b810a000b0603020010000100110102001a000b00160000ffff0010000a000c4c696e6520496e20352f36000039810900080490010000010002002b810a000b0603020012000100130102001a000b00160000ffff0010000a000c4c696e6520496e20372f380000368109000805900100000100020028810a000b06040200140001001501020017000b00130000ffff000d000a0009535044494620496e000039810900080690010200010002002b810a000b0d0a020016000100170102001a000b00160000ffff0010000a000c4d69646953656374696f6e0000318109000807900102000100020023810a00030d0a00001a000b00160000ffff0010000a000c4d69646953656374696f6e00002f8109000808900103000100020021810a0007400901001800010014000b00100000ffff000a000a000653796e636800002b810900080990010300010002001d810a00034009000014000b00100000ffff000a000a000653796e636800011981090008009001000006000b002b810a000b060302000c0101000d0602001a000b00160000ffff0010000a000c4c696e6520496e20312f3200002b810a000b060302000e0201000f0702001a000b00160000ffff0010000a000c4c696e6520496e20332f3400002b810a000b0603020010030100110802001a000b00160000ffff0010000a000c4c696e6520496e20352f3600002b810a000b0603020012040100130902001a000b00160000ffff0010000a000c4c696e6520496e20372f38000028810a000b06040200140001001505020017000b00130000ffff000d000a0009535044494620496e00002d810a000b0d0a0200160a0100160a02001c000b00180000ffff0012000a000e4d69646953656374696f6e2e300000618109000801900100000100080053810a00230603080000000100010402000201030003050400040205000506060006030700070708002a000b00260000ffff0020000a001c50484153453838204657204d756c74696368616e6e656c204f757400003b810900080290010000010002002d810a000b0004020008000100090102001c000b00180000ffff0012000a000e53504449462f414333204f757400002f8109000805900103000100020021810a0007400901001800010014000b00100000ffff000a000a000653796e6368000012810b000e00000000f000ff0102f101ff00010012810b000e00000100f000ff0601f101ff04010012810b000e00000200f000ff0201f101ff01020012810b000e00000300f000ff0702f101ff05020012810b000e00000400f000ff0302f101ff02010012810b000e00000500f000ff0801f101ff06010012810b000e00000600f000ff0401f101ff03020012810b000e00000700f000ff0902f101ff07020012810b000e00000800f000ff0001f102ff00010012810b000e00000900f000ff0502f102ff01020012810b000e01000a00f000ff0a01f1ffff00010012810b000e01000b00f000ff0a01f1ffff00010012810b000e00000c00f001ff0001f100ff01020012810b000e00000d00f001ff0102f100ff06010012810b000e00000e00f002ff0001f100ff02010012810b000e00000f00f002ff0102f100ff07020012810b000e00001000f003ff0001f100ff03020012810b000e00001100f003ff0102f100ff08010012810b000e00001200f004ff0001f100ff04010012810b000e00001300f004ff0102f100ff09020012810b000e00001400f005ff0001f100ff00010012810b000e00001500f005ff0102f100ff05020012810b000e01001600f006ff0001f100ff0a010012810b000e01001700f006ff0102f100ff0a010012810b000e80001800f008ff0001f105ff0001";

// Phase 88 Audio Subunit Identifier (0x00, 658 bytes)
const std::string kPhase88AudioIdHex =
    "02900202000200011800028402820001027e000100080202c000000000010bf002f003f004800af0068009800180028003800480051600448101000201820100220802800140012001100108010401020101010003000400050006000700080009000a00170015000200c000c000c000c000c000c000c000c000c00000208102000b01f002000a020280014001000c000d000b0009000200c000c000c00000208103000e01f003000a020280014001000f0010000b0009000200c000c000c00000208104001101f004000a02028001400100120013000b0009000200c000c000c00000208105001401f005000a02028001400100150016000b0009000200c000c000c00000208106001701f006000a02028001400100180019000b0009000200c000c000c00000208107001a018007000a020280014001001b001c000b0009000200c000c000c000000d8001001d028202800600000000000d80020020028203800600000000000d80030023028204800600000000000d80040026028205800600000000000d8005002902f00180060000000000158006002c068101810181018101810181010000000000138007002f05f001820282038204820500000000000d8008003202f006f00700000000000d8009003502feff800800000000000d800a003802f005feff00000000003a8201003b068107810281038104810581060022080280014001200110010801040102010101003c003d003e003f0040004100420043000301000000188202004401f000000a02028001400100450046000301000000188203004701f000000a02028001400100480049000301000000188204004a01f000000a020280014001004b004c000301000000188205004d01f000000a020280014001004e004f00030100000000";

// Phase 88 Text Database List entries 0x1801 (partial, containing positions 0 through 11)
const std::string kPhase88TextDbHex =
    "0f3586080010000103000b00070007800000000000000050"
    "002f9308002b0001030007800a000300ffff001d000a00194c696e655f312f32206c656674205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4c696e655f312f32207269676874205048415345383820465700"
    "0029930800250001030007800a000300ffff0017000a00134d69786572204f7574707574204c6576656c00"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2031205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2032205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2033205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2034205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2035205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2036205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2037205048415345383820465700"
    "00309308002c0001030007800a000300ffff001e000a001a4d756c74696368616e6e656c2038205048415345383820465700"
    "00339308002f0001030007800a000300ffff0021000a001d4d6978657220496e707574204c696e65496e20312f32204c6576656c00";

} // namespace

// =============================================================================
// Phase 88 Graph Construction Tests
// =============================================================================

TEST(AvcGraphBuilderTests, Phase88DeviceGraphConstruction) {
    auto musicOpt = Descriptors::MusicSubunitDescriptorParser::ParseStatusDescriptor(
        HexToBytes(kPhase88MusicStatusHex));
    ASSERT_TRUE(musicOpt.has_value());

    auto audioIdOpt = Descriptors::AudioSubunitDescriptorParser::ParseIdentifierDescriptor(
        HexToBytes(kPhase88AudioIdHex));
    ASSERT_TRUE(audioIdOpt.has_value());

    auto textDb = Descriptors::AudioSubunitDescriptorParser::ParseTextDatabaseList(
        HexToBytes(kPhase88TextDbHex));
    // Provide additional selector text entries from full Phase 88 0x1801 text DB
    textDb[0x32] = "external Clocksource Selector";
    textDb[0x35] = "Clock Selector";
    Descriptors::AudioSubunitDescriptorParser::ResolveNames(*audioIdOpt, textDb);

    AvcGraphBuilder::Options options{
        .modelName = "TerraTec PHASE 88 Rack FW",
        .playbackDataBlockSize = 11,
        .captureDataBlockSize = 11,
    };

    DeviceGraph graph = AvcGraphBuilder::BuildGraph(*musicOpt, &*audioIdOpt, options);
    EXPECT_EQ(graph.modelName, "TerraTec PHASE 88 Rack FW");

    // -------------------------------------------------------------------------
    // Capture Stream Validation (Subunit Source Plug 0)
    // -------------------------------------------------------------------------
    const auto& cap = graph.capture;
    EXPECT_EQ(cap.subunitPlugId, 0);
    EXPECT_FALSE(cap.isDestination);
    EXPECT_EQ(cap.channelCount, 10u);
    EXPECT_EQ(cap.midiStreamCount, 2u); // MidiSection.0 with 2 signals
    EXPECT_FALSE(cap.usingFallbackMap);

    // AM824 Slot Map verification: [1, 6, 2, 7, 3, 8, 4, 9, 0, 5]
    // Exactly matches BridgeCo C0 and PR #160 (tested by ear)
    const std::vector<uint8_t> expectedCaptureSlots = {1, 6, 2, 7, 3, 8, 4, 9, 0, 5};
    ASSERT_EQ(cap.channels.size(), 10u);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(cap.channels[i].slotIndex, expectedCaptureSlots[i])
            << "Mismatch at capture channel " << i;
        EXPECT_EQ(cap.slotMap.SlotFor(static_cast<uint32_t>(i)), expectedCaptureSlots[i])
            << "SlotMap mismatch at channel " << i;
    }

    // Capture Channel Names (resolved from per-plug names in 0x8101 -> 0x8102 -> 0x8103)
    EXPECT_EQ(cap.channelNames[0], "Line_1/2 left PHASE88 FW");
    EXPECT_EQ(cap.channelNames[1], "Line_1/2 right PHASE88 FW");
    EXPECT_EQ(cap.channelNames[2], "Line_3/4 left PHASE88 FW");
    EXPECT_EQ(cap.channelNames[3], "Line_3/4 right PHASE88 FW");
    EXPECT_EQ(cap.channelNames[8], "SPDIF left PHASE88 FW");
    EXPECT_EQ(cap.channelNames[9], "SPDIF right PHASE88 FW");

    // -------------------------------------------------------------------------
    // Playback Stream Validation (Subunit Destination Plug 0)
    // -------------------------------------------------------------------------
    const auto& play = graph.playback;
    EXPECT_EQ(play.subunitPlugId, 0);
    EXPECT_TRUE(play.isDestination);
    EXPECT_EQ(play.channelCount, 10u);
    EXPECT_EQ(play.midiStreamCount, 2u); // MidiSection.0 with 2 signals
    EXPECT_FALSE(play.usingFallbackMap);

    // Playback slots: Multichannel Out (1, 6, 2, 7, 3, 8, 4, 9) + SPDIF Out (0, 5)
    const std::vector<uint8_t> expectedPlaySlots = {1, 6, 2, 7, 3, 8, 4, 9, 0, 5};
    ASSERT_EQ(play.channels.size(), 10u);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(play.channels[i].slotIndex, expectedPlaySlots[i])
            << "Mismatch at playback channel " << i;
        EXPECT_EQ(play.slotMap.SlotFor(static_cast<uint32_t>(i)), expectedPlaySlots[i])
            << "SlotMap mismatch at playback channel " << i;
    }

    // -------------------------------------------------------------------------
    // Function Blocks and Master Volume
    // -------------------------------------------------------------------------
    ASSERT_GE(graph.controls.size(), 20u);

    // Feature 1: Mixer Output Level (Master volume)
    const auto itFb1 = std::find_if(graph.controls.begin(), graph.controls.end(),
        [](const ControlBlockInfo& c) { return c.id == 1 && c.type == Descriptors::AudioFunctionBlockType::kFeature; });
    ASSERT_NE(itFb1, graph.controls.end());
    EXPECT_EQ(itFb1->name, "Mixer Output Level");
    EXPECT_TRUE(itFb1->isMasterVolume);
    EXPECT_EQ(itFb1->channelCount, 8);

    // Feature 2: Mixer Input LineIn 1/2 Level
    const auto itFb2 = std::find_if(graph.controls.begin(), graph.controls.end(),
        [](const ControlBlockInfo& c) { return c.id == 2 && c.type == Descriptors::AudioFunctionBlockType::kFeature; });
    ASSERT_NE(itFb2, graph.controls.end());
    EXPECT_EQ(itFb2->name, "Mixer Input LineIn 1/2 Level");
    EXPECT_FALSE(itFb2->isMasterVolume);

    // -------------------------------------------------------------------------
    // Clock Sources Validation
    // -------------------------------------------------------------------------
    // Plugs 8 and 9 are sync destination plugs (format 0x40)
    bool hasSyncDest8 = false;
    bool hasSyncDest9 = false;
    bool hasClockSelector = false;
    bool hasExtClockSelector = false;

    for (const auto& clk : graph.clockSources) {
        if (clk.subunitPlugId == 8) hasSyncDest8 = true;
        if (clk.subunitPlugId == 9) hasSyncDest9 = true;
        if (clk.name == "Clock Selector") hasClockSelector = true;
        if (clk.name == "external Clocksource Selector") hasExtClockSelector = true;
    }

    EXPECT_TRUE(hasSyncDest8);
    EXPECT_TRUE(hasSyncDest9);
    EXPECT_TRUE(hasClockSelector);
    EXPECT_TRUE(hasExtClockSelector);
}

// =============================================================================
// Apogee Duet Graph Construction Tests
// =============================================================================

TEST(AvcGraphBuilderTests, DuetDeviceGraphConstruction) {
    auto musicOpt = Descriptors::MusicSubunitDescriptorParser::ParseStatusDescriptor(
        HexToBytes(kDuetMusicStatusHex));
    ASSERT_TRUE(musicOpt.has_value());

    auto audioIdOpt = Descriptors::AudioSubunitDescriptorParser::ParseIdentifierDescriptor(
        HexToBytes(kDuetAudioIdHex));
    ASSERT_TRUE(audioIdOpt.has_value());

    AvcGraphBuilder::Options options{
        .modelName = "Apogee Duet",
        .playbackDataBlockSize = 2,
        .captureDataBlockSize = 2,
    };

    DeviceGraph graph = AvcGraphBuilder::BuildGraph(*musicOpt, &*audioIdOpt, options);
    EXPECT_EQ(graph.modelName, "Apogee Duet");

    // Playback: 2 channels ("Analog Out 1", "Analog Out 2")
    const auto& play = graph.playback;
    EXPECT_EQ(play.channelCount, 2u);
    EXPECT_EQ(play.midiStreamCount, 0u);
    EXPECT_FALSE(play.usingFallbackMap);
    EXPECT_TRUE(play.slotMap.IsIdentity()); // [0, 1] is identity
    ASSERT_EQ(play.channelNames.size(), 2u);
    EXPECT_EQ(play.channelNames[0], "Analog Out 1");
    EXPECT_EQ(play.channelNames[1], "Analog Out 2");

    // Capture: 2 channels ("Analog In 1", "Analog In 2")
    const auto& cap = graph.capture;
    EXPECT_EQ(cap.channelCount, 2u);
    EXPECT_EQ(cap.midiStreamCount, 0u);
    EXPECT_FALSE(cap.usingFallbackMap);
    EXPECT_TRUE(cap.slotMap.IsIdentity()); // [0, 1] is identity
    ASSERT_EQ(cap.channelNames.size(), 2u);
    EXPECT_EQ(cap.channelNames[0], "Analog In 1");
    EXPECT_EQ(cap.channelNames[1], "Analog In 2");

    // Clock sources
    ASSERT_FALSE(graph.clockSources.empty());
    EXPECT_EQ(graph.clockSources[0].subunitPlugId, 2);
    EXPECT_EQ(graph.clockSources[0].name, "Sync");

    // Controls: FB 1
    ASSERT_EQ(graph.controls.size(), 1u);
    const auto& fb = graph.controls[0];
    EXPECT_EQ(fb.id, 1);
    EXPECT_EQ(fb.type, Descriptors::AudioFunctionBlockType::kFeature);
    EXPECT_EQ(fb.channelCount, 2);
    EXPECT_EQ(fb.masterControls, Descriptors::FeatureControlMask::kMute | Descriptors::FeatureControlMask::kVolume);
}

// =============================================================================
// Rejection and Fallback Tests
// =============================================================================

TEST(AvcGraphBuilderTests, DescriptorMapRejectionWhenSlotExceedsDataBlockSize) {
    auto musicOpt = Descriptors::MusicSubunitDescriptorParser::ParseStatusDescriptor(
        HexToBytes(kPhase88MusicStatusHex));
    ASSERT_TRUE(musicOpt.has_value());

    // Phase 88 slots go up to 9. If dataBlockSize is 8, the map MUST be rejected.
    AvcGraphBuilder::Options options{
        .playbackDataBlockSize = 8,
        .captureDataBlockSize = 8,
    };

    DeviceGraph graph = AvcGraphBuilder::BuildGraph(*musicOpt, nullptr, options);

    EXPECT_TRUE(graph.playback.usingFallbackMap);
    EXPECT_TRUE(graph.playback.slotMap.IsIdentity());

    EXPECT_TRUE(graph.capture.usingFallbackMap);
    EXPECT_TRUE(graph.capture.slotMap.IsIdentity());
}

} // namespace ASFW::Protocols::AVC::Graph::Test
