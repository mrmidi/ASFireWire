// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcCodecTests.cpp - Unit tests for the rebuilt AV/C frame codec (ASFW::AVC).
//
// Tests conform to docs/avc-rebuild/phase-1.md Steps 1.2 and 1.3:
// - CommandFrame::Make (headers, boundary checks, extended address rejection, quadlet padding)
// - ParseResponse (bounds, non-response detection, response code mapping)
// - ParseResponseFor (address/opcode echo checks)
// - Real hardware captures from TerraTec Phase 88 (2026-09-27)
// - General Commands: UNIT INFO, SUBUNIT INFO, PLUG INFO, Plug Signal Format, Vendor Dependent
// - Stream Format Commands: 0x2F / 0xBF, single / list, Compound AM824 encode/decode
// - Signal Source Commands: STATUS wildcard, CONTROL, Prism panel variant
// - Function Block Commands: Selector, Feature Mute / Volume (with differential tests)
// - BridgeCo Extensions: Extended PLUG INFO, Plug Type, Channel Count, Positions, Cluster Type

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/SignalSourceCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcError.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Core/RateCodes.hpp"
#include "ASFWDriver/Protocols/AVC/Extensions/BridgeCoPlugInfo.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::AVC::Test {

// ===========================================================================
// Step 1.2: CommandFrame::Make Tests
// ===========================================================================

TEST(AvcFrameTests, MakeSetsHeaderAndAccessors) {
    const uint8_t operands[] = {0x01, 0x02, 0x03};
    auto frame = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, operands);
    ASSERT_TRUE(frame.has_value());

    EXPECT_EQ(frame->Type(), CommandType::kStatus);
    EXPECT_EQ(frame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kUnitInfo);

    ASSERT_EQ(frame->Bytes().size(), 6u);
    EXPECT_EQ(frame->Bytes()[0], 0x01); // CTS=0, ctype=1 (Status)
    EXPECT_EQ(frame->Bytes()[1], 0xFF); // Unit address
    EXPECT_EQ(frame->Bytes()[2], 0x30); // Opcode::kUnitInfo
    EXPECT_EQ(frame->Bytes()[3], 0x01);
    EXPECT_EQ(frame->Bytes()[4], 0x02);
    EXPECT_EQ(frame->Bytes()[5], 0x03);

    ASSERT_EQ(frame->Operands().size(), 3u);
    EXPECT_EQ(frame->Operands()[0], 0x01);
    EXPECT_EQ(frame->Operands()[1], 0x02);
    EXPECT_EQ(frame->Operands()[2], 0x03);
}

TEST(AvcFrameTests, MakeRejectsOperandsExceedingCapacity) {
    std::vector<uint8_t> validOperands(kMaxOperandBytes, 0xAA);
    auto frameValid = CommandFrame::Make(CommandType::kControl, kAudioSubunit0, Opcode::kFunctionBlock, validOperands);
    ASSERT_TRUE(frameValid.has_value());
    EXPECT_EQ(frameValid->Bytes().size(), kMaxFrameBytes);

    std::vector<uint8_t> overflowOperands(kMaxOperandBytes + 1, 0xBB);
    auto frameOverflow = CommandFrame::Make(CommandType::kControl, kAudioSubunit0, Opcode::kFunctionBlock, overflowOperands);
    ASSERT_FALSE(frameOverflow.has_value());
    EXPECT_EQ(frameOverflow.error().kind, AvcErrorKind::kFrameTooLong);
}

TEST(AvcFrameTests, MakeRejectsExtendedSubunitAddress) {
    const SubunitAddress extendedAddr = SubunitAddress::Of(SubunitType::kExtended, 0);
    const uint8_t operands[] = {0x00};
    auto frame = CommandFrame::Make(CommandType::kStatus, extendedAddr, Opcode::kPlugInfo, operands);
    ASSERT_FALSE(frame.has_value());
    EXPECT_EQ(frame.error().kind, AvcErrorKind::kUnsupported);
}

TEST(AvcFrameTests, WireBytesPadsToQuadletBoundary) {
    auto f0 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, {});
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(f0->Bytes().size(), 3u);
    EXPECT_EQ(f0->WireBytes().size(), 4u);
    EXPECT_EQ(f0->WireBytes()[3], 0x00);

    const uint8_t op1[] = {0x10};
    auto f1 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, op1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->Bytes().size(), 4u);
    EXPECT_EQ(f1->WireBytes().size(), 4u);

    const uint8_t op5[] = {1, 2, 3, 4, 5};
    auto f5 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, op5);
    ASSERT_TRUE(f5.has_value());
    EXPECT_EQ(f5->Bytes().size(), 8u);
    EXPECT_EQ(f5->WireBytes().size(), 8u);

    const uint8_t op8[] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto f8 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, op8);
    ASSERT_TRUE(f8.has_value());
    EXPECT_EQ(f8->Bytes().size(), 11u);
    EXPECT_EQ(f8->WireBytes().size(), 12u);
    EXPECT_EQ(f8->WireBytes()[11], 0x00);
}

// ===========================================================================
// Step 1.2: ParseResponse Tests
// ===========================================================================

TEST(AvcFrameTests, ParseResponseRejectsShortOrLongFrames) {
    const uint8_t short1[] = {0x0C};
    EXPECT_EQ(ParseResponse(short1).error().kind, AvcErrorKind::kFrameTooShort);

    const uint8_t short2[] = {0x0C, 0xFF};
    EXPECT_EQ(ParseResponse(short2).error().kind, AvcErrorKind::kFrameTooShort);

    std::vector<uint8_t> tooLong(513, 0x0C);
    EXPECT_EQ(ParseResponse(tooLong).error().kind, AvcErrorKind::kFrameTooLong);
}

TEST(AvcFrameTests, ParseResponseRejectsNonResponseFrames) {
    const uint8_t cmdFrame[] = {0x01, 0xFF, 0x30}; // Status
    EXPECT_EQ(ParseResponse(cmdFrame).error().kind, AvcErrorKind::kNotAResponse);

    const uint8_t nonzeroCts[] = {0x1C, 0xFF, 0x30}; // CTS=1, code=0xC
    EXPECT_EQ(ParseResponse(nonzeroCts).error().kind, AvcErrorKind::kNotAResponse);

    const uint8_t reservedCode[] = {0x0E, 0xFF, 0x30};
    EXPECT_EQ(ParseResponse(reservedCode).error().kind, AvcErrorKind::kNotAResponse);
}

TEST(AvcFrameTests, ParseResponseMapsAllValidResponseCodes) {
    struct Case {
        uint8_t codeNibble;
        ResponseCode expectedCode;
    };
    const Case cases[] = {
        {0x8, ResponseCode::kNotImplemented},
        {0x9, ResponseCode::kAccepted},
        {0xA, ResponseCode::kRejected},
        {0xB, ResponseCode::kInTransition},
        {0xC, ResponseCode::kImplementedStable},
        {0xD, ResponseCode::kChanged},
        {0xF, ResponseCode::kInterim},
    };

    for (const auto& c : cases) {
        const uint8_t raw[] = {c.codeNibble, 0x60, 0x2F, 0xAA, 0xBB};
        auto resp = ParseResponse(raw);
        ASSERT_TRUE(resp.has_value()) << "Failed for code: 0x" << std::hex << static_cast<int>(c.codeNibble);
        EXPECT_EQ(resp->code, c.expectedCode);
        EXPECT_EQ(resp->address, kMusicSubunit0);
        EXPECT_EQ(resp->opcode, Opcode::kStreamFormatSupport);
        ASSERT_EQ(resp->operands.size(), 2u);
        EXPECT_EQ(resp->operands[0], 0xAA);
        EXPECT_EQ(resp->operands[1], 0xBB);
    }
}

TEST(AvcFrameTests, OperandsIfReturnsOperandsOrUnexpected) {
    const uint8_t stableRaw[] = {0x0C, 0xFF, 0x30, 0x11, 0x22};
    auto stableResp = ParseResponse(stableRaw);
    ASSERT_TRUE(stableResp.has_value());

    auto operandsOk = OperandsIf(*stableResp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(operandsOk.has_value());
    EXPECT_EQ(operandsOk->size(), 2u);

    auto operandsMismatch = OperandsIf(*stableResp, ResponseCode::kAccepted);
    ASSERT_FALSE(operandsMismatch.has_value());
    EXPECT_EQ(operandsMismatch.error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(operandsMismatch.error().response, ResponseCode::kImplementedStable);
}

// ===========================================================================
// Step 1.2: ParseResponseFor Tests
// ===========================================================================

TEST(AvcFrameTests, ParseResponseForValidatesAddressAndOpcodeMatch) {
    const uint8_t cmdOps[] = {0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    auto cmd = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, cmdOps);
    ASSERT_TRUE(cmd.has_value());

    const uint8_t matchRaw[] = {0x0C, 0xFF, 0x30, 0x07, 0x01, 0x00, 0x11, 0x98};
    auto match = ParseResponseFor(*cmd, matchRaw);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->code, ResponseCode::kImplementedStable);

    const uint8_t addrMismatchRaw[] = {0x0C, 0x08, 0x30, 0x07};
    auto addrErr = ParseResponseFor(*cmd, addrMismatchRaw);
    ASSERT_FALSE(addrErr.has_value());
    EXPECT_EQ(addrErr.error().kind, AvcErrorKind::kAddressMismatch);

    const uint8_t opcodeMismatchRaw[] = {0x0C, 0xFF, 0x31, 0x07};
    auto opErr = ParseResponseFor(*cmd, opcodeMismatchRaw);
    ASSERT_FALSE(opErr.has_value());
    EXPECT_EQ(opErr.error().kind, AvcErrorKind::kOpcodeMismatch);
}

// ===========================================================================
// Step 1.3: General Commands Tests
// ===========================================================================

TEST(GeneralCommandsTests, BuildUnitInfoStatusMatchesSpec) {
    auto cmd = Cmd::BuildUnitInfoStatus();
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->Type(), CommandType::kStatus);
    EXPECT_EQ(cmd->Address(), SubunitAddress::Unit());
    EXPECT_EQ(cmd->OpcodeValue(), Opcode::kUnitInfo);

    const auto bytes = cmd->Bytes();
    ASSERT_EQ(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x01); // STATUS
    EXPECT_EQ(bytes[1], 0xFF); // UNIT
    EXPECT_EQ(bytes[2], 0x30); // UNIT INFO
    EXPECT_EQ(bytes[3], 0x07); // first operand per ta1394 general.rs:37
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0xFF);
    EXPECT_EQ(bytes[6], 0xFF);
    EXPECT_EQ(bytes[7], 0xFF);

    // Differential note: Legacy AVCUnit::ProbeUnitInfo sent 0 operands (01 FF 30).
    // The spec (AV/C General 4.2 §10.1) and Linux ta1394 require [0x07, FF, FF, FF, FF].
}

TEST(GeneralCommandsTests, ParseUnitInfoSuccessAndErrors) {
    // Vector: Unit type 0x1F (unit), unit id 7, company ID 0x000AAC (TerraTec)
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x30, 0x07, 0xFF, 0x00, 0x0A, 0xAC};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto unitInfo = Cmd::ParseUnitInfo(*resp);
    ASSERT_TRUE(unitInfo.has_value());
    EXPECT_EQ(unitInfo->unitType, SubunitType::kUnit);
    EXPECT_EQ(unitInfo->unitId, 7);
    EXPECT_EQ(unitInfo->companyId, (CompanyId{0x00, 0x0A, 0xAC}));

    // Error: malformed first operand
    const uint8_t badFirst[] = {0x0C, 0xFF, 0x30, 0x00, 0xFF, 0x00, 0x0A, 0xAC};
    auto badResp = ParseResponse(badFirst);
    ASSERT_TRUE(badResp.has_value());
    auto err1 = Cmd::ParseUnitInfo(*badResp);
    ASSERT_FALSE(err1.has_value());
    EXPECT_EQ(err1.error().kind, AvcErrorKind::kMalformedOperands);

    // Error: short operands
    const uint8_t shortRespBytes[] = {0x0C, 0xFF, 0x30, 0x07, 0xFF};
    auto shortResp = ParseResponse(shortRespBytes);
    ASSERT_TRUE(shortResp.has_value());
    auto err2 = Cmd::ParseUnitInfo(*shortResp);
    ASSERT_FALSE(err2.has_value());
    EXPECT_EQ(err2.error().kind, AvcErrorKind::kOperandsTooShort);
}

TEST(GeneralCommandsTests, BuildSubunitInfoStatusMatchesLegacy) {
    auto cmd = Cmd::BuildSubunitInfoStatus(0, 7);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->Type(), CommandType::kStatus);
    EXPECT_EQ(cmd->Address(), SubunitAddress::Unit());
    EXPECT_EQ(cmd->OpcodeValue(), Opcode::kSubunitInfo);

    // Legacy AVCSubunitInfoCommand::BuildCdb(0) creates [0x07, 0xFF, 0xFF, 0xFF, 0xFF].
    const auto ops = cmd->Operands();
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], 0x07);
    EXPECT_EQ(ops[1], 0xFF);
    EXPECT_EQ(ops[2], 0xFF);
    EXPECT_EQ(ops[3], 0xFF);
    EXPECT_EQ(ops[4], 0xFF);
}

TEST(GeneralCommandsTests, ParseSubunitInfoVector) {
    // Vector from ta1394 general.rs:170:
    // Page 0, ext 7, entry 0 = Music subunit (0x0C << 3 = 0x60, max id 0), no other subunits.
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x31, 0x07, 0x60, 0xFF, 0xFF, 0xFF};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto info = Cmd::ParseSubunitInfo(*resp);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->page, 0);
    EXPECT_EQ(info->extensionCode, 7);
    ASSERT_EQ(info->entryCount, 1u);
    EXPECT_EQ(info->entries[0].type, SubunitType::kMusic);
    EXPECT_EQ(info->entries[0].maximumId, 0);
}

TEST(GeneralCommandsTests, PlugInfoUnitAndSubunit) {
    auto unitCmd = Cmd::BuildUnitPlugInfoStatus(Cmd::UnitPlugInfoKind::kIsochronousExternal);
    ASSERT_TRUE(unitCmd.has_value());
    EXPECT_EQ(unitCmd->Address(), SubunitAddress::Unit());
    EXPECT_EQ(unitCmd->Operands()[0], 0x00);

    auto subunitCmd = Cmd::BuildSubunitPlugInfoStatus(kMusicSubunit0);
    ASSERT_TRUE(subunitCmd.has_value());
    EXPECT_EQ(subunitCmd->Address(), kMusicSubunit0);
    EXPECT_EQ(subunitCmd->Operands()[0], 0x00);

    // Subunit plug info on Unit address must be rejected
    auto badSubunitCmd = Cmd::BuildSubunitPlugInfoStatus(SubunitAddress::Unit());
    ASSERT_FALSE(badSubunitCmd.has_value());
    EXPECT_EQ(badSubunitCmd.error().kind, AvcErrorKind::kInvalidArgument);

    // Parse Phase 88 capture: 2 iso in, 2 iso out, 8 ext in, 7 ext out
    const uint8_t p88Resp[] = {0x0C, 0xFF, 0x02, 0x00, 0x02, 0x02, 0x08, 0x07};
    auto r = ParseResponse(p88Resp);
    ASSERT_TRUE(r.has_value());
    auto plugs = Cmd::ParseUnitIsochronousExternalPlugs(*r);
    ASSERT_TRUE(plugs.has_value());
    EXPECT_EQ(plugs->isochronousInputs, 2);
    EXPECT_EQ(plugs->isochronousOutputs, 2);
    EXPECT_EQ(plugs->externalInputs, 8);
    EXPECT_EQ(plugs->externalOutputs, 7);
}

TEST(GeneralCommandsTests, PlugSignalFormatBuildAndParse) {
    auto statusCmd = Cmd::BuildPlugSignalFormatStatus(Cmd::PlugSignalDirection::kInput, 0, Cmd::SignalFormatQuery::kAm824Wildcard);
    ASSERT_TRUE(statusCmd.has_value());
    EXPECT_EQ(statusCmd->OpcodeValue(), Opcode::kInputPlugSignalFormat);
    EXPECT_EQ(statusCmd->Operands()[0], 0x00); // Plug 0
    EXPECT_EQ(statusCmd->Operands()[1], 0x90); // AM824
    EXPECT_EQ(statusCmd->Operands()[2], 0xFF);

    const Cmd::PlugSignalFormat fmt{
        .plugId = 0,
        .fmt = 0x90,
        .fdf = {0x40, 0x02, 0x00},
    };
    auto ctrlCmd = Cmd::BuildPlugSignalFormatControl(Cmd::PlugSignalDirection::kOutput, fmt);
    ASSERT_TRUE(ctrlCmd.has_value());
    EXPECT_EQ(ctrlCmd->OpcodeValue(), Opcode::kOutputPlugSignalFormat);
    EXPECT_EQ(ctrlCmd->Type(), CommandType::kControl);

    // Parse response
    const uint8_t respBytes[] = {0x09, 0xFF, 0x18, 0x00, 0x90, 0x40, 0x02, 0x00};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto parsedFmt = Cmd::ParsePlugSignalFormat(*resp, ResponseCode::kAccepted);
    ASSERT_TRUE(parsedFmt.has_value());
    EXPECT_EQ(parsedFmt->plugId, 0);
    EXPECT_EQ(parsedFmt->fmt, 0x90);
    EXPECT_EQ(parsedFmt->fdf[0], 0x40);
}

TEST(GeneralCommandsTests, VendorDependentBuildAndParse) {
    const CompanyId appleId = {0x00, 0x0A, 0x27};
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

    auto cmd = Cmd::BuildVendorDependent(CommandType::kControl, SubunitAddress::Unit(), appleId, payload);
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->OpcodeValue(), Opcode::kVendorDependent);
    ASSERT_EQ(cmd->Operands().size(), 7u);
    EXPECT_EQ(cmd->Operands()[0], 0x00);
    EXPECT_EQ(cmd->Operands()[1], 0x0A);
    EXPECT_EQ(cmd->Operands()[2], 0x27);
    EXPECT_EQ(cmd->Operands()[3], 0x01);

    // Parse response
    const uint8_t respBytes[] = {0x09, 0xFF, 0x00, 0x00, 0x0A, 0x27, 0x01, 0x02, 0x03, 0x04};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto parsed = Cmd::ParseVendorDependent(*resp, ResponseCode::kAccepted);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->companyId, appleId);
    ASSERT_EQ(parsed->payload.size(), 4u);
    EXPECT_EQ(parsed->payload[0], 0x01);
}

// ===========================================================================
// Step 1.3: Stream Format Command Tests
// ===========================================================================

TEST(StreamFormatTests, PlugAddressEncodeDecode) {
    // Unit PCR plug 0
    const auto unitPlug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0);
    const auto unitEncoded = unitPlug.Encode();
    EXPECT_EQ(unitEncoded, (std::array<uint8_t, 5>{0x00, 0x00, 0x00, 0x00, 0xFF}));
    auto unitDecoded = Cmd::PlugAddress::Decode(unitEncoded);
    ASSERT_TRUE(unitDecoded.has_value());
    EXPECT_EQ(*unitDecoded, unitPlug);

    // Subunit plug 2
    const auto subPlug = Cmd::PlugAddress::SubunitPlug(Cmd::PlugDirection::kOutput, 2);
    const auto subEncoded = subPlug.Encode();
    EXPECT_EQ(subEncoded, (std::array<uint8_t, 5>{0x01, 0x01, 0x02, 0xFF, 0xFF}));
    auto subDecoded = Cmd::PlugAddress::Decode(subEncoded);
    ASSERT_TRUE(subDecoded.has_value());
    EXPECT_EQ(*subDecoded, subPlug);

    // Function block plug
    const auto fbPlug = Cmd::PlugAddress::FunctionBlockPlug(Cmd::PlugDirection::kInput, 0x81, 0x03, 0x01);
    const auto fbEncoded = fbPlug.Encode();
    EXPECT_EQ(fbEncoded, (std::array<uint8_t, 5>{0x00, 0x02, 0x81, 0x03, 0x01}));
    auto fbDecoded = Cmd::PlugAddress::Decode(fbEncoded);
    ASSERT_TRUE(fbDecoded.has_value());
    EXPECT_EQ(*fbDecoded, fbPlug);

    // Decode error: malformed mode
    const uint8_t badMode[] = {0x00, 0x03, 0x00, 0x00, 0xFF};
    EXPECT_EQ(Cmd::PlugAddress::Decode(badMode).error().kind, AvcErrorKind::kMalformedOperands);
}

TEST(StreamFormatTests, CompoundAm824ParseAndEncodeRoundTrip) {
    // Phase 88 hardware capture format block:
    // 90 40 02 01 03 08 06 02 00 01 0D
    const uint8_t rawBlock[] = {
        0x90, 0x40, 0x02, 0x01, 0x03, 0x08, 0x06, 0x02, 0x00, 0x01, 0x0D
    };
    auto parsed = Cmd::ParseStreamFormatBlock(rawBlock);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, Cmd::StreamFormat::Kind::kCompoundAm824);
    EXPECT_EQ(parsed->compound.rate, StreamFormatRate::k32000);
    EXPECT_FALSE(parsed->compound.syncSource);
    EXPECT_EQ(parsed->compound.rateControl, Cmd::RateControl::kDontCare);
    ASSERT_EQ(parsed->compound.entryCount, 3u);

    EXPECT_EQ(parsed->compound.entries[0].count, 8);
    EXPECT_EQ(parsed->compound.entries[0].format, Cmd::Am824Format::kMultiBitLinearAudioRaw);
    EXPECT_EQ(parsed->compound.entries[1].count, 2);
    EXPECT_EQ(parsed->compound.entries[1].format, Cmd::Am824Format::kIec60958_3);
    EXPECT_EQ(parsed->compound.entries[2].count, 1);
    EXPECT_EQ(parsed->compound.entries[2].format, Cmd::Am824Format::kMidiConformant);

    EXPECT_EQ(parsed->compound.PcmChannels(), 10u);
    EXPECT_EQ(parsed->compound.MidiChannels(), 1u);
    EXPECT_TRUE(parsed->compound.OnlyPcmAndMidi());

    // Round-trip encode
    std::array<uint8_t, 32> encoded{};
    auto encSize = Cmd::EncodeCompoundAm824(parsed->compound, encoded);
    ASSERT_TRUE(encSize.has_value());
    ASSERT_EQ(*encSize, sizeof(rawBlock));
    EXPECT_TRUE(std::equal(rawBlock, rawBlock + sizeof(rawBlock), encoded.data()));
}

TEST(StreamFormatTests, BuildAndParseStreamFormatListAndSingle) {
    const auto plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0);

    // List query STATUS frame
    auto listCmd = Cmd::BuildStreamFormatListStatus(Cmd::StreamFormatOpcode::kStreamFormatSupport,
                                                    SubunitAddress::Unit(), plug, 0);
    ASSERT_TRUE(listCmd.has_value());
    const auto listBytes = listCmd->Bytes();
    // cmd 01 FF 2F C1 00 00 00 00 FF FF 00 (matches Phase 88 capture exactly)
    const uint8_t expectedCmd[] = {0x01, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    ASSERT_EQ(listBytes.size(), sizeof(expectedCmd));
    EXPECT_TRUE(std::equal(expectedCmd, expectedCmd + sizeof(expectedCmd), listBytes.data()));

    // Parse list response (Phase 88 capture)
    const uint8_t listResp[] = {
        0x0C, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00,
        0x90, 0x40, 0x02, 0x01, 0x03, 0x08, 0x06, 0x02, 0x00, 0x01, 0x0D
    };
    auto resp = ParseResponse(listResp);
    ASSERT_TRUE(resp.has_value());
    auto listEntry = Cmd::ParseStreamFormatList(*resp, 0);
    ASSERT_TRUE(listEntry.has_value());
    EXPECT_EQ(listEntry->index, 0);
    EXPECT_EQ(listEntry->status, Cmd::SupportStatus::kNotUsed);
    EXPECT_EQ(listEntry->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
    EXPECT_EQ(listEntry->format.compound.PcmChannels(), 10u);

    // Index mismatch error
    auto listEntryMismatch = Cmd::ParseStreamFormatList(*resp, 1);
    ASSERT_FALSE(listEntryMismatch.has_value());
    EXPECT_EQ(listEntryMismatch.error().kind, AvcErrorKind::kMalformedOperands);

    // Single query STATUS frame
    auto singleCmd = Cmd::BuildStreamFormatSingleStatus(Cmd::StreamFormatOpcode::kExtendedStreamFormat,
                                                        SubunitAddress::Unit(), plug);
    ASSERT_TRUE(singleCmd.has_value());
    EXPECT_EQ(singleCmd->Operands()[0], 0xC0);
    EXPECT_EQ(singleCmd->Operands()[6], 0xFF);
}

// ===========================================================================
// Step 1.3: Signal Source Command Tests
// ===========================================================================

TEST(SignalSourceTests, BuildStatusAndControl) {
    const auto dst = Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 0);
    auto statusCmd = Cmd::BuildSignalSourceStatus(dst);
    ASSERT_TRUE(statusCmd.has_value());
    EXPECT_EQ(statusCmd->Type(), CommandType::kStatus);
    EXPECT_EQ(statusCmd->Address(), SubunitAddress::Unit());
    EXPECT_EQ(statusCmd->OpcodeValue(), Opcode::kSignalSource);

    // Operands: [FF][FF FE][60 00]
    const auto ops = statusCmd->Operands();
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], 0xFF);
    EXPECT_EQ(ops[1], 0xFF);
    EXPECT_EQ(ops[2], 0xFE); // Wildcard per ta1394 ccm lib.rs:183
    EXPECT_EQ(ops[3], 0x60); // Music subunit 0
    EXPECT_EQ(ops[4], 0x00); // Plug 0

    // Control frame: connects isochronous unit plug 0 to destination
    const auto src = Cmd::SignalAddress::UnitIsochronousPlug(0);
    auto ctrlCmd = Cmd::BuildSignalSourceControl(src, dst);
    ASSERT_TRUE(ctrlCmd.has_value());
    EXPECT_EQ(ctrlCmd->Type(), CommandType::kControl);
    EXPECT_EQ(ctrlCmd->Operands()[1], 0xFF);
    EXPECT_EQ(ctrlCmd->Operands()[2], 0x00);
}

TEST(SignalSourceTests, ParseSignalSourceResponse) {
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x1A, 0xFF, 0xFF, 0x00, 0x60, 0x00};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto sig = Cmd::ParseSignalSource(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(sig.has_value());
    EXPECT_EQ(sig->firstByte, 0xFF);
    EXPECT_TRUE(sig->source.IsUnit());
    EXPECT_EQ(sig->source.PlugId(), 0);
    EXPECT_FALSE(sig->destination.IsUnit());
    EXPECT_EQ(sig->destination.PlugId(), 0);
    EXPECT_EQ(sig->destination.Subunit(), kMusicSubunit0);
}

// ===========================================================================
// Step 1.3: Function Block Command Tests
// ===========================================================================

TEST(FunctionBlockTests, SelectorBuildMatchesLegacyAndSpec) {
    // STATUS
    auto statusCmd = Cmd::BuildSelectorStatus(kAudioSubunit0, 1);
    ASSERT_TRUE(statusCmd.has_value());
    // Operands: [80][01][10][02][FF][01]
    const uint8_t expectedStatus[] = {0x80, 0x01, 0x10, 0x02, 0xFF, 0x01};
    ASSERT_EQ(statusCmd->Operands().size(), sizeof(expectedStatus));
    EXPECT_TRUE(std::equal(expectedStatus, expectedStatus + sizeof(expectedStatus), statusCmd->Operands().data()));

    // CONTROL
    auto ctrlCmd = Cmd::BuildSelectorControl(kAudioSubunit0, 1, 2);
    ASSERT_TRUE(ctrlCmd.has_value());
    const uint8_t expectedCtrl[] = {0x80, 0x01, 0x10, 0x02, 0x02, 0x01};
    ASSERT_EQ(ctrlCmd->Operands().size(), sizeof(expectedCtrl));
    EXPECT_TRUE(std::equal(expectedCtrl, expectedCtrl + sizeof(expectedCtrl), ctrlCmd->Operands().data()));

    // Parse response
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x80, 0x01, 0x10, 0x02, 0x02, 0x01};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto parsed = Cmd::ParseSelector(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->functionBlockId, 1);
    EXPECT_EQ(parsed->inputPlug, 2);
}

TEST(FunctionBlockTests, FeatureMuteBuildAndParse) {
    // Differential Note: Legacy BeBoBProtocol::SetFeatureMute sent selector length 4 and 0x00 for mute.
    // Per TA 1999008 §10.3 and §10.3.1, selector_length is ALWAYS 2, and mute on is 0x70, mute off is 0x60.
    auto muteOnCmd = Cmd::BuildFeatureMuteControl(kAudioSubunit0, 3, 0, true);
    ASSERT_TRUE(muteOnCmd.has_value());
    const uint8_t expectedMuteOn[] = {0x81, 0x03, 0x10, 0x02, 0x00, 0x01, 0x01, 0x70};
    ASSERT_EQ(muteOnCmd->Operands().size(), sizeof(expectedMuteOn));
    EXPECT_TRUE(std::equal(expectedMuteOn, expectedMuteOn + sizeof(expectedMuteOn), muteOnCmd->Operands().data()));

    auto muteOffCmd = Cmd::BuildFeatureMuteControl(kAudioSubunit0, 3, 0, false);
    ASSERT_TRUE(muteOffCmd.has_value());
    EXPECT_EQ(muteOffCmd->Operands()[7], 0x60);

    // Parse mute status response
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x81, 0x03, 0x10, 0x02, 0x00, 0x01, 0x01, 0x70};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto muted = Cmd::ParseFeatureMute(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(muted.has_value());
    EXPECT_TRUE(*muted);

    // Rejection of invalid mute boolean values (e.g. 0x00)
    const uint8_t invalidMuteResp[] = {0x0C, 0x08, 0xB8, 0x81, 0x03, 0x10, 0x02, 0x00, 0x01, 0x01, 0x00};
    auto rBad = ParseResponse(invalidMuteResp);
    ASSERT_TRUE(rBad.has_value());
    EXPECT_EQ(Cmd::ParseFeatureMute(*rBad, ResponseCode::kImplementedStable).error().kind, AvcErrorKind::kMalformedOperands);
}

TEST(FunctionBlockTests, FeatureVolumeBuildAndParse) {
    // Differential Note: Legacy sent selector length 5.
    // Per TA 1999008 §10.3, selector_length is ALWAYS 2.
    // Volume: 0 dB = 0x0000
    auto volCmd = Cmd::BuildFeatureVolumeControl(kAudioSubunit0, 2, 0, 0);
    ASSERT_TRUE(volCmd.has_value());
    const uint8_t expectedVol[] = {0x81, 0x02, 0x10, 0x02, 0x00, 0x02, 0x02, 0x00, 0x00};
    ASSERT_EQ(volCmd->Operands().size(), sizeof(expectedVol));
    EXPECT_TRUE(std::equal(expectedVol, expectedVol + sizeof(expectedVol), volCmd->Operands().data()));

    // Vector adapted from ta1394 audio lib.rs:1511:
    // ta1394 defines multi-channel volume [-1234, 5678, 3210]; here we adapt the
    // first channel int16 -1234 (0xFB2E) for single-channel master volume testing.
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x81, 0x03, 0x10, 0x02, 0x00, 0x02, 0x02, 0xFB, 0x2E};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto vol = Cmd::ParseFeatureVolume(*resp, ResponseCode::kImplementedStable);
    ASSERT_TRUE(vol.has_value());
    EXPECT_EQ(*vol, -1234);
}

// ===========================================================================
// Step 1.3: BridgeCo Extended PLUG INFO Tests
// ===========================================================================

TEST(BridgeCoPlugInfoTests, BuildStatusCommands) {
    const auto plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0);

    // Plug Type query: 7 operands, no extra
    auto typeCmd = BridgeCo::BuildExtendedPlugInfoStatus(SubunitAddress::Unit(), plug, BridgeCo::InfoType::kPlugType);
    ASSERT_TRUE(typeCmd.has_value());
    ASSERT_EQ(typeCmd->Operands().size(), 7u);
    EXPECT_EQ(typeCmd->Operands()[0], 0xC0);
    EXPECT_EQ(typeCmd->Operands()[6], 0x00);
    EXPECT_EQ(typeCmd->WireBytes().size(), 12u); // 3-byte header + 7 operands = 10, padded to 12

    // Cluster Info query: requires extra (1-based section id)
    auto badClusterCmd = BridgeCo::BuildExtendedPlugInfoStatus(SubunitAddress::Unit(), plug, BridgeCo::InfoType::kClusterInfo);
    ASSERT_FALSE(badClusterCmd.has_value());
    EXPECT_EQ(badClusterCmd.error().kind, AvcErrorKind::kInvalidArgument);

    auto clusterCmd = BridgeCo::BuildExtendedPlugInfoStatus(SubunitAddress::Unit(), plug, BridgeCo::InfoType::kClusterInfo, 1);
    ASSERT_TRUE(clusterCmd.has_value());
    ASSERT_EQ(clusterCmd->Operands().size(), 8u);
    EXPECT_EQ(clusterCmd->Operands()[6], 0x07);
    EXPECT_EQ(clusterCmd->Operands()[7], 0x01);
}

TEST(BridgeCoPlugInfoTests, ParsePlugTypeAndChannelCount) {
    // Plug type: IsochronousStream (0x00)
    const uint8_t typeResp[] = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00};
    auto r1 = ParseResponse(typeResp);
    ASSERT_TRUE(r1.has_value());
    auto plugType = BridgeCo::ParsePlugType(*r1);
    ASSERT_TRUE(plugType.has_value());
    EXPECT_EQ(*plugType, BridgeCo::PlugType::kIsochronousStream);

    // Channel count: 10 channels
    const uint8_t chResp[] = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x02, 0x0A};
    auto r2 = ParseResponse(chResp);
    ASSERT_TRUE(r2.has_value());
    auto chCount = BridgeCo::ParseChannelCount(*r2);
    ASSERT_TRUE(chCount.has_value());
    EXPECT_EQ(*chCount, 10);
}

TEST(BridgeCoPlugInfoTests, ParseClusterPortType) {
    // Cluster info response: echoed section id 1 at operand 7, port type Line (0x03) at operand 8
    const uint8_t clusterResp[] = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x01, 0x03};
    auto r = ParseResponse(clusterResp);
    ASSERT_TRUE(r.has_value());

    auto portType = BridgeCo::ParseClusterPortType(*r, 1);
    ASSERT_TRUE(portType.has_value());
    EXPECT_EQ(*portType, BridgeCo::PortType::kLine);

    // Section id mismatch
    auto badSection = BridgeCo::ParseClusterPortType(*r, 2);
    ASSERT_FALSE(badSection.has_value());
    EXPECT_EQ(badSection.error().kind, AvcErrorKind::kMalformedOperands);
}

TEST(BridgeCoPlugInfoTests, ParseChannelPositionsMap) {
    // Channel position data: 1 section, 2 channels:
    // ch 0: wire streamPos 1, secLoc 1 -> 0-based streamPos 0, secLoc 0
    // ch 1: wire streamPos 2, secLoc 2 -> 0-based streamPos 1, secLoc 1
    const uint8_t posResp[] = {
        0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03,
        0x01,       // 1 section
        0x02,       // 2 channels
        0x01, 0x01, // stream 1, loc 1
        0x02, 0x02  // stream 2, loc 2
    };
    auto r = ParseResponse(posResp);
    ASSERT_TRUE(r.has_value());

    auto map = BridgeCo::ParseChannelPositions(*r);
    ASSERT_TRUE(map.has_value());
    EXPECT_EQ(map->sectionCount, 1);
    ASSERT_EQ(map->sections[0].positionCount, 2);
    EXPECT_EQ(map->sections[0].positions[0].streamPosition, 0);
    EXPECT_EQ(map->sections[0].positions[0].sectionLocation, 0);
    EXPECT_EQ(map->sections[0].positions[1].streamPosition, 1);
    EXPECT_EQ(map->sections[0].positions[1].sectionLocation, 1);

    // Rejection of invalid 0 wire positions
    const uint8_t badPosResp[] = {
        0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x03,
        0x01, 0x01, 0x00, 0x01
    };
    auto rBad = ParseResponse(badPosResp);
    ASSERT_TRUE(rBad.has_value());
    EXPECT_EQ(BridgeCo::ParseChannelPositions(*rBad).error().kind, AvcErrorKind::kMalformedOperands);
}

} // namespace ASFW::AVC::Test
