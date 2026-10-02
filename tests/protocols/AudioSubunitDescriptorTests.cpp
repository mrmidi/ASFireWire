// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioSubunitDescriptorTests.cpp - Unit tests for Audio Subunit Identifier
// and Text Database parsing against captured hardware fixtures (Phase 88 & Duet).
//

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Descriptors/AudioSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/DescriptorCommands.hpp"
#include "tests/support/Phase88DescriptorFixtures.hpp"
#include "tests/support/DuetDescriptorFixture.hpp"

#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Descriptors::Test {


// Compile-time regression checks: the captured device descriptors are parsed by
// the same constexpr parser the driver uses. A parser change that breaks either
// device fails the build, not just a test run.
namespace ConstexprFixtures {
namespace F = ASFW::AVC::Testing::Fixtures;
constexpr bool DuetIdentifierParses() {
    const auto parsed = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(F::kDuetAudioIdentifierBytes);
    if (!parsed || parsed->functionBlocks.size() != 1) return false;
    const auto& block = parsed->functionBlocks[0];
    return block.id == 1 && block.type == AudioFunctionBlockType::kFeature && block.clusterChannels == 2 &&
           (block.masterControls & FeatureControlMask::kVolume) && (block.masterControls & FeatureControlMask::kMute);
}
constexpr size_t Phase88Selectors() {
    const auto parsed = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(F::kPhase88AudioIdentifierBytes);
    if (!parsed) return 0;
    size_t selectors = 0;
    for (const auto& block : parsed->functionBlocks) selectors += block.type == AudioFunctionBlockType::kSelector;
    return selectors;
}
constexpr bool Phase88FeatureBlocksParse() {
    const auto parsed = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(F::kPhase88AudioIdentifierBytes);
    return parsed && parsed->FindBlock(AudioFunctionBlockType::kFeature, 1) &&
           parsed->FindBlock(AudioFunctionBlockType::kFeature, 2);
}
constexpr bool TruncatedPhase88IdentifierIsRejected() {
    std::array<uint8_t, 40> head{};
    for (size_t i = 0; i < head.size(); ++i) head[i] = F::kPhase88AudioIdentifierBytes[i];
    return !AudioSubunitDescriptorParser::ParseIdentifierDescriptor(head).has_value();
}
static_assert(DuetIdentifierParses());
static_assert(Phase88FeatureBlocksParse());
static_assert(Phase88Selectors() == 10);
static_assert(TruncatedPhase88IdentifierIsRejected());
} // namespace ConstexprFixtures

namespace {

// Helper to convert hex string to byte vector
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

} // namespace

// =============================================================================
// Duet Audio Subunit Identifier Tests (56 bytes, fixtures/duet_descriptors.json)
// =============================================================================

TEST(AudioSubunitDescriptorTests, DuetAudioIdentifierParsing) {
    const std::string duetHex =
        "0036000200020000002c002a00010026000000040202c0000181000100188101ffff01f00000040202c00000090802000003000200020000";
    const auto bytes = HexToBytes(duetHex);
    ASSERT_EQ(bytes.size(), 56u);

    auto result = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->generationId, 0);
    EXPECT_EQ(result->sizeOfListId, 2);
    EXPECT_EQ(result->sizeOfObjectPosition, 2);
    EXPECT_TRUE(result->rootListIds.empty()); // Duet has 0 root lists

    // Source plug link
    ASSERT_EQ(result->sourcePlugLinks.size(), 1u);
    EXPECT_EQ(result->sourcePlugLinks[0].type, 0x81); // Feature block
    EXPECT_EQ(result->sourcePlugLinks[0].id, 0x00);

    // Function blocks
    ASSERT_EQ(result->functionBlocks.size(), 1u);
    const auto& fb = result->functionBlocks[0];
    EXPECT_EQ(fb.type, AudioFunctionBlockType::kFeature);
    EXPECT_EQ(fb.id, 1);
    EXPECT_EQ(fb.nameIndex, 0xFFFF); // No text DB on Duet

    ASSERT_EQ(fb.inputSources.size(), 1u);
    EXPECT_EQ(fb.inputSources[0].type, 0xF0); // Subunit destination plug
    EXPECT_EQ(fb.inputSources[0].id, 0);

    EXPECT_EQ(fb.clusterChannels, 2);
    // Master controls: 0x0003 (Mute + Volume)
    EXPECT_EQ(fb.masterControls, FeatureControlMask::kMute | FeatureControlMask::kVolume);
}

// =============================================================================
// Phase 88 Audio Subunit Identifier Tests (658 bytes, fixtures/phase88_descriptors.json)
// =============================================================================

TEST(AudioSubunitDescriptorTests, Phase88AudioIdentifierParsing) {
    // Exact hex from phase88_descriptors.json
    const std::string phase88Hex =
        "02900202000200011800028402820001027e000100080202c000000000010bf002f003f004800af0068009800180028003800480051600448101000201820100220802800140012001100108010401020101010003000400050006000700080009000a00170015000200c000c000c000c000c000c000c000c000c00000208102000b01f002000a020280014001000c000d000b0009000200c000c000c00000208103000e01f003000a020280014001000f0010000b0009000200c000c000c00000208104001101f004000a02028001400100120013000b0009000200c000c000c00000208105001401f005000a02028001400100150016000b0009000200c000c000c00000208106001701f006000a02028001400100180019000b0009000200c000c000c00000208107001a018007000a020280014001001b001c000b0009000200c000c000c000000d8001001d028202800600000000000d80020020028203800600000000000d80030023028204800600000000000d80040026028205800600000000000d8005002902f00180060000000000158006002c068101810181018101810181010000000000138007002f05f001820282038204820500000000000d8008003202f006f00700000000000d8009003502feff800800000000000d800a003802f005feff00000000003a8201003b068107810281038104810581060022080280014001200110010801040102010101003c003d003e003f0040004100420043000301000000188202004401f000000a02028001400100450046000301000000188203004701f000000a02028001400100480049000301000000188204004a01f000000a020280014001004b004c000301000000188205004d01f000000a020280014001004e004f00030100000000";
    const auto bytes = HexToBytes(phase88Hex);
    ASSERT_EQ(bytes.size(), 658u);

    auto result = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(bytes);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->generationId, 2);
    ASSERT_EQ(result->rootListIds.size(), 1u);
    EXPECT_EQ(result->rootListIds[0], 0x1800); // Points to text DB root list

    // 11 subunit source plug links
    ASSERT_EQ(result->sourcePlugLinks.size(), 11u);
    EXPECT_EQ(result->sourcePlugLinks[0].type, 0xF0); // Dest plug 2
    EXPECT_EQ(result->sourcePlugLinks[0].id, 2);

    // 22 Function Blocks
    ASSERT_EQ(result->functionBlocks.size(), 22u);

    // Feature 1: Mixer Output Level (FB1 = Master)
    const auto* fb1 = result->FindBlock(AudioFunctionBlockType::kFeature, 1);
    ASSERT_NE(fb1, nullptr);
    EXPECT_EQ(fb1->nameIndex, 0x0002); // Object position 2 in text DB (0-based)
    ASSERT_EQ(fb1->inputSources.size(), 1u);
    EXPECT_EQ(fb1->inputSources[0].type, 0x82); // Fed by Processing 1 (Main Mixer)
    EXPECT_EQ(fb1->inputSources[0].id, 1);
    EXPECT_EQ(fb1->generalTag, 0);
    EXPECT_EQ(fb1->masterControls, 0xC000);
    ASSERT_EQ(fb1->channelControls.size(), 8);
    for (auto bitmap : fb1->channelControls) EXPECT_EQ(bitmap, 0xC000);
    EXPECT_EQ(fb1->clusterChannels, 8); // 8-channel multichannel master

    // Feature 2..7 (Inputs)
    const auto* fb2 = result->FindBlock(AudioFunctionBlockType::kFeature, 2);
    ASSERT_NE(fb2, nullptr);
    EXPECT_EQ(fb2->nameIndex, 0x000B); // Object position 11 in text DB (0-based)
    ASSERT_EQ(fb2->inputSources.size(), 1u);
    EXPECT_EQ(fb2->inputSources[0].type, 0xF0); // Dest plug 2
    EXPECT_EQ(fb2->inputSources[0].id, 2);

    // Selector 6: Mixer Output Selector FB
    const auto* sel6 = result->FindBlock(AudioFunctionBlockType::kSelector, 6);
    ASSERT_NE(sel6, nullptr);
    EXPECT_EQ(sel6->nameIndex, 0x002C);
    ASSERT_EQ(sel6->inputSources.size(), 6u);
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(sel6->inputSources[i].type, 0x81); // Feeds from Feature 1
        EXPECT_EQ(sel6->inputSources[i].id, 1);
    }

    // Processing 1: Main Mixer
    const auto* proc1 = result->FindBlock(AudioFunctionBlockType::kProcessing, 1);
    ASSERT_NE(proc1, nullptr);
    EXPECT_EQ(proc1->nameIndex, 0x003B);
    ASSERT_EQ(proc1->inputSources.size(), 6u); // 6 inputs into mixer (Waveplay + LineIns)
}

// =============================================================================
// Phase 88 Text Database Tests (fixtures/phase88_descriptors.json)
// =============================================================================

TEST(AudioSubunitDescriptorTests, Phase88TextDatabaseParsingAndResolution) {
    auto idResult = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(ASFW::AVC::Testing::Fixtures::kPhase88AudioIdentifier);
    ASSERT_TRUE(idResult.has_value());

    const auto& textBytes = ASFW::AVC::Testing::Fixtures::kPhase88TextChild;

    auto textDb = AudioSubunitDescriptorParser::ParseTextDatabaseList(textBytes);
    EXPECT_EQ(textDb[2], "Mixer Output Level");
    EXPECT_EQ(textDb[11], "Mixer Input LineIn 1/2 Level");

    // Resolve names in identifier
    AudioSubunitDescriptorParser::ResolveNames(*idResult, textDb);

    const auto* fb1 = idResult->FindBlock(AudioFunctionBlockType::kFeature, 1);
    ASSERT_NE(fb1, nullptr);
    EXPECT_EQ(fb1->name, "Mixer Output Level");

    const auto* fb2 = idResult->FindBlock(AudioFunctionBlockType::kFeature, 2);
    ASSERT_NE(fb2, nullptr);
    EXPECT_EQ(fb2->name, "Mixer Input LineIn 1/2 Level");
}

TEST(AudioSubunitDescriptorTests, RejectsTruncatedAndLengthMismatchedDescriptors) {
    auto identifier = ASFW::AVC::Testing::Fixtures::kPhase88AudioIdentifier;
    identifier.pop_back();
    EXPECT_FALSE(AudioSubunitDescriptorParser::ParseIdentifierDescriptor(identifier).has_value());

    identifier = ASFW::AVC::Testing::Fixtures::kPhase88AudioIdentifier;
    identifier[1] = static_cast<uint8_t>(identifier[1] + 1);
    EXPECT_FALSE(AudioSubunitDescriptorParser::ParseIdentifierDescriptor(identifier).has_value());

    identifier = ASFW::AVC::Testing::Fixtures::kPhase88AudioIdentifier;
    identifier[10] = 0xFF;
    identifier[11] = 0xFF;
    EXPECT_FALSE(AudioSubunitDescriptorParser::ParseIdentifierDescriptor(identifier).has_value());

    auto root = ASFW::AVC::Testing::Fixtures::kPhase88TextRoot;
    root.pop_back();
    EXPECT_FALSE(AudioSubunitDescriptorParser::ParseChildListIds(root).has_value());
    EXPECT_TRUE(AudioSubunitDescriptorParser::ParseTextDatabaseList(root).empty());
}

TEST(AudioSubunitDescriptorTests, ReadReplyRejectsReportedLengthBeyondPayload) {
    const uint8_t operands[] = {
        0x00,       // subunit identifier specifier
        0x10, 0x00, // complete, reserved
        0x00, 0x04, // reports four payload bytes
        0x00, 0x00, // offset
        0xAA, 0xBB, // only two payload bytes arrived
    };
    ASFW::AVC::Cmd::ReadDescriptorOperands commandOperands{};
    const auto decoded = commandOperands.Read(operands);
    EXPECT_FALSE(decoded.has_value());

    const uint8_t wrongOffset[] = {
        0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0xAA,
    };
    ASFW::AVC::Cmd::ReadDescriptorOperands offsetZero{};
    EXPECT_FALSE(offsetZero.Read(wrongOffset).has_value());
}

} // namespace ASFW::Protocols::AVC::Descriptors::Test
