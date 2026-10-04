// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioFunctionBlockTests.cpp - Tests for the Audio subunit FUNCTION BLOCK codec (TA 1999008 §10) and
// CHANGE CONFIGURATION (§11.1).
//
// Frames come from the spec's figures (every field position of every command form). Where a proven codec
// already exists (the typed Selector and Feature operands that discovery sends to real devices), the same
// request is built both ways and the bytes compared: there is one wire format, and the proven path is the
// reference for it. Wire bytes appear as expected values; requests are built with named factories.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/AudioNames.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/AudioFunctionBlock.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/ChangeConfigurationCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::AVC::Test {

namespace {

constexpr uint8_t kStatusReply = 0x0C;
constexpr uint8_t kAcceptedReply = 0x09;
constexpr uint8_t kAudioAddress = 0x08;  // Audio subunit 0

std::vector<uint8_t> Operands(const CommandFrame& frame) {
    const auto ops = frame.Operands();
    return {ops.begin(), ops.end()};
}

template <class Command>
std::vector<uint8_t> Encoded(const Command& command, CommandType type) {
    auto frame = command.Encode(type);
    EXPECT_TRUE(frame.has_value());
    return frame ? Operands(*frame) : std::vector<uint8_t>{};
}

template <class Command>
bool Refused(const Command& command, CommandType type) {
    return !command.Encode(type).has_value();
}

std::vector<uint8_t> ReplyFrame(uint8_t code, Opcode opcode, std::vector<uint8_t> operands) {
    std::vector<uint8_t> frame{code, kAudioAddress, static_cast<uint8_t>(opcode)};
    frame.insert(frame.end(), operands.begin(), operands.end());
    return frame;
}

template <class Command>
Expected<typename Command::Reply> Decode(const Command& command, const std::vector<uint8_t>& frameBytes) {
    auto response = ParseResponse(frameBytes);
    EXPECT_TRUE(response.has_value());
    return command.Decode(response->operands);
}

template <class Command>
typename Command::Reply DecodeOk(const Command& command, const std::vector<uint8_t>& frameBytes) {
    auto reply = Decode(command, frameBytes);
    EXPECT_TRUE(reply.has_value());
    return *reply;
}

using Cmd::ControlValue;
namespace C = Cmd::controls;

} // namespace

// ===========================================================================
// Spec tables and the catalogue
// ===========================================================================

TEST(AudioControlCatalogueTests, ControlSelectorsMatchTableA4) {
    EXPECT_EQ(C::kMuteControl.selector, 0x01);
    EXPECT_EQ(C::kLoudnessControl.selector, 0x0C);
    EXPECT_EQ(C::kMixerControl.selector, 0x03);
    EXPECT_EQ(C::kReverbDelayFeedbackControl.selector, 0x07);
    EXPECT_EQ(C::kAc3Roomtype2StatusControl.selector, 0x19);
    EXPECT_EQ(C::kMpegHighLowScalingControl.selector, 0x08);
    // The same selector number means different controls in different blocks (Table A.4).
    ASSERT_NE(Cmd::FindControl(C::kMixerControl), nullptr);
    EXPECT_EQ(Cmd::FindControl(C::kMixerControl)->name, "MIXER_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kProcessing, 5, 0x03)->name, "SPACIOUSNESS_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kProcessing, 6, 0x03)->name, "REVERBTYPE_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kProcessing, 7, 0x03)->name, "CHORUSRATE_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kProcessing, 8, 0x03)->name, "COMPRESSION_RATIO_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kCodec, 1, 0x03)->name, "AC3_DYNAMIC_RANGE_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kCodec, 2, 0x03)->name, "MPEG_DUAL_CHANNEL_CONTROL");
    // Enable and Mode apply to every process and CODEC type.
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kProcessing, 6, 0x01)->name, "ENABLE_CONTROL");
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kCodec, 3, 0x02)->name, "MODE_CONTROL");
    // A selector Table A.4 does not define, and a DTS decoder has no specific controls (§10.6.4.1).
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kFeature, 0, 0x0D), nullptr);
    EXPECT_EQ(Cmd::FindControl(Cmd::FunctionBlockType::kCodec, 3, 0x03), nullptr);
}

TEST(AudioControlCatalogueTests, EveryEntryIsUniqueAndTheFeatureOnesAgreeWithTheProvenEnum) {
    for (size_t i = 0; i < Cmd::kControlCatalogue.size(); ++i) {
        for (size_t j = i + 1; j < Cmd::kControlCatalogue.size(); ++j) {
            EXPECT_FALSE(Cmd::kControlCatalogue[i].id == Cmd::kControlCatalogue[j].id) << i << " " << j;
        }
    }
    // The Feature selectors are the proven FeatureControl enum, and the widths agree.
    for (const auto& entry : Cmd::kFeatureControlWidths) {
        const auto* spec = Cmd::FindControl(Cmd::FunctionBlockType::kFeature, 0, static_cast<uint8_t>(entry.control));
        ASSERT_NE(spec, nullptr);
        const auto width = Cmd::FixedWidth(spec->kind);
        if (entry.width == 0) {
            EXPECT_FALSE(width.has_value());
        } else {
            ASSERT_TRUE(width.has_value());
            EXPECT_EQ(*width, entry.width);
        }
    }
}

// ===========================================================================
// Feature function block
// ===========================================================================

TEST(AudioFeatureTests, FirstFormMatchesTheProvenFeatureOperands) {
    // Mute CONTROL, Volume CONTROL and STATUS, built by both codecs.
    const auto proven = [](auto operands, CommandType type) {
        return Encoded(Cmd::FeatureCommand{.address = kAudioSubunit0, .operands = operands}, type);
    };
    EXPECT_EQ(Encoded(Cmd::SetFeatureControl(kAudioSubunit0, 3, 1, C::kMuteControl, ControlValue::Boolean(true)),
                      CommandType::kControl),
              proven(Cmd::FeatureOperands::Mute(3, 1, true), CommandType::kControl));
    EXPECT_EQ(Encoded(Cmd::SetFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl,
                                             ControlValue::Volume(AvcVolume::FromRaw(static_cast<int16_t>(0xFD00)))),
                      CommandType::kControl),
              proven(Cmd::FeatureOperands::Volume(1, 0, AvcVolume::FromRaw(static_cast<int16_t>(0xFD00))),
                     CommandType::kControl));
    EXPECT_EQ(Encoded(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 2, C::kMuteControl), CommandType::kStatus),
              proven(Cmd::FeatureOperands::MuteStatus(1, 2), CommandType::kStatus));
    EXPECT_EQ(Encoded(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 2, C::kVolumeControl), CommandType::kStatus),
              proven(Cmd::FeatureOperands::VolumeStatus(1, 2), CommandType::kStatus));
    // The attributes of a volume STATUS (§9.1.4): MINIMUM, MAXIMUM, RESOLUTION.
    EXPECT_EQ(Encoded(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl, Cmd::ControlAttribute::kMaximum),
                      CommandType::kStatus),
              proven(Cmd::FeatureOperands::VolumeStatus(1, 0, Cmd::ControlAttribute::kMaximum), CommandType::kStatus));
}

TEST(AudioFeatureTests, VolumeControlMatchesFigure1010) {
    // Operand[3] 2, [4] channel, [5] VOLUME_CONTROL, [6] length 2, [7..8] Volume.
    EXPECT_EQ(Encoded(Cmd::SetFeatureControl(kAudioSubunit0, 1, 5, C::kVolumeControl,
                                             ControlValue::Volume(AvcVolume::FromRaw(0x0100))),
                      CommandType::kControl),
              (std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x05, 0x02, 0x02, 0x01, 0x00}));
}

TEST(AudioFeatureTests, EveryFeatureControlHasItsSpecWidth) {
    const auto statusOf = [](Cmd::ControlId control) {
        return Encoded(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, control), CommandType::kStatus);
    };
    // Operand[6] is the control data length; the data are FF placeholders (Figures 10.4, 10.8, 10.10, 10.14, 10.16,
    // 10.18, 10.22, 10.26, 10.32, 10.36, 10.40, 10.44).
    EXPECT_EQ(statusOf(C::kMuteControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kVolumeControl).at(6), 2);
    EXPECT_EQ(statusOf(C::kLrBalanceControl).at(6), 2);
    EXPECT_EQ(statusOf(C::kFrBalanceControl).at(6), 2);
    EXPECT_EQ(statusOf(C::kBassControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kMidControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kTrebleControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kAutomaticGainControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kDelayControl).at(6), 2);
    EXPECT_EQ(statusOf(C::kBassBoostControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kLoudnessControl).at(6), 1);
    EXPECT_EQ(statusOf(C::kBassControl), (std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00, 0x05, 0x01, 0xFF}));
}

TEST(AudioFeatureTests, EachValueKindEncodesItsSpecLayout) {
    const auto setOf = [](Cmd::ControlId control, ControlValue value) {
        return Encoded(Cmd::SetFeatureControl(kAudioSubunit0, 1, 0, control, value), CommandType::kControl);
    };
    // Bass +31.50 dB (7E), treble -32.00 dB (80) (Tables 10.8, 10.10).
    EXPECT_EQ(setOf(C::kBassControl, ControlValue::Tone(0x7E)).back(), 0x7E);
    EXPECT_EQ(setOf(C::kTrebleControl, ControlValue::Tone(static_cast<int8_t>(0x80))).back(), 0x80);
    // Delay 1.0000 ms is 0040 (Table 10.16).
    EXPECT_EQ(setOf(C::kDelayControl, ControlValue::Delay(0x0040)),
              (std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00, 0x0A, 0x02, 0x00, 0x40}));
    // AGC, bass boost and loudness are booleans: 70 on, 60 off.
    EXPECT_EQ(setOf(C::kBassBoostControl, ControlValue::Boolean(false)).back(), 0x60);
    EXPECT_EQ(setOf(C::kLoudnessControl, ControlValue::Boolean(true)).back(), 0x70);
    // LR balance 0x7FFE is 0 dB / -inf dB (Table 10.6).
    EXPECT_EQ(setOf(C::kLrBalanceControl, ControlValue::Balance(0x7FFE)),
              (std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00, 0x03, 0x02, 0x7F, 0xFE}));
}

TEST(AudioFeatureTests, SecondFormAddressesAllChannelsWithOneParameterEach) {
    // Figure 10.12: channel FF, length 2*n, Volume_High/Low per channel.
    const std::array<ControlValue, 3> volumes = {ControlValue::Volume(AvcVolume::FromRaw(0x0100)),
                                                 ControlValue::Volume(AvcVolume::FromRaw(0x0000)),
                                                 ControlValue::Volume(AvcVolume::FromRaw(static_cast<int16_t>(0xFF00)))};
    EXPECT_EQ(Encoded(Cmd::SetFeatureControlAllChannels(kAudioSubunit0, 2, C::kVolumeControl, volumes),
                      CommandType::kControl),
              (std::vector<uint8_t>{0x81, 0x02, 0x10, 0x02, 0xFF, 0x02, 0x06, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x00}));
    // Figure 10.13: STATUS carries the number of available controls x the width.
    EXPECT_EQ(Encoded(Cmd::QueryFeatureControlAllChannels(kAudioSubunit0, 2, C::kVolumeControl, 3), CommandType::kStatus),
              (std::vector<uint8_t>{0x81, 0x02, 0x10, 0x02, 0xFF, 0x02, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    // Figure 10.6: mute is one byte per control.
    const std::array<ControlValue, 2> mutes = {ControlValue::Boolean(true), ControlValue::Boolean(false)};
    EXPECT_EQ(Encoded(Cmd::SetFeatureControlAllChannels(kAudioSubunit0, 1, C::kMuteControl, mutes), CommandType::kControl),
              (std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0xFF, 0x01, 0x02, 0x70, 0x60}));
    // A list of the wrong kind, or one holding an invalid value, is not sent.
    EXPECT_TRUE(Refused(Cmd::SetFeatureControlAllChannels(kAudioSubunit0, 1, C::kVolumeControl, mutes),
                        CommandType::kControl));
    const std::array<ControlValue, 1> oneMute = {ControlValue::Boolean(true)};
    EXPECT_TRUE(Refused(Cmd::SetFeatureControlAllChannels(kAudioSubunit0, 1, C::kMuteControl,
                                                          std::array<ControlValue, 1>{ControlValue::OfByte(Cmd::AudioValueKind::kBoolean, 0xFF)}),
                        CommandType::kControl));
    EXPECT_TRUE(Encoded(Cmd::SetFeatureControlAllChannels(kAudioSubunit0, 1, C::kMuteControl, oneMute),
                        CommandType::kControl).size() > 0);
}

TEST(AudioFeatureTests, MoveAndDeltaCarryAStepCount) {
    // §9.1.4, Table 10.3: DELTA +3 steps on the master volume; control_attribute 19.
    EXPECT_EQ(Encoded(Cmd::StepFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl, Cmd::ControlAttribute::kDelta,
                                              Cmd::Steps::Of(3)),
                      CommandType::kControl),
              (std::vector<uint8_t>{0x81, 0x01, 0x19, 0x02, 0x00, 0x02, 0x02, 0x00, 0x03}));
    EXPECT_EQ(Encoded(Cmd::StepFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl, Cmd::ControlAttribute::kMove,
                                              Cmd::Steps::Of(-1)),
                      CommandType::kControl),
              (std::vector<uint8_t>{0x81, 0x01, 0x18, 0x02, 0x00, 0x02, 0x02, 0xFF, 0xFF}));
    // Only MOVE and DELTA take steps, and only two-byte controls.
    EXPECT_TRUE(Refused(Cmd::StepFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl, Cmd::ControlAttribute::kCurrent,
                                                Cmd::Steps::Of(1)),
                        CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::StepFeatureControl(kAudioSubunit0, 1, 0, C::kBassControl, Cmd::ControlAttribute::kDelta,
                                                Cmd::Steps::Of(1)),
                        CommandType::kControl));
}

TEST(AudioFeatureTests, GraphicEqualizerCarriesTwoBitmapsAndOneGainPerBand) {
    // Figure 10.30: BandsPresent (4), ExtraBandsPresent (4), gains in ascending order; length 8 + n.
    Cmd::GraphicEqualizerBands bands;
    bands.bandsPresent = 0x00000003;  // bands 14 and 15
    bands.gains = {0x02, 0xFE};       // +0.50 dB, -0.50 dB
    EXPECT_TRUE(bands.IsConsistent());
    EXPECT_EQ(Encoded(Cmd::SetGraphicEqualizer(kAudioSubunit0, 1, 0, bands), CommandType::kControl),
              (std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
                                    0x00, 0x02, 0xFE}));
    // A gain count that does not match the bits set is the NOT IMPLEMENTED case (§10.3.8): not sent.
    bands.gains = {0x02};
    EXPECT_FALSE(bands.IsConsistent());
    EXPECT_TRUE(Refused(Cmd::SetGraphicEqualizer(kAudioSubunit0, 1, 0, bands), CommandType::kControl));
    // Reserved bits 30 and 31 of BandsPresent are not valid.
    Cmd::GraphicEqualizerBands reserved;
    reserved.bandsPresent = 0x40000000;
    reserved.gains = {0x00};
    EXPECT_FALSE(reserved.IsConsistent());
    EXPECT_EQ(Cmd::GraphicEqualizerBands::BandNumber(0), 14);   // Table 10.13
    EXPECT_EQ(Cmd::GraphicEqualizerBands::BandNumber(29), 43);
    // Extra bands count too.
    Cmd::GraphicEqualizerBands extra;
    extra.bandsPresent = 0x00000001;
    extra.extraBandsPresent = 0x80000000;
    extra.gains = {0x00, 0x00};
    EXPECT_TRUE(extra.IsConsistent());
}

TEST(AudioFeatureTests, RefusesWhatTheSpecDoesNotAllow) {
    // The wrong subunit, and a command type FUNCTION BLOCK does not have.
    EXPECT_TRUE(Refused(Cmd::QueryFeatureControl(SubunitAddress::Unit(), 1, 0, C::kMuteControl), CommandType::kStatus));
    EXPECT_TRUE(Refused(Cmd::QueryFeatureControl(kMusicSubunit0, 1, 0, C::kMuteControl), CommandType::kStatus));
    EXPECT_TRUE(Refused(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kMuteControl), CommandType::kSpecificInquiry));
    // NOTIFY has the STATUS syntax (§9.1).
    EXPECT_EQ(Encoded(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kMuteControl), CommandType::kNotify),
              Encoded(Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kMuteControl), CommandType::kStatus));
    // A value of the wrong kind for the control, a control of another block, an invalid value in a CONTROL.
    EXPECT_TRUE(Refused(Cmd::SetFeatureControl(kAudioSubunit0, 1, 0, C::kMuteControl,
                                               ControlValue::Volume(AvcVolume::FromRaw(0))),
                        CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::SetFeatureControl(kAudioSubunit0, 1, 0, C::kMixerControl, ControlValue::Volume(AvcVolume::FromRaw(0))),
                        CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::SetFeatureControl(kAudioSubunit0, 1, 0, C::kMuteControl,
                                               ControlValue::OfByte(Cmd::AudioValueKind::kBoolean, 0xFF)),
                        CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::SetFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl, ControlValue::Volume(AvcVolume::Invalid())),
                        CommandType::kControl));
    // The same invalid value is fine in what a STATUS reply carries: it is only refused as a command.
    EXPECT_FALSE(Cmd::ControlValue::Volume(AvcVolume::Invalid()).AsVolume().has_value());
}

TEST(AudioFeatureTests, RepliesDecodeToTypedValues) {
    const auto command = Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl);
    // STATUS reply: Volume -1.0 dB (FF00), then a mute TRUE.
    const auto volume = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                     {0x81, 0x01, 0x10, 0x02, 0x00, 0x02, 0x02, 0xFF, 0x00}));
    EXPECT_EQ(volume.type, Cmd::FunctionBlockType::kFeature);
    EXPECT_EQ(volume.functionBlockId, 1);
    EXPECT_EQ(volume.audioSelectorData, (std::vector<uint8_t>{0x00}));
    EXPECT_EQ(volume.controlSelector, 0x02);
    ASSERT_TRUE(volume.Value(Cmd::AudioValueKind::kVolume).has_value());
    EXPECT_EQ(volume.Value(Cmd::AudioValueKind::kVolume)->AsVolume()->Raw(), static_cast<int16_t>(0xFF00));
    EXPECT_EQ(volume.Control()->name, "VOLUME_CONTROL");

    const auto mute = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                   {0x81, 0x01, 0x10, 0x02, 0x00, 0x01, 0x01, 0x70}));
    EXPECT_EQ(mute.Value(Cmd::AudioValueKind::kBoolean)->AsBoolean(), true);
    // A volume read of mute data is not a volume.
    EXPECT_FALSE(mute.Value(Cmd::AudioValueKind::kVolume).has_value());

    // Second form reply: three volumes.
    const auto all = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                  {0x81, 0x01, 0x10, 0x02, 0xFF, 0x02, 0x06, 0x01, 0x00, 0x00, 0x00, 0xFF,
                                                   0x00}));
    const auto values = all.Values(Cmd::AudioValueKind::kVolume);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[2].AsVolume()->Raw(), static_cast<int16_t>(0xFF00));
    // A DELTA / MOVE reply is a step count.
    const auto step = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                   {0x81, 0x01, 0x19, 0x02, 0x00, 0x02, 0x02, 0xFF, 0xFE}));
    EXPECT_EQ(step.StepCount()->count, -2);
}

TEST(AudioFeatureTests, ShortRepliesAreErrorsAtTheirOffset) {
    const auto command = Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl);
    for (const auto& operands : {std::vector<uint8_t>{0x81, 0x01, 0x10},
                                 std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00},         // selector data cut off
                                 std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00, 0x02},   // no data length
                                 std::vector<uint8_t>{0x81, 0x01, 0x10, 0x02, 0x00, 0x02, 0x02, 0x00}}) {  // data cut off
        auto reply = Decode(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock, operands));
        ASSERT_FALSE(reply.has_value());
        EXPECT_EQ(reply.error().kind, AvcErrorKind::kOperandsTooShort);
    }
    auto zeroLength = Decode(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock, {0x81, 0x01, 0x10, 0x00}));
    ASSERT_FALSE(zeroLength.has_value());
    EXPECT_EQ(zeroLength.error().kind, AvcErrorKind::kMalformedOperands);
}

// ===========================================================================
// Selector block through the generic codec
// ===========================================================================

TEST(AudioSelectorTests, GenericCodecAgreesWithTheProvenSelectorOperands) {
    // Figure 10.2: [80][id][attr][2][input fb-plug][SELECTOR_CONTROL], no control data.
    Cmd::FunctionBlockControlOperands ops;
    ops.type = Cmd::FunctionBlockType::kSelector;
    ops.functionBlockId = 4;
    ops.audioSelectorData = {0x02};
    ops.control = C::kSelectorControl;
    const Cmd::FunctionBlockControlCommand command{.address = kAudioSubunit0, .operands = ops};
    const Cmd::SelectorCommand proven{.address = kAudioSubunit0, .operands = {.functionBlockId = 4, .inputPlug = 2}};
    EXPECT_EQ(Encoded(command, CommandType::kControl), Encoded(proven, CommandType::kControl));
    EXPECT_EQ(Encoded(command, CommandType::kControl), (std::vector<uint8_t>{0x80, 0x04, 0x10, 0x02, 0x02, 0x01}));
    // A reply carries no control data.
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                    {0x80, 0x04, 0x10, 0x02, 0x03, 0x01}));
    EXPECT_EQ(reply.audioSelectorData, (std::vector<uint8_t>{0x03}));
    EXPECT_TRUE(reply.controlData.empty());
}

// ===========================================================================
// Processing function block
// ===========================================================================

TEST(AudioProcessingTests, EnableProcessingMatchesFigure1050) {
    // Figure 10.48: selector length 4: fb-plug, ICN, OCN, control selector; then length 1 and Processing_On.
    EXPECT_EQ(Encoded(Cmd::EnableProcessing(kAudioSubunit0, 2, true), CommandType::kControl),
              (std::vector<uint8_t>{0x82, 0x02, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x01, 0x01, 0x70}));
    EXPECT_EQ(Encoded(Cmd::EnableProcessing(kAudioSubunit0, 2, false), CommandType::kControl).back(), 0x60);
    EXPECT_EQ(Encoded(Cmd::QueryProcessingControl(kAudioSubunit0, 2, Cmd::ProcessingTarget::Block(),
                                                  C::kEnableProcessingControl),
                      CommandType::kStatus),
              (std::vector<uint8_t>{0x82, 0x02, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x01, 0x01, 0xFF}));
}

TEST(AudioProcessingTests, ModeSelectCarriesItsSizeAndModeBytes) {
    // Figure 10.52: length m+1, Size_of_modes (m), Mode[1..m].
    EXPECT_EQ(Encoded(Cmd::SelectProcessingMode(kAudioSubunit0, 3, 2), CommandType::kControl),
              (std::vector<uint8_t>{0x82, 0x03, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x02, 0x02, 0x01, 0x02}));
    // A two-byte mode number is big-endian.
    EXPECT_EQ(Encoded(Cmd::SelectProcessingMode(kAudioSubunit0, 3, 0x0102, 2), CommandType::kControl),
              (std::vector<uint8_t>{0x82, 0x03, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x02, 0x03, 0x02, 0x01, 0x02}));
    // STATUS: every mode byte FF (§10.4.2.2).
    EXPECT_EQ(Encoded(Cmd::QueryProcessingMode(kAudioSubunit0, 3), CommandType::kStatus),
              (std::vector<uint8_t>{0x82, 0x03, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x02, 0x02, 0x01, 0xFF}));
    // There is no CONTROL form of a query, and a mode wider than four bytes is not sent.
    EXPECT_TRUE(Refused(Cmd::QueryProcessingMode(kAudioSubunit0, 3), CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::SelectProcessingMode(kAudioSubunit0, 3, 1, 5), CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::SelectProcessingMode(kAudioSubunit0, 3, 1, 0), CommandType::kStatus));
    const auto reply = DecodeOk(Cmd::QueryProcessingMode(kAudioSubunit0, 3),
                                ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                           {0x82, 0x03, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x02, 0x02, 0x01, 0x03}));
    EXPECT_EQ(reply.ProcessingMode(), 3u);
}

TEST(AudioProcessingTests, MixerFirstFormAddressesOneInputOutputPair) {
    // Figure 10.54 / 10.55: fb-plug, ICN, OCN, MIXER_CONTROL, length 2, Mixer_Setting.
    EXPECT_EQ(Encoded(Cmd::SetMixerControl(kAudioSubunit0, 1, 0, 3, 5, AvcVolume::FromRaw(0x0100)), CommandType::kControl),
              (std::vector<uint8_t>{0x82, 0x01, 0x10, 0x04, 0x00, 0x03, 0x05, 0x03, 0x02, 0x01, 0x00}));
    EXPECT_EQ(Encoded(Cmd::QueryMixerControl(kAudioSubunit0, 1, 2, 1, 1, Cmd::ControlAttribute::kMinimum),
                      CommandType::kStatus),
              (std::vector<uint8_t>{0x82, 0x01, 0x02, 0x04, 0x02, 0x01, 0x01, 0x03, 0x02, 0xFF, 0xFF}));
}

TEST(AudioProcessingTests, MixerSecondAndThirdFormsListManyControls) {
    // Figure 10.58 / 10.59: ICN = OCN = FF, length 2 x the number of programmable controls.
    const std::array<AvcVolume, 2> settings = {AvcVolume::FromRaw(0x0100), AvcVolume::FromRaw(static_cast<int16_t>(0x8000))};
    EXPECT_EQ(Encoded(Cmd::SetMixerProgrammable(kAudioSubunit0, 1, settings), CommandType::kControl),
              (std::vector<uint8_t>{0x82, 0x01, 0x10, 0x04, 0x00, 0xFF, 0xFF, 0x03, 0x04, 0x01, 0x00, 0x80, 0x00}));
    EXPECT_EQ(Encoded(Cmd::QueryMixerProgrammable(kAudioSubunit0, 1, 3), CommandType::kStatus),
              (std::vector<uint8_t>{0x82, 0x01, 0x10, 0x04, 0x00, 0xFF, 0xFF, 0x03, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    // Figure 10.60 / 10.61: ICN = OCN = 00 (third form), every mixer control.
    EXPECT_EQ(Encoded(Cmd::QueryMixerAll(kAudioSubunit0, 1, 2), CommandType::kStatus),
              (std::vector<uint8_t>{0x82, 0x01, 0x10, 0x04, 0x00, 0x00, 0x00, 0x03, 0x04, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(Encoded(Cmd::SetMixerAll(kAudioSubunit0, 1, settings), CommandType::kControl),
              (std::vector<uint8_t>{0x82, 0x01, 0x10, 0x04, 0x00, 0x00, 0x00, 0x03, 0x04, 0x01, 0x00, 0x80, 0x00}));
    // The reply is a list of mixer settings.
    const auto reply = DecodeOk(Cmd::QueryMixerAll(kAudioSubunit0, 1, 2),
                                ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                           {0x82, 0x01, 0x10, 0x04, 0x00, 0x00, 0x00, 0x03, 0x04, 0x01, 0x00, 0x80, 0x00}));
    const auto values = reply.Values(Cmd::AudioValueKind::kVolume);
    ASSERT_EQ(values.size(), 2u);
    EXPECT_TRUE(values[1].AsVolume()->IsNegativeInfinity());
}

TEST(AudioProcessingTests, TypeSpecificControlsUseTheirOwnSelectors) {
    const auto set = [](Cmd::ControlId control, ControlValue value, Cmd::ProcessingTarget target = Cmd::ProcessingTarget::Block()) {
        return Encoded(Cmd::SetProcessingControl(kAudioSubunit0, 7, target, control, value), CommandType::kControl);
    };
    // 3D stereo extender: Spaciousness 100 % (§10.5.1, selector 03).
    EXPECT_EQ(set(C::kSpaciousnessControl, ControlValue::Percent(100)),
              (std::vector<uint8_t>{0x82, 0x07, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x03, 0x01, 0x64}));
    // Reverberation: type Hall 1 (3), time 1.0 s (0100), early time (§10.5.2-10.5.5, selectors 03, 05, 06).
    EXPECT_EQ(set(C::kReverbTypeControl, ControlValue::ReverbType(3)).at(7), 0x03);
    EXPECT_EQ(set(C::kReverbTimeControl, ControlValue::Seconds(0x0100)),
              (std::vector<uint8_t>{0x82, 0x07, 0x10, 0x04, 0x00, 0xFE, 0xFE, 0x05, 0x02, 0x01, 0x00}));
    EXPECT_EQ(set(C::kReverbEarlyTimeControl, ControlValue::Seconds(0x0080)).at(7), 0x06);
    EXPECT_EQ(set(C::kReverbDelayFeedbackControl, ControlValue::Percent(50)).at(7), 0x07);
    // Chorus rate and depth, compressor controls (selectors 03/04 and 03..07).
    EXPECT_EQ(set(C::kChorusRateControl, ControlValue::Hertz(0x0100)).at(7), 0x03);
    EXPECT_EQ(set(C::kChorusDepthControl, ControlValue::Milliseconds(0x0100)).at(7), 0x04);
    EXPECT_EQ(set(C::kCompressionRatioControl, ControlValue::Ratio(0x0200)).at(7), 0x03);
    EXPECT_EQ(set(C::kMaxAmplControl, ControlValue::Volume(AvcVolume::FromRaw(0))).at(7), 0x04);
    EXPECT_EQ(set(C::kThresholdControl, ControlValue::Volume(AvcVolume::FromRaw(0))).at(7), 0x05);
    EXPECT_EQ(set(C::kAttackTimeControl, ControlValue::Milliseconds(0x0100)).at(7), 0x06);
    EXPECT_EQ(set(C::kReleaseTimeControl, ControlValue::Milliseconds(0x0100)).at(7), 0x07);
    // A per-channel control names its output channel (§10.4): input VOID, OCN 2.
    EXPECT_EQ(set(C::kReverbLevelControl, ControlValue::Percent(10), Cmd::ProcessingTarget::OutputChannel(2)),
              (std::vector<uint8_t>{0x82, 0x07, 0x10, 0x04, 0x00, 0xFE, 0x02, 0x04, 0x01, 0x0A}));
    // A kind that does not match the control is refused: reverb time takes seconds, not a percentage.
    EXPECT_TRUE(Refused(Cmd::SetProcessingControl(kAudioSubunit0, 7, Cmd::ProcessingTarget::Block(), C::kReverbTimeControl,
                                                  ControlValue::Percent(10)),
                        CommandType::kControl));
    // A control whose block is not Processing is refused.
    EXPECT_TRUE(Refused(Cmd::SetProcessingControl(kAudioSubunit0, 7, Cmd::ProcessingTarget::Block(), C::kMuteControl,
                                                  ControlValue::Boolean(true)),
                        CommandType::kControl));
}

// ===========================================================================
// CODEC function block
// ===========================================================================

TEST(AudioCodecTests, CodecFrameHasNoSelectorDataAndALengthedParameterBlock) {
    // Figure 10.96: selector_length 1, control selector, length, data.
    EXPECT_EQ(Encoded(Cmd::EnableCodec(kAudioSubunit0, 1, true), CommandType::kControl),
              (std::vector<uint8_t>{0x83, 0x01, 0x10, 0x01, 0x01, 0x01, 0x70}));
    EXPECT_EQ(Encoded(Cmd::SelectCodecMode(kAudioSubunit0, 1, 3), CommandType::kControl),
              (std::vector<uint8_t>{0x83, 0x01, 0x10, 0x01, 0x02, 0x01, 0x03}));
    EXPECT_EQ(Encoded(Cmd::QueryCodecMode(kAudioSubunit0, 1), CommandType::kStatus),
              (std::vector<uint8_t>{0x83, 0x01, 0x10, 0x01, 0x02, 0x01, 0xFF}));
    EXPECT_TRUE(Refused(Cmd::QueryCodecMode(kAudioSubunit0, 1), CommandType::kControl));
}

TEST(AudioCodecTests, DecoderSpecificControlsUseTheirOwnSelectors) {
    const auto set = [](Cmd::ControlId control, ControlValue value) {
        return Encoded(Cmd::SetCodecControl(kAudioSubunit0, 2, control, value), CommandType::kControl);
    };
    // MPEG (§10.6.4.2): dual channel 03, multilingual 05, scale 07, high/low scaling 08 (LowScale, HighScale).
    EXPECT_EQ(set(C::kMpegDualChannelControl, ControlValue::Boolean(true)),
              (std::vector<uint8_t>{0x83, 0x02, 0x10, 0x01, 0x03, 0x01, 0x70}));
    EXPECT_EQ(set(C::kMpegMultilingualControl, ControlValue::Raw8(2)).at(4), 0x05);
    EXPECT_EQ(set(C::kMpegScaleControl, ControlValue::Scale(0x80)).at(4), 0x07);
    EXPECT_EQ(set(C::kMpegHighLowScalingControl, ControlValue::LowHighScale(0x10, 0x20)),
              (std::vector<uint8_t>{0x83, 0x02, 0x10, 0x01, 0x08, 0x02, 0x10, 0x20}));
    // AC-3 (§10.6.4.3): the same two scale bytes the other way round (HighScale first, Figure 10.119).
    EXPECT_EQ(set(C::kAc3HighLowScalingControl, ControlValue::HighLowScale(0x20, 0x10)),
              (std::vector<uint8_t>{0x83, 0x02, 0x10, 0x01, 0x05, 0x02, 0x20, 0x10}));
    EXPECT_EQ(set(C::kAc3DolbySurroundControl, ControlValue::Boolean(false)).at(6), 0x60);
    // The AC-3 status controls are read-only values: STATUS carries FF.
    EXPECT_EQ(Encoded(Cmd::QueryCodecControl(kAudioSubunit0, 2, C::kAc3SampleRateStatusControl), CommandType::kStatus),
              (std::vector<uint8_t>{0x83, 0x02, 0x10, 0x01, 0x09, 0x01, 0xFF}));
    // Passing a LowHigh pair for a HighLow control (the AC-3 order) is a kind mismatch.
    EXPECT_TRUE(Refused(Cmd::SetCodecControl(kAudioSubunit0, 2, C::kAc3HighLowScalingControl,
                                             ControlValue::LowHighScale(0x10, 0x20)),
                        CommandType::kControl));
}

TEST(AudioCodecTests, ReplyDecodesThroughTheCatalogue) {
    const auto command = Cmd::QueryCodecControl(kAudioSubunit0, 2, C::kMpegDualChannelControl);
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                    {0x83, 0x02, 0x10, 0x01, 0x03, 0x01, 0x60}));
    EXPECT_TRUE(reply.audioSelectorData.empty());
    EXPECT_EQ(reply.Control(static_cast<uint8_t>(Cmd::CodecType::kMpegDecoder))->name, "MPEG_DUAL_CHANNEL_CONTROL");
    EXPECT_EQ(reply.Value(Cmd::AudioValueKind::kBoolean)->AsBoolean(), false);
    // The same selector number in an AC-3 decoder is a different control.
    EXPECT_EQ(reply.Control(static_cast<uint8_t>(Cmd::CodecType::kAc3Decoder))->name, "AC3_DYNAMIC_RANGE_CONTROL");
    const auto mode = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                   {0x83, 0x02, 0x10, 0x01, 0x02, 0x01, 0x01}));
    EXPECT_EQ(mode.CodecMode(), 1u);
}

// ===========================================================================
// CHANGE CONFIGURATION
// ===========================================================================

TEST(ChangeConfigurationTests, FramesMatchFigure111) {
    // CONTROL: configuration_ID, big-endian. STATUS and NOTIFY: FFFF.
    EXPECT_EQ(Encoded(Cmd::SelectConfiguration(kAudioSubunit0, Cmd::ConfigurationId::Of(0x0102)), CommandType::kControl),
              (std::vector<uint8_t>{0x01, 0x02}));
    auto frame = Cmd::QueryConfiguration(kAudioSubunit0).Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kChangeConfiguration);
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0xFF, 0xFF}));
    EXPECT_EQ(Encoded(Cmd::QueryConfiguration(kAudioSubunit0), CommandType::kNotify), (std::vector<uint8_t>{0xFF, 0xFF}));
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kChangeConfiguration), 0xC0);
}

TEST(ChangeConfigurationTests, RefusesWhatTheSpecDoesNotAllow) {
    // Table 11.2: 0000 is reserved and FFFF is invalid, so neither can be selected.
    EXPECT_TRUE(Refused(Cmd::SelectConfiguration(kAudioSubunit0, Cmd::ConfigurationId::Of(0)), CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::SelectConfiguration(kAudioSubunit0, Cmd::ConfigurationId::Invalid()), CommandType::kControl));
    EXPECT_TRUE(Refused(Cmd::QueryConfiguration(kAudioSubunit0), CommandType::kControl));  // FFFF cannot be selected
    EXPECT_TRUE(Refused(Cmd::QueryConfiguration(kAudioSubunit0), CommandType::kSpecificInquiry));
    EXPECT_TRUE(Refused(Cmd::QueryConfiguration(SubunitAddress::Unit()), CommandType::kStatus));
    EXPECT_TRUE(Refused(Cmd::QueryConfiguration(kMusicSubunit0), CommandType::kStatus));
}

TEST(ChangeConfigurationTests, ReplyIsTheCurrentConfiguration) {
    const auto command = Cmd::QueryConfiguration(kAudioSubunit0);
    EXPECT_EQ(DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kChangeConfiguration, {0x00, 0x03})).Id(), 3);
    EXPECT_EQ(DecodeOk(command, ReplyFrame(kAcceptedReply, Opcode::kChangeConfiguration, {0x00, 0x03})).Raw(), 3);
    auto shortReply = Decode(command, ReplyFrame(kStatusReply, Opcode::kChangeConfiguration, {0x00}));
    ASSERT_FALSE(shortReply.has_value());
    EXPECT_EQ(shortReply.error().kind, AvcErrorKind::kOperandsTooShort);
}

// ===========================================================================
// Names
// ===========================================================================

TEST(AudioNamesTests, OpcodesAreNamedBySubunitType) {
    // 0xC0 is CHANGE CONFIGURATION on an Audio subunit and MUSIC PLUG INFO on a Music subunit.
    EXPECT_EQ(Describe(SubunitType::kAudio, Opcode::kChangeConfiguration), "CHANGE CONFIGURATION(0xc0)");
    EXPECT_EQ(Describe(SubunitType::kMusic, Opcode::kMusicPlugInfo), "MUSIC PLUG INFO(0xc0)");
    EXPECT_EQ(DescribeOpcodeOf(0x08, 0xC0), "CHANGE CONFIGURATION(0xc0)");
    EXPECT_EQ(DescribeOpcodeOf(0x60, 0xC0), "MUSIC PLUG INFO(0xc0)");
    EXPECT_EQ(DescribeOpcodeOf(0xFF, 0xC0), "UNKNOWN(opcode:0xc0)");
    // Common opcodes resolve under any subunit type; a subunit-specific one with no subunit does not.
    EXPECT_EQ(Describe(SubunitType::kAudio, Opcode::kFunctionBlock), "FUNCTION BLOCK(0xb8)");
    EXPECT_EQ(Describe(SubunitType::kMusic, Opcode::kSignalSource), "SIGNAL SOURCE(0x1a)");
    EXPECT_EQ(Describe(Opcode::kChangeConfiguration), "UNKNOWN(opcode:0xc0)");
    EXPECT_EQ(Describe(SubunitType::kAudio, Opcode::kMusicPlugInfo), "CHANGE CONFIGURATION(0xc0)");
    EXPECT_EQ(Describe(SubunitType::kAudio, Opcode::kCurrentCapability), "UNKNOWN(opcode:0xc1)");
}

TEST(AudioNamesTests, ControlsPrintByTheSpecIdentifier) {
    EXPECT_EQ(Describe(C::kMuteControl), "MUTE_CONTROL(0x01)");
    EXPECT_EQ(Describe(C::kMixerControl), "MIXER_CONTROL(0x03)");
    EXPECT_EQ(Cmd::DescribeControl(Cmd::FunctionBlockType::kProcessing, 6, 0x03), "REVERBTYPE_CONTROL(0x03)");
    EXPECT_EQ(Cmd::DescribeControl(Cmd::FunctionBlockType::kFeature, 0, 0x2A), "UNKNOWN(control_selector:0x2a)");
    EXPECT_EQ(Describe(Cmd::ProcessingType::kDynamicRangeCompression), "DYNAMIC_RANGE_COMPRESSION(0x08)");
    EXPECT_EQ(Describe(Cmd::CodecType::kAc3Decoder), "AC3_DECODER(0x01)");
    EXPECT_EQ(Cmd::DescribeSubType(Cmd::FunctionBlockType::kProcessing, 0x42), "UNKNOWN(process_type:0x42)");
    EXPECT_EQ(Describe(Cmd::ConfigurationId::Of(2)), "0x0002");
    EXPECT_EQ(Describe(Cmd::ConfigurationId::Invalid()), "invalid(0xffff)");
}

TEST(AudioNamesTests, ValuesPrintTheirMeaningAndTheirRawBytes) {
    // The anchor values of the spec's own tables (Tables 10.5, 10.8, 10.16, 10.17).
    const auto volume = [](int16_t raw) { return Describe(ControlValue::Volume(AvcVolume::FromRaw(raw))); };
    EXPECT_EQ(volume(0x0100), "1.0000 dB(0x0100)");
    EXPECT_EQ(volume(0x7FFE), "127.9922 dB(0x7ffe)");
    EXPECT_EQ(volume(0x0001), "0.0039 dB(0x0001)");
    EXPECT_EQ(volume(static_cast<int16_t>(0xFF00)), "-1.0000 dB(0xff00)");
    EXPECT_EQ(volume(static_cast<int16_t>(0x8001)), "-127.9961 dB(0x8001)");
    EXPECT_EQ(volume(static_cast<int16_t>(0x8000)), "-inf dB(0x8000)");
    EXPECT_EQ(volume(0x7FFF), "invalid(0x7fff)");
    EXPECT_EQ(Describe(ControlValue::Tone(0x7E)), "31.50 dB(0x7e)");
    EXPECT_EQ(Describe(ControlValue::Tone(static_cast<int8_t>(0x82))), "-31.50 dB(0x82)");
    EXPECT_EQ(Describe(ControlValue::Tone(static_cast<int8_t>(0x80))), "-32.00 dB(0x80)");
    EXPECT_EQ(Describe(ControlValue::Tone(0x7F)), "invalid(0x7f)");
    EXPECT_EQ(Describe(ControlValue::Delay(0x0040)), "1.0000 ms(0x0040)");
    EXPECT_EQ(Describe(ControlValue::Delay(0x0001)), "0.0156 ms(0x0001)");
    EXPECT_EQ(Describe(ControlValue::Delay(0xFFFF)), "invalid(0xffff)");
    EXPECT_EQ(Describe(ControlValue::Boolean(true)), "TRUE(0x70)");
    EXPECT_EQ(Describe(ControlValue::Boolean(false)), "FALSE(0x60)");
    EXPECT_EQ(Describe(ControlValue::OfByte(Cmd::AudioValueKind::kBoolean, 0xFF)), "invalid(0xff)");
    EXPECT_EQ(Describe(ControlValue::OfByte(Cmd::AudioValueKind::kBoolean, 0x42)), "UNKNOWN(boolean:0x42)");
    EXPECT_EQ(Describe(ControlValue::Percent(254)), "254 %(0xfe)");
    EXPECT_EQ(Describe(ControlValue::ReverbType(3)), "Hall 1(0x03)");
    EXPECT_EQ(Describe(ControlValue::ReverbType(9)), "type 9(0x09)");
    EXPECT_EQ(Describe(ControlValue::Seconds(0x0100)), "1.0000 s(0x0100)");
    EXPECT_EQ(Describe(ControlValue::Hertz(0x0180)), "1.5000 Hz(0x0180)");
    EXPECT_EQ(Describe(ControlValue::LowHighScale(0x10, 0x20)), "low=0x10 high=0x20");
    EXPECT_EQ(Describe(ControlValue::HighLowScale(0x20, 0x10)), "high=0x20 low=0x10");
    EXPECT_EQ(Describe(Cmd::Steps::Of(3)), "+3 steps(0x0003)");
    EXPECT_EQ(Describe(Cmd::Steps::Of(-2)), "-2 steps(0xfffe)");
}

TEST(AudioNamesTests, RepliesPrintEveryFieldByName) {
    const auto command = Cmd::QueryFeatureControl(kAudioSubunit0, 1, 0, C::kVolumeControl);
    const auto volume = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                     {0x81, 0x01, 0x10, 0x02, 0x00, 0x02, 0x02, 0xFF, 0x00}));
    EXPECT_EQ(Describe(volume),
              "Feature(0x81) id=1 attribute=CURRENT(0x10) selector_data=[00] control=VOLUME_CONTROL(0x02) "
              "value=-1.0000 dB(0xff00)");
    const auto mixer = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                    {0x82, 0x01, 0x10, 0x04, 0x00, 0x03, 0x05, 0x03, 0x02, 0x01, 0x00}));
    EXPECT_EQ(Describe(mixer, static_cast<uint8_t>(Cmd::ProcessingType::kMixer)),
              "Processing(0x82) id=1 attribute=CURRENT(0x10) selector_data=[00 03 05] control=MIXER_CONTROL(0x03) "
              "value=1.0000 dB(0x0100)");
    const auto step = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                   {0x81, 0x01, 0x19, 0x02, 0x00, 0x02, 0x02, 0x00, 0x03}));
    EXPECT_NE(Describe(step).find("steps=+3 steps(0x0003)"), std::string::npos);
    // A control Table A.4 does not define, and a control with the wrong length, keep their bytes.
    const auto unknown = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                      {0x81, 0x01, 0x10, 0x02, 0x00, 0x2A, 0x02, 0xAB, 0xCD}));
    EXPECT_EQ(Describe(unknown),
              "Feature(0x81) id=1 attribute=CURRENT(0x10) selector_data=[00] control=UNKNOWN(control_selector:0x2a) "
              "data=[ab cd]");
    const auto wrongLength = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kFunctionBlock,
                                                          {0x81, 0x01, 0x10, 0x02, 0x00, 0x02, 0x01, 0x01}));
    EXPECT_NE(Describe(wrongLength).find("data=[01]"), std::string::npos);
    // Graphic equalizer: bands by number, gains by dB.
    Cmd::GraphicEqualizerBands bands;
    bands.bandsPresent = 0x5;
    bands.gains = {0x02, 0xFE};
    EXPECT_EQ(Describe(bands), "bands=[14,16] extra=[] gains=[0.50 dB(0x02),-0.50 dB(0xfe)]");
}

} // namespace ASFW::AVC::Test
