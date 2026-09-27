// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "Audio/Protocols/BeBoB/BeBoBCaptureChannelMap.hpp"
#include "Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Discovery/DiscoveryTypes.hpp"

namespace {

using ASFW::Audio::BeBoB::ParseStreamFormation;
using ASFW::Audio::BeBoB::ParseExtendedStreamFormatListResponse;
using ASFW::Audio::BeBoB::ParseExtendedStreamFormatSingleResponse;
using ASFW::Audio::BeBoB::ParseChannelPositionSections;
using ASFW::Audio::BeBoB::BuildReadOnlyProbeCommand;
using ASFW::Audio::BeBoB::PlugDirection;
using ASFW::Audio::BeBoB::ReadOnlyProbeCommand;
using ASFW::Audio::BeBoB::StartBeBoBPlug0Discovery;
using ASFW::Protocols::AVC::AVCCdb;
using ASFW::Protocols::AVC::AVCCompletion;
using ASFW::Protocols::AVC::AVCResult;
using ASFW::Protocols::AVC::IAVCCommandSubmitter;

AVCCdb MakeCdb(uint8_t ctype, uint8_t opcode, std::initializer_list<uint8_t> operands) {
    AVCCdb cdb{};
    cdb.ctype = ctype;
    cdb.subunit = 0xff;
    cdb.opcode = opcode;
    cdb.operandLength = operands.size();
    std::copy(operands.begin(), operands.end(), cdb.operands.begin());
    return cdb;
}

class ScriptedBeBoBSubmitter final : public IAVCCommandSubmitter {
public:
    struct Step {
        AVCCdb expected{};
        AVCResult result{AVCResult::kImplementedStable};
        AVCCdb response{};
    };

    explicit ScriptedBeBoBSubmitter(std::vector<Step> steps) : steps_(std::move(steps)) {}

    void SubmitCommand(const AVCCdb& cdb, AVCCompletion completion) override {
        ASSERT_LT(next_, steps_.size()) << "unexpected BeBoB FCP command";
        const auto& step = steps_[next_++];
        EXPECT_EQ(cdb.ctype, step.expected.ctype);
        EXPECT_EQ(cdb.subunit, step.expected.subunit);
        EXPECT_EQ(cdb.opcode, step.expected.opcode);
        EXPECT_EQ(cdb.operandLength, step.expected.operandLength);
        EXPECT_EQ(cdb.operands, step.expected.operands);
        completion(step.result, step.response);
    }

    [[nodiscard]] bool Finished() const noexcept { return next_ == steps_.size(); }

private:
    std::vector<Step> steps_{};
    size_t next_{0};
};

TEST(BridgeCoReadOnlyProbeTests, MatchesOnlyExactPhase88Identity) {
    auto isBeBoB = [](uint32_t vendor, uint32_t model) {
        ASFW::Discovery::DeviceIdentityEvidence evidence{};
        evidence.rootVendorId = vendor;
        evidence.rootModelId = model;
        evidence.units.push_back(ASFW::Discovery::UnitIdentityEvidence{
            .unitDirectoryOffset = 0x400,
            .specifierId = 0x00A02D,
            .version = 0x010001,
        });
        const auto res = ASFW::DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(evidence);
        return res.has_value() && res->family == ASFW::DeviceProfiles::Audio::AudioFamilyProviderId::BeBoB;
    };
    EXPECT_TRUE(isBeBoB(0x000aac, 0x000003));
    EXPECT_FALSE(isBeBoB(0x000aac, 0x000004));
    EXPECT_FALSE(isBeBoB(0x000a92, 0x000003));
}

TEST(BridgeCoReadOnlyProbeTests, BuildsLinuxGenericUnitPlugInfoBeforeBridgeCoExtensions) {
    const auto cdb = BuildReadOnlyProbeCommand(ReadOnlyProbeCommand::kUnitPlugCounts);
    EXPECT_EQ(cdb.ctype, 0x01);
    EXPECT_EQ(cdb.subunit, 0xff);
    EXPECT_EQ(cdb.opcode, 0x02);
    EXPECT_EQ(cdb.operandLength, 5U);
    EXPECT_EQ(cdb.operands[0], 0x00);
    EXPECT_EQ(cdb.operands[1], 0x00);
    EXPECT_EQ(cdb.operands[4], 0x00);
}

TEST(BridgeCoReadOnlyProbeTests, BuildsBridgeCoFormatListWithSupportStatusBeforeIndex) {
    const auto cdb = BuildReadOnlyProbeCommand(ReadOnlyProbeCommand::kStreamFormatList,
                                                PlugDirection::kOutput, 3);
    EXPECT_EQ(cdb.ctype, 0x01);
    EXPECT_EQ(cdb.subunit, 0xff);
    EXPECT_EQ(cdb.opcode, 0x2f);
    EXPECT_EQ(cdb.operandLength, 8U);
    EXPECT_EQ(cdb.operands[0], 0xc1);
    EXPECT_EQ(cdb.operands[1], 0x01);
    EXPECT_EQ(cdb.operands[2], 0x00);
    EXPECT_EQ(cdb.operands[3], 0x00);
    EXPECT_EQ(cdb.operands[4], 0x00);
    EXPECT_EQ(cdb.operands[5], 0xff);
    EXPECT_EQ(cdb.operands[6], 0xff);
    EXPECT_EQ(cdb.operands[7], 0x03);
}

TEST(BridgeCoReadOnlyProbeTests, FollowsLinuxPlugInfoThenBridgeCoFormatListChoreography) {
    // The command/response offsets follow the ALSA BeBoB BridgeCo codec:
    // bridgeco.rs:1003-1043 (extended plug info), 1600-1626 (format common
    // fields), and 1743-1764 (list index and formation). No reference code is
    // copied; this is an independent FCP mock fixture.
    ScriptedBeBoBSubmitter submitter({
        // Generic unit PLUG_INFO returns isoc-in/out, ext-in/out.
        {MakeCdb(0x01, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0x00, 0x01, 0x01, 0x00, 0x00})},
        // BridgeCo ISO input plug type.
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        // Input format-list entry 0: 48 kHz, 10 PCM slots.
        {MakeCdb(0x01, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        // Input channel positions; unavailable here, so the map stays identity.
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03}),
         AVCResult::kNotImplemented, {}},
        // BridgeCo ISO output plug type.
        {MakeCdb(0x01, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        // Output format-list entry 0: same 48 kHz / 10 PCM formation.
        {MakeCdb(0x01, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        {MakeCdb(0x01, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03}),
         AVCResult::kNotImplemented, {}},
        // Linux treats the first invalid next list entry as end-of-list.
        {MakeCdb(0x01, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         AVCResult::kNotImplemented, {}},
        {MakeCdb(0x01, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         AVCResult::kNotImplemented, {}},
    });

    std::optional<ASFW::Audio::BeBoB::DeviceModel> model;
    StartBeBoBPlug0Discovery(submitter, 0x000aac0300b1d1f7ULL,
                              [&model](const auto& discovered) { model = discovered; });

    ASSERT_TRUE(submitter.Finished());
    ASSERT_TRUE(model.has_value());
    ASSERT_TRUE(model->unitPlugCounts.has_value());
    EXPECT_EQ(model->unitPlugCounts->isochronousInputs, 1);
    EXPECT_EQ(model->unitPlugCounts->isochronousOutputs, 1);
    ASSERT_EQ(model->input.supportedFormations.size(), 1U);
    ASSERT_EQ(model->output.supportedFormations.size(), 1U);
    EXPECT_EQ(model->input.supportedFormations[0].rateCode, 0x04);
    EXPECT_EQ(model->input.supportedFormations[0].pcmChannels, 10);
    EXPECT_EQ(model->input.supportedFormations[0].midiSlots, 1);
    EXPECT_EQ(model->output.supportedFormations[0].pcmChannels, 10);
    EXPECT_EQ(model->output.supportedFormations[0].midiSlots, 1);
    EXPECT_TRUE(model->SupportsDuplexFormation(10, 1));
    EXPECT_FALSE(model->SupportsDuplexFormation(10, 2));
}

TEST(BridgeCoReadOnlyProbeTests, Phase88PlaybackPositionsProducePlanarSlotMap) {
    // Replies captured from a Terratec Phase 88 (2026-09-27, over MCP). The
    // playback (ISO input plug) data block is planar: slot 0 SPDIF L, slots
    // 1-4 Out 1/3/5/7, slot 5 SPDIF R, slots 6-9 Out 2/4/6/8, slot 10 MIDI.
    constexpr uint8_t kSectionInfo = 0x07;
    ScriptedBeBoBSubmitter submitter({
        {MakeCdb(0x01, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0x00, 0x01, 0x01, 0x00, 0x00})},
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        {MakeCdb(0x01, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        // Three sections: 8 line outs, 2 SPDIF, MIDI (two entries on slot 11).
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03,
                               0x03,
                               0x08, 0x02, 0x01, 0x07, 0x02, 0x03, 0x03, 0x08, 0x04,
                                     0x04, 0x05, 0x09, 0x06, 0x05, 0x07, 0x0a, 0x08,
                               0x02, 0x01, 0x01, 0x06, 0x02,
                               0x02, 0x0b, 0x01, 0x0b, 0x02})},
        {MakeCdb(0x01, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        {MakeCdb(0x01, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        {MakeCdb(0x01, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03}),
         AVCResult::kNotImplemented, {}},
        {MakeCdb(0x01, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         AVCResult::kNotImplemented, {}},
        // Section-info replies echo the section id at operand 7, type at 8.
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x01}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x01, 0x03})},
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x02}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x02, 0x04})},
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x03}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x03, 0x0a})},
        {MakeCdb(0x01, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         AVCResult::kNotImplemented, {}},
    });

    std::optional<ASFW::Audio::BeBoB::DeviceModel> model;
    StartBeBoBPlug0Discovery(submitter, 0x000aac0300b1d1f7ULL,
                              [&model](const auto& discovered) { model = discovered; });

    ASSERT_TRUE(submitter.Finished());
    ASSERT_TRUE(model.has_value());
    const auto& sections = model->input.channelSections;
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[0].type, std::optional<uint8_t>{0x03});
    EXPECT_EQ(sections[1].type, std::optional<uint8_t>{0x04});
    EXPECT_EQ(sections[2].type, std::optional<uint8_t>{0x0a});

    const auto map = ASFW::Audio::BeBoBProbe::PlaybackChannelMapFromProbe(model->input, 10, 11);
    ASSERT_FALSE(map.IsIdentity());
    // Channels 1-8 are Out 1-8; channels 9-10 are SPDIF L/R.
    constexpr uint8_t kExpectedSlots[]{1, 6, 2, 7, 3, 8, 4, 9, 0, 5};
    for (uint32_t channel = 0; channel < 10; ++channel) {
        EXPECT_EQ(map.SlotFor(channel), kExpectedSlots[channel]) << "channel " << channel;
    }
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
    const uint8_t operands[]{0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x03,
                             0x90, 0x40, 0x04, 0x00, 0x01, 0x0a, 0x06};
    const auto formation = ParseExtendedStreamFormatListResponse(3, operands);
    ASSERT_TRUE(formation.has_value());
    EXPECT_EQ(formation->rateCode, 0x04);
    EXPECT_EQ(formation->pcmChannels, 10);
    EXPECT_EQ(formation->midiSlots, 0);
    EXPECT_FALSE(ParseExtendedStreamFormatListResponse(2, operands).has_value());
}

TEST(BridgeCoReadOnlyProbeTests, ParsesCurrentFormationAndRejectsUnknownSupportState) {
    const uint8_t active[]{0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
                           0x90, 0x40, 0x04, 0x00, 0x01, 0x0a, 0x06};
    const auto current = ParseExtendedStreamFormatSingleResponse(active);
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(current->formation.has_value());
    EXPECT_EQ(current->formation->rateCode, 0x04);
    EXPECT_EQ(current->formation->pcmChannels, 10);
    const uint8_t invalid[]{0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x7f};
    EXPECT_FALSE(ParseExtendedStreamFormatSingleResponse(invalid).has_value());
}

TEST(BridgeCoReadOnlyProbeTests, ParsesOneBasedSectionPositionsWithoutGuessingMidi) {
    const uint8_t payload[]{0x02,
                            0x02, 0x01, 0x01, 0x02, 0x02,
                            0x01, 0x03, 0x01};
    const auto sections = ParseChannelPositionSections(payload);
    ASSERT_TRUE(sections.has_value());
    ASSERT_EQ(sections->size(), 2U);
    ASSERT_EQ((*sections)[0].positions.size(), 2U);
    EXPECT_EQ((*sections)[0].positions[1].streamPosition, 1);
    EXPECT_EQ((*sections)[1].positions[0].streamPosition, 2);
    const uint8_t zeroPosition[]{0x01, 0x01, 0x00, 0x01};
    EXPECT_FALSE(ParseChannelPositionSections(zeroPosition).has_value());
}

} // namespace
