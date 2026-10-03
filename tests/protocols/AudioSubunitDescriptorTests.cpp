// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioSubunitDescriptorTests.cpp - Unit tests for Audio Subunit Identifier
// and Text Database parsing against captured hardware fixtures (Phase 88 & Duet).
//

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Descriptors/AudioControlBits.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/AudioSubunitDescriptor.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/DescriptorCommands.hpp"
#include "tests/support/Phase88DescriptorFixtures.hpp"
#include "tests/support/DuetDescriptorFixture.hpp"

#include <algorithm>
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

TEST(AudioSubunitDescriptorTests, SourceIdSentinelsFollowTheFunctionBlockTypeTable) {
    // TA 1999008 Tables 8.2 and 9.1: F0 = subunit destination plug, F1 = subunit source plug,
    // 80..8F = audio function blocks, FE = not connected.
    using ASFW::Protocols::AVC::Descriptors::AudioSourceId;
    EXPECT_FALSE((AudioSourceId{.type = 0xFE, .id = 0}).IsConnected());
    EXPECT_TRUE((AudioSourceId{.type = 0xF0, .id = 1}).IsConnected());
    EXPECT_TRUE((AudioSourceId{.type = 0xF0, .id = 1}).IsSubunitDestPlug());
    EXPECT_FALSE((AudioSourceId{.type = 0xF1, .id = 1}).IsSubunitDestPlug());
    EXPECT_TRUE((AudioSourceId{.type = 0x81, .id = 3}).IsFunctionBlock());
    EXPECT_TRUE((AudioSourceId{.type = 0x8F, .id = 3}).IsFunctionBlock());
    EXPECT_FALSE((AudioSourceId{.type = 0xF1, .id = 3}).IsFunctionBlock());
    EXPECT_FALSE(AudioSourceId{}.IsConnected());  // a default source id is "not connected"
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

// =============================================================================
// Processing and CODEC dependent information (TA 1999008 §8.4, §8.5)
// =============================================================================

namespace {

namespace P = AudioIdentifierParse;

std::vector<uint8_t> Phase88Bytes() {
    const auto& fixture = ASFW::AVC::Testing::Fixtures::kPhase88AudioIdentifierBytes;
    return {fixture.begin(), fixture.end()};
}

/// Reads `bytes` as the type dependent information of a block of `type`.
Parsed<AudioFunctionBlockInfo> ReadTypeInfo(AudioFunctionBlockType type, std::vector<uint8_t> bytes) {
    AudioFunctionBlockInfo block;
    block.type = type;
    ParseReader reader(bytes);
    const auto read = type == AudioFunctionBlockType::kCodec ? P::CodecInformation(reader, block)
                                                             : P::ProcessingInformation(reader, block);
    if (!read) return std::unexpected(read.error());
    return block;
}

} // namespace

TEST(AudioDependentInfoTests, Phase88MixersCarryNoProgrammableControls) {
    // The five processing blocks of the captured Phase 88: process_type 01 (mixer), size_of_controls 0000.
    const auto bytes = Phase88Bytes();
    const auto result = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(bytes);
    ASSERT_TRUE(result.has_value());
    size_t mixers = 0;
    for (const auto& block : result->functionBlocks) {
        if (block.type != AudioFunctionBlockType::kProcessing) continue;
        ++mixers;
        ASSERT_TRUE(block.typeInfo.has_value());
        EXPECT_FALSE(block.typeInfoError.has_value());
        EXPECT_EQ(block.typeInfo->subType, 0x01);
        EXPECT_EQ(block.processType, 0x01);
        EXPECT_TRUE(block.typeInfo->controls.empty());
    }
    EXPECT_EQ(mixers, 5u);
}

TEST(AudioDependentInfoTests, ABadTypeInformationDoesNotLoseTheRestOfTheDescriptor) {
    // Claim five bytes of Controls in the second mixer, which carries none. The block records the error and
    // every other block still parses.
    auto bytes = Phase88Bytes();
    const std::vector<uint8_t> second = {0x02, 0x02, 0x80, 0x01, 0x40, 0x01, 0x00, 0x45, 0x00, 0x46, 0x00, 0x03, 0x01, 0x00, 0x00};
    const auto at = std::search(bytes.begin(), bytes.end(), second.begin(), second.end());
    ASSERT_NE(at, bytes.end());
    *(at + second.size() - 1) = 0x05;  // size_of_controls = 5
    const auto result = AudioSubunitDescriptorParser::ParseIdentifierDescriptor(bytes);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->functionBlocks.size(), 22u);
    const auto* broken = result->FindBlock(AudioFunctionBlockType::kProcessing, 2);
    ASSERT_NE(broken, nullptr);
    EXPECT_FALSE(broken->typeInfo.has_value());
    ASSERT_TRUE(broken->typeInfoError.has_value());
    EXPECT_EQ(broken->typeInfoError->kind, ParseErrorKind::Truncated);
    EXPECT_EQ(broken->processType, 0x01);  // what was read before the failure is kept
    EXPECT_TRUE(result->FindBlock(AudioFunctionBlockType::kProcessing, 3)->typeInfo.has_value());
}

TEST(AudioDependentInfoTests, DolbyProLogicListsItsModes) {
    // Table 8.8: process_type 04, size_of_controls 1, Controls (Enable, Mode), number_of_modes, size 2, Modes.
    // The five modes are Table 8.7's.
    const auto block = ReadTypeInfo(AudioFunctionBlockType::kProcessing,
                                    {0x04, 0x00, 0x01, 0xC0, 0x05, 0x02, 0x00, 0x07, 0x08, 0x07, 0x01, 0x03, 0x01, 0x07,
                                     0x09, 0x07});
    ASSERT_TRUE(block.has_value());
    const auto& info = *block->typeInfo;
    EXPECT_EQ(info.subType, 0x04);
    EXPECT_EQ(info.sizeOfModes, 2);
    EXPECT_EQ(info.modes, (std::vector<uint32_t>{0x0007, 0x0807, 0x0103, 0x0107, 0x0907}));
    EXPECT_TRUE(info.ControlsBit(0));
    EXPECT_TRUE(info.ControlsBit(1));
    EXPECT_FALSE(info.ControlsBit(2));
    EXPECT_EQ(DescribeControlBits(block->type, info.subType, info.controls), "ENABLE_CONTROL(bit 0), MODE_CONTROL(bit 1)");
}

TEST(AudioDependentInfoTests, UpDownMixModesCanBeOneByteWide) {
    const auto block = ReadTypeInfo(AudioFunctionBlockType::kProcessing,
                                    {0x03, 0x00, 0x01, 0x80, 0x02, 0x01, 0x03, 0x0F});
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->typeInfo->sizeOfModes, 1);
    EXPECT_EQ(block->typeInfo->modes, (std::vector<uint32_t>{0x03, 0x0F}));
}

TEST(AudioDependentInfoTests, GenericProcessingCarriesAGuid) {
    // Table 8.5: Controls, the 8-byte GUID (RAC_ID + 40 bits), GUID_dependent_information_length (2) and data.
    const auto block = ReadTypeInfo(AudioFunctionBlockType::kProcessing,
                                    {0x02, 0x00, 0x01, 0xC0, 0x00, 0x03, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03,
                                     0xAA, 0xBB, 0xCC});
    ASSERT_TRUE(block.has_value());
    EXPECT_EQ(block->typeInfo->guid, (std::vector<uint8_t>{0x00, 0x03, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x01}));
    EXPECT_EQ(block->typeInfo->guidInformation, (std::vector<uint8_t>{0xAA, 0xBB, 0xCC}));
}

TEST(AudioDependentInfoTests, TypeSpecificControlBitsKeepTheSpecsOrder) {
    const auto names = [](uint8_t subType, uint8_t bits) {
        const auto block = ReadTypeInfo(AudioFunctionBlockType::kProcessing, {subType, 0x00, 0x01, bits});
        EXPECT_TRUE(block.has_value());
        return block ? DescribeControlBits(AudioFunctionBlockType::kProcessing, subType, block->typeInfo->controls) : std::string();
    };
    // Table 8.10: the reverberation bit order (Enable, Type, Level, Time, Delay Feedback, Early Time) is not
    // the selector order (Type 03, Level 04, Time 05, Early Time 06, Delay 07).
    EXPECT_EQ(names(0x06, 0xFC),
              "ENABLE_CONTROL(bit 0), REVERBTYPE_CONTROL(bit 1), REVERBLEVEL_CONTROL(bit 2), REVERBTIME_CONTROL(bit 3), "
              "REVERBDELAY_CONTROL(bit 4), REVERBEARLYTIME_CONTROL(bit 5)");
    // Table 8.12: Chorus Level has a bit but no control_selector (Table A.4): named by the spec, not invented a value.
    EXPECT_EQ(names(0x07, 0xF0),
              "ENABLE_CONTROL(bit 0), CHORUSLEVEL_CONTROL(bit 1), CHORUSRATE_CONTROL(bit 2), CHORUSDEPTH_CONTROL(bit 3)");
    // Table 8.15.
    EXPECT_EQ(names(0x08, 0xFC),
              "ENABLE_CONTROL(bit 0), COMPRESSION_RATIO_CONTROL(bit 1), MAXAMPL_CONTROL(bit 2), THRESHOLD_CONTROL(bit 3), "
              "ATTACKTIME_CONTROL(bit 4), RELEASETIME_CONTROL(bit 5)");
    // Table 8.9.
    EXPECT_EQ(names(0x05, 0xC0), "ENABLE_CONTROL(bit 0), SPACIOUSNESS_CONTROL(bit 1)");
    // A set bit the table does not define is shown as unknown, not dropped.
    EXPECT_EQ(names(0x08, 0x02), "UNKNOWN(control_bit:6)");
}

TEST(AudioDependentInfoTests, CodecBlocksCarryModesAndDecoderSpecificBytes) {
    // Table 8.16 then the AC-3 decoder bytes (Table 8.21): BSID 8 and AC3Features 0x0F (all four modes).
    const auto ac3 = ReadTypeInfo(AudioFunctionBlockType::kCodec,
                                  {0x01, 0x00, 0x01, 0xC0, 0x01, 0x02, 0x00, 0x3F, 0x08, 0x0F});
    ASSERT_TRUE(ac3.has_value());
    EXPECT_EQ(ac3->typeInfo->subType, 0x01);
    EXPECT_EQ(ac3->typeInfo->modes, (std::vector<uint32_t>{0x003F}));
    EXPECT_EQ(ac3->typeInfo->codecSpecific, (std::vector<uint8_t>{0x08, 0x0F}));
    EXPECT_EQ(DescribeControlBits(AudioFunctionBlockType::kCodec, 0x01, ac3->typeInfo->controls),
              "ENABLE_CONTROL(bit 0), MODE_CONTROL(bit 1)");
    // DTS (Table 8.18): two more bits, named by the spec though no control_selector exists for them.
    const auto dts = ReadTypeInfo(AudioFunctionBlockType::kCodec,
                                  {0x03, 0x00, 0x01, 0xF0, 0x01, 0x02, 0x00, 0x3F, 0x05, 0x01});
    ASSERT_TRUE(dts.has_value());
    EXPECT_EQ(DescribeControlBits(AudioFunctionBlockType::kCodec, 0x03, dts->typeInfo->controls),
              "ENABLE_CONTROL(bit 0), MODE_CONTROL(bit 1), LFE_Select(bit 2), Matrixed_Stereo_Select(bit 3)");
}

TEST(AudioDependentInfoTests, MalformedTypeInformationIsAnErrorWithAnOffset) {
    // Controls cut short.
    EXPECT_FALSE(ReadTypeInfo(AudioFunctionBlockType::kProcessing, {0x06, 0x00, 0x03, 0xFC}).has_value());
    // A mode field of width 0, or wider than four bytes, cannot be held.
    const auto zero = ReadTypeInfo(AudioFunctionBlockType::kProcessing, {0x03, 0x00, 0x01, 0x80, 0x01, 0x00});
    ASSERT_FALSE(zero.has_value());
    EXPECT_EQ(zero.error().kind, ParseErrorKind::InvalidValue);
    EXPECT_FALSE(ReadTypeInfo(AudioFunctionBlockType::kProcessing, {0x03, 0x00, 0x01, 0x80, 0x01, 0x05, 1, 2, 3, 4, 5}).has_value());
    // Fewer modes than announced.
    EXPECT_FALSE(ReadTypeInfo(AudioFunctionBlockType::kCodec, {0x01, 0x00, 0x01, 0xC0, 0x02, 0x02, 0x00, 0x3F}).has_value());
    // A generic block whose GUID is cut short.
    EXPECT_FALSE(ReadTypeInfo(AudioFunctionBlockType::kProcessing, {0x02, 0x00, 0x01, 0xC0, 0x00, 0x03}).has_value());
}

} // namespace ASFW::Protocols::AVC::Descriptors::Test
