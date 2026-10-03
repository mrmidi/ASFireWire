// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicCommandTests.cpp - Tests for the Music subunit command codecs (TA 2001007 §7) and the
// Music subunit identifier descriptor parser (§5).
//
// No reference stack implements these commands and no capture holds one, so every frame here is
// built from the spec itself: the Annex A frames (Figures A.7.1, A.1.2, A.1.9, A.1.13), the
// field figures, and the worked sequence tables 7.15 and 7.17. Wire bytes appear as expected
// values; requests are built with the named factories. Reply frames are the only raw inputs,
// as a device would send them.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/MusicNames.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/MusicPlugCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Descriptors/MusicSubunitIdentifier.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::AVC::Test {

namespace {

namespace D = Protocols::AVC::Descriptors;

constexpr uint8_t kStatusReply = 0x0C;   // IMPLEMENTED/STABLE
constexpr uint8_t kAcceptedReply = 0x09;
constexpr uint8_t kMusicAddress = 0x60;  // Music subunit 0

std::vector<uint8_t> Operands(const CommandFrame& frame) {
    const auto ops = frame.Operands();
    return {ops.begin(), ops.end()};
}

/// A reply frame as a device sends it: response code, address, opcode, operands.
std::vector<uint8_t> ReplyFrame(uint8_t code, Opcode opcode, std::vector<uint8_t> operands) {
    std::vector<uint8_t> frame{code, kMusicAddress, static_cast<uint8_t>(opcode)};
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

} // namespace

// ===========================================================================
// Opcodes and vocabulary
// ===========================================================================

TEST(MusicOpcodeTests, ValuesMatchTable71) {
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kDestinationPlugConfigure), 0x40);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kSourcePlugConfigure), 0x41);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kDestinationConfigurations), 0x42);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kSourceConfigurations), 0x43);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kMusicPlugInfo), 0xC0);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kCurrentCapability), 0xC1);
}

TEST(MusicVocabularyTests, PlugTypeValuesMatchTable73) {
    EXPECT_EQ(static_cast<uint8_t>(Cmd::MusicPlugType::kAudio), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(Cmd::MusicPlugType::kMidi), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(Cmd::MusicPlugType::kSmpteTimeCode), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(Cmd::MusicPlugType::kSampleCount), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(Cmd::MusicPlugType::kAudioSync), 0x80);
    EXPECT_EQ(Cmd::kAllMusicPlugTypes, 0xFF);  // Table 7.18
}

TEST(MusicVocabularyTests, ReservedValuesAreKeptNotRejected) {
    EXPECT_FALSE(Cmd::MusicPlugTypeField::FromRaw(0x07).Type().has_value());
    EXPECT_FALSE(Cmd::MusicPlugTypeField::NotApplicable().Type().has_value());  // FF is reserved in Table 7.3
    EXPECT_FALSE(Cmd::SubunitPlugId::FromRaw(0x20).Number().has_value());       // 1F-FE reserved (Table 7.5)
    EXPECT_EQ(Cmd::SubunitPlugId::FromRaw(0x1E).Number(), 30);
    EXPECT_FALSE(Cmd::MusicPlugId::FromRaw(0xFFFF).Number().has_value());
    EXPECT_EQ(Cmd::MusicPlugId::FromRaw(0x1234).Number(), 0x1234);
    Cmd::PlugConfigureEntry entry;
    entry.first = 0x09;
    EXPECT_FALSE(entry.Subfunction().has_value());
    EXPECT_FALSE(entry.StatusResult().has_value());
}

TEST(MusicVocabularyTests, StreamPositionHasThreeLayouts) {
    // Figure 7.3: stream number, FF.
    const auto audio = Cmd::StreamPosition::Sequence(6);
    EXPECT_EQ(audio.bytes, (std::array<uint8_t, 2>{0x06, 0xFF}));
    EXPECT_EQ(audio.AsSequence(), 6);
    // Figure 7.4: stream number, multiplex index 0..7.
    const auto midi = Cmd::StreamPosition::Multiplexed(8, 3);
    EXPECT_EQ(midi.bytes, (std::array<uint8_t, 2>{0x08, 0x03}));
    ASSERT_TRUE(midi.AsMultiplexed().has_value());
    EXPECT_EQ(midi.AsMultiplexed()->streamNumber, 8);
    EXPECT_EQ(midi.AsMultiplexed()->multiplexIndex, 3);
    EXPECT_FALSE(Cmd::StreamPosition::FromBytes(8, 9).AsMultiplexed().has_value());
    // Figure 7.5: FF FF.
    EXPECT_TRUE(Cmd::StreamPosition::NotApplicable().IsNotApplicable());
    EXPECT_EQ(Cmd::StreamPosition::NotApplicable().bytes, (std::array<uint8_t, 2>{0xFF, 0xFF}));
}

// ===========================================================================
// DESTINATION PLUG CONFIGURE / SOURCE PLUG CONFIGURE
// ===========================================================================

TEST(PlugConfigureTests, DestinationStatusQueryMatchesFigureA19) {
    // Figure A.1.9: one subcommand, music plug type audio, music_plug_ID, the rest FF.
    const auto command = Cmd::QueryDestinationPlugConfigure(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(5))});
    auto frame = command.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kStatus);
    EXPECT_EQ(frame->Address(), kMusicSubunit0);
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kDestinationPlugConfigure);
    EXPECT_EQ(Operands(*frame),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x05, 0xFF, 0xFF, 0xFF}));
}

TEST(PlugConfigureTests, SourceStatusQueryMatchesFigureA71) {
    // Figure A.7.1 (SOURCE PLUG CONFIGURE): same shape, MIDI plug 0.
    const auto command = Cmd::QuerySourcePlugConfigure(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kMidi, Cmd::MusicPlugId::Of(0))});
    auto frame = command.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kSourcePlugConfigure);
    EXPECT_EQ(Operands(*frame),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF}));
}

TEST(PlugConfigureTests, QueryByPositionLeavesThePlugIdUnspecified) {
    // Table 7.10: to ask which music plug sits at a subunit plug and position, music_plug_ID is FFFF.
    const auto command = Cmd::QueryDestinationPlugConfigure(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::QueryPlugAt(Cmd::MusicPlugType::kAudio, Cmd::SubunitPlugId::Of(1),
                                                              Cmd::StreamPosition::Sequence(4))});
    auto frame = command.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(Operands(*frame),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0x01, 0x04, 0xFF}));
}

TEST(PlugConfigureTests, ControlConnectMatchesFigureA113) {
    // Figure A.1.13: CONNECT music input plug 2 (audio) to subunit destination plug 1 at stream 2.
    const auto command = Cmd::ConfigureDestinationPlugs(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(2),
                                                          Cmd::SubunitPlugId::Of(1), Cmd::StreamPosition::Sequence(2))});
    auto frame = command.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kControl);
    // operand[0] n, [1] result_status FF, [2] completed FF, then the 7-byte subcommand (Figure 7.1).
    EXPECT_EQ(Operands(*frame),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x02, 0x01, 0x02, 0xFF}));
}

TEST(PlugConfigureTests, ControlSubfunctionsCarryFFWhereTheSpecSaysSo) {
    const auto frameOf = [](Cmd::PlugConfigureEntry entry) {
        auto frame = Cmd::ConfigureDestinationPlugs(kMusicSubunit0, {entry}).Encode(CommandType::kControl);
        EXPECT_TRUE(frame.has_value());
        return Operands(*frame);
    };
    // DISCONNECT: subunit_plug_ID and stream_position FF (§7.1.1.1).
    EXPECT_EQ(frameOf(Cmd::PlugConfigureEntry::Disconnect(Cmd::MusicPlugType::kMidi, Cmd::MusicPlugId::Of(1))),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0x02, 0x01, 0x00, 0x01, 0xFF, 0xFF, 0xFF}));
    // DISCONNECT_ALL and DEFAULT_CONFIGURE: every parameter FF.
    EXPECT_EQ(frameOf(Cmd::PlugConfigureEntry::DisconnectAll()),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_EQ(frameOf(Cmd::PlugConfigureEntry::DefaultConfigure()),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    // CHANGE_CONNECTION is subfunction 01.
    EXPECT_EQ(frameOf(Cmd::PlugConfigureEntry::ChangeConnection(
                  Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0), Cmd::SubunitPlugId::Of(0),
                  Cmd::StreamPosition::Sequence(0))),
              (std::vector<uint8_t>{0x01, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}));
}

TEST(PlugConfigureTests, SeveralSubcommandsKeepTheirOrder) {
    const auto command = Cmd::ConfigureDestinationPlugs(
        kMusicSubunit0,
        {Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0), Cmd::SubunitPlugId::Of(0),
                                          Cmd::StreamPosition::Sequence(0)),
         Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kMidi, Cmd::MusicPlugId::Of(0), Cmd::SubunitPlugId::Of(0),
                                          Cmd::StreamPosition::Multiplexed(8, 0))});
    auto frame = command.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    const auto ops = Operands(*frame);
    ASSERT_EQ(ops.size(), 3u + 2 * 7);
    EXPECT_EQ(ops[0], 2);
    EXPECT_EQ(ops[4], 0x00);   // first subcommand: audio
    EXPECT_EQ(ops[11], 0x01);  // second subcommand: MIDI
    EXPECT_EQ(ops[15], 0x08);  // MIDI stream_position[0]
    EXPECT_EQ(ops[16], 0x00);  // MIDI multiplex_index
}

TEST(PlugConfigureTests, RefusesWhatTheSpecDoesNotAllow) {
    const auto query = Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0));
    const auto connect = Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0),
                                                          Cmd::SubunitPlugId::Of(0), Cmd::StreamPosition::Sequence(0));
    // SOURCE PLUG CONFIGURE has only a STATUS command (§7.2); NOTIFY and INQUIRY are not defined (Table 7.1).
    EXPECT_FALSE(Cmd::QuerySourcePlugConfigure(kMusicSubunit0, {query}).Encode(CommandType::kControl).has_value());
    EXPECT_FALSE(Cmd::QueryDestinationPlugConfigure(kMusicSubunit0, {query}).Encode(CommandType::kNotify).has_value());
    EXPECT_FALSE(
        Cmd::ConfigureDestinationPlugs(kMusicSubunit0, {connect}).Encode(CommandType::kSpecificInquiry).has_value());
    // A STATUS subcommand carries FF in result_status; a CONTROL subcommand names a subfunction.
    EXPECT_FALSE(Cmd::QueryDestinationPlugConfigure(kMusicSubunit0, {connect}).Encode(CommandType::kStatus).has_value());
    EXPECT_FALSE(Cmd::ConfigureDestinationPlugs(kMusicSubunit0, {query}).Encode(CommandType::kControl).has_value());
    // At least one subcommand, at most 72 (§7.1.1.1).
    EXPECT_FALSE(Cmd::QueryDestinationPlugConfigure(kMusicSubunit0, {}).Encode(CommandType::kStatus).has_value());
    EXPECT_TRUE(Cmd::QueryDestinationPlugConfigure(
                    kMusicSubunit0, std::vector<Cmd::PlugConfigureEntry>(Cmd::kMaxPlugConfigureSubcommands, query))
                    .Encode(CommandType::kStatus)
                    .has_value());
    EXPECT_FALSE(Cmd::QueryDestinationPlugConfigure(
                     kMusicSubunit0, std::vector<Cmd::PlugConfigureEntry>(Cmd::kMaxPlugConfigureSubcommands + 1, query))
                     .Encode(CommandType::kStatus)
                     .has_value());
    // A Music subunit command goes to a Music subunit, not the unit or another subunit type.
    EXPECT_FALSE(Cmd::QueryDestinationPlugConfigure(SubunitAddress::Unit(), {query}).Encode(CommandType::kStatus).has_value());
    EXPECT_FALSE(Cmd::QueryDestinationPlugConfigure(SubunitAddress::Of(SubunitType::kAudio, 0), {query})
                     .Encode(CommandType::kStatus)
                     .has_value());
}

TEST(PlugConfigureTests, StatusReplyGivesTheConnectionOfEachPlug) {
    // Figure 7.9: result_status per subcommand (Table 7.8), subunit plug and stream position filled in.
    const auto command = Cmd::QueryDestinationPlugConfigure(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(1)),
                         Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kMidi, Cmd::MusicPlugId::Of(0))});
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kDestinationPlugConfigure,
                                                    {0x02, 0xFF, 0x02,
                                                     0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0xFF,   // audio 1 -> subunit plug 0, stream 1
                                                     0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00}));  // MIDI 0 -> plug 0, stream 8 mux 0
    EXPECT_EQ(reply.numberOfSubcommands, 2);
    EXPECT_EQ(reply.completedSubcommands, 2);
    ASSERT_EQ(reply.entries.size(), 2u);
    EXPECT_EQ(reply.entries[0].StatusResult(), Cmd::PlugConfigureStatusResult::kOk);
    EXPECT_EQ(reply.entries[0].plugType.Type(), Cmd::MusicPlugType::kAudio);
    EXPECT_EQ(reply.entries[0].plugId, Cmd::MusicPlugId::Of(1));
    EXPECT_EQ(reply.entries[0].subunitPlug.Number(), 0);
    EXPECT_EQ(reply.entries[0].position.AsSequence(), 1);
    EXPECT_EQ(reply.entries[1].plugType.Type(), Cmd::MusicPlugType::kMidi);
    ASSERT_TRUE(reply.entries[1].position.AsMultiplexed().has_value());
    EXPECT_EQ(reply.entries[1].position.AsMultiplexed()->streamNumber, 8);
}

TEST(PlugConfigureTests, StatusReplyForAPlugWithNoConnection) {
    const auto command = Cmd::QueryDestinationPlugConfigure(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(9))});
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kDestinationPlugConfigure,
                                                    {0x01, 0xFF, 0x01, 0x01, 0x00, 0x00, 0x09, 0xFF, 0xFF, 0xFF}));
    ASSERT_EQ(reply.entries.size(), 1u);
    EXPECT_EQ(reply.entries[0].StatusResult(), Cmd::PlugConfigureStatusResult::kNoConnection);
    EXPECT_TRUE(reply.entries[0].position.IsNotApplicable());
}

TEST(PlugConfigureTests, ControlReplyReportsTheResultAndHowFarItGot) {
    // Table 7.6: the second plug does not exist; one subcommand completed.
    const auto command = Cmd::ConfigureDestinationPlugs(
        kMusicSubunit0,
        {Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0), Cmd::SubunitPlugId::Of(0),
                                          Cmd::StreamPosition::Sequence(0)),
         Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(99),
                                          Cmd::SubunitPlugId::Of(0), Cmd::StreamPosition::Sequence(1))});
    const auto reply = DecodeOk(command, ReplyFrame(kAcceptedReply, Opcode::kDestinationPlugConfigure,
                                                    {0x02, 0x03, 0x01,
                                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
                                                     0x00, 0x00, 0x00, 0x63, 0x00, 0x01, 0xFF}));
    EXPECT_EQ(reply.ControlResult(), Cmd::PlugConfigureControlResult::kMusicPlugDoesNotExist);
    EXPECT_EQ(reply.completedSubcommands, 1);
    ASSERT_EQ(reply.entries.size(), 2u);
    EXPECT_EQ(reply.entries[1].Subfunction(), Cmd::PlugConfigureSubfunction::kConnect);
}

TEST(PlugConfigureTests, ShortRepliesAreErrorsAtTheirOffset) {
    const auto command = Cmd::QueryDestinationPlugConfigure(
        kMusicSubunit0, {Cmd::PlugConfigureEntry::QueryConnection(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0))});
    // Under the three header operands.
    auto tooShort = Decode(command, ReplyFrame(kStatusReply, Opcode::kDestinationPlugConfigure, {0x01, 0xFF}));
    ASSERT_FALSE(tooShort.has_value());
    EXPECT_EQ(tooShort.error().kind, AvcErrorKind::kOperandsTooShort);
    // Claims two subcommands, carries one.
    auto missing = Decode(command, ReplyFrame(kStatusReply, Opcode::kDestinationPlugConfigure,
                                              {0x02, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF}));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind, AvcErrorKind::kOperandsTooShort);
    EXPECT_EQ(missing.error().operandOffset, 10);
}

// ===========================================================================
// DESTINATION CONFIGURATIONS / SOURCE CONFIGURATIONS
// ===========================================================================

TEST(PlugConfigurationsTests, RequestsCarryOnlyTheSubunitPlug) {
    // Figures 7.11 and 7.15; Figure A.1.5.
    auto destination = Cmd::QueryDestinationConfigurations(kMusicSubunit0, Cmd::SubunitPlugId::Of(1))
                           .Encode(CommandType::kStatus);
    ASSERT_TRUE(destination.has_value());
    EXPECT_EQ(destination->OpcodeValue(), Opcode::kDestinationConfigurations);
    EXPECT_EQ(Operands(*destination), (std::vector<uint8_t>{0x01}));
    auto source = Cmd::QuerySourceConfigurations(kMusicSubunit0, Cmd::SubunitPlugId::Of(0)).Encode(CommandType::kStatus);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->OpcodeValue(), Opcode::kSourceConfigurations);
    EXPECT_EQ(Operands(*source), (std::vector<uint8_t>{0x00}));
    EXPECT_FALSE(Cmd::QuerySourceConfigurations(kMusicSubunit0, Cmd::SubunitPlugId::Of(0))
                     .Encode(CommandType::kControl)
                     .has_value());
}

TEST(PlugConfigurationsTests, SourceConfigurationsReplyMatchesTable717) {
    // Table 7.17: a talker with 8 audio sequences, 8 multiplexed MIDI streams in sequence 8, and audio SYNC.
    std::vector<uint8_t> operands{0x00, 0x00, 0x00, 0x00, 0x07};  // subunit plug 0; plug IDs 0000..0007
    for (uint8_t i = 0; i < 8; ++i) operands.insert(operands.end(), {0x00, 0x00, i, i, 0xFF});
    for (uint8_t i = 0; i < 8; ++i) operands.insert(operands.end(), {0x01, 0x00, i, 0x08, i});
    operands.insert(operands.end(), {0x80, 0x00, 0x00, 0xFF, 0xFF});
    const auto command = Cmd::QuerySourceConfigurations(kMusicSubunit0, Cmd::SubunitPlugId::Of(0));
    auto frame = ReplyFrame(kStatusReply, Opcode::kSourceConfigurations, operands);
    frame.insert(frame.end(), {0x00, 0x00, 0x00});  // 93 bytes padded to a quadlet boundary: padding is not an entry
    const auto reply = DecodeOk(command, frame);
    EXPECT_EQ(reply.subunitPlug.Number(), 0);
    EXPECT_EQ(reply.startOfMusicPlugId, Cmd::MusicPlugId::Of(0));
    EXPECT_EQ(reply.endOfMusicPlugId, Cmd::MusicPlugId::Of(7));
    ASSERT_EQ(reply.plugs.size(), 17u);
    EXPECT_EQ(reply.plugs[7].plugType.Type(), Cmd::MusicPlugType::kAudio);
    EXPECT_EQ(reply.plugs[7].plugId, Cmd::MusicPlugId::Of(7));
    EXPECT_EQ(reply.plugs[7].position.AsSequence(), 7);
    EXPECT_EQ(reply.plugs[8].plugType.Type(), Cmd::MusicPlugType::kMidi);
    EXPECT_EQ(reply.plugs[15].position.AsMultiplexed()->multiplexIndex, 7);
    EXPECT_EQ(reply.plugs[15].position.AsMultiplexed()->streamNumber, 8);
    EXPECT_EQ(reply.plugs[16].plugType.Type(), Cmd::MusicPlugType::kAudioSync);
    EXPECT_TRUE(reply.plugs[16].position.IsNotApplicable());
}

TEST(PlugConfigurationsTests, DestinationConfigurationsReplyMatchesTable715) {
    // Table 7.15: serial bus input plug 0 carries audio sequences 0..7 and one MIDI sequence.
    std::vector<uint8_t> operands{0x00, 0x00, 0x00, 0x00, 0x05};
    for (uint8_t i = 0; i < 4; ++i) operands.insert(operands.end(), {0x00, 0x00, i, i, 0xFF});
    operands.insert(operands.end(), {0x01, 0x00, 0x00, 0x08, 0x00});
    const auto command = Cmd::QueryDestinationConfigurations(kMusicSubunit0, Cmd::SubunitPlugId::Of(0));
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kDestinationConfigurations, operands));
    ASSERT_EQ(reply.plugs.size(), 5u);
    EXPECT_EQ(reply.plugs[4].plugType.Type(), Cmd::MusicPlugType::kMidi);
    EXPECT_EQ(reply.plugs[4].position.AsMultiplexed()->streamNumber, 8);
}

TEST(PlugConfigurationsTests, AReplyWithNoEntriesIsLegitimate) {
    // §7.3.1.3: a stream no music plug is assigned to may be omitted.
    const auto command = Cmd::QueryDestinationConfigurations(kMusicSubunit0, Cmd::SubunitPlugId::Of(1));
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kDestinationConfigurations,
                                                    {0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
    EXPECT_TRUE(reply.plugs.empty());
    auto tooShort = Decode(command, ReplyFrame(kStatusReply, Opcode::kDestinationConfigurations, {0x01, 0x00}));
    ASSERT_FALSE(tooShort.has_value());
    EXPECT_EQ(tooShort.error().kind, AvcErrorKind::kOperandsTooShort);
}

// ===========================================================================
// MUSIC PLUG INFO
// ===========================================================================

TEST(MusicPlugInfoTests, RequestNamesAPlugTypeOrAllKinds) {
    auto audio = Cmd::QueryMusicPlugInfo(kMusicSubunit0, Cmd::MusicPlugType::kAudio).Encode(CommandType::kStatus);
    ASSERT_TRUE(audio.has_value());
    EXPECT_EQ(audio->OpcodeValue(), Opcode::kMusicPlugInfo);
    EXPECT_EQ(Operands(*audio), (std::vector<uint8_t>{0x00}));
    auto sync = Cmd::QueryMusicPlugInfo(kMusicSubunit0, Cmd::MusicPlugType::kAudioSync).Encode(CommandType::kStatus);
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(Operands(*sync), (std::vector<uint8_t>{0x80}));
    auto all = Cmd::QueryAllMusicPlugInfo(kMusicSubunit0).Encode(CommandType::kStatus);  // Table 7.18
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(Operands(*all), (std::vector<uint8_t>{0xFF}));
    EXPECT_FALSE(Cmd::QueryAllMusicPlugInfo(kMusicSubunit0).Encode(CommandType::kControl).has_value());
    EXPECT_FALSE(Cmd::QueryAllMusicPlugInfo(SubunitAddress::Unit()).Encode(CommandType::kStatus).has_value());
}

TEST(MusicPlugInfoTests, ReplyListsInputAndOutputPlugCountsPerType) {
    // Figure 7.19: FF, n = 2; Figure 7.20: type, inputs (2), outputs (2).
    const auto command = Cmd::QueryAllMusicPlugInfo(kMusicSubunit0);
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kMusicPlugInfo,
                                                    {0xFF, 0x02,
                                                     0x00, 0x00, 0x12, 0x00, 0x10,
                                                     0x01, 0x00, 0x01, 0x00, 0x01}));
    ASSERT_EQ(reply.types.size(), 2u);
    const auto* audio = reply.Find(Cmd::MusicPlugType::kAudio);
    ASSERT_NE(audio, nullptr);
    EXPECT_EQ(audio->inputPlugs, 18);
    EXPECT_EQ(audio->outputPlugs, 16);
    const auto* midi = reply.Find(Cmd::MusicPlugType::kMidi);
    ASSERT_NE(midi, nullptr);
    EXPECT_EQ(midi->inputPlugs, 1);
    EXPECT_EQ(reply.Find(Cmd::MusicPlugType::kSmpteTimeCode), nullptr);
}

TEST(MusicPlugInfoTests, ShortReplyIsAnErrorAndAnEmptyOneIsNot) {
    const auto command = Cmd::QueryAllMusicPlugInfo(kMusicSubunit0);
    EXPECT_TRUE(DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kMusicPlugInfo, {0xFF, 0x00})).types.empty());
    auto missing = Decode(command, ReplyFrame(kStatusReply, Opcode::kMusicPlugInfo, {0xFF, 0x02, 0x00, 0x00, 0x01}));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind, AvcErrorKind::kOperandsTooShort);
    auto noHeader = Decode(command, ReplyFrame(kStatusReply, Opcode::kMusicPlugInfo, {0xFF}));
    ASSERT_FALSE(noHeader.has_value());
}

// ===========================================================================
// CURRENT CAPABILITY
// ===========================================================================

TEST(CurrentCapabilityTests, RequestMatchesFigure721) {
    const auto command = Cmd::QueryCurrentCapability(kMusicSubunit0, Cmd::MusicPlugDirection::kOutput,
                                                     Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0),
                                                     Cmd::MusicPlugId::Of(7));
    auto frame = command.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kCurrentCapability);
    // direction, type, FF, start (2), end (2).
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x01, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x07}));
    EXPECT_FALSE(command.Encode(CommandType::kControl).has_value());
}

TEST(CurrentCapabilityTests, ReplyCarriesAFormatPerPlug) {
    // Figure 7.22 with the 4-byte format entry of Figure 7.23. Audio: FDF 00 and an AM824 label (Figure 5.9).
    const auto command = Cmd::QueryCurrentCapability(kMusicSubunit0, Cmd::MusicPlugDirection::kInput,
                                                     Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0),
                                                     Cmd::MusicPlugId::Of(1));
    const auto reply = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kCurrentCapability,
                                                    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                                     0x00, 0x00, 0x00, 0x40,
                                                     0x00, 0x01, 0x00, 0x00}));
    EXPECT_EQ(reply.Direction(), Cmd::MusicPlugDirection::kInput);
    EXPECT_EQ(reply.plugType.Type(), Cmd::MusicPlugType::kAudio);
    EXPECT_EQ(reply.Attribute(), Cmd::MusicPlugAttribute::kSimple);
    EXPECT_EQ(reply.startOfMusicPlugId, Cmd::MusicPlugId::Of(0));
    EXPECT_EQ(reply.endOfMusicPlugId, Cmd::MusicPlugId::Of(1));
    ASSERT_EQ(reply.formats.size(), 2u);
    EXPECT_EQ(reply.formats[0].format.AsAudio().fdf, 0x00);
    EXPECT_EQ(reply.formats[0].format.AsAudio().am824Label, 0x40);  // multi-bit linear audio (Table 5.8)
    EXPECT_EQ(reply.formats[1].plugId, Cmd::MusicPlugId::Of(1));
}

TEST(CurrentCapabilityTests, FormatInfoIsReadInEachTypesLayout) {
    const auto Format = [](uint8_t first, uint8_t second) { return Cmd::MusicFormatInfo{{first, second}}; };
    // MIDI (Figure 7.24): version 1, revision 0, adaptation layer 0.
    const Cmd::MusicFormatInfo midi{{0x10, 0x00}};
    EXPECT_EQ(midi.AsMidi().version, 1);
    EXPECT_EQ(midi.AsMidi().revision, 0);
    EXPECT_EQ(midi.AsMidi().adaptationLayerVersion, 0);
    // SMPTE and sample count (Tables 7.21, 7.22): bit 0 Rx, bit 1 Tx.
    EXPECT_TRUE(Format(0x01, 0x00).AsTransfer().receive);
    EXPECT_FALSE(Format(0x01, 0x00).AsTransfer().transmit);
    EXPECT_TRUE(Format(0x02, 0x00).AsTransfer().transmit);
    // Audio SYNC (Table 7.23): bit 0 Bus, bit 1 Ex.
    EXPECT_TRUE(Format(0x01, 0x00).AsSync().bus);
    EXPECT_TRUE(Format(0x02, 0x00).AsSync().external);
}

TEST(CurrentCapabilityTests, ShortReplyIsAnError) {
    const auto command = Cmd::QueryCurrentCapability(kMusicSubunit0, Cmd::MusicPlugDirection::kInput,
                                                     Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(0),
                                                     Cmd::MusicPlugId::Of(0));
    auto shortReply = Decode(command, ReplyFrame(kStatusReply, Opcode::kCurrentCapability, {0x00, 0x00, 0x00, 0x00}));
    ASSERT_FALSE(shortReply.has_value());
    EXPECT_EQ(shortReply.error().kind, AvcErrorKind::kOperandsTooShort);
    // Three trailing bytes are padding, not a format entry.
    const auto padded = DecodeOk(command, ReplyFrame(kStatusReply, Opcode::kCurrentCapability,
                                                     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    EXPECT_TRUE(padded.formats.empty());
}

// ===========================================================================
// Names
// ===========================================================================

TEST(MusicNamesTests, EveryOpcodeAndFieldValueHasItsSpecName) {
    EXPECT_EQ(Describe(Opcode::kDestinationPlugConfigure), "DESTINATION PLUG CONFIGURE(0x40)");
    EXPECT_EQ(Describe(Opcode::kSourcePlugConfigure), "SOURCE PLUG CONFIGURE(0x41)");
    EXPECT_EQ(Describe(Opcode::kDestinationConfigurations), "DESTINATION CONFIGURATIONS(0x42)");
    EXPECT_EQ(Describe(Opcode::kSourceConfigurations), "SOURCE CONFIGURATIONS(0x43)");
    EXPECT_EQ(Describe(Opcode::kMusicPlugInfo), "MUSIC PLUG INFO(0xc0)");
    EXPECT_EQ(Describe(Opcode::kCurrentCapability), "CURRENT CAPABILITY(0xc1)");
    EXPECT_EQ(Cmd::Describe(Cmd::PlugConfigureSubfunction::kChangeConnection), "CHANGE_CONNECTION(0x01)");
    EXPECT_EQ(Cmd::Describe(Cmd::PlugConfigureControlResult::kMusicPlugAlreadyConnected),
              "music_plug already connected(0x05)");
    EXPECT_EQ(Cmd::Describe(Cmd::PlugConfigureStatusResult::kNoConnection), "no connection(0x01)");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugDirection::kOutput), "music_output_plug(0x01)");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugAttribute::kCompound), "compound(0x01)");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugTypeField::Of(Cmd::MusicPlugType::kAudioSync)), "audio SYNC(0x80)");
}

TEST(MusicNamesTests, UnnamedValuesPrintAsUnknownWithTheirTable) {
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugTypeField::FromRaw(0x07)), "UNKNOWN(music_plug_type:0x07)");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugTypeField::NotApplicable()), "none(0xff)");
    EXPECT_EQ(Cmd::DescribeMusicPlugInfoRequestType(std::nullopt), "all kinds(0xff)");
    EXPECT_EQ(Cmd::Describe(static_cast<Cmd::PlugConfigureStatusResult>(9)), "UNKNOWN(plug_configure_status:0x09)");
    EXPECT_EQ(Cmd::Describe(static_cast<Cmd::PlugConfigureSubfunction>(7)), "UNKNOWN(plug_configure_subfunction:0x07)");
    EXPECT_EQ(Cmd::Describe(Cmd::SubunitPlugId::FromRaw(0x20)), "UNKNOWN(subunit_plug_id:0x20)");
    EXPECT_EQ(Cmd::Describe(Cmd::SubunitPlugId::NoPlug()), "no plug(0xff)");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugId::Unspecified()), "unspecified(0xffff)");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugId::Of(2)), "0x0002");
}

TEST(MusicNamesTests, PositionsAndEntriesReadInTheirPlugTypesLayout) {
    const auto audio = Cmd::MusicPlugTypeField::Of(Cmd::MusicPlugType::kAudio);
    const auto midi = Cmd::MusicPlugTypeField::Of(Cmd::MusicPlugType::kMidi);
    EXPECT_EQ(Cmd::Describe(Cmd::StreamPosition::Sequence(3), audio), "stream 3");
    EXPECT_EQ(Cmd::Describe(Cmd::StreamPosition::Multiplexed(8, 5), midi), "stream 8 multiplex 5");
    EXPECT_EQ(Cmd::Describe(Cmd::StreamPosition::NotApplicable(), audio), "none(0xff 0xff)");
    // A MIDI-looking position on an audio plug, and a multiplex index out of range, are not guessed at.
    EXPECT_EQ(Cmd::Describe(Cmd::StreamPosition::Multiplexed(8, 5), audio), "UNKNOWN(stream_position:08 05)");
    EXPECT_EQ(Cmd::Describe(Cmd::StreamPosition::FromBytes(8, 9), midi), "UNKNOWN(stream_position:08 09)");

    const auto connect = Cmd::PlugConfigureEntry::Connect(Cmd::MusicPlugType::kAudio, Cmd::MusicPlugId::Of(1),
                                                          Cmd::SubunitPlugId::Of(0), Cmd::StreamPosition::Sequence(1));
    EXPECT_EQ(Cmd::Describe(connect, Cmd::PlugConfigureKind::kControl),
              "subfunction=CONNECT(0x00) type=audio(0x00) plug=0x0001 subunit_plug=0x00 position=stream 1");
    auto status = connect;
    status.first = 0x01;
    EXPECT_EQ(Cmd::Describe(status, Cmd::PlugConfigureKind::kStatus),
              "result=no connection(0x01) type=audio(0x00) plug=0x0001 subunit_plug=0x00 position=stream 1");
    status.first = 0x42;
    EXPECT_NE(Cmd::Describe(status, Cmd::PlugConfigureKind::kStatus).find("result=UNKNOWN(plug_configure_status:0x42) "),
              std::string::npos);

    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugCounts{audio, 18, 16}), "type=audio(0x00) input_plugs=18 output_plugs=16");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicPlugInfoEntry{midi, Cmd::MusicPlugId::Of(0), Cmd::StreamPosition::Multiplexed(8, 0)}),
              "type=MIDI(0x01) plug=0x0000 position=stream 8 multiplex 0");
}

TEST(MusicNamesTests, FormatInfoReadsPerType) {
    const auto type = [](Cmd::MusicPlugType t) { return Cmd::MusicPlugTypeField::Of(t); };
    EXPECT_EQ(Cmd::Describe(Cmd::MusicFormatInfo{{0x00, 0x40}}, type(Cmd::MusicPlugType::kAudio)),
              "fdf=0x00 am824_label=0x40");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicFormatInfo{{0x10, 0x00}}, type(Cmd::MusicPlugType::kMidi)),
              "MIDI 1.0 adaptation_layer=0x00");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicFormatInfo{{0x03, 0x00}}, type(Cmd::MusicPlugType::kSampleCount)),
              "rx=yes tx=yes");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicFormatInfo{{0x02, 0x00}}, type(Cmd::MusicPlugType::kAudioSync)),
              "bus=no external=yes");
    EXPECT_EQ(Cmd::Describe(Cmd::MusicFormatInfo{{0xAB, 0xCD}}, Cmd::MusicPlugTypeField::FromRaw(0x55)),
              "UNKNOWN(music_format_info:ab cd)");
}

// ===========================================================================
// Music subunit identifier descriptor (§5)
// ===========================================================================

namespace {

std::vector<uint8_t> WithLength(std::vector<uint8_t> body) {
    std::vector<uint8_t> out{static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size() & 0xFF)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<uint8_t> Concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

// Capability fields (Figures 5.4-5.17), each led by its one-byte length.
const std::vector<uint8_t> kGeneral{0x06, 0x03, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};  // tx NB+B, rx NB, latency FFFFFFFF
const std::vector<uint8_t> kAudio{0x0D, 0x02, 0x00, 0x08, 0x00, 0x08, 0x00, 0x40,  // 8 in, 8 out, FDF 00, label 40
                                  0x00, 0x02, 0x00, 0x02, 0x01, 0x00};             // 2 in, 2 out, FDF 01
const std::vector<uint8_t> kMidi{0x06, 0x10, 0x00, 0x00, 0x01, 0x00, 0x02};        // 1.0, layer 0, 1 in, 2 out
const std::vector<uint8_t> kSmpte{0x01, 0x03};
const std::vector<uint8_t> kSampleCount{0x01, 0x01};
const std::vector<uint8_t> kSync{0x01, 0x02};

/// A complete identifier descriptor around the given capability attributes and capability fields.
std::vector<uint8_t> IdentifierDescriptor(std::vector<uint8_t> capabilityAttributes, std::vector<uint8_t> capabilities,
                                          std::vector<uint8_t> manufacturer = {0x00, 0x00},
                                          std::vector<uint8_t> optionalBlocks = {}) {
    const auto specific = WithLength(Concat({capabilityAttributes, capabilities}));
    const auto fields = WithLength(Concat({{0x00, 0x10}, specific, optionalBlocks}));  // attributes 00, version 1.0
    const auto dependent = WithLength(fields);
    return WithLength(Concat({{0x02, 0x02, 0x00, 0x02, 0x00, 0x00}, dependent, manufacturer}));
    // generation_ID 02, size_of_list_ID 2, size_of_object_ID 0, size_of_object_position 2, no root lists
}

} // namespace

TEST(MusicIdentifierTests, ParsesEveryCapabilityField) {
    const auto bytes = IdentifierDescriptor({0x3F}, Concat({kGeneral, kAudio, kMidi, kSmpte, kSampleCount, kSync}));
    const auto parsed = D::MusicSubunitIdentifierParser::Parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->generationId, D::kMusicGenerationAvc40);
    EXPECT_EQ(parsed->sizeOfListId, 2);
    EXPECT_EQ(parsed->sizeOfObjectPosition, 2);
    EXPECT_TRUE(parsed->rootListIds.empty());
    EXPECT_EQ(parsed->attributes, (std::vector<uint8_t>{0x00}));
    EXPECT_EQ(parsed->version, D::kMusicSubunitVersion10);
    ASSERT_TRUE(parsed->general.has_value());
    EXPECT_EQ(parsed->general->transmit, D::kMusicCapabilityNonBlockingBit | D::kMusicCapabilityBlockingBit);
    EXPECT_EQ(parsed->general->receive, D::kMusicCapabilityNonBlockingBit);
    EXPECT_EQ(parsed->general->latency, D::kMusicLatencyNotSpecified);
    ASSERT_TRUE(parsed->audio.has_value());
    ASSERT_EQ(parsed->audio->size(), 2u);
    EXPECT_EQ((*parsed->audio)[0].maxInputChannels, 8);
    EXPECT_EQ((*parsed->audio)[0].maxOutputChannels, 8);
    EXPECT_EQ((*parsed->audio)[0].am824Label, 0x40);
    EXPECT_EQ((*parsed->audio)[1].fdf, 0x01);
    ASSERT_TRUE(parsed->midi.has_value());
    EXPECT_EQ(parsed->midi->version, 1);
    EXPECT_EQ(parsed->midi->revision, 0);
    EXPECT_EQ(parsed->midi->maxInputPorts, 1);
    EXPECT_EQ(parsed->midi->maxOutputPorts, 2);
    EXPECT_EQ(parsed->smpteTimeCode, D::kMusicCapabilityRxBit | D::kMusicCapabilityTxBit);
    EXPECT_EQ(parsed->sampleCount, D::kMusicCapabilityRxBit);
    EXPECT_EQ(parsed->audioSync, D::kMusicCapabilityTxBit);
    EXPECT_TRUE(parsed->hasManufacturerInformation);
    EXPECT_EQ(parsed->manufacturerInformationBytes, 0u);
}

TEST(MusicIdentifierTests, OnlyTheFieldsTheAttributesNameArePresent) {
    // Table 5.4: audio and audio SYNC only; the fields appear in table order, none for the cleared bits.
    const auto bytes = IdentifierDescriptor({0x22}, Concat({kAudio, kSync}));
    const auto parsed = D::MusicSubunitIdentifierParser::Parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->general.has_value());
    EXPECT_TRUE(parsed->audio.has_value());
    EXPECT_FALSE(parsed->midi.has_value());
    EXPECT_FALSE(parsed->smpteTimeCode.has_value());
    EXPECT_FALSE(parsed->sampleCount.has_value());
    EXPECT_EQ(parsed->audioSync, D::kMusicCapabilityTxBit);
}

TEST(MusicIdentifierTests, AttributeChainsAndFutureBytesAreAccepted) {
    // Table 5.4: bit 7 says another attributes byte follows. Optional info blocks and manufacturer bytes are bounded and skipped.
    const auto bytes = IdentifierDescriptor({0x81, 0x00}, kGeneral, {0x00, 0x03, 0xDE, 0xAD, 0x01}, {0xAA, 0xBB, 0xCC});
    const auto parsed = D::MusicSubunitIdentifierParser::Parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->capabilityAttributes, (std::vector<uint8_t>{0x81, 0x00}));
    EXPECT_TRUE(parsed->general.has_value());
    EXPECT_EQ(parsed->optionalInfoBytes, 3u);
    EXPECT_EQ(parsed->manufacturerInformationBytes, 3u);
}

TEST(MusicIdentifierTests, ReadsRootListIdsOfTheDeclaredSize) {
    // Figure 5.1: size_of_list_ID bytes per root list.
    const auto specific = WithLength(Concat({{0x00}, {}}));
    const auto fields = WithLength(Concat({{0x00, 0x10}, specific}));
    const auto dependent = WithLength(fields);
    const auto bytes = WithLength(Concat({{0x02, 0x02, 0x00, 0x02, 0x00, 0x02, 0x10, 0x00, 0x10, 0x01}, dependent, {0x00, 0x00}}));
    const auto parsed = D::MusicSubunitIdentifierParser::Parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->rootListIds, (std::vector<uint64_t>{0x1000, 0x1001}));
}

TEST(MusicIdentifierTests, ABrokenDescriptorIsAnErrorAtItsOffset) {
    auto bytes = IdentifierDescriptor({0x3F}, Concat({kGeneral, kAudio, kMidi, kSmpte, kSampleCount, kSync}));
    // Declared length longer than the bytes.
    auto longer = bytes;
    longer[1] = static_cast<uint8_t>(longer[1] + 4);
    EXPECT_FALSE(D::MusicSubunitIdentifierParser::Parse(longer).has_value());
    // Capability attributes promise a MIDI field the descriptor does not carry.
    const auto missing = D::MusicSubunitIdentifierParser::Parse(IdentifierDescriptor({0x04}, {}));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().kind, D::ParseErrorKind::Truncated);
    // A general capability field shorter than tx, rx and latency.
    const auto shortGeneral = D::MusicSubunitIdentifierParser::Parse(IdentifierDescriptor({0x01}, {0x02, 0x03, 0x01}));
    ASSERT_FALSE(shortGeneral.has_value());
    EXPECT_EQ(shortGeneral.error().kind, D::ParseErrorKind::Truncated);
    // A list ID wider than eight bytes cannot be held.
    auto wide = IdentifierDescriptor({0x00}, {});
    wide[3] = 9;  // size_of_list_ID
    const auto wideResult = D::MusicSubunitIdentifierParser::Parse(wide);
    ASSERT_FALSE(wideResult.has_value());
    EXPECT_EQ(wideResult.error(), (D::ParseError{3, D::ParseErrorKind::InvalidValue}));
    // Fewer than the declared audio formats.
    const std::vector<uint8_t> audioShort{0x07, 0x02, 0x00, 0x08, 0x00, 0x08, 0x00, 0x40};
    EXPECT_FALSE(D::MusicSubunitIdentifierParser::Parse(IdentifierDescriptor({0x02}, audioShort)).has_value());
}

TEST(MusicIdentifierTests, ManufacturerInformationMayBeAbsent) {
    // A descriptor that ends after the type dependent information is read, not rejected.
    const auto specific = WithLength(Concat({{0x00}, {}}));
    const auto dependent = WithLength(WithLength(Concat({{0x00, 0x10}, specific})));
    const auto bytes = WithLength(Concat({{0x02, 0x02, 0x00, 0x02, 0x00, 0x00}, dependent}));
    const auto parsed = D::MusicSubunitIdentifierParser::Parse(bytes);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->hasManufacturerInformation);
}

} // namespace ASFW::AVC::Test
