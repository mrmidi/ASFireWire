// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "Protocols/AVC/Probe/AVCPlug0StreamDiscovery.hpp"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace {

using ASFW::Protocols::AVC::Probe::ParseStreamFormation;
using ASFW::Protocols::AVC::Probe::ParseExtendedStreamFormatListResponse;
using ASFW::Protocols::AVC::Probe::ParseExtendedStreamFormatSingleResponse;
using ASFW::Protocols::AVC::Probe::ParseChannelPositionSections;
using ASFW::Protocols::AVC::Probe::BuildReadOnlyProbeCommand;
using ASFW::Protocols::AVC::Probe::DeviceModel;
using ASFW::Protocols::AVC::Probe::PlugDirection;
using ASFW::Protocols::AVC::Probe::RateCodeToHz;
using ASFW::Protocols::AVC::Probe::ReadOnlyProbeCommand;
using ASFW::Protocols::AVC::Probe::StartAVCPlug0Discovery;
using ASFW::Protocols::AVC::Probe::StreamFormation;
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

TEST(AVCPlug0StreamDiscoveryTests, DecodesExtendedStreamFormatRatesNotMusicSubunitOrdinals) {
    // Linux BeBoB bridgeco_freq_table + snd_bebob_rate_table and Linux OXFW's
    // avc_stream_rate_table define this exact mapping. In particular, 0x03 is
    // 44.1 kHz and 0x04 is 48 kHz; treating them as ordinal/FDF codes caused
    // both existing-family probes to reject otherwise valid devices.
    constexpr std::array expected{
        std::pair{uint8_t{0x00}, uint32_t{22050}},
        std::pair{uint8_t{0x01}, uint32_t{24000}},
        std::pair{uint8_t{0x02}, uint32_t{32000}},
        std::pair{uint8_t{0x03}, uint32_t{44100}},
        std::pair{uint8_t{0x04}, uint32_t{48000}},
        std::pair{uint8_t{0x0A}, uint32_t{88200}},
        std::pair{uint8_t{0x05}, uint32_t{96000}},
        std::pair{uint8_t{0x06}, uint32_t{176400}},
        std::pair{uint8_t{0x07}, uint32_t{192000}},
    };
    for (const auto& [code, hz] : expected) {
        EXPECT_EQ(RateCodeToHz(code), hz) << "rate code 0x" << std::hex
                                           << static_cast<unsigned>(code);
    }
    EXPECT_EQ(RateCodeToHz(0x08), 0U);
    EXPECT_EQ(RateCodeToHz(0x0F), 0U);
}

TEST(AVCPlug0StreamDiscoveryTests, DerivesDuplexRatesForOxfwDuetAndBeBoBFromOneTable) {
    DeviceModel duet{};
    duet.input.supportedFormations = {
        StreamFormation{0x03, 2, 0}, StreamFormation{0x04, 2, 0},
        StreamFormation{0x0A, 2, 0}, StreamFormation{0x05, 2, 0}};
    duet.output.supportedFormations = duet.input.supportedFormations;
    EXPECT_EQ(duet.CommonDuplexRatesHz(),
              (std::vector<uint32_t>{44100, 48000, 88200, 96000}));

    DeviceModel bebob{};
    bebob.input.supportedFormations = {
        StreamFormation{0x02, 10, 1}, StreamFormation{0x03, 10, 1},
        StreamFormation{0x04, 10, 1}, StreamFormation{0x0A, 8, 1},
        StreamFormation{0x05, 8, 1}, StreamFormation{0x06, 4, 1},
        StreamFormation{0x07, 4, 1}};
    bebob.output.supportedFormations = bebob.input.supportedFormations;
    EXPECT_EQ(bebob.CommonDuplexRatesHz(),
              (std::vector<uint32_t>{32000, 44100, 48000, 88200, 96000,
                                     176400, 192000}));

    // A rate advertised in only one direction is not a usable duplex rate.
    bebob.output.supportedFormations.pop_back();
    EXPECT_EQ(bebob.CommonDuplexRatesHz(),
              (std::vector<uint32_t>{32000, 44100, 48000, 88200, 96000,
                                     176400}));
}

TEST(AVCPlug0StreamDiscoveryTests, BuildsUnitPlugInfoBeforeExtendedPlugInfo) {
    const auto cdb = BuildReadOnlyProbeCommand(ReadOnlyProbeCommand::kUnitPlugCounts);
    EXPECT_EQ(cdb.ctype, 0x01);
    EXPECT_EQ(cdb.subunit, 0xff);
    EXPECT_EQ(cdb.opcode, 0x02);
    EXPECT_EQ(cdb.operandLength, 5U);
    EXPECT_EQ(cdb.operands[0], 0x00);
    EXPECT_EQ(cdb.operands[1], 0x00);
    EXPECT_EQ(cdb.operands[4], 0x00);
}

TEST(AVCPlug0StreamDiscoveryTests, BuildsExtendedFormatListWithSupportStatusBeforeIndex) {
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

TEST(AVCPlug0StreamDiscoveryTests, FollowsPlugInfoThenExtendedFormatListChoreography) {
    // The command/response offsets follow the ALSA BeBoB BridgeCo codec:
    // bridgeco.rs:1003-1043 (extended plug info), 1600-1626 (format common
    // fields), and 1743-1764 (list index and formation). No reference code is
    // copied; this is an independent FCP mock fixture.
    ScriptedBeBoBSubmitter submitter({
        // Generic unit PLUG_INFO returns isoc-in/out, ext-in/out.
        {MakeCdb(0x01, 0x02, {0x00, 0x00, 0x00, 0x00, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0x00, 0x01, 0x01, 0x00, 0x00})},
        // ISO input plug type.
        {MakeCdb(0x01, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        // Input format-list entry 0: 48 kHz, 10 PCM slots.
        {MakeCdb(0x01, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        // ISO output plug type.
        {MakeCdb(0x01, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00})},
        // Output format-list entry 0: same 48 kHz / 10 PCM formation.
        {MakeCdb(0x01, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00}),
         AVCResult::kImplementedStable,
         MakeCdb(0x0c, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
                               0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d})},
        // Linux treats the first invalid next list entry as end-of-list.
        {MakeCdb(0x01, 0x2f, {0xc1, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         AVCResult::kNotImplemented, {}},
        {MakeCdb(0x01, 0x2f, {0xc1, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0x01}),
         AVCResult::kNotImplemented, {}},
    });

    std::optional<ASFW::Protocols::AVC::Probe::DeviceModel> model;
    StartAVCPlug0Discovery(submitter, 0x000aac0300b1d1f7ULL,
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

TEST(AVCPlug0StreamDiscoveryTests, ParsesPcmAndMidiSlotsWithoutGuessingPorts) {
    // AM824 compound, 48k rate code, 10 PCM slots and one MIDI slot.
    const uint8_t payload[]{0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06, 0x01, 0x0d};
    const auto formation = ParseStreamFormation(payload);
    ASSERT_TRUE(formation.has_value());
    EXPECT_EQ(formation->rateCode, 0x04);
    EXPECT_EQ(formation->pcmChannels, 10);
    EXPECT_EQ(formation->midiSlots, 1);
}

TEST(AVCPlug0StreamDiscoveryTests, RejectsTruncatedAndUnsupportedFormations) {
    const uint8_t truncated[]{0x90, 0x40, 0x04, 0x00, 0x02, 0x0a, 0x06};
    const uint8_t unsupported[]{0x90, 0x40, 0x04, 0x00, 0x01, 0x02, 0x40};
    EXPECT_FALSE(ParseStreamFormation(truncated).has_value());
    EXPECT_FALSE(ParseStreamFormation(unsupported).has_value());
}

TEST(AVCPlug0StreamDiscoveryTests, ParsesFormatListAtTheExtendedResponseOffset) {
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

TEST(AVCPlug0StreamDiscoveryTests, ParsesCurrentFormationAndRejectsUnknownSupportState) {
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

TEST(AVCPlug0StreamDiscoveryTests, ParsesOneBasedSectionPositionsWithoutGuessingMidi) {
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
