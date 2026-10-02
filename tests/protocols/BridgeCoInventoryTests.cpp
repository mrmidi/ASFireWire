// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

#include "Audio/Protocols/BeBoB/BeBoBCaptureChannelMap.hpp"
#include "Audio/Protocols/BeBoB/BridgeCoInventory.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "Discovery/DiscoveryTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"
#include "tests/support/DeferredAvcUnit.hpp"
#include <memory>

namespace {

using ASFW::Audio::BeBoB::ParseChannelPositionSections;
using ASFW::Audio::BeBoB::ProbeChannelSections;
namespace E = ASFW::AVC::DiscoveryEngine;
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

TEST(BridgeCoReadOnlyProbeTests, Phase88PlaybackPositionsProducePlanarSlotMap) {
    // Replies captured from a Terratec Phase 88 (2026-09-27, over MCP). The
    // playback (ISO input plug) data block is planar: slot 0 SPDIF L, slots
    // 1-4 Out 1/3/5/7, slot 5 SPDIF R, slots 6-9 Out 2/4/6/8, slot 10 MIDI.
    // Only the BridgeCo-specific queries are sent: generic discovery already
    // holds plug counts, formations and signal formats.
    constexpr uint8_t kSectionInfo = 0x07;
    ScriptedBeBoBUnit submitter({
        // Three sections: 8 line outs, 2 SPDIF, MIDI (two entries on slot 11).
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03,
                               0x03,
                               0x08, 0x02, 0x01, 0x07, 0x02, 0x03, 0x03, 0x08, 0x04,
                                     0x04, 0x05, 0x09, 0x06, 0x05, 0x07, 0x0a, 0x08,
                               0x02, 0x01, 0x01, 0x06, 0x02,
                               0x02, 0x0b, 0x01, 0x0b, 0x02})},
        // Section-info replies echo the section id at operand 7, type at 8.
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x01}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x01, 0x03})},
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x02}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x02, 0x04})},
        {Command(0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x03}),
         Reply(0x0c, 0x02, {0xc0, 0x00, 0x00, 0x00, 0x00, 0xff, kSectionInfo, 0x03, 0x0a})},
        {Command(0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03}),
         Reply(0x08, 0x02, {0xc0, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03})},
    });

    std::optional<ASFW::Audio::BeBoB::ChannelSections> sections;
    ProbeChannelSections(submitter, 0x000aac0300b1d1f7ULL,
                         [&sections](ASFW::Audio::BeBoB::ChannelSections found) { sections = std::move(found); });

    ASSERT_TRUE(submitter.Finished());
    ASSERT_TRUE(sections.has_value());
    ASSERT_EQ(sections->playback.size(), 3U);
    EXPECT_EQ(sections->playback[0].type, std::optional<uint8_t>{0x03});
    EXPECT_EQ(sections->playback[1].type, std::optional<uint8_t>{0x04});
    EXPECT_EQ(sections->playback[2].type, std::optional<uint8_t>{0x0a});
    EXPECT_TRUE(sections->capture.empty()) << "the capture positions query was NOT IMPLEMENTED";

    const auto map = ASFW::Audio::BeBoBProbe::ChannelMapFromSections(sections->playback, 10, 11);
    ASSERT_FALSE(map.IsIdentity());
    // Channels 1-8 are Out 1-8; channels 9-10 are SPDIF L/R.
    constexpr uint8_t kExpectedSlots[]{1, 6, 2, 7, 3, 8, 4, 9, 0, 5};
    for (uint32_t channel = 0; channel < 10; ++channel) {
        EXPECT_EQ(map.SlotFor(channel), kExpectedSlots[channel]) << "channel " << channel;
    }
}

namespace FactsFixtures {
Cmd::StreamFormat Compound(StreamFormatRate rate, uint8_t pcm, uint8_t midi) {
    Cmd::StreamFormat format;
    format.kind = Cmd::StreamFormat::Kind::kCompoundAm824;
    format.compound.rate = rate;
    format.compound.entryCount = midi ? 2 : 1;
    format.compound.entries[0] = {pcm, Cmd::Am824Format::kMultiBitLinearAudioRaw};
    format.compound.entries[1] = {midi, Cmd::Am824Format::kMidiConformant};
    return format;
}
E::PlugContents IsoPlug0(Cmd::PlugDirection direction, uint8_t sfc, std::vector<Cmd::StreamFormat> formations) {
    E::PlugContents plug;
    plug.direction = direction;
    plug.signalFormat = Cmd::PlugSignalFormat{.plugId = 0, .fmt = 0x90, .fdf = {sfc, 0xFF, 0xFF}};
    plug.formations = std::move(formations);
    return plug;
}
} // namespace FactsFixtures

TEST(BridgeCoInventoryFacts, FormationsAndAgreedRateComeFromTheSnapshot) {
    using namespace FactsFixtures;
    E::DiscoverySnapshot snapshot;
    snapshot.unit.unitPlugs.isochronousInputs = 2;
    snapshot.unit.unitPlugs.isochronousOutputs = 2;
    snapshot.plugs = {
        IsoPlug0(Cmd::PlugDirection::kInput, 0x02, {Compound(StreamFormatRate::k48000, 10, 1),
                                                    Compound(StreamFormatRate::k96000, 4, 1)}),
        IsoPlug0(Cmd::PlugDirection::kOutput, 0x02, {Compound(StreamFormatRate::k48000, 8, 0)}),
    };
    ASSERT_TRUE(ASFW::Audio::BeBoB::HasDuplexIsoPlugPair(snapshot));
    const auto facts = ASFW::Audio::BeBoB::BridgeCoFormationFacts(snapshot);
    ASSERT_EQ(facts.playback.formations.size(), 2U);
    EXPECT_EQ(facts.playback.formations[0], (E::Formation{48000U, 10U, 1U}));
    EXPECT_EQ(facts.playback.formations[1], (E::Formation{96000U, 4U, 1U}));
    ASSERT_EQ(facts.capture.formations.size(), 1U);
    EXPECT_EQ(facts.capture.formations[0], (E::Formation{48000U, 8U, 0U}));
    EXPECT_EQ(facts.playback.currentRateHz, 48000U); // CIP SFC 0x02 = 48 kHz.
    EXPECT_EQ(facts.capture.currentRateHz, 48000U);
}

TEST(BridgeCoInventoryFacts, ConflictingDirectionalRatesAreUnknown) {
    using namespace FactsFixtures;
    E::DiscoverySnapshot snapshot;
    snapshot.plugs = {
        IsoPlug0(Cmd::PlugDirection::kInput, 0x02, {Compound(StreamFormatRate::k48000, 2, 0)}),
        IsoPlug0(Cmd::PlugDirection::kOutput, 0x04, {Compound(StreamFormatRate::k96000, 2, 0)}),
    };
    const auto facts = ASFW::Audio::BeBoB::BridgeCoFormationFacts(snapshot);
    EXPECT_EQ(facts.playback.currentRateHz, 0U);
    EXPECT_EQ(facts.capture.currentRateHz, 0U);
    EXPECT_FALSE(ASFW::Audio::BeBoB::HasDuplexIsoPlugPair(snapshot)) << "no unit plug counts were discovered";
}

TEST(BridgeCoInventoryFacts, AReplyForADestroyedUnitEndsTheProbeWithoutTouchingIt) {
    // The probe holds the unit by LiveRef, not ownership: a reply that arrives
    // after the unit is gone must neither submit again nor complete.
    auto pending = std::make_shared<ASFW::AVC::Testing::DeferredAvcUnit::Pending>();
    auto unit = std::make_unique<ASFW::AVC::Testing::DeferredAvcUnit>(pending);
    bool completed = false;
    ProbeChannelSections(*unit, 0x000aac0300b1d1f7ULL, [&completed](auto) { completed = true; });
    ASSERT_TRUE(pending->callback.has_value());
    unit.reset();
    auto reply = std::move(*pending->callback);
    pending->callback.reset();
    reply(std::unexpected(AvcError::Of(AvcErrorKind::kTimeout)));
    EXPECT_EQ(pending->submitted, 1U);
    EXPECT_FALSE(completed);
}

} // namespace
