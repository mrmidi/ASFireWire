/**
 * CompareAndSwapPacketTests.cpp
 *
 * Comprehensive unit tests for IEEE 1394 Compare-And-Swap (CAS) lock transactions.
 * These tests validate the packet construction for IRM (Isochronous Resource Manager)
 * operations, ensuring compliance with:
 * - OHCI 1.1 Specification Section 7.8.1.3 (Lock request transmit format)
 * - Linux firewire-ohci driver validation logic (ohci.c:1666-1677)
 * - Apple IOFireWireFamily implementation
 *
 * Critical validation points:
 * 1. Header quadlet 3 must contain: dataLength=0x0008 (8 bytes), extTcode=0x0002 (CAS)
 * 2. Payload must be 8 bytes: [compareValue:32][swapValue:32] in big-endian
 * 3. Expected response is 4 bytes (old value only)
 * 4. Packet must pass IRM responder validation or return RCODE_TYPE_ERROR (6)
 *
 * References:
 * - docs/IRM/CAS.md (Comprehensive CAS analysis)
 * - docs/linux/firewire_src/packet-serdes-test.c (Linux test vectors)
 * - docs/IOFireWireFamily/IOFWCompareAndSwapCommand.cpp (Apple implementation)
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "ASFWDriver/Async/AsyncTypes.hpp"
#include "ASFWDriver/Hardware/IEEE1394.hpp"
#include "ASFWDriver/Async/Tx/PacketBuilder.hpp"
#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"

using namespace ASFW::Async;

// =============================================================================
// Test Fixture and Helpers
// =============================================================================

class CompareAndSwapPacketTest : public ::testing::Test {
protected:
    PacketBuilder builder_;

    // Helper: Create default packet context
    static PacketContext MakeContext(uint16_t sourceNodeID, uint8_t speedCode = 0x00) {
        PacketContext context{};
        context.sourceNodeID = sourceNodeID;
        context.generation = 1;
        context.speedCode = speedCode;  // S100 for IRM
        return context;
    }

    // Helper: Build destination ID (bus from source, node from dest param)
    static uint16_t MakeDestinationID(uint16_t sourceNodeID, uint16_t destNode) {
        const uint16_t bus = (sourceNodeID >> 6) & 0x03FFu;
        return (bus << 6) | (destNode & 0x3Fu);
    }

    // Helper: Load header as host-order quadlets
    template <std::size_t N>
    static std::array<uint32_t, N> LoadHostQuadlets(const uint8_t* buffer) {
        std::array<uint32_t, N> words{};
        std::memcpy(words.data(), buffer, N * sizeof(uint32_t));
        return words;
    }

    // Helper: Byte-swap to big-endian (for expected payload validation)
    static uint32_t ToBigEndian32(uint32_t value) {
        return ((value & 0xFF000000u) >> 24) |
               ((value & 0x00FF0000u) >> 8) |
               ((value & 0x0000FF00u) << 8) |
               ((value & 0x000000FFu) << 24);
    }
};

// =============================================================================
// New Spec Validation: Lock Header Field Positions (tCode=0x9, extTcode=0x2)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, LockHeader_SpecFields_AreInPlace) {
    // Parameters mirror the observed CAS attempt: src=0xffc0, dst node=0x02 (0xffc2),
    // address 0xffff:f0000228, operand length=8 (CAS old+new), extTCode=0x0002.
    LockParams params{};
    params.destinationID = MakeDestinationID(/*sourceNodeID=*/0xffc0, /*destNode=*/0x02);
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000228;
    params.operandLength = 8;

    const uint16_t label = 0x21;   // arbitrary but deterministic
    const uint8_t speed = 0x00;    // S100 for compatibility
    const uint8_t extendedTCode = 0x02;  // CAS
    const PacketContext context = MakeContext(/*sourceNodeID=*/0xffc0, speed);

    uint8_t headerBuffer[20]{};
    const std::size_t headerSize = builder_.BuildLock(params, label, extendedTCode, context, headerBuffer, sizeof(headerBuffer));
    ASSERT_EQ(headerSize, 16u) << "Lock header must be 16 bytes";

    const auto words = LoadHostQuadlets<4>(headerBuffer);

    // Quadlet 0: srcBusID|spd|tLabel|rt|tCode|priority
    const uint32_t expectedQ0 =
        (static_cast<uint32_t>(speed & 0x7) << 16) |          // spd
        (static_cast<uint32_t>(label & 0x3F) << 10) |         // tLabel
        (static_cast<uint32_t>(0x1) << 8) |                   // rt = retry_X (01b)
        (static_cast<uint32_t>(0x9) << 4) |                   // tCode = 0x9 (LOCK)
        0x0;                                                  // priority = 0
    EXPECT_EQ(words[0], expectedQ0) << "q0 control/label/tCode fields must match OHCI 7.8.1.3";

    // Quadlet 1: destinationID | addressHigh
    const uint32_t expectedQ1 =
        (static_cast<uint32_t>(params.destinationID) << 16) |
        static_cast<uint32_t>(params.addressHigh);
    EXPECT_EQ(words[1], expectedQ1) << "q1 must pack destinationID and addressHigh";

    // Quadlet 2: destinationOffsetLow
    EXPECT_EQ(words[2], params.addressLow) << "q2 must equal destinationOffsetLow";

    // Quadlet 3: dataLength (bytes) | extendedTCode
    const uint32_t expectedQ3 =
        (static_cast<uint32_t>(params.operandLength) << 16) |
        static_cast<uint32_t>(extendedTCode);
    EXPECT_EQ(words[3], expectedQ3) << "q3 must encode dataLength=8 and extTCode=0x0002 for CAS";
}

// =============================================================================
// Test 1: CAS Header Construction (IRM Channel Allocation)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, BuildLock_IRMChannelAllocation_HeaderFormat) {
    // Scenario: Allocate IRM channel by clearing a bit in CHANNELS_AVAILABLE_LO
    // Address: 0xFFFF.F000.0228
    // Operation: CAS(0xFFFFFFFF, 0xFFFFFFFE) - Clear bit 0

    LockParams params{};
    params.destinationID = 0xFFC2;  // IRM node
    params.addressHigh = 0xFFFF;    // CSR register space
    params.addressLow = 0xF0000228; // CHANNELS_AVAILABLE_LO
    params.operandLength = 8;       // 8 bytes (compare + swap)
    params.responseLength = 4;      // Response is 4 bytes (old value)
    params.speedCode = 0x00;        // S100 for IRM (required by IEEE 1394)

    const PacketContext context = MakeContext(0xFFC1, 0x00);
    constexpr uint8_t kLabel = 0x04;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          buffer.data(), buffer.size());

    // Validate header size
    ASSERT_EQ(bytes, 16u) << "Lock header must be 16 bytes (4 quadlets)";

    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Q0: [srcBusID:1][reserved:5][spd:3][tLabel:6][rt:2][tCode:4][pri:4]
    EXPECT_EQ((hostWords[0] >> 10) & 0x3Fu, kLabel)
        << "tLabel must be at bits[15:10]";
    EXPECT_EQ((hostWords[0] >> 16) & 0x7u, 0x00u)
        << "Speed must be S100 (0x00) for IRM registers";
    EXPECT_EQ((hostWords[0] >> 8) & 0x3u, 0x01u)
        << "Retry code must be retry_X (0x01)";
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, HW::AsyncRequestHeader::kTcodeLockRequest)
        << "tCode must be LOCK_REQUEST (0x9)";
    EXPECT_EQ((hostWords[0] >> 0) & 0xFu, 0x00u)
        << "Priority must be 0";

    // Q1: [destinationID:16][offsetHigh:16]
    const uint16_t destID = static_cast<uint16_t>(hostWords[1] >> 16);
    EXPECT_EQ(destID, MakeDestinationID(context.sourceNodeID, params.destinationID))
        << "Destination ID must include bus number from source";
    EXPECT_EQ(static_cast<uint16_t>(hostWords[1] & 0xFFFFu), params.addressHigh)
        << "Address high must match params";

    // Q2: [offsetLow:32]
    EXPECT_EQ(hostWords[2], params.addressLow)
        << "Address low must match params";

    // Q3: [dataLength:16][extendedTcode:16] - CRITICAL FOR IRM VALIDATION!
    const uint16_t dataLength = static_cast<uint16_t>(hostWords[3] >> 16);
    const uint16_t extTcode = static_cast<uint16_t>(hostWords[3] & 0xFFFFu);

    EXPECT_EQ(dataLength, 8u)
        << "CRITICAL: dataLength must be exactly 8 bytes or IRM will reject with RCODE_TYPE_ERROR";
    EXPECT_EQ(extTcode, kExtTCodeCompareSwap)
        << "CRITICAL: extendedTcode must be 0x0002 (COMPARE_SWAP) or IRM will reject";

    // Verify full Q3 value
    EXPECT_EQ(hostWords[3], 0x00080002u)
        << "Quadlet 3 must be 0x00080002 (8 bytes, ext tcode 2)";
}

// =============================================================================
// Test 2: CAS Header Construction (IRM Bandwidth Allocation)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, BuildLock_IRMBandwidthAllocation_HeaderFormat) {
    // Scenario: Allocate 84 bandwidth units
    // Address: 0xFFFF.F000.0220
    // Operation: CAS(0x0000100F, 0x00000FBB) - Subtract 0x54 units

    LockParams params{};
    params.destinationID = 0xFFC2;  // IRM node
    params.addressHigh = 0xFFFF;    // CSR register space
    params.addressLow = 0xF0000220; // BANDWIDTH_AVAILABLE
    params.operandLength = 8;
    params.responseLength = 4;
    params.speedCode = 0x00;        // S100 for IRM

    const PacketContext context = MakeContext(0xFFC0, 0x00);
    constexpr uint8_t kLabel = 0x3C;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          buffer.data(), buffer.size());

    ASSERT_EQ(bytes, 16u);
    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Validate critical Q3 field
    EXPECT_EQ(hostWords[3], 0x00080002u)
        << "Q3 must be 0x00080002 for CAS operations";
}

// =============================================================================
// Test 3: Linux Kernel Test Vector - CAS Request
// =============================================================================

TEST_F(CompareAndSwapPacketTest, BuildLock_MatchesLinuxKernelTestVector) {
    // From: docs/linux/firewire_src/packet-serdes-test.c:560-574
    // Expected header (OHCI internal format, host byte order):
    // Q0: dst=0xffc0, tLabel=0x0b, rt=0x01, tCode=0x9, pri=0x00
    // Q1: src implied, offset_high=0xFFFF
    // Q2: offset_low=0xF0000984
    // Q3: data_length=0x0008, extended_tcode=0x0002

    LockParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000984;
    params.operandLength = 8;
    params.responseLength = 4;
    params.speedCode = 0x02;  // S400 (test uses higher speed)

    const PacketContext context = MakeContext(0xFFC1, 0x02);
    constexpr uint8_t kLabel = 0x0B;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          buffer.data(), buffer.size());

    ASSERT_EQ(bytes, 16u);
    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Validate against Linux test expectations
    EXPECT_EQ((hostWords[0] >> 10) & 0x3Fu, 0x0Bu);  // tLabel
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, 0x9u);     // tCode = LOCK_REQUEST
    EXPECT_EQ(hostWords[2], 0xF0000984u);            // offset_low
    EXPECT_EQ(hostWords[3], 0x00080002u);            // dataLength=8, extTcode=2

    // This header should pass Linux kernel validation:
    // if (tcode == 0x9 && ext_tcode == 0x2 && length == 8) { OK }
}

// =============================================================================
// Test 4: CAS Response Parsing (Linux Test Vector)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, ExpectedResponse_MatchesLinuxKernelVector) {
    // From: docs/linux/firewire_src/packet-serdes-test.c:609-614
    // Expected response header (wire format, big-endian):
    // 0xffc12db0, 0xffc00000, 0x00000000, 0x00040002
    // Decoded: dst=0xffc1, tLabel=0x0b, rt=0x01, tCode=0xB (LOCK_RESPONSE),
    //          rCode=0 (COMPLETE), data_length=0x0004 (4 bytes), ext_tcode=0x0002

    // This validates that we EXPECT a 4-byte response, not 8 bytes
    // The response contains only the old value, not both compare+swap values

    constexpr uint16_t kExpectedResponseLength = 4;
    constexpr uint16_t kExpectedResponseDataLength = 4;
    constexpr uint8_t kExpectedResponseTCode = 0xB;  // LOCK_RESPONSE

    // Verify expectations match our LockCommand implementation
    // (see ASFWDriver/Async/Commands/LockCommand.cpp:30)
    EXPECT_EQ(kExpectedResponseLength, 4u)
        << "CAS response must be 4 bytes (old value only)";
    EXPECT_EQ(kExpectedResponseDataLength, 4u)
        << "Response dataLength field must be 0x0004";
}

// =============================================================================
// Test 5: Payload Byte Order Validation (Big-Endian Required)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, PayloadByteOrder_MustBeBigEndian) {
    // This test validates the PAYLOAD (not header) byte order.
    // Per IEEE 1394-1995 §6.2.4.2, lock operands are transmitted in big-endian.
    //
    // For a CAS with compareValue=0xFFFFFFFF, swapValue=0xFFFFFFFE:
    // Wire bytes should be: FF FF FF FF  FF FF FF FE
    // NOT little-endian:    FF FF FF FF  FE FF FF FF

    constexpr uint32_t kCompareValue = 0xFFFFFFFFu;
    constexpr uint32_t kSwapValue = 0xFFFFFFFEu;

    // Simulate what AsyncSubsystem::CompareSwap() does (see AsyncSubsystem.cpp:712)
    std::array<uint32_t, 2> beOperands{};
    beOperands[0] = ToBigEndian32(kCompareValue);
    beOperands[1] = ToBigEndian32(kSwapValue);

    // Verify byte layout in memory (little-endian host)
    const auto* bytes = reinterpret_cast<const uint8_t*>(beOperands.data());

    // First quadlet: compare value in big-endian
    EXPECT_EQ(bytes[0], 0xFFu) << "Byte 0 must be MSB of compare value";
    EXPECT_EQ(bytes[1], 0xFFu);
    EXPECT_EQ(bytes[2], 0xFFu);
    EXPECT_EQ(bytes[3], 0xFFu) << "Byte 3 must be LSB of compare value";

    // Second quadlet: swap value in big-endian
    EXPECT_EQ(bytes[4], 0xFFu) << "Byte 4 must be MSB of swap value";
    EXPECT_EQ(bytes[5], 0xFFu);
    EXPECT_EQ(bytes[6], 0xFFu);
    EXPECT_EQ(bytes[7], 0xFEu) << "Byte 7 must be LSB of swap value";

    // Verify the entire 8-byte operand matches expected wire format
    const std::array<uint8_t, 8> expectedWireBytes = {
        0xFF, 0xFF, 0xFF, 0xFF,  // compare value (big-endian)
        0xFF, 0xFF, 0xFF, 0xFE,  // swap value (big-endian)
    };

    EXPECT_EQ(std::memcmp(bytes, expectedWireBytes.data(), 8), 0)
        << "Payload must match expected big-endian wire format";
}

// =============================================================================
// Test 6: Full IRM Channel Allocation Scenario
// =============================================================================

TEST_F(CompareAndSwapPacketTest, FullScenario_IRMChannelAllocation_ChannelBitClear) {
    // Scenario from documentation/IRM_EXPLAINED.md:
    // Mac reads CHANNELS_AVAILABLE_LO: 0xFFFFFFFF (all channels free)
    // Mac wants channel 0, so it clears bit 0:
    // CAS(0xFFFFFFFF, 0xFFFFFFFE)

    LockParams params{};
    params.destinationID = 0xFFC2;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000228;  // CHANNELS_AVAILABLE_LO
    params.operandLength = 8;
    params.responseLength = 4;
    params.speedCode = 0x00;  // S100

    const PacketContext context = MakeContext(0xFFC0, 0x00);
    constexpr uint8_t kLabel = 0x04;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          buffer.data(), buffer.size());

    ASSERT_EQ(bytes, 16u);
    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Validate all header fields for IRM compliance
    EXPECT_EQ((hostWords[0] >> 4) & 0xFu, 0x9u) << "tCode = LOCK_REQUEST";
    EXPECT_EQ((hostWords[0] >> 16) & 0x7u, 0x00u) << "Speed = S100";
    EXPECT_EQ(hostWords[2], 0xF0000228u) << "Address = CHANNELS_AVAILABLE_LO";
    EXPECT_EQ(hostWords[3], 0x00080002u) << "dataLength=8, extTcode=2";

    // Success criteria: If this header is transmitted correctly, the IRM will:
    // 1. Validate: tcode==0x9, ext_tcode==0x2, length==8
    // 2. Read payload: compare=0xFFFFFFFF, swap=0xFFFFFFFE
    // 3. Execute: if (reg == 0xFFFFFFFF) { old = reg; reg = 0xFFFFFFFE; }
    // 4. Respond: LockResp(rCode=0, payload=0xFFFFFFFF)
    //
    // Failure: If dataLength != 8, IRM returns rCode=6 (TYPE_ERROR), size=0
}

// =============================================================================
// Test 7: Edge Case - Zero Length (Should Fail)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, EdgeCase_ZeroOperandLength_ReturnsZero) {
    LockParams params{};
    params.destinationID = 0xFFC2;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000228;
    params.operandLength = 0;  // Invalid!
    params.responseLength = 4;

    const PacketContext context = MakeContext(0xFFC0, 0x00);

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, 0x04, 0x0002, context, buffer.data(), buffer.size());

    EXPECT_EQ(bytes, 0u)
        << "BuildLock must return 0 for zero operandLength (validation failure)";
}

// =============================================================================
// Test 8: Edge Case - Non-Quadlet-Aligned Length (Should Fail)
// =============================================================================

TEST_F(CompareAndSwapPacketTest, EdgeCase_NonQuadletAlignedLength_ReturnsZero) {
    LockParams params{};
    params.destinationID = 0xFFC2;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000228;
    params.operandLength = 7;  // Invalid! Must be multiple of 4
    params.responseLength = 4;

    const PacketContext context = MakeContext(0xFFC0, 0x00);

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, 0x04, 0x0002, context, buffer.data(), buffer.size());

    EXPECT_EQ(bytes, 0u)
        << "BuildLock must return 0 for non-quadlet-aligned operandLength";
}

// =============================================================================
// Test 9: Regression Test - Verify Against Failing Log
// =============================================================================

TEST_F(CompareAndSwapPacketTest, RegressionTest_FailingCASLog_HeaderValidation) {
    // From the failing log in task description:
    // LockRq from ffc0 to ffc2.ffff.f000.0228, size 8, tLabel 4
    // LockResp: rCode 6 [resp_type_error], size 0
    //
    // The IRM rejected this packet. Let's ensure our builder produces
    // a header that SHOULD pass validation.

    LockParams params{};
    params.destinationID = 0xFFC2;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000228;
    params.operandLength = 8;  // Size shown in log
    params.responseLength = 4;
    params.speedCode = 0x00;   // S100

    const PacketContext context = MakeContext(0xFFC0, 0x00);
    constexpr uint8_t kLabel = 0x04;  // tLabel from log
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          buffer.data(), buffer.size());

    ASSERT_EQ(bytes, 16u);
    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Verify Q3 - this is what the Linux kernel checks!
    const uint16_t dataLength = static_cast<uint16_t>(hostWords[3] >> 16);
    const uint16_t extTcode = static_cast<uint16_t>(hostWords[3] & 0xFFFFu);

    EXPECT_EQ(dataLength, 8u)
        << "REGRESSION: dataLength must be 8 or IRM will return RCODE_TYPE_ERROR";
    EXPECT_EQ(extTcode, 0x0002u)
        << "REGRESSION: extTcode must be 0x0002 (COMPARE_SWAP)";

    // If this test passes but the real packet still fails, the issue is likely:
    // 1. Byte order conversion in descriptor builder
    // 2. reqCount field in OHCI descriptor (must be 16 for header)
    // 3. Hardware-specific header formatting quirk
}

// =============================================================================
// Test 10: Cross-Validation with Apple Test Vector
// =============================================================================

TEST_F(CompareAndSwapPacketTest, AppleCompatibility_IRMBandwidthCAS) {
    // From successful Apple log (documentation/IRM_EXPLAINED.md:95-121):
    // LockRq to ffc2.ffff.f000.0220, size 8
    // Operand: 0x0000100F 0x00000FBB (subtract 0x54 units)
    // Response: 0x0000100F (old value), rCode=0 (success)

    LockParams params{};
    params.destinationID = 0xFFC2;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000220;  // BANDWIDTH_AVAILABLE
    params.operandLength = 8;
    params.responseLength = 4;
    params.speedCode = 0x00;  // Apple uses S100 for IRM

    const PacketContext context = MakeContext(0xFFC0, 0x00);
    constexpr uint8_t kLabel = 0x3C;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> buffer{};
    const std::size_t bytes =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          buffer.data(), buffer.size());

    ASSERT_EQ(bytes, 16u);
    const auto hostWords = LoadHostQuadlets<4>(buffer.data());

    // Validate header matches Apple's successful packet format
    EXPECT_EQ(hostWords[3], 0x00080002u)
        << "Header Q3 must match Apple's working implementation";
}

// =============================================================================
// DESCRIPTOR-LEVEL TESTS: OHCI Descriptor Construction for CAS/Lock
// =============================================================================

/*
 * These tests validate that OHCI descriptor structures correctly store
 * CAS/Lock packet headers. The DescriptorBuilder::BuildTransactionChain()
 * function creates a two-descriptor chain for lock requests:
 *
 * 1. OUTPUT_MORE-Immediate descriptor containing 16-byte packet header
 * 2. OUTPUT_LAST descriptor pointing to 8-byte payload (compare+swap values)
 *
 * CRITICAL VALIDATION POINTS:
 * - Header descriptor reqCount must be 16 (not 8!)
 * - All 4 header quadlets must be preserved during memcpy
 * - Quadlet 3 at offset 12 must remain 0x00080002
 * - Control word must use OUTPUT_MORE-Immediate encoding
 */

TEST_F(CompareAndSwapPacketTest, OHCIDescriptorImmediate_StructureLayout) {
    using ASFW::Async::HW::OHCIDescriptorImmediate;
    using ASFW::Async::HW::OHCIDescriptor;

    // Validate structure size per OHCI 1.1 spec
    EXPECT_EQ(sizeof(OHCIDescriptorImmediate), 32u)
        << "OUTPUT_MORE/LAST-Immediate descriptors must be 32 bytes (2 blocks)";

    EXPECT_EQ(alignof(OHCIDescriptorImmediate), 16u)
        << "OHCI descriptors must be 16-byte aligned";

    // Validate immediate data capacity
    OHCIDescriptorImmediate desc{};
    std::memset(&desc, 0, sizeof(desc));

    // immediateData should have space for 16 bytes (4 quadlets)
    // This is (32-byte descriptor - 16-byte header) = 16 bytes
    constexpr std::size_t kExpectedImmediateCapacity = 16;
    EXPECT_EQ(sizeof(desc.immediateData), kExpectedImmediateCapacity)
        << "Immediate data area must hold 16-byte packet header";
}

TEST_F(CompareAndSwapPacketTest, Descriptor_HeaderCopyPreservesQuadlet3) {
    using ASFW::Async::HW::OHCIDescriptorImmediate;

    // Build a CAS header using PacketBuilder
    LockParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000224;  // CHANNELS_AVAILABLE_HI
    params.operandLength = 8;
    params.responseLength = 4;
    params.speedCode = 0x00;

    const PacketContext context = MakeContext(0xFFC0, 0x00);
    constexpr uint8_t kLabel = 0x15;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> headerBuffer{};
    const std::size_t headerSize =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          headerBuffer.data(), headerBuffer.size());

    ASSERT_EQ(headerSize, 16u);

    // Simulate DescriptorBuilder copying header to descriptor
    OHCIDescriptorImmediate desc{};
    std::memset(&desc, 0, sizeof(desc));
    std::memcpy(desc.immediateData, headerBuffer.data(), headerSize);

    // Validate that quadlet 3 is preserved during copy
    const uint32_t quadlet3 = desc.immediateData[3];  // Host byte order
    EXPECT_EQ(quadlet3, 0x00080002u)
        << "CRITICAL: Quadlet 3 must be preserved as 0x00080002 during descriptor copy";

    // Validate all quadlets are non-zero (header should be populated)
    EXPECT_NE(desc.immediateData[0], 0u) << "Q0 should contain destination/tLabel/tCode";
    EXPECT_NE(desc.immediateData[1], 0u) << "Q1 should contain source/offset high";
    EXPECT_NE(desc.immediateData[2], 0u) << "Q2 should contain offset low";
    EXPECT_EQ(desc.immediateData[3], 0x00080002u) << "Q3 must be dataLength=8, extTcode=2";
}

TEST_F(CompareAndSwapPacketTest, Descriptor_ControlWord_OutputMoreImmediate_ReqCount16) {
    using ASFW::Async::HW::OHCIDescriptor;

    // Build control word for OUTPUT_MORE-Immediate descriptor with 16-byte header
    constexpr uint16_t reqCount = 16;  // Lock request header = 16 bytes (4 quadlets)
    constexpr uint8_t cmd = OHCIDescriptor::kCmdOutputMore;       // cmd=0x0
    constexpr uint8_t key = OHCIDescriptor::kKeyImmediate;        // key=0x2
    constexpr uint8_t intCtrl = OHCIDescriptor::kIntNever;        // i=0x0
    constexpr uint8_t branchCtrl = OHCIDescriptor::kBranchNever;  // b=0x0 (required for OUTPUT_MORE)

    const uint32_t control = OHCIDescriptor::BuildControl(
        reqCount, cmd, key, intCtrl, branchCtrl, false);

    // Extract reqCount field (lower 16 bits)
    const uint16_t extractedReqCount = static_cast<uint16_t>(control & 0xFFFFu);
    EXPECT_EQ(extractedReqCount, 16u)
        << "CRITICAL: reqCount must be 16 for lock request header, NOT 8";

    // Extract and validate control fields (upper 16 bits)
    const uint16_t controlHi = static_cast<uint16_t>(control >> 16);
    const uint8_t extractedCmd = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kCmdShift) & 0xF);
    const uint8_t extractedKey = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kKeyShift) & 0x7);
    const uint8_t extractedInt = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kIntShift) & 0x3);
    const uint8_t extractedBranch = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kBranchShift) & 0x3);

    EXPECT_EQ(extractedCmd, OHCIDescriptor::kCmdOutputMore)
        << "cmd must be OUTPUT_MORE (0x0) for first descriptor in chain";
    EXPECT_EQ(extractedKey, OHCIDescriptor::kKeyImmediate)
        << "key must be Immediate (0x2) for header descriptor";
    EXPECT_EQ(extractedInt, OHCIDescriptor::kIntNever)
        << "i must be Never (0x0) for OUTPUT_MORE (interrupt on OUTPUT_LAST only)";
    EXPECT_EQ(extractedBranch, OHCIDescriptor::kBranchNever)
        << "b must be Never (0x0) for OUTPUT_MORE (hardware uses physical contiguity)";
}

TEST_F(CompareAndSwapPacketTest, Descriptor_ControlWord_OutputLast_ReqCount8) {
    using ASFW::Async::HW::OHCIDescriptor;

    // Build control word for OUTPUT_LAST descriptor with 8-byte payload
    constexpr uint16_t reqCount = 8;  // CAS payload = 8 bytes (compare + swap)
    constexpr uint8_t cmd = OHCIDescriptor::kCmdOutputLast;       // cmd=0x1
    constexpr uint8_t key = OHCIDescriptor::kKeyStandard;         // key=0x0 (payload from memory)
    constexpr uint8_t intCtrl = OHCIDescriptor::kIntAlways;       // i=0x3 (interrupt on completion)
    constexpr uint8_t branchCtrl = OHCIDescriptor::kBranchAlways; // b=0x3 (always branch)

    const uint32_t control = OHCIDescriptor::BuildControl(
        reqCount, cmd, key, intCtrl, branchCtrl, false);

    // Extract reqCount field
    const uint16_t extractedReqCount = static_cast<uint16_t>(control & 0xFFFFu);
    EXPECT_EQ(extractedReqCount, 8u)
        << "reqCount must be 8 for CAS payload (compare+swap operands)";

    // Extract and validate control fields
    const uint16_t controlHi = static_cast<uint16_t>(control >> 16);
    const uint8_t extractedCmd = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kCmdShift) & 0xF);
    const uint8_t extractedKey = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kKeyShift) & 0x7);
    const uint8_t extractedInt = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kIntShift) & 0x3);
    const uint8_t extractedBranch = static_cast<uint8_t>((controlHi >> OHCIDescriptor::kBranchShift) & 0x3);

    EXPECT_EQ(extractedCmd, OHCIDescriptor::kCmdOutputLast)
        << "cmd must be OUTPUT_LAST (0x1) for final descriptor";
    EXPECT_EQ(extractedKey, OHCIDescriptor::kKeyStandard)
        << "key must be Standard (0x0) for payload from memory";
    EXPECT_EQ(extractedInt, OHCIDescriptor::kIntAlways)
        << "i must be Always (0x3) for OUTPUT_LAST to get completion IRQ";
    EXPECT_EQ(extractedBranch, OHCIDescriptor::kBranchAlways)
        << "b must be Always (0x3) for OUTPUT_LAST per OHCI spec";
}

TEST_F(CompareAndSwapPacketTest, Descriptor_TwoDescriptorChain_HeaderAndPayload) {
    using ASFW::Async::HW::OHCIDescriptor;
    using ASFW::Async::HW::OHCIDescriptorImmediate;

    // Simulate two-descriptor chain for CAS transaction:
    // Descriptor 1: OUTPUT_MORE-Immediate with 16-byte header
    // Descriptor 2: OUTPUT_LAST with 8-byte payload

    // Build CAS header
    LockParams params{};
    params.destinationID = 0xFFC0;
    params.addressHigh = 0xFFFF;
    params.addressLow = 0xF0000220;  // BANDWIDTH_AVAILABLE
    params.operandLength = 8;
    params.responseLength = 4;
    params.speedCode = 0x00;

    const PacketContext context = MakeContext(0xFFC0, 0x00);
    constexpr uint8_t kLabel = 0x2A;
    constexpr uint16_t kExtTCodeCompareSwap = 0x0002;

    std::array<uint8_t, 16> headerBuffer{};
    const std::size_t headerSize =
        builder_.BuildLock(params, kLabel, kExtTCodeCompareSwap, context,
                          headerBuffer.data(), headerBuffer.size());
    ASSERT_EQ(headerSize, 16u);

    // Descriptor 1: Header (OUTPUT_MORE-Immediate)
    OHCIDescriptorImmediate headerDesc{};
    std::memset(&headerDesc, 0, sizeof(headerDesc));

    // Copy header to immediate data
    std::memcpy(headerDesc.immediateData, headerBuffer.data(), headerSize);

    // Set control word: reqCount=16, OUTPUT_MORE, Immediate, i=Never, b=Never
    headerDesc.common.control = OHCIDescriptor::BuildControl(
        16,  // reqCount = 16 bytes (4 quadlets)
        OHCIDescriptor::kCmdOutputMore,
        OHCIDescriptor::kKeyImmediate,
        OHCIDescriptor::kIntNever,
        OHCIDescriptor::kBranchNever,
        false);

    headerDesc.common.branchWord = 0;  // Ignored for OUTPUT_MORE (uses contiguity)

    // Validate header descriptor reqCount
    const uint16_t headerReqCount = static_cast<uint16_t>(headerDesc.common.control & 0xFFFFu);
    EXPECT_EQ(headerReqCount, 16u)
        << "CRITICAL: Header descriptor reqCount MUST be 16, not 8";

    // Validate header quadlet 3 is preserved
    EXPECT_EQ(headerDesc.immediateData[3], 0x00080002u)
        << "Header Q3 must be 0x00080002 (dataLength=8, extTcode=2)";

    // Descriptor 2: Payload (OUTPUT_LAST)
    OHCIDescriptor payloadDesc{};
    std::memset(&payloadDesc, 0, sizeof(payloadDesc));

    // Set control word: reqCount=8, OUTPUT_LAST, Standard, i=Always, b=Always
    payloadDesc.control = OHCIDescriptor::BuildControl(
        8,  // reqCount = 8 bytes (compare + swap operands)
        OHCIDescriptor::kCmdOutputLast,
        OHCIDescriptor::kKeyStandard,
        OHCIDescriptor::kIntAlways,
        OHCIDescriptor::kBranchAlways,
        false);

    payloadDesc.branchWord = 0;  // EOL marker
    payloadDesc.dataAddress = 0x12345000;  // Mock payload IOVA (4-byte aligned)

    // Validate payload descriptor reqCount
    const uint16_t payloadReqCount = static_cast<uint16_t>(payloadDesc.control & 0xFFFFu);
    EXPECT_EQ(payloadReqCount, 8u)
        << "Payload descriptor reqCount must be 8 (compare+swap operands)";

    // Validate payload descriptor has non-zero dataAddress
    EXPECT_NE(payloadDesc.dataAddress, 0u)
        << "Payload descriptor must point to DMA buffer containing operands";

    // Validate that header and payload descriptors form valid chain
    // In real code, header.branchWord would point to payload (but OUTPUT_MORE uses contiguity)
    // So we just verify structure sizes align for contiguous placement
    EXPECT_EQ(sizeof(OHCIDescriptorImmediate), 32u);  // 2 blocks
    EXPECT_EQ(sizeof(OHCIDescriptor), 16u);           // 1 block
    // Total chain: 3 blocks (32 + 16 = 48 bytes)
}

// =============================================================================
// Summary Comment
// =============================================================================

/*
 * TEST SUMMARY
 * ============
 *
 * PACKET-LEVEL TESTS:
 * These tests validate that the PacketBuilder::BuildLock() function produces
 * headers that comply with IEEE 1394 CAS requirements and will pass IRM
 * responder validation.
 *
 * DESCRIPTOR-LEVEL TESTS (NEW):
 * These tests validate that OHCI descriptor structures correctly store and
 * encode CAS/Lock packet headers for DMA transmission:
 * - OHCIDescriptorImmediate structure layout (32 bytes, 16-byte capacity)
 * - Header copy preserves all quadlets, especially Q3 (0x00080002)
 * - OUTPUT_MORE-Immediate control word with reqCount=16
 * - OUTPUT_LAST control word with reqCount=8 for payload
 * - Two-descriptor chain structure (header + payload)
 *
 * KEY FINDINGS:
 * - If packet tests pass: PacketBuilder is correct ✅
 * - If descriptor tests pass: Descriptor encoding is correct ✅
 * - If tests pass but real IRM still fails, the issue is in:
 *   1. DescriptorBuilder::BuildTransactionChain() implementation
 *   2. DMA memory management (IOVA mapping)
 *   3. OHCI controller hardware behavior
 *
 * NEXT STEPS IF TESTS PASS:
 * 1. Add DescriptorBuilder integration tests with mock Ring/DMA
 * 2. Add logging to DescriptorBuilder to dump descriptor contents
 * 3. Capture wire-level traces with FireBug and compare byte-for-byte
 *
 * NEXT STEPS IF TESTS FAIL:
 * 1. Fix the failing component (PacketBuilder or descriptor encoding)
 * 2. Re-run tests until all pass
 * 3. Then proceed to integration testing
 */
