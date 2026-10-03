// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CcmCodecTests.cpp - Tests for the AV/C CCM vocabulary and the SIGNAL SOURCE codec
// (TA 2002010, AV/C CCM Specification 1.1).
//
// Frames come from three places, named in each test:
// - the spec's own worked examples (Annex C Tables C.1, C.2, C.6);
// - the figures (every field position of every command);
// - STATUS replies captured from a TerraTec Phase 88 and an Apogee Duet
//   (documentation/avc-rebuild/fixtures/*_descriptors.json).
// Wire bytes appear here as the expected values, never as inputs: requests are built with
// the named factories.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/CcmTypes.hpp"
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

} // namespace ASFW::AVC::Test
