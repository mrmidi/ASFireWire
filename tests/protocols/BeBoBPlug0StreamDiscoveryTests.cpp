// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

#include "Audio/Protocols/BeBoB/BeBoBCaptureChannelMap.hpp"
#include "Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Discovery/DiscoveryTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"

namespace {

using ASFW::Audio::BeBoB::ParseStreamFormation;
using ASFW::Audio::BeBoB::ParseExtendedStreamFormatListResponse;
using ASFW::Audio::BeBoB::ParseExtendedStreamFormatSingleResponse;
using ASFW::Audio::BeBoB::ParseChannelPositionSections;
using ASFW::Audio::BeBoB::PlugDirection;
using ASFW::Audio::BeBoB::StartBeBoBPlug0Discovery;
using namespace ASFW::AVC;

class ScriptedBeBoBUnit final : public IAvcUnit {
public:
    struct Step {
        std::vector<uint8_t> expectedFrame;
        std::vector<uint8_t> responseFrame;
    };

    explicit ScriptedBeBoBUnit(std::vector<Step> steps) : steps_(std::move(steps)) {}

    void Submit(const CommandFrame& frame,
                ASFW::FW::Generation,
                ResponseCallback completion) override {
        ASSERT_LT(next_, steps_.size()) << "unexpected BeBoB AV/C command";
        const auto& step = steps_[next_++];
        std::vector<uint8_t> actual(frame.WireBytes().begin(), frame.WireBytes().end());
        EXPECT_EQ(actual, step.expectedFrame);
        if (step.responseFrame.empty()) {
            completion(std::unexpected(AvcError::Of(AvcErrorKind::kTimeout)));
        } else {
            auto resp = ParseResponse(step.responseFrame);
            if (resp) {
                completion(*resp);
            } else {
                completion(std::unexpected(resp.error()));
            }
        }
    }

    [[nodiscard]] ASFW::FW::NodeId NodeId() const noexcept override { return ASFW::FW::NodeId{0}; }
    [[nodiscard]] ASFW::FW::Generation CurrentGeneration() const noexcept override { return ASFW::FW::Generation{1}; }
    [[nodiscard]] uint64_t Guid() const noexcept override { return 0x000aac0300b1d1f7ULL; }

    [[nodiscard]] bool Finished() const noexcept { return next_ == steps_.size(); }

private:
    std::vector<Step> steps_{};
    size_t next_{0};
};

// AV/C frames as the scripted unit sees them: our encoder pads a command to
// a quadlet with zeros; the scripted reply carries only its operands.
std::vector<uint8_t> Command(uint8_t opcode, std::initializer_list<uint8_t> operands) {
    std::vector<uint8_t> frame{0x01, 0xFF, opcode};
    frame.insert(frame.end(), operands.begin(), operands.end());
    while (frame.size() % 4 != 0) frame.push_back(0x00);
    return frame;
}

std::vector<uint8_t> Reply(uint8_t response, uint8_t opcode, std::initializer_list<uint8_t> operands) {
    std::vector<uint8_t> frame{response, 0xFF, opcode};
    frame.insert(frame.end(), operands.begin(), operands.end());
    return frame;
}

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
    Cmd::PlugInfoCommand cmd{
        .operands = {
            .form = Cmd::PlugInfoForm::kUnitIsoExternal,
            .dummyByte = 0x00,
        }
    };
    auto enc = cmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(enc.has_value());
    const std::vector<uint8_t> expected{0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(std::vector<uint8_t>(enc->WireBytes().begin(), enc->WireBytes().end()), expected);
}

TEST(BridgeCoReadOnlyProbeTests, BuildsBridgeCoFormatListWithSupportStatusBeforeIndex) {
    Cmd::StreamFormatCommand cmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::StreamFormatOperands{
            .form = Cmd::StreamFormatSubfunction::kList,
            .opcode = Cmd::StreamFormatOpcode::kStreamFormatSupport,
            .plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kOutput, Cmd::UnitPlugType::kPcr, 0),
            .index = 3,
        }
    };
    auto enc = cmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(enc.has_value());
    const std::vector<uint8_t> expected{0x01, 0xFF, 0x2F, 0xC1, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00};
    EXPECT_EQ(std::vector<uint8_t>(enc->WireBytes().begin(), enc->WireBytes().end()), expected);
}

TEST(BridgeCoReadOnlyProbeTests, FollowsLinuxPlugInfoThenBridgeCoFormatListChoreography) {
    ScriptedBeBoBUnit unit({
        // Generic unit PLUG_INFO returns isoc-in/out, ext-in/out.
        {{0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00},
         {0x0C, 0xFF, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00}},
        // BridgeCo ISO input plug type.
        {{0x01, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00},
         {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00}},
        // Input format-list entry 0: 48 kHz, 10 PCM slots.
        {Command(0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         Reply(0x0c, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        // Input channel positions; unavailable here, so the map stays identity.
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03}),
         Reply(0x08, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03})},
        // Input signal format STATUS query (wildcard). SFC 0x02 = 48 kHz.
        {{0x01, 0xFF, 0x19, 0x00, 0x90, 0xFF, 0xFF, 0xFF},
         {0x0C, 0xFF, 0x19, 0x00, 0x90, 0x02, 0xFF, 0xFF}},
        // BridgeCo ISO output plug type.
        {{0x01, 0xFF, 0x02, 0xC0, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00},
         {0x0C, 0xFF, 0x02, 0xC0, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00}},
        // Output format-list entry 0: same 48 kHz / 10 PCM formation.
        {Command(0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         Reply(0x0c, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        {Command(0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03}),
         Reply(0x08, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03})},
        // Output signal format STATUS query (wildcard). SFC 0x02 = 48 kHz.
        {{0x01, 0xFF, 0x18, 0x00, 0x90, 0xFF, 0xFF, 0xFF},
         {0x0C, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF}},
        // Linux treats the first invalid next list entry as end-of-list.
        {{0x01, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x01, 0x00},
         {0x08, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x01}},
        {{0x01, 0xFF, 0x2F, 0xC1, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x01, 0x00},
         {0x08, 0xFF, 0x2F, 0xC1, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x01}},
    });

    std::optional<ASFW::Audio::BeBoB::DeviceModel> model;
    StartBeBoBPlug0Discovery(unit, 0x000aac0300b1d1f7ULL,
                              [&model](const auto& discovered) { model = discovered; });

    ASSERT_TRUE(unit.Finished());
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
    EXPECT_EQ(model->input.activeRateHz, 48000U);
    EXPECT_EQ(model->output.activeRateHz, 48000U);
    EXPECT_EQ(model->CurrentRateHz(), 48000U);
    EXPECT_EQ(model->SupportedRatesHz(), std::vector<uint32_t>{48000U});
}

TEST(BridgeCoReadOnlyProbeTests, Phase88PlaybackPositionsProducePlanarSlotMap) {
    // Replies captured from a Terratec Phase 88 (2026-09-27, over MCP). The
    // playback (ISO input plug) data block is planar: slot 0 SPDIF L, slots
    // 1-4 Out 1/3/5/7, slot 5 SPDIF R, slots 6-9 Out 2/4/6/8, slot 10 MIDI.
    constexpr uint8_t kSectionInfo = 0x07;
    ScriptedBeBoBUnit submitter({
        {Command(0x02, {0x00, 0x00, 0x00, 0x00, 0x00}),
         Reply(0x0c, 0x02, {0x00, 0x01, 0x01, 0x00, 0x00})},
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        {Command(0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         Reply(0x0c, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        // Three sections: 8 line outs, 2 SPDIF, MIDI (two entries on slot 11).
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03,
                               0x03,
                               0x08, 0x02, 0x01, 0x07, 0x02, 0x03, 0x03, 0x08, 0x04,
                                     0x04, 0x05, 0x09, 0x06, 0x05, 0x07, 0x0a, 0x08,
                               0x02, 0x01, 0x01, 0x06, 0x02,
                               0x02, 0x0b, 0x01, 0x0b, 0x02})},
        {{0x01, 0xFF, 0x19, 0x00, 0x90, 0xFF, 0xFF, 0xFF},
         {0x0C, 0xFF, 0x19, 0x00, 0x90, 0x02, 0xFF, 0xFF}},
        {Command(0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00}),
         Reply(0x0c, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        {Command(0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         Reply(0x0c, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        {Command(0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03}),
         Reply(0x08, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03})},
        {{0x01, 0xFF, 0x18, 0x00, 0x90, 0xFF, 0xFF, 0xFF},
         {0x0C, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF}},
        {Command(0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         Reply(0x08, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01})},
        // Section-info replies echo the section id at operand 7, type at 8.
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x01}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x01, 0x03})},
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x02}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x02, 0x04})},
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x03}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x03, 0x0a})},
        {Command(0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         Reply(0x08, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01})},
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

TEST(BridgeCoReadOnlyProbeTests, DecodesStreamFormatRateCodesAccordingToBridgeCoTable) {
    using ASFW::Audio::BeBoB::StreamFormation;
    // 0x02 is 32 kHz in BridgeCo/compound stream format, NOT 48 kHz (which is CIP SFC)
    EXPECT_EQ((StreamFormation{.rateCode = 0x02}.RateHz()), 32000U);
    EXPECT_EQ((StreamFormation{.rateCode = 0x03}.RateHz()), 44100U);
    EXPECT_EQ((StreamFormation{.rateCode = 0x04}.RateHz()), 48000U);
    EXPECT_EQ((StreamFormation{.rateCode = 0x05}.RateHz()), 96000U);
    EXPECT_EQ((StreamFormation{.rateCode = 0x0A}.RateHz()), 88200U);
    EXPECT_EQ((StreamFormation{.rateCode = 0x06}.RateHz()), 176400U);
    EXPECT_EQ((StreamFormation{.rateCode = 0x07}.RateHz()), 192000U);
    EXPECT_FALSE((StreamFormation{.rateCode = 0xFF}.RateHz().has_value()));
}

TEST(BridgeCoReadOnlyProbeTests, DecodesPlugSignalFormatSfcAccordingToCipTable) {
    using ASFW::AVC::Cmd::PlugSignalFormat;
    using ASFW::AVC::Cmd::SfcOf;
    using ASFW::AVC::ToHz;

    // AM824 with FDF SFC 0x02 is 48 kHz in CIP SFC table
    PlugSignalFormat fmt48{.plugId = 0, .fmt = 0x90, .fdf = {0x02, 0xFF, 0xFF}};
    auto sfc48 = SfcOf(fmt48);
    ASSERT_TRUE(sfc48.has_value());
    EXPECT_EQ(ToHz(*sfc48), 48000U);

    // AM824 with FDF SFC 0x00 is 32 kHz
    PlugSignalFormat fmt32{.plugId = 0, .fmt = 0x90, .fdf = {0x00, 0xFF, 0xFF}};
    auto sfc32 = SfcOf(fmt32);
    ASSERT_TRUE(sfc32.has_value());
    EXPECT_EQ(ToHz(*sfc32), 32000U);

    // Non-AM824 FMT is rejected
    PlugSignalFormat nonAm824{.plugId = 0, .fmt = 0x00, .fdf = {0x02, 0xFF, 0xFF}};
    EXPECT_FALSE(SfcOf(nonAm824).has_value());
}

TEST(BridgeCoReadOnlyProbeTests, ResolvesAgreedRateAndSupportedRates) {
    using ASFW::Audio::BeBoB::DeviceModel;
    using ASFW::Audio::BeBoB::StreamFormation;

    DeviceModel model;
    model.input.activeRateHz = 48000U;
    model.output.activeRateHz = 48000U;
    EXPECT_TRUE(model.HasAgreedCurrentRate());
    EXPECT_EQ(model.CurrentRateHz(), 48000U);

    // Formations with BridgeCo rates: 0x02 (32k), 0x03 (44.1k), 0x04 (48k)
    model.input.supportedFormations.push_back(StreamFormation{.rateCode = 0x02});
    model.input.supportedFormations.push_back(StreamFormation{.rateCode = 0x03});
    model.output.supportedFormations.push_back(StreamFormation{.rateCode = 0x03});
    model.output.supportedFormations.push_back(StreamFormation{.rateCode = 0x04});

    // The generic duplex path may advertise only rates both directions support.
    const std::vector<uint32_t> expectedRates = {44100U};
    EXPECT_EQ(model.SupportedRatesHz(), expectedRates);
}

TEST(BridgeCoReadOnlyProbeTests, SelectsAsymmetricGeometryAtTheAgreedHighRate) {
    using ASFW::Audio::BeBoB::DeviceModel;
    using ASFW::Audio::BeBoB::StreamFormation;

    DeviceModel model;
    model.currentRateHz = 96000U;
    model.input.supportedFormations = {
        StreamFormation{.rateCode = 0x04, .pcmChannels = 2, .midiSlots = 1},
        StreamFormation{.rateCode = 0x05, .pcmChannels = 4, .midiSlots = 1},
    };
    model.output.supportedFormations = {
        StreamFormation{.rateCode = 0x04, .pcmChannels = 6, .midiSlots = 2},
        StreamFormation{.rateCode = 0x05, .pcmChannels = 8, .midiSlots = 0},
    };

    EXPECT_EQ(model.SupportedRatesHz(), (std::vector<uint32_t>{48000U, 96000U}));
    EXPECT_EQ(model.SelectDuplexRateHz(), 96000U);
    const auto playback = model.InputFormationAtRate(96000U);
    const auto capture = model.OutputFormationAtRate(96000U);
    ASSERT_TRUE(playback.has_value());
    ASSERT_TRUE(capture.has_value());
    EXPECT_EQ(playback->pcmChannels, 4U);
    EXPECT_EQ(playback->midiSlots, 1U);
    EXPECT_EQ(capture->pcmChannels, 8U);
    EXPECT_EQ(capture->midiSlots, 0U);
}

TEST(BridgeCoReadOnlyProbeTests, DoesNotInventGeometryForUnsupportedCurrentRate) {
    using ASFW::Audio::BeBoB::DeviceModel;
    using ASFW::Audio::BeBoB::StreamFormation;

    DeviceModel model;
    model.currentRateHz = 176400U;
    model.input.supportedFormations.push_back(
        StreamFormation{.rateCode = 0x05, .pcmChannels = 4, .midiSlots = 1});
    model.output.supportedFormations.push_back(
        StreamFormation{.rateCode = 0x05, .pcmChannels = 8, .midiSlots = 0});

    EXPECT_EQ(model.SupportedRatesHz(), (std::vector<uint32_t>{96000U}));
    EXPECT_FALSE(model.SelectDuplexRateHz().has_value());
}

TEST(BridgeCoReadOnlyProbeTests, ConflictingDirectionalCurrentRatesAreUnavailable) {
    using ASFW::Audio::BeBoB::DeviceModel;
    using ASFW::Audio::BeBoB::StreamFormation;

    DeviceModel model;
    model.currentRateHz = 48000U;
    model.input.activeRateHz = 48000U;
    model.output.activeRateHz = 96000U;
    model.input.supportedFormations = {
        StreamFormation{.rateCode = 0x04, .pcmChannels = 2},
        StreamFormation{.rateCode = 0x05, .pcmChannels = 4},
    };
    model.output.supportedFormations = model.input.supportedFormations;

    EXPECT_FALSE(model.CurrentRateHz().has_value());
    EXPECT_FALSE(model.SelectDuplexRateHz().has_value());
}

} // namespace
