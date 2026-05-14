/**
 * AsyncCommandBuilderTests.cpp
 *
 * Unit tests for the header-building routing logic in ReadCommand, WriteCommand,
 * and LockCommand.  These tests verify that each command correctly selects the
 * right IEEE 1394 tCode (quadlet vs. block) based on the supplied parameters and
 * produces the expected header structure via PacketBuilder.
 *
 * No hardware, DriverKit, or AsyncSubsystem is required — the tests call
 * BuildHeader() directly after constructing the command on the stack.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "ASFWDriver/Async/Commands/ReadCommand.hpp"
#include "ASFWDriver/Async/Commands/WriteCommand.hpp"
#include "ASFWDriver/Async/Commands/LockCommand.hpp"
#include "ASFWDriver/Async/Commands/PhyCommand.hpp"
#include "ASFWDriver/Async/Tx/PacketBuilder.hpp"
#include "ASFWDriver/Hardware/IEEE1394.hpp"

using namespace ASFW::Async;

// ============================================================================
// Helpers
// ============================================================================

namespace {

PacketContext MakeCtx(uint16_t srcNodeID = 0xFFC1, uint8_t speed = 0x02) {
    PacketContext ctx{};
    ctx.sourceNodeID = srcNodeID;
    ctx.generation   = 1;
    ctx.speedCode    = speed;
    return ctx;
}

// Extract tCode from quadlet-0 of an OHCI internal-format header (bits[7:4]).
uint8_t TCodeFromQ0(uint32_t q0) {
    return static_cast<uint8_t>((q0 >> 4) & 0xFu);
}

// Extract tLabel from quadlet-0 (bits[15:10]).
uint8_t TLabelFromQ0(uint32_t q0) {
    return static_cast<uint8_t>((q0 >> 10) & 0x3Fu);
}

// Extract dataLength from quadlet-3 (bits[31:16]).
uint16_t DataLengthFromQ3(uint32_t q3) {
    return static_cast<uint16_t>((q3 >> 16) & 0xFFFFu);
}

} // namespace

// ============================================================================
// ReadCommand — BuildHeader routing
// ============================================================================

TEST(AsyncCommandBuilder, ReadCommand_Length4_SelectsQuadletTCode) {
    // length == 4, forceBlock == false  →  tCode = READ_QUADLET_REQUEST (0x4)
    ReadParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000400;
    params.length        = 4;
    params.forceBlock    = false;

    ReadCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(0x01, ctx, builder, buf.data());

    ASSERT_EQ(bytes, 12u);  // Quadlet read = 3 quadlets (12 bytes)
    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeReadQuad);
}

TEST(AsyncCommandBuilder, ReadCommand_Length256_SelectsBlockTCode) {
    // length > 4, forceBlock == false  →  tCode = READ_BLOCK_REQUEST (0x5)
    ReadParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000400;
    params.length        = 256;
    params.forceBlock    = false;

    ReadCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(0x02, ctx, builder, buf.data());

    ASSERT_EQ(bytes, 16u);  // Block read = 4 quadlets (16 bytes)
    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeReadBlock);

    // Q3 must contain the requested length in bits[31:16].
    uint32_t q3{};
    std::memcpy(&q3, buf.data() + 12, sizeof(q3));
    EXPECT_EQ(DataLengthFromQ3(q3), static_cast<uint16_t>(params.length));
}

TEST(AsyncCommandBuilder, ReadCommand_ForceBlock_UsesBlockTCodeEvenFor4Bytes) {
    // length == 4 but forceBlock == true  →  tCode = READ_BLOCK_REQUEST (0x5)
    ReadParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000400;
    params.length        = 4;
    params.forceBlock    = true;

    ReadCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(0x05, ctx, builder, buf.data());

    ASSERT_EQ(bytes, 16u);
    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeReadBlock);
}

TEST(AsyncCommandBuilder, ReadCommand_TLabelIsEmbedded) {
    // Verify tLabel is correctly placed at bits[15:10] in Q0.
    ReadParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000400;
    params.length        = 4;

    ReadCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    constexpr uint8_t kLabel = 0x2A;
    std::array<uint8_t, 20> buf{};
    cmd.BuildHeader(kLabel, ctx, builder, buf.data());

    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TLabelFromQ0(q0), kLabel);
}

// ============================================================================
// WriteCommand — BuildHeader routing
// ============================================================================

TEST(AsyncCommandBuilder, WriteCommand_Length4_SelectsQuadletTCode) {
    // length == 4, forceBlock == false  →  tCode = WRITE_QUADLET_REQUEST (0x0)
    uint32_t payload = 0xDEADBEEF;

    WriteParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000984;
    params.payload       = &payload;
    params.length        = 4;
    params.forceBlock    = false;

    WriteCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(0x01, ctx, builder, buf.data());

    ASSERT_EQ(bytes, 16u);  // Write quadlet = 4 quadlets (inline data in Q3)
    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeWriteQuad);
}

TEST(AsyncCommandBuilder, WriteCommand_Length8_SelectsBlockTCode) {
    // length > 4, forceBlock == false  →  tCode = WRITE_BLOCK_REQUEST (0x1)
    uint8_t payload[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    WriteParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000984;
    params.payload       = payload;
    params.length        = 8;
    params.forceBlock    = false;

    WriteCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(0x03, ctx, builder, buf.data());

    ASSERT_EQ(bytes, 16u);  // Block write header = 4 quadlets; payload is separate DMA
    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeWriteBlock);

    // Q3: dataLength must match payload length.
    uint32_t q3{};
    std::memcpy(&q3, buf.data() + 12, sizeof(q3));
    EXPECT_EQ(DataLengthFromQ3(q3), static_cast<uint16_t>(params.length));
}

TEST(AsyncCommandBuilder, WriteCommand_ForceBlock_UsesBlockTCodeEvenFor4Bytes) {
    uint32_t payload = 0x12345678;

    WriteParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0xF0000984;
    params.payload       = &payload;
    params.length        = 4;
    params.forceBlock    = true;

    WriteCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    cmd.BuildHeader(0x07, ctx, builder, buf.data());

    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeWriteBlock);
}

// ============================================================================
// LockCommand — BuildHeader
// ============================================================================

TEST(AsyncCommandBuilder, LockCommand_CompareSwap_SetsCorrectTCodeAndExtendedTCode) {
    // COMPARE_SWAP: extendedTCode = 0x0002, operand = 8 bytes (compare+swap quadlets)
    uint8_t operand[8] = {0x00, 0x00, 0x00, 0x00,   // compare value (big-endian)
                          0x00, 0x00, 0x00, 0x01};   // swap value (big-endian)

    LockParams params{};
    params.destinationID  = 0x3F;   // IRM
    params.addressHigh    = 0xFFFF;
    params.addressLow     = 0xF0000984;
    params.operand        = operand;
    params.operandLength  = 8;
    params.responseLength = 4;
    constexpr uint16_t kExtTCode = 0x0002; // COMPARE_SWAP

    LockCommand cmd{params, kExtTCode, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(0x0B, ctx, builder, buf.data());

    ASSERT_EQ(bytes, 16u);

    uint32_t q0{}, q3{};
    std::memcpy(&q0, buf.data() + 0,  sizeof(q0));
    std::memcpy(&q3, buf.data() + 12, sizeof(q3));

    // tCode must be LOCK_REQUEST (0x9)
    EXPECT_EQ(TCodeFromQ0(q0), HW::AsyncRequestHeader::kTcodeLockRequest);

    // Q3: [dataLength:16][extendedTCode:16]
    EXPECT_EQ(DataLengthFromQ3(q3),          static_cast<uint16_t>(params.operandLength));
    EXPECT_EQ(static_cast<uint16_t>(q3 & 0xFFFFu), kExtTCode);
}

TEST(AsyncCommandBuilder, LockCommand_FetchAdd_SetsExtendedTCode) {
    uint32_t operand = 0x00000001;

    LockParams params{};
    params.destinationID  = 1;
    params.addressHigh    = 0xFFFF;
    params.addressLow     = 0xF0000984;
    params.operand        = &operand;
    params.operandLength  = 4;
    params.responseLength = 0;
    constexpr uint16_t kExtTCode = 0x0003; // FETCH_ADD

    LockCommand cmd{params, kExtTCode, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    cmd.BuildHeader(0x1F, ctx, builder, buf.data());

    uint32_t q3{};
    std::memcpy(&q3, buf.data() + 12, sizeof(q3));

    EXPECT_EQ(static_cast<uint16_t>(q3 & 0xFFFFu), kExtTCode);
    EXPECT_EQ(DataLengthFromQ3(q3), static_cast<uint16_t>(params.operandLength));
}

TEST(AsyncCommandBuilder, LockCommand_NonQuadletAlignedLength_BuildHeaderReturnsZero) {
    // Operand not quadlet-aligned → BuildLock should reject it.
    uint8_t operand[3] = {};

    LockParams params{};
    params.destinationID = 1;
    params.addressHigh   = 0xFFFF;
    params.addressLow    = 0x0;
    params.operand       = operand;
    params.operandLength = 3;   // Not quadlet-aligned — should be rejected

    LockCommand cmd{params, 0x0002, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    std::array<uint8_t, 20> buf{};
    EXPECT_EQ(cmd.BuildHeader(0x01, ctx, builder, buf.data()), 0u);
}

// ============================================================================
// PhyCommand — BuildHeader
// ============================================================================

TEST(AsyncCommandBuilder, PhyCommand_SetsPhyTCodeAndReturns12Bytes) {
    // PHY packets: tCode=0xE, 12 bytes (3 quadlets), immediate data only.
    PhyParams params{};
    params.quadlet1 = 0x00400000;  // Example: gap_count config
    params.quadlet2 = ~0x00400000; // Complement (standard PHY packet pattern)

    PhyCommand cmd{params, nullptr};
    PacketBuilder builder;
    const PacketContext ctx = MakeCtx();

    constexpr uint8_t kLabel = 0x15;
    std::array<uint8_t, 20> buf{};
    const size_t bytes = cmd.BuildHeader(kLabel, ctx, builder, buf.data());

    // PHY packets are 12 bytes (3 quadlets), not 16.
    EXPECT_EQ(bytes, 12u);

    uint32_t q0{};
    std::memcpy(&q0, buf.data(), sizeof(q0));

    // tCode = 0xE at bits[7:4]
    EXPECT_EQ(TCodeFromQ0(q0), 0xEu);
    // tLabel embedded at bits[15:10]
    EXPECT_EQ(TLabelFromQ0(q0), kLabel);
}
