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

#include "ASFWDriver/Protocols/AVC/Commands/DescriptorCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/GeneralCommands.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/SignalSourceCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Commands/StreamFormatCommand.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcError.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Core/IAvcUnit.hpp"
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

TEST(AvcFrameTests, EngineResponsePolicy) {
    EXPECT_TRUE(IsResponseCodeAccepted(ResponseCode::kImplementedStable, CommandType::kStatus));
    EXPECT_FALSE(IsResponseCodeAccepted(ResponseCode::kRejected, CommandType::kStatus));
    EXPECT_TRUE(IsResponseCodeAccepted(ResponseCode::kAccepted, CommandType::kControl));
    EXPECT_FALSE(IsResponseCodeAccepted(ResponseCode::kImplementedStable, CommandType::kControl));
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

TEST(GeneralCommandsTests, BuildUnitInfoStatusEncodesBothAppleAndLinuxStyles) {
    // 1. Default form: Apple AppleFWAudio + legacy ASFW (0 operands, 3 header bytes padded to 4)
    Cmd::UnitInfoCommand defaultCmd{};
    auto defaultFrame = defaultCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(defaultFrame.has_value());
    EXPECT_EQ(defaultFrame->Type(), CommandType::kStatus);
    EXPECT_EQ(defaultFrame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(defaultFrame->OpcodeValue(), Opcode::kUnitInfo);
    EXPECT_EQ(defaultFrame->Bytes().size(), 3u);
    EXPECT_EQ(defaultFrame->WireBytes().size(), 4u);
    EXPECT_EQ(defaultFrame->WireBytes()[0], 0x01); // STATUS
    EXPECT_EQ(defaultFrame->WireBytes()[1], 0xFF); // UNIT
    EXPECT_EQ(defaultFrame->WireBytes()[2], 0x30); // UNIT INFO
    EXPECT_EQ(defaultFrame->WireBytes()[3], 0x00); // quadlet zero padding

    // 2. Linux form: ta1394 general.rs:37 (5 dummy operands [0x07, FF, FF, FF, FF])
    Cmd::UnitInfoCommand linuxCmd{
        .operands = Cmd::UnitInfoOperands{Cmd::UnitInfoStyle::kLinuxFiveDummyOperands}
    };
    auto linuxFrame = linuxCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(linuxFrame.has_value());
    EXPECT_EQ(linuxFrame->Bytes().size(), 8u);
    EXPECT_EQ(linuxFrame->WireBytes().size(), 8u);
    EXPECT_EQ(linuxFrame->WireBytes()[0], 0x01);
    EXPECT_EQ(linuxFrame->WireBytes()[1], 0xFF);
    EXPECT_EQ(linuxFrame->WireBytes()[2], 0x30);
    EXPECT_EQ(linuxFrame->WireBytes()[3], 0x07);
    EXPECT_EQ(linuxFrame->WireBytes()[4], 0xFF);
    EXPECT_EQ(linuxFrame->WireBytes()[5], 0xFF);
    EXPECT_EQ(linuxFrame->WireBytes()[6], 0xFF);
    EXPECT_EQ(linuxFrame->WireBytes()[7], 0xFF);
}

TEST(GeneralCommandsTests, ParseUnitInfoSuccessAndErrors) {
    // Vector: Unit type 0x1F (unit), unit id 7, company ID 0x000AAC (TerraTec)
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x30, 0x07, 0xFF, 0x00, 0x0A, 0xAC};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto unitInfo = Cmd::UnitInfoOperands::Read(resp->operands);
    ASSERT_TRUE(unitInfo.has_value());
    EXPECT_EQ(unitInfo->unitType, SubunitType::kUnit);
    EXPECT_EQ(unitInfo->unitId, 7);
    EXPECT_EQ(unitInfo->companyId, (CompanyId{0x00, 0x0A, 0xAC}));

    // Error: malformed first operand
    const uint8_t badFirst[] = {0x0C, 0xFF, 0x30, 0x00, 0xFF, 0x00, 0x0A, 0xAC};
    auto badResp = ParseResponse(badFirst);
    ASSERT_TRUE(badResp.has_value());
    auto err1 = Cmd::UnitInfoOperands::Read(badResp->operands);
    ASSERT_FALSE(err1.has_value());
    EXPECT_EQ(err1.error().kind, AvcErrorKind::kMalformedOperands);

    // Error: short operands
    const uint8_t shortRespBytes[] = {0x0C, 0xFF, 0x30, 0x07, 0xFF};
    auto shortResp = ParseResponse(shortRespBytes);
    ASSERT_TRUE(shortResp.has_value());
    auto err2 = Cmd::UnitInfoOperands::Read(shortResp->operands);
    ASSERT_FALSE(err2.has_value());
    EXPECT_EQ(err2.error().kind, AvcErrorKind::kOperandsTooShort);
}

TEST(GeneralCommandsTests, BuildSubunitInfoStatusMatchesLegacy) {
    Cmd::SubunitInfoCommand cmd{
        .operands = Cmd::SubunitInfoOperands{.page = 0, .extensionCode = 7}
    };
    auto frame = cmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Type(), CommandType::kStatus);
    EXPECT_EQ(frame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kSubunitInfo);

    // Legacy AVCSubunitInfoCommand::BuildCdb(0) creates [0x07, 0xFF, 0xFF, 0xFF, 0xFF].
    const auto ops = frame->Operands();
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

    auto info = Cmd::SubunitInfoOperands::Read(resp->operands);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->page, 0);
    EXPECT_EQ(info->extensionCode, 7);
    ASSERT_EQ(info->entryCount, 1u);
    EXPECT_EQ(info->entries[0].type, SubunitType::kMusic);
    EXPECT_EQ(info->entries[0].maximumId, 0);
}

TEST(GeneralCommandsTests, PlugInfoUnitAndSubunit) {
    Cmd::PlugInfoCommand unitCmd{};
    auto unitFrame = unitCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(unitFrame.has_value());
    EXPECT_EQ(unitFrame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(unitFrame->Operands()[0], 0x00);

    Cmd::PlugInfoCommand subunitCmd{
        .address = kMusicSubunit0,
        .operands = Cmd::PlugInfoOperands{.form = Cmd::PlugInfoForm::kSubunit},
    };
    auto subunitFrame = subunitCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(subunitFrame.has_value());
    EXPECT_EQ(subunitFrame->Address(), kMusicSubunit0);
    EXPECT_EQ(subunitFrame->Operands()[0], 0x00);

    // Subunit plug info on Unit address must be rejected
    Cmd::PlugInfoCommand badSubunitCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::PlugInfoOperands{.form = Cmd::PlugInfoForm::kSubunit},
    };
    auto badSubunitFrame = badSubunitCmd.Encode(CommandType::kStatus);
    ASSERT_FALSE(badSubunitFrame.has_value());
    EXPECT_EQ(badSubunitFrame.error().kind, AvcErrorKind::kInvalidArgument);

    // Parse Phase 88 capture: 2 iso in, 2 iso out, 8 ext in, 7 ext out
    const uint8_t p88Resp[] = {0x0C, 0xFF, 0x02, 0x00, 0x02, 0x02, 0x08, 0x07};
    auto r = ParseResponse(p88Resp);
    ASSERT_TRUE(r.has_value());
    auto plugs = Cmd::PlugInfoOperands{}.Read(r->operands);
    ASSERT_TRUE(plugs.has_value());
    EXPECT_EQ(plugs->unit.isochronousInputs, 2);
    EXPECT_EQ(plugs->unit.isochronousOutputs, 2);
    EXPECT_EQ(plugs->unit.externalInputs, 8);
    EXPECT_EQ(plugs->unit.externalOutputs, 7);
}

TEST(GeneralCommandsTests, PlugSignalFormatBuildAndParse) {
    Cmd::PlugSignalFormatCommand statusCmd{
        .operands = Cmd::PlugSignalFormatOperands{
            .direction = Cmd::PlugSignalDirection::kInput,
            .plugId = 0,
            .format = std::nullopt,
            .query = Cmd::SignalFormatQuery::kAm824Wildcard,
        }
    };
    auto statusFrame = statusCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(statusFrame.has_value());
    EXPECT_EQ(statusFrame->OpcodeValue(), Opcode::kInputPlugSignalFormat);
    EXPECT_EQ(statusFrame->Operands()[0], 0x00); // Plug 0
    EXPECT_EQ(statusFrame->Operands()[1], 0x90); // AM824
    EXPECT_EQ(statusFrame->Operands()[2], 0xFF);

    const Cmd::PlugSignalFormat fmt{
        .plugId = 0,
        .fmt = 0x90,
        .fdf = {0x40, 0x02, 0x00},
    };
    Cmd::PlugSignalFormatCommand ctrlCmd{
        .operands = Cmd::PlugSignalFormatOperands{
            .direction = Cmd::PlugSignalDirection::kOutput,
            .plugId = 0,
            .format = fmt,
        }
    };
    auto ctrlFrame = ctrlCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(ctrlFrame.has_value());
    EXPECT_EQ(ctrlFrame->OpcodeValue(), Opcode::kOutputPlugSignalFormat);
    EXPECT_EQ(ctrlFrame->Type(), CommandType::kControl);

    // Parse response
    const uint8_t respBytes[] = {0x09, 0xFF, 0x18, 0x00, 0x90, 0x40, 0x02, 0x00};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto parsedFmt = Cmd::PlugSignalFormatOperands::Read(resp->operands);
    ASSERT_TRUE(parsedFmt.has_value());
    EXPECT_EQ(parsedFmt->plugId, 0);
    EXPECT_EQ(parsedFmt->fmt, 0x90);
    EXPECT_EQ(parsedFmt->fdf[0], 0x40);
}

TEST(GeneralCommandsTests, VendorDependentBuildAndParse) {
    const CompanyId appleId = {0x00, 0x0A, 0x27};
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

    Cmd::RawVendorDependentCommand cmd{
        .operands = Cmd::RawVendorDependentOperands(appleId, payload)
    };
    auto frame = cmd.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->OpcodeValue(), Opcode::kVendorDependent);
    ASSERT_EQ(frame->Operands().size(), 7u);
    EXPECT_EQ(frame->Operands()[0], 0x00);
    EXPECT_EQ(frame->Operands()[1], 0x0A);
    EXPECT_EQ(frame->Operands()[2], 0x27);
    EXPECT_EQ(frame->Operands()[3], 0x01);

    // Parse response
    const uint8_t respBytes[] = {0x09, 0xFF, 0x00, 0x00, 0x0A, 0x27, 0x01, 0x02, 0x03, 0x04};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto parsed = Cmd::RawVendorDependentOperands::Read(resp->operands);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->size(), 4u);
    EXPECT_EQ((*parsed)[0], 0x01);
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
    auto parsed = Cmd::DecodeStreamFormatBlock(rawBlock);
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
    Cmd::StreamFormatCommand listCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::StreamFormatOperands{.form = Cmd::StreamFormatSubfunction::kList,
            .opcode = Cmd::StreamFormatOpcode::kStreamFormatSupport,
            .plug = plug,
            .index = 0,
        }
    };
    auto listFrame = listCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(listFrame.has_value());
    const auto listBytes = listFrame->Bytes();
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
    auto listEntry = Cmd::StreamFormatOperands{.form = Cmd::StreamFormatSubfunction::kList, .index = 0}.Read(resp->operands);
    ASSERT_TRUE(listEntry.has_value());
    EXPECT_EQ(listEntry->index, 0);
    EXPECT_EQ(listEntry->status, Cmd::SupportStatus::kNotUsed);
    EXPECT_EQ(listEntry->format.kind, Cmd::StreamFormat::Kind::kCompoundAm824);
    EXPECT_EQ(listEntry->format.compound.PcmChannels(), 10u);

    // Index mismatch error
    auto listEntryMismatch = Cmd::StreamFormatOperands{.form = Cmd::StreamFormatSubfunction::kList, .index = 1}.Read(resp->operands);
    ASSERT_FALSE(listEntryMismatch.has_value());
    EXPECT_EQ(listEntryMismatch.error().kind, AvcErrorKind::kMalformedOperands);

    // Single query STATUS frame
    Cmd::StreamFormatCommand singleCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::StreamFormatOperands{
            .opcode = Cmd::StreamFormatOpcode::kExtendedStreamFormat,
            .plug = plug,
        }
    };
    auto singleFrame = singleCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(singleFrame.has_value());
    EXPECT_EQ(singleFrame->Operands()[0], 0xC0);
    EXPECT_EQ(singleFrame->Operands()[6], 0xFF);
}

// ===========================================================================
// Step 1.3: Signal Source Command Tests
// ===========================================================================

TEST(SignalSourceTests, BuildStatusAndControl) {
    const auto dst = Cmd::SignalAddress::SubunitPlug(kMusicSubunit0, 0);
    Cmd::SignalSourceCommand statusCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::SignalSourceOperands{
            .destination = dst,
        }
    };
    auto statusFrame = statusCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(statusFrame.has_value());
    EXPECT_EQ(statusFrame->Type(), CommandType::kStatus);
    EXPECT_EQ(statusFrame->Address(), SubunitAddress::Unit());
    EXPECT_EQ(statusFrame->OpcodeValue(), Opcode::kSignalSource);

    // Operands: [FF][FF FE][60 00]
    const auto ops = statusFrame->Operands();
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], 0xFF);
    EXPECT_EQ(ops[1], 0xFF);
    EXPECT_EQ(ops[2], 0xFE); // Wildcard per ta1394 ccm lib.rs:183
    EXPECT_EQ(ops[3], 0x60); // Music subunit 0
    EXPECT_EQ(ops[4], 0x00); // Plug 0

    // Control frame: connects isochronous unit plug 0 to destination
    const auto src = Cmd::SignalAddress::UnitIsochronousPlug(0);
    Cmd::SignalSourceCommand ctrlCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::SignalSourceOperands{
            .destination = dst,
            .source = src,
        }
    };
    auto ctrlFrame = ctrlCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(ctrlFrame.has_value());
    EXPECT_EQ(ctrlFrame->Type(), CommandType::kControl);
    EXPECT_EQ(ctrlFrame->Operands()[1], 0xFF);
    EXPECT_EQ(ctrlFrame->Operands()[2], 0x00);
}

TEST(SignalSourceTests, ParseSignalSourceResponse) {
    const uint8_t respBytes[] = {0x0C, 0xFF, 0x1A, 0xFF, 0xFF, 0x00, 0x60, 0x00};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());

    auto sig = Cmd::SignalSourceOperands::Read(resp->operands);
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
    Cmd::SelectorCommand statusCmd{
        .address = kAudioSubunit0,
        .operands = Cmd::SelectorOperands{
            .functionBlockId = 1,
        }
    };
    auto statusFrame = statusCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(statusFrame.has_value());
    // Operands: [80][01][10][02][FF][01]
    const uint8_t expectedStatus[] = {0x80, 0x01, 0x10, 0x02, 0xFF, 0x01};
    ASSERT_EQ(statusFrame->Operands().size(), sizeof(expectedStatus));
    EXPECT_TRUE(std::equal(expectedStatus, expectedStatus + sizeof(expectedStatus), statusFrame->Operands().data()));

    // CONTROL
    Cmd::SelectorCommand ctrlCmd{
        .address = kAudioSubunit0,
        .operands = Cmd::SelectorOperands{
            .functionBlockId = 1,
            .inputPlug = 2,
        }
    };
    auto ctrlFrame = ctrlCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(ctrlFrame.has_value());
    const uint8_t expectedCtrl[] = {0x80, 0x01, 0x10, 0x02, 0x02, 0x01};
    ASSERT_EQ(ctrlFrame->Operands().size(), sizeof(expectedCtrl));
    EXPECT_TRUE(std::equal(expectedCtrl, expectedCtrl + sizeof(expectedCtrl), ctrlFrame->Operands().data()));

    // Parse response
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x80, 0x01, 0x10, 0x02, 0x02, 0x01};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto parsed = Cmd::SelectorOperands::Read(resp->operands);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->functionBlockId, 1);
    EXPECT_EQ(parsed->inputPlug, 2);
}

TEST(FunctionBlockTests, FeatureMuteBuildAndParse) {
    // Differential Note: Legacy BeBoBProtocol::SetFeatureMute sent selector length 4 and 0x00 for mute.
    // Per TA 1999008 §10.3 and §10.3.1, selector_length is ALWAYS 2, and mute on is 0x70, mute off is 0x60.
    Cmd::FeatureCommand muteOnCmd{
        .address = kAudioSubunit0,
        .operands = Cmd::FeatureOperands::Mute(3, 0, true)
    };
    auto muteOnFrame = muteOnCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(muteOnFrame.has_value());
    const uint8_t expectedMuteOn[] = {0x81, 0x03, 0x10, 0x02, 0x00, 0x01, 0x01, 0x70};
    ASSERT_EQ(muteOnFrame->Operands().size(), sizeof(expectedMuteOn));
    EXPECT_TRUE(std::equal(expectedMuteOn, expectedMuteOn + sizeof(expectedMuteOn), muteOnFrame->Operands().data()));

    Cmd::FeatureCommand muteOffCmd{
        .address = kAudioSubunit0,
        .operands = Cmd::FeatureOperands::Mute(3, 0, false)
    };
    auto muteOffFrame = muteOffCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(muteOffFrame.has_value());
    EXPECT_EQ(muteOffFrame->Operands()[7], 0x60);

    // Parse mute status response
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x81, 0x03, 0x10, 0x02, 0x00, 0x01, 0x01, 0x70};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto muted = Cmd::FeatureOperands::Read(resp->operands);
    ASSERT_TRUE(muted.has_value());
    EXPECT_TRUE(muted->AsMute());

    // Rejection of invalid mute boolean values (e.g. 0x00)
    const uint8_t invalidMuteResp[] = {0x0C, 0x08, 0xB8, 0x81, 0x03, 0x10, 0x02, 0x00, 0x01, 0x01, 0x00};
    auto rBad = ParseResponse(invalidMuteResp);
    ASSERT_TRUE(rBad.has_value());
    auto badMuted = Cmd::FeatureOperands::Read(rBad->operands);
    ASSERT_TRUE(badMuted.has_value());
    EXPECT_FALSE(badMuted->AsMute());
}

TEST(FunctionBlockTests, FeatureVolumeBuildAndParse) {
    // Differential Note: Legacy sent selector length 5.
    // Per TA 1999008 §10.3, selector_length is ALWAYS 2.
    // Volume: 0 dB = 0x0000
    Cmd::FeatureCommand volCmd{
        .address = kAudioSubunit0,
        .operands = Cmd::FeatureOperands::Volume(2, 0, AvcVolume::FromDb(0.0f))
    };
    auto volFrame = volCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(volFrame.has_value());
    const uint8_t expectedVol[] = {0x81, 0x02, 0x10, 0x02, 0x00, 0x02, 0x02, 0x00, 0x00};
    ASSERT_EQ(volFrame->Operands().size(), sizeof(expectedVol));
    EXPECT_TRUE(std::equal(expectedVol, expectedVol + sizeof(expectedVol), volFrame->Operands().data()));

    // Vector adapted from ta1394 audio lib.rs:1511:
    // ta1394 defines multi-channel volume [-1234, 5678, 3210]; here we adapt the
    // first channel int16 -1234 (0xFB2E) for single-channel master volume testing.
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x81, 0x03, 0x10, 0x02, 0x00, 0x02, 0x02, 0xFB, 0x2E};
    auto resp = ParseResponse(respBytes);
    ASSERT_TRUE(resp.has_value());
    auto vol = Cmd::FeatureOperands::Read(resp->operands);
    ASSERT_TRUE(vol.has_value());
    EXPECT_EQ(vol->AsVolume().Raw(), static_cast<int16_t>(0xFB2E));
}

// ===========================================================================
// Step 1.3: BridgeCo Extended PLUG INFO Tests
// ===========================================================================

TEST(BridgeCoPlugInfoTests, BuildStatusCommands) {
    const auto plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0);

    // Plug Type query: 7 operands, no extra
    BridgeCo::ExtendedPlugInfoCommand typeCmd{
        .address = SubunitAddress::Unit(),
        .operands = BridgeCo::ExtendedPlugInfoOperands{
            .plug = plug,
            .type = BridgeCo::InfoType::kPlugType,
        }
    };
    auto typeFrame = typeCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(typeFrame.has_value());
    ASSERT_EQ(typeFrame->Operands().size(), 7u);
    EXPECT_EQ(typeFrame->Operands()[0], 0xC0);
    EXPECT_EQ(typeFrame->Operands()[6], 0x00);
    EXPECT_EQ(typeFrame->WireBytes().size(), 12u); // 3-byte header + 7 operands = 10, padded to 12

    // Cluster Info query: requires extra (1-based section id)
    BridgeCo::ExtendedPlugInfoCommand badClusterCmd{
        .address = SubunitAddress::Unit(),
        .operands = BridgeCo::ExtendedPlugInfoOperands{
            .plug = plug,
            .type = BridgeCo::InfoType::kClusterInfo,
        }
    };
    auto badClusterFrame = badClusterCmd.Encode(CommandType::kStatus);
    ASSERT_FALSE(badClusterFrame.has_value());
    EXPECT_EQ(badClusterFrame.error().kind, AvcErrorKind::kInvalidArgument);

    BridgeCo::ExtendedPlugInfoCommand clusterCmd{
        .address = SubunitAddress::Unit(),
        .operands = BridgeCo::ExtendedPlugInfoOperands{
            .plug = plug,
            .type = BridgeCo::InfoType::kClusterInfo,
            .extra = 1,
        }
    };
    auto clusterFrame = clusterCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(clusterFrame.has_value());
    ASSERT_EQ(clusterFrame->Operands().size(), 8u);
    EXPECT_EQ(clusterFrame->Operands()[6], 0x07);
    EXPECT_EQ(clusterFrame->Operands()[7], 0x01);
}

TEST(BridgeCoPlugInfoTests, ParsePlugTypeAndChannelCount) {
    // Plug type: IsochronousStream (0x00)
    const uint8_t typeResp[] = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00};
    auto r1 = ParseResponse(typeResp);
    ASSERT_TRUE(r1.has_value());
    auto r1Reply = BridgeCo::ExtendedPlugInfoOperands::Read(r1->operands);
    ASSERT_TRUE(r1Reply.has_value());
    auto plugType = r1Reply->AsPlugType();
    ASSERT_TRUE(plugType.has_value());
    EXPECT_EQ(*plugType, BridgeCo::PlugType::kIsochronousStream);

    // Channel count: 10 channels
    const uint8_t chResp[] = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x02, 0x0A};
    auto r2 = ParseResponse(chResp);
    ASSERT_TRUE(r2.has_value());
    auto r2Reply = BridgeCo::ExtendedPlugInfoOperands::Read(r2->operands);
    ASSERT_TRUE(r2Reply.has_value());
    auto chCount = r2Reply->AsChannelCount();
    ASSERT_TRUE(chCount.has_value());
    EXPECT_EQ(*chCount, 10);
}

TEST(BridgeCoPlugInfoTests, ParseClusterPortType) {
    // Cluster info response: echoed section id 1 at operand 7, port type Line (0x03) at operand 8
    const uint8_t clusterResp[] = {0x0C, 0xFF, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x07, 0x01, 0x03};
    auto r = ParseResponse(clusterResp);
    ASSERT_TRUE(r.has_value());

    auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(r->operands);
    ASSERT_TRUE(reply.has_value());
    auto portType = reply->AsClusterPortType(1);
    ASSERT_TRUE(portType.has_value());
    EXPECT_EQ(*portType, BridgeCo::PortType::kLine);

    // Section id mismatch
    auto badSection = reply->AsClusterPortType(2);
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

    auto reply = BridgeCo::ExtendedPlugInfoOperands::Read(r->operands);
    ASSERT_TRUE(reply.has_value());
    auto map = reply->AsChannelPositions();
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
    auto badReply = BridgeCo::ExtendedPlugInfoOperands::Read(rBad->operands);
    ASSERT_TRUE(badReply.has_value());
    EXPECT_EQ(badReply->AsChannelPositions().error().kind, AvcErrorKind::kMalformedOperands);
}

// ===========================================================================
// Phase 2a: IAvcUnit Seam & Typed Command Dispatch Tests
// ===========================================================================

static_assert(AvcCommand<Cmd::UnitInfoCommand>);
static_assert(AvcCommand<Cmd::SubunitInfoCommand>);
static_assert(AvcCommand<Cmd::PlugInfoCommand>);
static_assert(AvcCommand<Cmd::PlugInfoCommand>);
static_assert(AvcCommand<Cmd::PlugSignalFormatCommand>);

class MockAvcUnit final : public IAvcUnit {
public:
    explicit MockAvcUnit(FW::NodeId node = FW::NodeId{2},
                         FW::Generation gen = FW::Generation{5},
                         uint64_t guid = 0x0003DB0001001234ULL)
        : nodeId_(node), generation_(gen), guid_(guid) {}

    void Submit(const CommandFrame& frame,
                FW::Generation generation,
                ResponseCallback completion) override {
        lastFrame_ = frame;
        lastGeneration_ = generation;
        pendingCompletion_ = std::move(completion);
    }

    [[nodiscard]] FW::NodeId NodeId() const noexcept override { return nodeId_; }
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override { return generation_; }
    [[nodiscard]] uint64_t Guid() const noexcept override { return guid_; }

    void Respond(Expected<Response> response) {
        if (pendingCompletion_) {
            auto cb = std::move(pendingCompletion_);
            cb(response);
        }
    }

    [[nodiscard]] const std::optional<CommandFrame>& LastFrame() const { return lastFrame_; }
    [[nodiscard]] const std::optional<FW::Generation>& LastGeneration() const { return lastGeneration_; }

private:
    FW::NodeId nodeId_;
    FW::Generation generation_;
    uint64_t guid_;
    std::optional<CommandFrame> lastFrame_;
    std::optional<FW::Generation> lastGeneration_;
    ResponseCallback pendingCompletion_;
};

TEST(AvcUnitSeamTests, IdentityAndDispatchSuccess) {
    MockAvcUnit unit(FW::NodeId{2}, FW::Generation{7}, 0x1122334455667788ULL);
    EXPECT_EQ(unit.NodeId().value, 2);
    EXPECT_EQ(unit.CurrentGeneration().value, 7);
    EXPECT_EQ(unit.Guid(), 0x1122334455667788ULL);
    EXPECT_EQ(unit.Identity().guid, 0x1122334455667788ULL);
    EXPECT_EQ(unit.Identity().nodeId.value, 2);
    EXPECT_EQ(unit.Identity().generation.value, 7);

    // Dispatch UnitInfoCommand via Send<Cmd>
    std::optional<Expected<Cmd::UnitInfo>> callbackResult;
    unit.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> result) {
        callbackResult = result;
    });

    ASSERT_TRUE(unit.LastFrame().has_value());
    EXPECT_EQ(unit.LastGeneration()->value, 7);
    // Verify default 0-operand Apple/legacy encoding: [01, FF, 30] (wire size 4)
    EXPECT_EQ(unit.LastFrame()->Bytes().size(), 3u);
    EXPECT_EQ(unit.LastFrame()->WireBytes().size(), 4u);

    // Mock unit receives FCP response from device
    const uint8_t rawResponse[] = {0x0C, 0xFF, 0x30, 0x07, 0x08, 0x00, 0x03, 0xDB};
    auto parsedResponse = ParseResponseFor(*unit.LastFrame(), rawResponse);
    ASSERT_TRUE(parsedResponse.has_value());

    unit.Respond(*parsedResponse);

    ASSERT_TRUE(callbackResult.has_value());
    ASSERT_TRUE(callbackResult->has_value());
    EXPECT_EQ((*callbackResult)->companyId[0], 0x00);
    EXPECT_EQ((*callbackResult)->companyId[1], 0x03);
    EXPECT_EQ((*callbackResult)->companyId[2], 0xDB);
    EXPECT_EQ((*callbackResult)->unitType, SubunitType::kAudio);
    EXPECT_EQ((*callbackResult)->unitId, 0x00);
}

TEST(AvcUnitSeamTests, DispatchPropagatesTransportFailure) {
    MockAvcUnit unit;
    std::optional<Expected<Cmd::UnitInfo>> callbackResult;

    unit.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> result) {
        callbackResult = result;
    });

    // Simulate transport timeout
    unit.Respond(std::unexpected(AvcError::Of(AvcErrorKind::kTimeout)));

    ASSERT_TRUE(callbackResult.has_value());
    ASSERT_FALSE(callbackResult->has_value());
    EXPECT_EQ(callbackResult->error().kind, AvcErrorKind::kTimeout);
}

TEST(AvcUnitSeamTests, DispatchPropagatesUnexpectedResponseCode) {
    MockAvcUnit unit;
    std::optional<Expected<Cmd::UnitInfo>> callbackResult;

    unit.Status(Cmd::UnitInfoCommand{}, [&](Expected<Cmd::UnitInfo> result) {
        callbackResult = result;
    });

    // Simulate REJECTED response
    const uint8_t rawResponse[] = {0x0A, 0xFF, 0x30, 0x07, 0x08, 0x00, 0x03, 0xDB};
    auto parsedResponse = ParseResponseFor(*unit.LastFrame(), rawResponse);
    ASSERT_TRUE(parsedResponse.has_value());

    unit.Respond(*parsedResponse);

    ASSERT_TRUE(callbackResult.has_value());
    ASSERT_FALSE(callbackResult->has_value());
    EXPECT_EQ(callbackResult->error().kind, AvcErrorKind::kUnexpectedResponse);
    ASSERT_TRUE(callbackResult->error().response.has_value());
    EXPECT_EQ(*callbackResult->error().response, ResponseCode::kRejected);
}

// ===========================================================================
// Phase 2b Reshaped API Tests
// ===========================================================================

TEST(AvcReshapedTests, FunctionBlock_SelectorCommand) {
    Cmd::SelectorCommand cmd{
        .address = kAudioSubunit0,
        .operands = Cmd::SelectorOperands{
            .functionBlockId = 0x01,
            .inputPlug = 0x03,
        }
    };

    // Status: plug byte is 0xFF
    auto statusFrame = cmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(statusFrame.has_value());
    EXPECT_EQ(statusFrame->Type(), CommandType::kStatus);
    ASSERT_EQ(statusFrame->Operands().size(), 6u);
    EXPECT_EQ(statusFrame->Operands()[0], 0x80); // Selector
    EXPECT_EQ(statusFrame->Operands()[1], 0x01); // FB ID
    EXPECT_EQ(statusFrame->Operands()[2], 0x10); // Current
    EXPECT_EQ(statusFrame->Operands()[3], 0x02); // Selector length
    EXPECT_EQ(statusFrame->Operands()[4], 0xFF); // Input plug (status wildcard)
    EXPECT_EQ(statusFrame->Operands()[5], 0x01); // Selector control

    // Control: plug byte is inputPlug
    auto controlFrame = cmd.Encode(CommandType::kControl);
    ASSERT_TRUE(controlFrame.has_value());
    EXPECT_EQ(controlFrame->Type(), CommandType::kControl);
    ASSERT_EQ(controlFrame->Operands().size(), 6u);
    EXPECT_EQ(controlFrame->Operands()[4], 0x03);

    // Decode operands only (Rule 3)
    const uint8_t replyOperands[] = {0x80, 0x01, 0x10, 0x02, 0x03, 0x01};
    auto decoded = Cmd::SelectorOperands::Read(replyOperands);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->functionBlockId, 0x01);
    EXPECT_EQ(decoded->inputPlug, 0x03);
}

TEST(AvcReshapedTests, FunctionBlock_FeatureCommandAndDataWidthTable) {
    // Rule 5: Per-control data width table
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kMute), 1);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kVolume), 2);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kLrBalance), 2);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kBass), 1);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kTreble), 1);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kAutomaticGain), 1);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kDelay), 2);
    EXPECT_EQ(Cmd::FeatureControlDataWidth(Cmd::FeatureControl::kGraphicEqualizer), std::nullopt);

    // Generic FeatureCommand STATUS automatically fills 0xFF based on table width
    Cmd::FeatureCommand statusCmd{
        .address = kAudioSubunit0,
        .operands = Cmd::FeatureOperands::VolumeStatus(0x02, Cmd::kMasterChannel),
    };
    auto frame = statusCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->Operands().size(), 9u);
    EXPECT_EQ(frame->Operands()[0], 0x81); // Feature
    EXPECT_EQ(frame->Operands()[1], 0x02); // FB ID
    EXPECT_EQ(frame->Operands()[3], 0x02); // Selector length
    EXPECT_EQ(frame->Operands()[5], 0x02); // Volume control
    EXPECT_EQ(frame->Operands()[6], 0x02); // Data length
    EXPECT_EQ(frame->Operands()[7], 0xFF); // Volume dummy byte 0
    EXPECT_EQ(frame->Operands()[8], 0xFF); // Volume dummy byte 1
}

TEST(AvcReshapedTests, FunctionBlock_AvcVolumeAndFeatureVolumeCommand) {
    // Rule 4: Typed values (AvcVolume in 1/256 dB)
    auto v0 = AvcVolume::FromDb(0.0f);
    EXPECT_EQ(v0.Raw(), 0x0000);
    EXPECT_FLOAT_EQ(v0.ToDb(), 0.0f);

    auto vMinus1 = AvcVolume::FromDb(-1.0f);
    EXPECT_EQ(vMinus1.Raw(), static_cast<int16_t>(0xFF00));
    EXPECT_FLOAT_EQ(vMinus1.ToDb(), -1.0f);

    auto vNegInf = AvcVolume::NegativeInfinity();
    EXPECT_TRUE(vNegInf.IsNegativeInfinity());
    EXPECT_EQ(vNegInf.Raw(), static_cast<int16_t>(0x8000));

    auto vInv = AvcVolume::Invalid();
    EXPECT_FALSE(vInv.IsValid());
    EXPECT_EQ(vInv.Raw(), 0x7FFF);

    Cmd::FeatureCommand cmd{
        .address = kAudioSubunit0,
        .operands = Cmd::FeatureOperands::Volume(0x03, 0x01, AvcVolume::FromDb(-6.0f)),
    };

    auto ctrl = cmd.Encode(CommandType::kControl);
    ASSERT_TRUE(ctrl.has_value());
    ASSERT_EQ(ctrl->Operands().size(), 9u);
    EXPECT_EQ(ctrl->Operands()[4], 0x01); // Channel 1
    EXPECT_EQ(ctrl->Operands()[5], 0x02); // Volume control
    EXPECT_EQ(ctrl->Operands()[6], 0x02); // Data length
    int16_t rawVol = static_cast<int16_t>((ctrl->Operands()[7] << 8) | ctrl->Operands()[8]);
    EXPECT_FLOAT_EQ(AvcVolume::FromRaw(rawVol).ToDb(), -6.0f);

    // Decode operands only
    const uint8_t volReply[] = {0x81, 0x03, 0x10, 0x02, 0x01, 0x02, 0x02, 0x00, 0x00};
    auto decVol = Cmd::FeatureOperands::Read(volReply);
    ASSERT_TRUE(decVol.has_value());
    EXPECT_FLOAT_EQ(decVol->AsVolume().ToDb(), 0.0f);
}

TEST(AvcReshapedTests, GeneralCommands_PlugSignalFormatCommand) {
    // Rule 1 & 2: Single type for command family, ctype at send time
    Cmd::PlugSignalFormatCommand statusCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::PlugSignalFormatOperands{
            .direction = Cmd::PlugSignalDirection::kInput,
            .plugId = 0x00,
            .format = std::nullopt,
            .query = Cmd::SignalFormatQuery::kAllWildcard,
        }
    };
    auto sFrame = statusCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(sFrame.has_value());
    EXPECT_EQ(sFrame->OpcodeValue(), Opcode::kInputPlugSignalFormat);
    EXPECT_EQ(sFrame->Operands()[0], 0x00);
    EXPECT_EQ(sFrame->Operands()[1], 0xFF);

    auto fmt = Cmd::Am824SignalFormat(0x01, CipSfc::k48000);
    Cmd::PlugSignalFormatCommand ctrlCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::PlugSignalFormatOperands{
            .direction = Cmd::PlugSignalDirection::kOutput,
            .plugId = 0x01,
            .format = fmt,
        }
    };
    auto cFrame = ctrlCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(cFrame.has_value());
    EXPECT_EQ(cFrame->OpcodeValue(), Opcode::kOutputPlugSignalFormat);
    EXPECT_EQ(cFrame->Operands()[0], 0x01);
    EXPECT_EQ(cFrame->Operands()[1], Cmd::kFmtAm824);

    // Decode operands only (Rule 3)
    const uint8_t reply[] = {0x01, Cmd::kFmtAm824, static_cast<uint8_t>(CipSfc::k48000), 0xFF, 0xFF};
    auto dec = Cmd::PlugSignalFormatOperands::Read(reply);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(dec->plugId, 0x01);
    EXPECT_EQ(dec->fmt, Cmd::kFmtAm824);
    EXPECT_EQ(Cmd::SfcOf(*dec), CipSfc::k48000);
}

TEST(AvcReshapedTests, SignalSource_SignalSourceCommand) {
    auto src = Cmd::SignalAddress::UnitIsochronousPlug(0x02);
    auto dst = Cmd::SignalAddress::SubunitPlug(kAudioSubunit0, 0x01);

    Cmd::SignalSourceCommand cmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::SignalSourceOperands{
            .destination = dst,
            .source = src,
        }
    };

    auto statusFrame = cmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(statusFrame.has_value());
    EXPECT_EQ(statusFrame->Operands()[1], 0xFF);
    EXPECT_EQ(statusFrame->Operands()[2], 0xFE); // Wildcard

    auto ctrlFrame = cmd.Encode(CommandType::kControl);
    ASSERT_TRUE(ctrlFrame.has_value());
    EXPECT_EQ(ctrlFrame->Operands()[1], 0xFF);
    EXPECT_EQ(ctrlFrame->Operands()[2], 0x02); // Source plug 2

    // Decode operands only
    const uint8_t reply[] = {0xFF, 0xFF, 0x02, dst.bytes[0], dst.bytes[1]};
    auto dec = Cmd::SignalSourceOperands::Read(reply);
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(dec->source.PlugId(), 0x02);
    EXPECT_EQ(dec->destination, dst);
}

TEST(AvcReshapedTests, StreamFormat_SingleAndListCommands) {
    auto plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kInput, Cmd::UnitPlugType::kPcr, 0);

    Cmd::StreamFormatCommand singleCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::StreamFormatOperands{
            .opcode = Cmd::StreamFormatOpcode::kStreamFormatSupport,
            .plug = plug,
        }
    };
    auto sFrame = singleCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(sFrame.has_value());
    EXPECT_EQ(sFrame->Operands()[0], static_cast<uint8_t>(Cmd::StreamFormatSubfunction::kSingle));

    Cmd::StreamFormatCommand listCmd{
        .address = SubunitAddress::Unit(),
        .operands = Cmd::StreamFormatOperands{.form = Cmd::StreamFormatSubfunction::kList,
            .opcode = Cmd::StreamFormatOpcode::kStreamFormatSupport,
            .plug = plug,
            .index = 2,
        }
    };
    auto lFrame = listCmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(lFrame.has_value());
    EXPECT_EQ(lFrame->Operands()[0], static_cast<uint8_t>(Cmd::StreamFormatSubfunction::kList));
    EXPECT_EQ(lFrame->Operands()[7], 2);
}

TEST(AvcReshapedTests, BridgeCo_ExtendedPlugInfoCommand) {
    auto plug = Cmd::PlugAddress::UnitPlug(Cmd::PlugDirection::kOutput, Cmd::UnitPlugType::kPcr, 0);
    BridgeCo::ExtendedPlugInfoCommand cmd{
        .address = kAudioSubunit0,
        .operands = BridgeCo::ExtendedPlugInfoOperands{
            .plug = plug,
            .type = BridgeCo::InfoType::kChannelCount,
        }
    };

    auto frame = cmd.Encode(CommandType::kStatus);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Operands()[0], BridgeCo::kExtendedPlugInfoSubfunction);
    EXPECT_EQ(frame->Operands()[6], static_cast<uint8_t>(BridgeCo::InfoType::kChannelCount));

    // Decode operands
    const uint8_t reply[] = {
        BridgeCo::kExtendedPlugInfoSubfunction,
        0x01, 0x00, 0x00, 0x00, 0xFF,
        static_cast<uint8_t>(BridgeCo::InfoType::kChannelCount),
        0x08, // 8 channels
    };
    auto dec = BridgeCo::ExtendedPlugInfoOperands::Read(reply);
    ASSERT_TRUE(dec.has_value());
    auto cc = dec->AsChannelCount();
    ASSERT_TRUE(cc.has_value());
    EXPECT_EQ(*cc, 0x08);
}

TEST(AvcReshapedTests, VendorDependentOwnsPayloadAndDecodesReply) {
    const uint8_t payload[] = {0x10, 0x20, 0x30};
    Cmd::RawVendorDependentCommand command{
        .operands = Cmd::RawVendorDependentOperands({0x00, 0x01, 0x02}, payload),
    };
    auto frame = command.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->Operands()[0], 0x00);
    EXPECT_EQ(frame->Operands()[1], 0x01);
    EXPECT_EQ(frame->Operands()[2], 0x02);
    EXPECT_EQ(frame->Operands()[3], 0x10);

    const uint8_t validReply[] = {0x00, 0x01, 0x02, 0x55, 0x66};
    auto parsed = command.Decode(validReply);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->size(), 2u);
    EXPECT_EQ((*parsed)[0], 0x55);

    // TASCAM replies with FF FF FF rather than its own company ID.
    const uint8_t tascamReply[] = {0xFF, 0xFF, 0xFF, 0x55, 0x66};
    auto tascamParsed = command.Decode(tascamReply);
    ASSERT_TRUE(tascamParsed.has_value());
    EXPECT_EQ((*tascamParsed)[0], 0x55);
}

TEST(AvcReshapedTests, IAvcUnit_StatusControlInquiryDispatch) {
    MockAvcUnit unit;

    // Dispatch via unit.Status()
    std::optional<Expected<Cmd::SelectorValue>> statusResult;
    unit.Status(Cmd::SelectorCommand{
                    .address = kAudioSubunit0,
                    .operands = Cmd::SelectorOperands{.functionBlockId = 1, .inputPlug = 2}
                },
                [&](Expected<Cmd::SelectorValue> res) { statusResult = res; });

    ASSERT_TRUE(unit.LastFrame().has_value());
    EXPECT_EQ(unit.LastFrame()->Type(), CommandType::kStatus);

    // Device responds with IMPLEMENTED/STABLE
    const uint8_t respBytes[] = {0x0C, 0x08, 0xB8, 0x80, 0x01, 0x10, 0x02, 0x05, 0x01};
    auto parsedResp = ParseResponseFor(*unit.LastFrame(), respBytes);
    ASSERT_TRUE(parsedResp.has_value());
    unit.Respond(*parsedResp);

    ASSERT_TRUE(statusResult.has_value());
    ASSERT_TRUE(statusResult->has_value());
    EXPECT_EQ((*statusResult)->inputPlug, 0x05);

    // Dispatch via unit.Control()
    std::optional<Expected<Cmd::SelectorValue>> controlResult;
    unit.Control(Cmd::SelectorCommand{
                     .address = kAudioSubunit0,
                     .operands = Cmd::SelectorOperands{.functionBlockId = 1, .inputPlug = 5}
                 },
                 [&](Expected<Cmd::SelectorValue> res) { controlResult = res; });

    ASSERT_TRUE(unit.LastFrame().has_value());
    EXPECT_EQ(unit.LastFrame()->Type(), CommandType::kControl);

    // Device rejects control
    const uint8_t rejectBytes[] = {0x0A, 0x08, 0xB8, 0x80, 0x01, 0x10, 0x02, 0x05, 0x01};
    auto rejectResp = ParseResponseFor(*unit.LastFrame(), rejectBytes);
    ASSERT_TRUE(rejectResp.has_value());
    unit.Respond(*rejectResp);

    ASSERT_TRUE(controlResult.has_value());
    ASSERT_FALSE(controlResult->has_value());
    EXPECT_EQ(controlResult->error().kind, AvcErrorKind::kUnexpectedResponse);
    EXPECT_EQ(*controlResult->error().response, ResponseCode::kRejected);
}

TEST(AvcReshapedTests, CtypeRejectionOnOperands) {
    // UnitInfo rejects kControl
    Cmd::UnitInfoOperands unitInfo{};
    Cmd::OperandWriter w1;
    EXPECT_EQ(unitInfo.Write(w1, CommandType::kControl).error().kind, AvcErrorKind::kInvalidArgument);

    // SubunitInfo rejects kControl
    Cmd::SubunitInfoOperands subunitInfo{};
    Cmd::OperandWriter w2;
    EXPECT_EQ(subunitInfo.Write(w2, CommandType::kControl).error().kind, AvcErrorKind::kInvalidArgument);

    // PlugInfo rejects kControl
    Cmd::PlugInfoOperands plugInfo{};
    Cmd::OperandWriter w3;
    EXPECT_EQ(plugInfo.Write(w3, CommandType::kControl).error().kind, AvcErrorKind::kInvalidArgument);

    // StreamFormatList rejects kControl
    Cmd::StreamFormatOperands sfList{.form = Cmd::StreamFormatSubfunction::kList};
    Cmd::OperandWriter w4;
    EXPECT_EQ(sfList.Write(w4, CommandType::kControl).error().kind, AvcErrorKind::kInvalidArgument);

    // ExtendedPlugInfo rejects kControl
    BridgeCo::ExtendedPlugInfoOperands extPlug{};
    Cmd::OperandWriter w5;
    EXPECT_EQ(extPlug.Write(w5, CommandType::kControl).error().kind, AvcErrorKind::kInvalidArgument);
}

TEST(AvcReshapedTests, MemoryIsolationCallerBufferTeardown) {
    // Verify that queued commands own their memory and don't dangle if caller buffer is destroyed
    Cmd::RawVendorDependentCommand cmd;
    {
        std::vector<uint8_t> callerBuffer = {0xAA, 0xBB, 0xCC, 0xDD};
        cmd = Cmd::RawVendorDependentCommand{
            .address = SubunitAddress::Unit(),
            .operands = Cmd::RawVendorDependentOperands({0x00, 0x01, 0x02}, callerBuffer)
        };
        // callerBuffer goes out of scope and is deallocated here
    }
    auto frame = cmd.Encode(CommandType::kControl);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->Operands().size(), 7u);
    EXPECT_EQ(frame->Operands()[3], 0xAA);
    EXPECT_EQ(frame->Operands()[4], 0xBB);
    EXPECT_EQ(frame->Operands()[5], 0xCC);
    EXPECT_EQ(frame->Operands()[6], 0xDD);
}

TEST(AvcReshapedTests, DescriptorCommands_OpenAndRead) {
    // 1. OPEN for read: 00 60 08 80 01 FF
    Cmd::OpenDescriptorCommand openCmd{
        .address = SubunitAddress::Of(SubunitType::kMusic, 0),
        .operands = {
            .specifier = Cmd::DescriptorSpecifier::SubunitStatus(),
            .subfunction = Cmd::OpenDescriptorSubfunction::kReadOpen,
        }
    };
    auto openFrame = openCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(openFrame.has_value());
    const uint8_t expectedOpen[] = {0x00, 0x60, 0x08, 0x80, 0x01, 0xFF};
    ASSERT_EQ(openFrame->Bytes().size(), sizeof(expectedOpen));
    EXPECT_TRUE(std::equal(openFrame->Bytes().begin(), openFrame->Bytes().end(), expectedOpen));

    // Rejection of non-Control
    EXPECT_EQ(openCmd.Encode(CommandType::kStatus).error().kind, AvcErrorKind::kUnsupported);

    // Decode OPEN reply
    const uint8_t openReplyBytes[] = {0x80, 0x01, 0x00};
    auto openDecoded = openCmd.Decode(openReplyBytes);
    ASSERT_TRUE(openDecoded.has_value());
    EXPECT_EQ(openDecoded->subfunction, Cmd::OpenDescriptorSubfunction::kReadOpen);

    // 2. CLOSE: 00 60 08 80 00 FF
    Cmd::OpenDescriptorCommand closeCmd{
        .address = SubunitAddress::Of(SubunitType::kMusic, 0),
        .operands = {
            .specifier = Cmd::DescriptorSpecifier::SubunitStatus(),
            .subfunction = Cmd::OpenDescriptorSubfunction::kClose,
        }
    };
    auto closeFrame = closeCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(closeFrame.has_value());
    const uint8_t expectedClose[] = {0x00, 0x60, 0x08, 0x80, 0x00, 0xFF};
    ASSERT_EQ(closeFrame->Bytes().size(), sizeof(expectedClose));
    EXPECT_TRUE(std::equal(closeFrame->Bytes().begin(), closeFrame->Bytes().end(), expectedClose));

    // 3. READ: 00 60 09 80 FF FF 00 8E 00 00
    Cmd::ReadDescriptorCommand readCmd{
        .address = SubunitAddress::Of(SubunitType::kMusic, 0),
        .operands = {
            .specifier = Cmd::DescriptorSpecifier::SubunitStatus(),
            .offset = 0x0000,
            .length = 0x008E,
        }
    };
    auto readFrame = readCmd.Encode(CommandType::kControl);
    ASSERT_TRUE(readFrame.has_value());
    const uint8_t expectedRead[] = {0x00, 0x60, 0x09, 0x80, 0xFF, 0x00, 0x00, 0x8E, 0x00, 0x00};
    ASSERT_EQ(readFrame->Bytes().size(), sizeof(expectedRead));
    EXPECT_TRUE(std::equal(readFrame->Bytes().begin(), readFrame->Bytes().end(), expectedRead));

    // Decode READ reply: [80] [status: 11] [pad: FF] [len: 00 04] [off: 00 00] [data: 01 02 03 04]
    const uint8_t readReplyBytes[] = {
        0x80, 0x11, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04
    };
    auto readDecoded = readCmd.Decode(readReplyBytes);
    ASSERT_TRUE(readDecoded.has_value());
    EXPECT_EQ(readDecoded->status, Cmd::ReadResultStatus::kMoreToRead);
    EXPECT_EQ(readDecoded->reportedLength, 4);
    EXPECT_EQ(readDecoded->reportedOffset, 0);
    ASSERT_EQ(readDecoded->data.size(), 4u);
    EXPECT_EQ(readDecoded->data[0], 0x01);
    EXPECT_EQ(readDecoded->data[3], 0x04);
}

} // namespace ASFW::AVC::Test

