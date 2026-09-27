// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcCodecTests.cpp - Unit tests for the rebuilt AV/C frame codec (ASFW::AVC).
//
// Tests conform to docs/avc-rebuild/phase-1.md Step 1.2:
// - CommandFrame::Make (headers, boundary checks, extended address rejection, quadlet padding)
// - ParseResponse (bounds, non-response detection, response code mapping)
// - ParseResponseFor (address/opcode echo checks)
// - Real hardware captures from TerraTec Phase 88 (2026-09-27)

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/AVC/Core/AvcError.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::AVC::Test {

// ===========================================================================
// CommandFrame::Make Tests
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
    // 0 operands -> 3 bytes header -> wire padded to 4 bytes
    auto f0 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, {});
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(f0->Bytes().size(), 3u);
    EXPECT_EQ(f0->WireBytes().size(), 4u);
    EXPECT_EQ(f0->WireBytes()[3], 0x00);

    // 1 operand -> 4 bytes total -> wire 4 bytes (no padding added)
    const uint8_t op1[] = {0x10};
    auto f1 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, op1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->Bytes().size(), 4u);
    EXPECT_EQ(f1->WireBytes().size(), 4u);

    // 5 operands -> 8 bytes total -> wire 8 bytes
    const uint8_t op5[] = {1, 2, 3, 4, 5};
    auto f5 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, op5);
    ASSERT_TRUE(f5.has_value());
    EXPECT_EQ(f5->Bytes().size(), 8u);
    EXPECT_EQ(f5->WireBytes().size(), 8u);

    // 8 operands -> 11 bytes total -> wire padded to 12 bytes
    const uint8_t op8[] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto f8 = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, op8);
    ASSERT_TRUE(f8.has_value());
    EXPECT_EQ(f8->Bytes().size(), 11u);
    EXPECT_EQ(f8->WireBytes().size(), 12u);
    EXPECT_EQ(f8->WireBytes()[11], 0x00);
}

// ===========================================================================
// ParseResponse Tests
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
    // Low nibble < 0x8 (command types)
    const uint8_t cmdFrame[] = {0x01, 0xFF, 0x30}; // Status
    EXPECT_EQ(ParseResponse(cmdFrame).error().kind, AvcErrorKind::kNotAResponse);

    // Nonzero CTS bits
    const uint8_t nonzeroCts[] = {0x1C, 0xFF, 0x30}; // CTS=1, code=0xC
    EXPECT_EQ(ParseResponse(nonzeroCts).error().kind, AvcErrorKind::kNotAResponse);

    // Code 0x0E (reserved, not a valid AV/C response code)
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
// ParseResponseFor Tests
// ===========================================================================

TEST(AvcFrameTests, ParseResponseForValidatesAddressAndOpcodeMatch) {
    const uint8_t cmdOps[] = {0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    auto cmd = CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, cmdOps);
    ASSERT_TRUE(cmd.has_value());

    // Matching response
    const uint8_t matchRaw[] = {0x0C, 0xFF, 0x30, 0x07, 0x01, 0x00, 0x11, 0x98};
    auto match = ParseResponseFor(*cmd, matchRaw);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->code, ResponseCode::kImplementedStable);

    // Address mismatch (response has subunit 0x08 instead of unit 0xFF)
    const uint8_t addrMismatchRaw[] = {0x0C, 0x08, 0x30, 0x07};
    auto addrErr = ParseResponseFor(*cmd, addrMismatchRaw);
    ASSERT_FALSE(addrErr.has_value());
    EXPECT_EQ(addrErr.error().kind, AvcErrorKind::kAddressMismatch);

    // Opcode mismatch (response has opcode 0x31 instead of 0x30)
    const uint8_t opcodeMismatchRaw[] = {0x0C, 0xFF, 0x31, 0x07};
    auto opErr = ParseResponseFor(*cmd, opcodeMismatchRaw);
    ASSERT_FALSE(opErr.has_value());
    EXPECT_EQ(opErr.error().kind, AvcErrorKind::kOpcodeMismatch);
}

// ===========================================================================
// Real Hardware Captures (Phase 88, 2026-09-27)
// ===========================================================================

TEST(AvcFrameTests, ParsesRealPhase88Captures) {
    // 1. Stream format list entry 0 response
    const uint8_t listResp[] = {
        0x0C, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00,
        0x90, 0x40, 0x02, 0x01, 0x03, 0x08, 0x06, 0x02, 0x00, 0x01, 0x0D
    };
    auto r1 = ParseResponse(listResp);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->code, ResponseCode::kImplementedStable);
    EXPECT_EQ(r1->address, SubunitAddress::Unit());
    EXPECT_EQ(r1->opcode, Opcode::kStreamFormatSupport);
    EXPECT_EQ(r1->operands.size(), 19u);
    EXPECT_EQ(r1->operands[0], 0xC1); // List subfunction
    EXPECT_EQ(r1->operands[7], 0x00); // Index 0
    EXPECT_EQ(r1->operands[8], 0x90); // AM root

    // 2. Opcode 0xBF list query -> NOT IMPLEMENTED
    const uint8_t bfListResp[] = {
        0x08, 0xFF, 0xBF, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00
    };
    auto r2 = ParseResponse(bfListResp);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->code, ResponseCode::kNotImplemented);
    EXPECT_EQ(r2->address, SubunitAddress::Unit());
    EXPECT_EQ(r2->opcode, Opcode::kExtendedStreamFormat);
    EXPECT_EQ(r2->operands.size(), 8u);

    // 3. Opcode 0xBF single query -> NOT IMPLEMENTED
    const uint8_t bfSingleResp[] = {
        0x08, 0xFF, 0xBF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF
    };
    auto r3 = ParseResponse(bfSingleResp);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->code, ResponseCode::kNotImplemented);
    EXPECT_EQ(r3->address, SubunitAddress::Unit());
    EXPECT_EQ(r3->opcode, Opcode::kExtendedStreamFormat);
    EXPECT_EQ(r3->operands.size(), 7u);

    // 4. PLUG INFO unit response
    const uint8_t plugInfoResp[] = {
        0x0C, 0xFF, 0x02, 0x00, 0x02, 0x02, 0x08, 0x07
    };
    auto r4 = ParseResponse(plugInfoResp);
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->code, ResponseCode::kImplementedStable);
    EXPECT_EQ(r4->address, SubunitAddress::Unit());
    EXPECT_EQ(r4->opcode, Opcode::kPlugInfo);
    ASSERT_EQ(r4->operands.size(), 5u);
    EXPECT_EQ(r4->operands[0], 0x00); // Subfunction 0 (Isochronous/External)
    EXPECT_EQ(r4->operands[1], 0x02); // 2 iso in
    EXPECT_EQ(r4->operands[2], 0x02); // 2 iso out
    EXPECT_EQ(r4->operands[3], 0x08); // 8 ext in
    EXPECT_EQ(r4->operands[4], 0x07); // 7 ext out
}

} // namespace ASFW::AVC::Test
