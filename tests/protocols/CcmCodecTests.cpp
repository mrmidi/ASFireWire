// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CcmCodecTests.cpp - Tests for the AV/C CCM codecs: SIGNAL SOURCE, INPUT SELECT,
// OUTPUT PRESET, CCM PROFILE (TA 2002010, AV/C CCM Specification 1.1).
//
// Frames come from three places, named in each test:
// - the spec's own worked examples (Annex C Tables C.1, C.2, C.6);
// - the figures (every field position of every command);
// - STATUS replies captured from a TerraTec Phase 88 and an Apogee Duet
//   (documentation/avc-rebuild/fixtures/*_descriptors.json).
// Wire bytes appear here as the expected values, never as inputs: requests are built with
// the named factories.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/CcmProfileCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/CcmTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/InputSelectCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/OutputPresetCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/SignalSourceCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::AVC::Test {

namespace {

constexpr SubunitAddress kTapeSubunit0 = SubunitAddress::Of(SubunitType::kTape, 0);
constexpr SubunitAddress kCameraSubunit0 = SubunitAddress::Of(SubunitType::kCamera, 0);

std::vector<uint8_t> Operands(const CommandFrame& frame) {
    const auto ops = frame.Operands();
    return {ops.begin(), ops.end()};
}

template <class Reply>
Reply DecodeReply(const auto& command, std::initializer_list<uint8_t> frameBytes) {
    const std::vector<uint8_t> bytes(frameBytes);
    auto response = ParseResponse(bytes);
    EXPECT_TRUE(response.has_value());
    auto reply = command.Decode(response->operands);
    EXPECT_TRUE(reply.has_value());
    return *reply;
}

} // namespace

// ===========================================================================
// Opcodes
// ===========================================================================

TEST(CcmOpcodeTests, ValuesMatchTheCommandTable) {
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kSignalSource), 0x1A);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kInputSelect), 0x1B);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kOutputPreset), 0x1C);
    EXPECT_EQ(static_cast<uint8_t>(Opcode::kCcmProfile), 0x1D);
}

// ===========================================================================
// Plug and address vocabulary
// ===========================================================================

TEST(CcmVocabularyTests, UnitPlugIdNamedValues) {
    using P = Cmd::UnitPlugId;
    EXPECT_EQ(P::SerialBus(0).Raw(), 0x00);
    EXPECT_EQ(P::SerialBus(30).Raw(), 0x1E);
    EXPECT_EQ(P::External(0).Raw(), 0x80);
    EXPECT_EQ(P::External(30).Raw(), 0x9E);
    EXPECT_EQ(P::AnyAvailableSerialBus().Raw(), 0x7F);   // Tables 7.2, 7.4
    EXPECT_EQ(P::NotApplicable().Raw(), 0x7F);           // Tables 7.15, 7.19
    EXPECT_EQ(P::AnyAvailableExternal().Raw(), 0xFF);
    EXPECT_EQ(P::Invalid().Raw(), 0xFE);
    EXPECT_EQ(P::External(5).Number(), 5);
    EXPECT_EQ(P::SerialBus(7).Number(), 7);
    EXPECT_FALSE(P::FromRaw(0x7F).Number().has_value());
    EXPECT_FALSE(P::FromRaw(0x9F).Number().has_value());  // reserved
}

TEST(CcmVocabularyTests, SignalAddressNamedEncodings) {
    using A = Cmd::SignalAddress;
    using K = Cmd::SignalAddressKind;
    EXPECT_EQ(A::NoSignal().bytes, (std::array<uint8_t, 2>{0xFF, 0xFE}));        // Figure 7.14
    EXPECT_EQ(A::Multiple().bytes, (std::array<uint8_t, 2>{0xFF, 0xFD}));        // Figure 7.16
    EXPECT_EQ(A::AvConvergent().bytes, (std::array<uint8_t, 2>{0xFF, 0xFC}));    // Figure 7.15
    EXPECT_EQ(A::AnyAvailableSerialBusPlug().bytes, (std::array<uint8_t, 2>{0xFF, 0x7F}));
    EXPECT_EQ(A::AnyAvailableExternalPlug().bytes, (std::array<uint8_t, 2>{0xFF, 0xFF}));
    EXPECT_EQ(A::UnitIsochronousPlug(2).bytes, (std::array<uint8_t, 2>{0xFF, 0x02}));
    EXPECT_EQ(A::UnitExternalPlug(1).bytes, (std::array<uint8_t, 2>{0xFF, 0x81}));
    EXPECT_EQ(A::SubunitPlug(kMusicSubunit0, 3).bytes, (std::array<uint8_t, 2>{0x60, 0x03}));
    EXPECT_EQ(A::AnyAvailableSubunitPlug(kMusicSubunit0).bytes, (std::array<uint8_t, 2>{0x60, 0xFF}));

    EXPECT_EQ(A::NoSignal().Kind(), K::kNoSignal);
    EXPECT_EQ(A::Multiple().Kind(), K::kMultiple);
    EXPECT_EQ(A::AvConvergent().Kind(), K::kAvConvergent);
    EXPECT_EQ(A::UnitIsochronousPlug(0).Kind(), K::kUnitPlug);
    EXPECT_EQ(A::SubunitPlug(kMusicSubunit0, 0).Kind(), K::kSubunitPlug);
    EXPECT_TRUE(A::UnitExternalPlug(1).IsExternalUnitPlug());
    EXPECT_EQ(A::UnitExternalPlug(1).PlugId(), 1);
}

TEST(CcmVocabularyTests, DestinationKindDecidesWhichStatusValuesAreLegal) {
    using A = Cmd::SignalAddress;
    using D = Cmd::DestinationPlugKind;
    EXPECT_EQ(Cmd::DestinationKindOf(A::UnitIsochronousPlug(0)), D::kSerialBusOutputPlug);
    EXPECT_EQ(Cmd::DestinationKindOf(A::UnitExternalPlug(0)), D::kExternalOutputPlug);
    EXPECT_EQ(Cmd::DestinationKindOf(A::SubunitPlug(kMusicSubunit0, 0)), D::kSubunitDestinationPlug);
    EXPECT_FALSE(Cmd::DestinationKindOf(A::NoSignal()).has_value());
    EXPECT_FALSE(Cmd::DestinationKindOf(A::AnyAvailableSerialBusPlug()).has_value());
}

TEST(CcmVocabularyTests, StatusFieldSplitsTheByte) {
    using F = Cmd::SignalSourceStatusField;
    const auto field = F::Make(3, true, 0x6);
    EXPECT_EQ(field.Raw(), 0x76);
    EXPECT_EQ(field.OutputStatusCode(), 3);
    EXPECT_EQ(field.Status(), Cmd::OutputStatus::kReady);
    EXPECT_TRUE(field.CanChangeFormat());
    EXPECT_EQ(field.Modifications().bits, 0x6);
    EXPECT_TRUE(field.Modifications().Converted());
    EXPECT_TRUE(field.Modifications().Filtered());
    EXPECT_FALSE(field.Modifications().Processed());
    EXPECT_FALSE(field.Modifications().OsdOverlaid());
    EXPECT_FALSE(field.Modifications().Identical());
    EXPECT_TRUE(Cmd::SignalModifications{0x8}.Processed());      // Figure 7.10 bit order
    EXPECT_TRUE(Cmd::SignalModifications{0x1}.OsdOverlaid());
}

TEST(CcmVocabularyTests, ReservedValuesAreKeptNotRejected) {
    const auto status = Cmd::SignalSourceStatusField::FromRaw(0xE0);  // output_status 7
    EXPECT_FALSE(status.Status().has_value());
    EXPECT_EQ(status.OutputStatusCode(), 7);
    const auto control = Cmd::SignalSourceControlField::FromRaw(0x35);
    EXPECT_FALSE(control.Result().has_value());
    EXPECT_EQ(control.ResultCode(), 5);
    EXPECT_FALSE(control.ReservedIsZero());
}

// ===========================================================================
// SIGNAL SOURCE
// ===========================================================================

TEST(SignalSourceCcmTests, StatusRequestCarriesTheStatusDefaults) {
    // Table C.1: STATUS to oPCR[2] of Camera1: operand[0] FF, source FF FE, destination FF 02.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(2));
    auto frame = command.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kStatus);
    EXPECT_EQ(frame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kSignalSource);
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0xFF, 0xFF, 0xFE, 0xFF, 0x02}));
}

TEST(SignalSourceCcmTests, StatusReplyForAReadyOutputPlug) {
    // Table C.1 reply: output_status 3 (ready), conv 0, signal_status 0, source = camera
    // subunit plug 0, destination oPCR[2].
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(2));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x60, 0x38, 0x00, 0xFF, 0x02});
    EXPECT_EQ(reply.first.Raw(), 0x60);
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kReady);
    EXPECT_FALSE(reply.Status().CanChangeFormat());
    EXPECT_TRUE(reply.Status().Modifications().Identical());
    EXPECT_EQ(reply.source, Cmd::SignalAddress::SubunitPlug(kCameraSubunit0, 0));
    EXPECT_EQ(reply.destination, Cmd::SignalAddress::UnitIsochronousPlug(2));
    const auto kind = Cmd::DestinationKindOf(reply.destination);
    ASSERT_TRUE(kind.has_value());
    EXPECT_FALSE(Cmd::CheckStatusAgainstSpec(reply.Status(), *kind).Any());
}

TEST(SignalSourceCcmTests, ControlRequestCarriesTheControlDefault) {
    // Table C.2: CONTROL, source = tape subunit plug 0, destination = any oPCR (FF 7F).
    const auto command = Cmd::ConnectSignalSource(Cmd::SignalAddress::SubunitPlug(kTapeSubunit0, 0),
                                                  Cmd::SignalAddress::AnyAvailableSerialBusPlug());
    auto frame = command.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kControl);
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x0F, 0x20, 0x00, 0xFF, 0x7F}));
}

TEST(SignalSourceCcmTests, ControlReplyFromASourceDevice) {
    // Table C.2 ACCEPTED reply: result_status 0, destination resolved to oPCR[2].
    // Table 7.6 (normative) names value 0 "source"; the Annex C table labels the same value
    // "not source", which contradicts Table 7.6. The codec follows Table 7.6.
    const auto command = Cmd::ConnectSignalSource(Cmd::SignalAddress::SubunitPlug(kTapeSubunit0, 0),
                                                  Cmd::SignalAddress::AnyAvailableSerialBusPlug());
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x09, 0xFF, 0x1A, 0x00, 0x20, 0x00, 0xFF, 0x02});
    EXPECT_EQ(reply.Control().Result(), Cmd::SignalSourceResult::kSource);
    EXPECT_TRUE(reply.Control().ReservedIsZero());
    EXPECT_EQ(reply.destination, Cmd::SignalAddress::UnitIsochronousPlug(2));
}

TEST(SignalSourceCcmTests, ControlReplyResultStatuses) {
    const auto command = Cmd::ConnectSignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0),
                                                  Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 0));
    auto notSource = DecodeReply<Cmd::SignalSource>(command, {0x09, 0xFF, 0x1A, 0x01, 0xFF, 0x00, 0x60, 0x00});
    EXPECT_EQ(notSource.Control().Result(), Cmd::SignalSourceResult::kNotSource);
    auto rejected = DecodeReply<Cmd::SignalSource>(command, {0x0A, 0xFF, 0x1A, 0x0F, 0xFF, 0x00, 0x60, 0x00});
    EXPECT_EQ(rejected.Control().Result(), Cmd::SignalSourceResult::kNoInformation);
}

TEST(SignalSourceCcmTests, SpecificInquiryUsesTheControlFormat) {
    // Apple QuerySyncPlugReconnect (ctype 2) sends 02 FF 1A 0F <src> <dest>.
    const auto command = Cmd::CanConnectSignalSource(Cmd::SignalAddress::UnitIsochronousPlug(1),
                                                     Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 8));
    auto frame = command.Encode(CommandType::kSpecificInquiry);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kSpecificInquiry);
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x0F, 0xFF, 0x01, 0x60, 0x08}));
}

TEST(SignalSourceCcmTests, NotifyHasTheStatusSyntax) {
    // §7.1.6.
    const auto command = Cmd::WatchSignalSource(Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 1));
    auto frame = command.Encode(CommandType::kNotify);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kNotify);
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0xFF, 0xFF, 0xFE, 0x60, 0x01}));
}

TEST(SignalSourceCcmTests, ControlAndInquiryNeedASource) {
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    auto control = command.Encode(CommandType::kControl);
    ASSERT_FALSE(control.has_value());
    EXPECT_EQ(control.error().kind, AvcErrorKind::kInvalidArgument);
    EXPECT_FALSE(command.Encode(CommandType::kSpecificInquiry).has_value());
    EXPECT_FALSE(command.Encode(CommandType::kGeneralInquiry).has_value());
}

TEST(SignalSourceCcmTests, SubunitAddressIsRefused) {
    Cmd::SignalSourceCommand command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    command.address = kMusicSubunit0;
    EXPECT_FALSE(command.Encode(CommandType::kStatus).has_value());
}

TEST(SignalSourceCcmTests, ShortReplyIsRefused) {
    auto reply = Cmd::SignalSourceOperands::Read(std::array<uint8_t, 4>{0x10, 0xFF, 0x00, 0x60});
    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, AvcErrorKind::kOperandsTooShort);
}

TEST(SignalSourceCcmTests, VirtualOutputReplyCarriesChannelAndInputPlug) {
    // Table C.6: output_status 4 (virtual output), conv 0, signal_status 0; signal_source C0 01
    // = channel 0 received on iPCR[1]; destination oPCR[0].
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x80, 0xC0, 0x01, 0xFF, 0x00});
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kVirtualOutput);
    const auto virtualOutput = reply.AsVirtualOutput();
    ASSERT_TRUE(virtualOutput.has_value());
    EXPECT_EQ(virtualOutput->isochronousChannel, 0);
    EXPECT_EQ(virtualOutput->inputPlug, Cmd::UnitPlugId::SerialBus(1));
}

TEST(SignalSourceCcmTests, VirtualOutputIsOnlyReadForThatStatus) {
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    const auto notVirtual = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x60, 0xC5, 0x01, 0xFF, 0x00});
    EXPECT_FALSE(notVirtual.AsVirtualOutput().has_value());
    const auto channel = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x80, 0xC5, 0x02, 0xFF, 0x00});
    ASSERT_TRUE(channel.AsVirtualOutput().has_value());
    EXPECT_EQ(channel.AsVirtualOutput()->isochronousChannel, 5);
}

TEST(SignalSourceCcmTests, VirtualOutputNeedsTheChannelPrefix) {
    // With output_status "virtual output" the first signal_source byte is "11" + a 6-bit channel
    // (Figure 7.11). A first byte without that prefix, such as a subunit address, is not a
    // virtual output source.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    const auto subunitPlug = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x80, 0x60, 0x01, 0xFF, 0x00});
    EXPECT_FALSE(subunitPlug.AsVirtualOutput().has_value());
    const auto audioSubunit = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x80, 0x08, 0x01, 0xFF, 0x00});
    EXPECT_FALSE(audioSubunit.AsVirtualOutput().has_value());
    // Channel 63 fills the six bits, so the byte is FF, the same value as the unit address.
    const auto lastChannel = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x80, 0xFF, 0x01, 0xFF, 0x00});
    ASSERT_TRUE(lastChannel.AsVirtualOutput().has_value());
    EXPECT_EQ(lastChannel.AsVirtualOutput()->isochronousChannel, 63);
}

TEST(SignalSourceCcmTests, NoSignalSourceReply) {
    // Phase 88 music dest 8: FF FE = Figure 7.14.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 8));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x30, 0xFF, 0xFE, 0x60, 0x08});
    EXPECT_EQ(reply.source.Kind(), Cmd::SignalAddressKind::kNoSignal);
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kNotEffective);
}

// ---------------------------------------------------------------------------
// Captured replies and the departures from the spec they show
// ---------------------------------------------------------------------------

TEST(SignalSourceCcmTests, Phase88CaptureStreamingIsoOutIsConformant) {
    // phase88_descriptors.json signal_source_0xFF_unit_iso_out_0: 0C FF 1A 10 60 00 FF 00.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x10, 0x60, 0x00, 0xFF, 0x00});
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kEffective);
    EXPECT_TRUE(reply.Status().CanChangeFormat());
    EXPECT_EQ(reply.source, Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 0));
    const auto kind = Cmd::DestinationKindOf(reply.destination);
    ASSERT_TRUE(kind.has_value());
    EXPECT_FALSE(Cmd::CheckStatusAgainstSpec(reply.Status(), *kind).Any());
}

TEST(SignalSourceCcmTests, Phase88CaptureIdleIsoOutIsReady) {
    // signal_source_0xFF_unit_iso_out_1: 0C FF 1A 70 60 05 FF 01.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(1));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x70, 0x60, 0x05, 0xFF, 0x01});
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kReady);
    const auto kind = Cmd::DestinationKindOf(reply.destination);
    ASSERT_TRUE(kind.has_value());
    EXPECT_FALSE(Cmd::CheckStatusAgainstSpec(reply.Status(), *kind).Any());
}

TEST(SignalSourceCcmTests, Phase88CaptureReadyOnExternalOutputDeviates) {
    // signal_source_0xFF_unit_ext_out_0: 0C FF 1A 70 08 06 FF 80. Table 7.8 allows only
    // effective / not effective on an external output plug, and conv must be zero there.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitExternalPlug(0));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x70, 0x08, 0x06, 0xFF, 0x80});
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kReady);  // still parsed and named
    const auto kind = Cmd::DestinationKindOf(reply.destination);
    ASSERT_EQ(kind, Cmd::DestinationPlugKind::kExternalOutputPlug);
    const auto deviations = Cmd::CheckStatusAgainstSpec(reply.Status(), *kind);
    EXPECT_TRUE(deviations.outputStatusNotAllowedForPlug);
    EXPECT_TRUE(deviations.convSetOnNonSerialBusPlug);
    EXPECT_FALSE(deviations.reservedOutputStatus);
}

TEST(SignalSourceCcmTests, Phase88CaptureSubunitDestinationDeviates) {
    // signal_source_0xFF_music_dest_1: 0C FF 1A 70 08 00 60 01.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 1));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x70, 0x08, 0x00, 0x60, 0x01});
    const auto kind = Cmd::DestinationKindOf(reply.destination);
    ASSERT_EQ(kind, Cmd::DestinationPlugKind::kSubunitDestinationPlug);
    EXPECT_TRUE(Cmd::CheckStatusAgainstSpec(reply.Status(), *kind).outputStatusNotAllowedForPlug);
}

TEST(SignalSourceCcmTests, Phase88CaptureEffectiveSubunitDestinationOnlyHasConv) {
    // signal_source_0xFF_music_dest_0: 0C FF 1A 10 FF 00 60 00: effective is allowed, conv is not.
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 0));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0x10, 0xFF, 0x00, 0x60, 0x00});
    EXPECT_EQ(reply.Status().Status(), Cmd::OutputStatus::kEffective);
    const auto deviations = Cmd::CheckStatusAgainstSpec(reply.Status(), Cmd::DestinationPlugKind::kSubunitDestinationPlug);
    EXPECT_FALSE(deviations.outputStatusNotAllowedForPlug);
    EXPECT_TRUE(deviations.convSetOnNonSerialBusPlug);
}

TEST(SignalSourceCcmTests, ReservedOutputStatusIsReportedNotRejected) {
    const auto command = Cmd::QuerySignalSource(Cmd::SignalAddress::UnitIsochronousPlug(0));
    const auto reply = DecodeReply<Cmd::SignalSource>(command, {0x0C, 0xFF, 0x1A, 0xA0, 0x60, 0x00, 0xFF, 0x00});
    EXPECT_FALSE(reply.Status().Status().has_value());
    EXPECT_TRUE(Cmd::CheckStatusAgainstSpec(reply.Status(), Cmd::DestinationPlugKind::kSerialBusOutputPlug)
                    .reservedOutputStatus);
}

// ===========================================================================
// INPUT SELECT
// ===========================================================================

TEST(InputSelectCcmTests, StatusRequestFillsUnusedFieldsWithOnes) {
    const auto command = Cmd::QueryInputPlug(Cmd::UnitPlugId::SerialBus(1));
    auto frame = command.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kInputSelect);
    EXPECT_EQ(Operands(*frame),
              (std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0xFF, 0xFE, 0x00}));
}

TEST(InputSelectCcmTests, ControlRequestsCarryEachSubfunction) {
    const auto node = Cmd::BusNodeId::FromRaw(0xFFC2);
    const auto outPlug = Cmd::UnitPlugId::SerialBus(2);
    struct Case {
        Cmd::InputSelectCommand command;
        uint8_t subfunction;  // Table 7.14
        uint8_t inputPlug;
    };
    const std::array<Case, 3> cases{{
        {Cmd::ConnectInput(node, outPlug), 0x00, 0xFF},
        {Cmd::ChangeInputPath(node, outPlug), 0x01, 0xFF},
        {Cmd::SelectInput(node, outPlug), 0x02, 0xFF},
    }};
    for (const auto& c : cases) {
        auto frame = c.command.Encode(CommandType::kControl);
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(Operands(*frame),
                  (std::vector<uint8_t>{c.subfunction, 0x0F, 0xFF, 0xC2, 0x02, c.inputPlug, 0xFF, 0xFE, 0x00}));
    }
}

TEST(InputSelectCcmTests, DisconnectNamesTheInputPlug) {
    // §7.2.1 4): the one subfunction with a specific input_plug; no destination.
    const auto command = Cmd::DisconnectInput(Cmd::BusNodeId::FromRaw(0xFFC1), Cmd::UnitPlugId::SerialBus(0),
                                              Cmd::UnitPlugId::SerialBus(1));
    auto frame = command.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x03, 0x0F, 0xFF, 0xC1, 0x00, 0x01, 0xFF, 0xFE, 0x00}));
}

TEST(InputSelectCcmTests, SignalDestinationIsCarried) {
    const auto command = Cmd::ConnectInput(Cmd::BusNodeId::FromRaw(0xFFC0), Cmd::UnitPlugId::External(1),
                                           Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 4));
    auto frame = command.Encode(CommandType::kSpecificInquiry);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kSpecificInquiry);
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x00, 0x0F, 0xFF, 0xC0, 0x81, 0xFF, 0x60, 0x04, 0x00}));
}

TEST(InputSelectCcmTests, NotifyIsNotDefined) {
    const auto command = Cmd::QueryInputPlug(Cmd::UnitPlugId::SerialBus(0));
    auto frame = command.Encode(CommandType::kNotify);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error().kind, AvcErrorKind::kInvalidArgument);
}

TEST(InputSelectCcmTests, ControlReplyDecodesResultStatus) {
    const auto command = Cmd::SelectInput(Cmd::BusNodeId::FromRaw(0xFFC2), Cmd::UnitPlugId::SerialBus(2));
    // ACCEPTED, no error, target chose iPCR[1], destination = subunit 0x60 plug 0.
    auto accepted = DecodeReply<Cmd::InputSelect>(
        command, {0x09, 0xFF, 0x1B, 0x02, 0x00, 0xFF, 0xC2, 0x02, 0x01, 0x60, 0x00, 0x00});
    const auto reading = accepted.AsControl();
    EXPECT_EQ(reading.subfunction, Cmd::InputSelectSubfunction::kSelect);
    EXPECT_EQ(reading.result, Cmd::InputSelectResult::kNoError);
    EXPECT_EQ(accepted.node, Cmd::BusNodeId::FromRaw(0xFFC2));
    EXPECT_EQ(accepted.outputPlug, Cmd::UnitPlugId::SerialBus(2));
    EXPECT_EQ(accepted.inputPlug, Cmd::UnitPlugId::SerialBus(1));
    EXPECT_EQ(accepted.destination, Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 0));

    // REJECTED, p-to-p (not owner) (Table 7.18).
    auto rejected = DecodeReply<Cmd::InputSelect>(
        command, {0x0A, 0xFF, 0x1B, 0x02, 0x03, 0xFF, 0xC2, 0x02, 0xFF, 0xFF, 0xFE, 0x00});
    EXPECT_EQ(rejected.AsControl().result, Cmd::InputSelectResult::kPointToPointNotOwner);

    // Reserved result_status 9 is kept, not named.
    auto reserved = DecodeReply<Cmd::InputSelect>(
        command, {0x0A, 0xFF, 0x1B, 0x02, 0x09, 0xFF, 0xC2, 0x02, 0xFF, 0xFF, 0xFE, 0x00});
    EXPECT_FALSE(reserved.AsControl().result.has_value());
    EXPECT_EQ(reserved.AsControl().resultCode, 9);
}

TEST(InputSelectCcmTests, StatusReplyDecodesPlugStatus) {
    const auto command = Cmd::QueryInputPlug(Cmd::UnitPlugId::SerialBus(0));
    // status 2 (no selection): node FFFF, output_plug FE, destination FF FE (Figure 7.27).
    auto idle = DecodeReply<Cmd::InputSelect>(
        command, {0x0C, 0xFF, 0x1B, 0xFF, 0x2F, 0xFF, 0xFF, 0xFE, 0x00, 0xFF, 0xFE, 0x00});
    EXPECT_EQ(idle.AsStatus().status, Cmd::InputPlugStatus::kNoSelection);
    EXPECT_TRUE(idle.node.IsUnspecified());
    EXPECT_EQ(idle.outputPlug, Cmd::UnitPlugId::Invalid());
    EXPECT_EQ(idle.destination.Kind(), Cmd::SignalAddressKind::kNoSignal);

    // status 0 (active) from node FFC1 oPCR[3].
    auto active = DecodeReply<Cmd::InputSelect>(
        command, {0x0C, 0xFF, 0x1B, 0xFF, 0x0F, 0xFF, 0xC1, 0x03, 0x00, 0xFF, 0xFD, 0x00});
    EXPECT_EQ(active.AsStatus().status, Cmd::InputPlugStatus::kActive);
    EXPECT_EQ(active.node, Cmd::BusNodeId::FromRaw(0xFFC1));
    EXPECT_EQ(active.destination.Kind(), Cmd::SignalAddressKind::kMultiple);  // Figure 7.26

    // status 7 is reserved.
    auto reserved = DecodeReply<Cmd::InputSelect>(
        command, {0x0C, 0xFF, 0x1B, 0xFF, 0x7F, 0xFF, 0xC1, 0x03, 0x00, 0xFF, 0xFE, 0x00});
    EXPECT_FALSE(reserved.AsStatus().status.has_value());
    EXPECT_EQ(reserved.AsStatus().statusCode, 7);
}

TEST(InputSelectCcmTests, ShortReplyIsRefused) {
    auto reply = Cmd::InputSelectOperands::Read(std::array<uint8_t, 8>{});
    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, AvcErrorKind::kOperandsTooShort);
}

// ===========================================================================
// OUTPUT PRESET
// ===========================================================================

TEST(OutputPresetCcmTests, StatusRequestsFillUnusedFieldsWithOnes) {
    auto count = Cmd::QueryPresetCount().Encode(CommandType::kStatus);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count->OpcodeValue(), Opcode::kOutputPreset);
    EXPECT_EQ(Operands(*count), (std::vector<uint8_t>{0x7F, 0xFF, 0xFF, 0xFF, 0xFF}));  // 7F = how many entries
    auto entry = Cmd::QueryPreset(3).Encode(CommandType::kStatus);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(Operands(*entry), (std::vector<uint8_t>{0x03, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(OutputPresetCcmTests, AddPresetAsksForANewEntry) {
    const auto command = Cmd::AddPreset(Cmd::BusNodeId::FromRaw(0xFFC3),
                                        Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 2));
    auto frame = command.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x7F, 0xFF, 0xC3, 0x60, 0x02}));  // Figure 7.28

    auto unspecified = Cmd::AddPreset(Cmd::BusNodeId::FromRaw(0xFFC3)).Encode(CommandType::kControl);
    ASSERT_TRUE(unspecified.has_value());
    EXPECT_EQ(Operands(*unspecified), (std::vector<uint8_t>{0x7F, 0xFF, 0xC3, 0xFF, 0xFE}));  // Figure 7.29
}

TEST(OutputPresetCcmTests, CancelClearsNodeAndDestination) {
    auto frame = Cmd::CancelPreset(2).Encode(CommandType::kControl);  // §7.3.1: all ones
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x02, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(OutputPresetCcmTests, EntryNumberKeepsBit7Clear) {
    auto frame = Cmd::QueryPreset(0xFF).Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(Operands(*frame)[0], 0x7F);
}

TEST(OutputPresetCcmTests, NotifyIsNotDefined) {
    EXPECT_FALSE(Cmd::QueryPresetCount().Encode(CommandType::kNotify).has_value());
}

TEST(OutputPresetCcmTests, StatusReplyDecodesSelfAndOccupancy) {
    const auto command = Cmd::QueryPreset(2);
    // Entry 2, set locally (self bit), node FFC3, destination subunit 0x60 plug 2.
    auto occupied = DecodeReply<Cmd::OutputPreset>(command, {0x0C, 0xFF, 0x1C, 0x82, 0xFF, 0xC3, 0x60, 0x02});
    EXPECT_EQ(occupied.EntryNumber(), 2);
    EXPECT_TRUE(occupied.IsSelf());
    EXPECT_FALSE(occupied.IsUnoccupied());
    EXPECT_EQ(occupied.destinationNode, Cmd::BusNodeId::FromRaw(0xFFC3));
    EXPECT_EQ(occupied.destination, Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 2));

    // §7.3.5: an unoccupied entry leaves node and destination at FF FF.
    auto empty = DecodeReply<Cmd::OutputPreset>(command, {0x0C, 0xFF, 0x1C, 0x03, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_FALSE(empty.IsSelf());
    EXPECT_TRUE(empty.IsUnoccupied());
}

TEST(OutputPresetCcmTests, CountReplyCarriesTheEntryNumber) {
    auto reply = DecodeReply<Cmd::OutputPreset>(Cmd::QueryPresetCount(),
                                                {0x0C, 0xFF, 0x1C, 0x04, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(reply.EntryNumber(), 4);  // highest entry plus one (§7.3.5)
}

TEST(OutputPresetCcmTests, ShortReplyIsRefused) {
    auto reply = Cmd::OutputPresetOperands::Read(std::array<uint8_t, 4>{});
    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, AvcErrorKind::kOperandsTooShort);
}

// ===========================================================================
// CCM PROFILE
// ===========================================================================

TEST(CcmProfileTests, StatusRequestSelectsTheSourceSubfunction) {
    auto frame = Cmd::QueryCcmProfile().Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kCcmProfile);
    EXPECT_EQ(frame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(Operands(*frame), (std::vector<uint8_t>{0x00, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(CcmProfileTests, OnlyStatusIsDefined) {
    const auto command = Cmd::QueryCcmProfile();
    EXPECT_FALSE(command.Encode(CommandType::kControl).has_value());
    EXPECT_FALSE(command.Encode(CommandType::kNotify).has_value());
    EXPECT_FALSE(command.Encode(CommandType::kSpecificInquiry).has_value());
}

TEST(CcmProfileTests, ReplyDecodesProfileBits) {
    const auto command = Cmd::QueryCcmProfile();
    auto both = DecodeReply<Cmd::CcmProfile>(command, {0x0C, 0xFF, 0x1D, 0x00, 0x03, 0x00, 0x00, 0x00});
    EXPECT_TRUE(both.IsSourceReply());
    EXPECT_TRUE(both.ConformsToCcm10());
    EXPECT_TRUE(both.ConformsToDigitalAnalogChangeover());
    auto onlyCcm10 = DecodeReply<Cmd::CcmProfile>(command, {0x0C, 0xFF, 0x1D, 0x00, 0x01, 0x00, 0x00, 0x00});
    EXPECT_TRUE(onlyCcm10.ConformsToCcm10());
    EXPECT_FALSE(onlyCcm10.ConformsToDigitalAnalogChangeover());
    auto onlyDa = DecodeReply<Cmd::CcmProfile>(command, {0x0C, 0xFF, 0x1D, 0x00, 0x02, 0x00, 0x00, 0x00});
    EXPECT_FALSE(onlyDa.ConformsToCcm10());
    EXPECT_TRUE(onlyDa.ConformsToDigitalAnalogChangeover());
}

TEST(CcmProfileTests, ShortReplyIsRefused) {
    auto reply = Cmd::CcmProfileOperands::Read(std::array<uint8_t, 4>{});
    ASSERT_FALSE(reply.has_value());
    EXPECT_EQ(reply.error().kind, AvcErrorKind::kOperandsTooShort);
}

} // namespace ASFW::AVC::Test
