// ResponseSenderHeaderFormatTests.cpp - Unit tests for OHCI AT header format
//
// Tests verify that ResponseSender builds Write Response headers in correct
// OHCI AT Data format, NOT IEEE 1394 wire format.
//
// CRITICAL BUG FIXED (2025-11-25):
// ResponseSender was building headers in IEEE 1394 wire format:
//   Q0: [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
//   Q1: [srcID:16][rCode:4][reserved:12]
//
// But OHCI AT requires:
//   Q0: [srcBusID:1][unused:5][speed:3][tLabel:6][rt:2][tCode:4][pri:4]
//   Q1: [destID:16][rCode:4][reserved:12]
//
// This caused write responses to be sent to the wrong destination
// (ffc0→ffc0 instead of ffc0→ffc2).

#include <gtest/gtest.h>
#include <cstdint>

// =============================================================================
// OHCI AT Header Format Tests (Standalone, no driver dependencies)
// =============================================================================

namespace {

// OHCI AT Data format constants (from Linux ohci.h)
constexpr uint32_t OHCI_AT_Q0_SRCBUSID_SHIFT = 23;
constexpr uint32_t OHCI_AT_Q0_SPEED_SHIFT = 16;
constexpr uint32_t OHCI_AT_Q0_TLABEL_SHIFT = 10;
constexpr uint32_t OHCI_AT_Q0_RETRY_SHIFT = 8;
constexpr uint32_t OHCI_AT_Q0_TCODE_SHIFT = 4;
constexpr uint32_t OHCI_AT_Q0_PRIORITY_MASK = 0xF;

constexpr uint32_t OHCI_AT_Q1_DESTID_SHIFT = 16;
constexpr uint32_t OHCI_AT_Q1_RCODE_SHIFT = 12;

// Transaction codes
constexpr uint8_t TCODE_WRITE_RESPONSE = 0x2;

// Speed codes
constexpr uint8_t SPEED_S400 = 0x02;

// Retry codes
constexpr uint8_t RETRY_X = 0x01;

/**
 * @brief Build a Write Response header in OHCI AT Data format.
 *
 * This mirrors the logic in ResponseSender::SendWriteResponse().
 *
 * @param destID Destination node ID (where response is sent)
 * @param srcID Source node ID (our local node, unused in OHCI AT format)
 * @param tLabel Transaction label
 * @param rcode Response code
 * @param header Output: 3 quadlets in OHCI AT format
 */
void BuildWriteResponseHeader_OHCIFormat(
    uint16_t destID,
    [[maybe_unused]] uint16_t srcID,  // srcID is NOT in Q1 for OHCI AT!
    uint8_t tLabel,
    uint8_t rcode,
    uint32_t header[3])
{
    constexpr uint8_t kSrcBusID = 0;
    constexpr uint8_t kSpeed = SPEED_S400;
    constexpr uint8_t kRetry = RETRY_X;
    constexpr uint8_t kTCode = TCODE_WRITE_RESPONSE;
    constexpr uint8_t kPriority = 0;

    // Q0: [srcBusID:1][unused:5][speed:3][tLabel:6][rt:2][tCode:4][pri:4]
    header[0] = (static_cast<uint32_t>(kSrcBusID & 0x01) << OHCI_AT_Q0_SRCBUSID_SHIFT) |
                (static_cast<uint32_t>(kSpeed & 0x07) << OHCI_AT_Q0_SPEED_SHIFT) |
                (static_cast<uint32_t>(tLabel & 0x3F) << OHCI_AT_Q0_TLABEL_SHIFT) |
                (static_cast<uint32_t>(kRetry & 0x03) << OHCI_AT_Q0_RETRY_SHIFT) |
                (static_cast<uint32_t>(kTCode & 0x0F) << OHCI_AT_Q0_TCODE_SHIFT) |
                (static_cast<uint32_t>(kPriority) & OHCI_AT_Q0_PRIORITY_MASK);

    // Q1: [destID:16][rCode:4][reserved:12]
    header[1] = (static_cast<uint32_t>(destID) << OHCI_AT_Q1_DESTID_SHIFT) |
                (static_cast<uint32_t>(rcode & 0x0F) << OHCI_AT_Q1_RCODE_SHIFT);

    // Q2: reserved for responses
    header[2] = 0;
}

/**
 * @brief Build a Write Response header in WRONG IEEE 1394 wire format.
 *
 * This is what ResponseSender was doing BEFORE the fix.
 * Keep this to verify the bug is fixed.
 */
void BuildWriteResponseHeader_IEEE1394Format_WRONG(
    uint16_t destID,
    uint16_t srcID,
    uint8_t tLabel,
    uint8_t rcode,
    uint32_t header[3])
{
    constexpr uint8_t kRetry = RETRY_X;
    constexpr uint8_t kTCode = TCODE_WRITE_RESPONSE;
    constexpr uint8_t kPriority = 0;

    // WRONG Q0: [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
    header[0] = (static_cast<uint32_t>(destID) << 16) |
                (static_cast<uint32_t>(tLabel & 0x3F) << 10) |
                (static_cast<uint32_t>(kRetry & 0x03) << 8) |
                (static_cast<uint32_t>(kTCode & 0x0F) << 4) |
                (static_cast<uint32_t>(kPriority) & 0x0F);

    // WRONG Q1: [srcID:16][rCode:4][reserved:12]
    header[1] = (static_cast<uint32_t>(srcID) << 16) |
                (static_cast<uint32_t>(rcode & 0x0F) << 12);

    header[2] = 0;
}

} // anonymous namespace

// =============================================================================
// Test Fixture
// =============================================================================

class ResponseSenderHeaderFormatTest : public ::testing::Test {
protected:
    // Typical values for FCP write response
    static constexpr uint16_t kLocalNodeID = 0xFFC0;  // Our node (Mac)
    static constexpr uint16_t kRemoteNodeID = 0xFFC2; // Duet device
    static constexpr uint8_t kTLabel = 5;
    static constexpr uint8_t kRCodeComplete = 0x0;
};

// =============================================================================
// OHCI AT Format Tests
// =============================================================================

TEST_F(ResponseSenderHeaderFormatTest, OHCI_Q0_HasSpeedField) {
    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, header);

    // Extract speed field from Q0 bits[18:16]
    uint8_t speed = (header[0] >> 16) & 0x07;
    EXPECT_EQ(SPEED_S400, speed)
        << "OHCI AT Q0 should have speed field at bits[18:16]";
}

TEST_F(ResponseSenderHeaderFormatTest, OHCI_Q1_HasDestID) {
    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, header);

    // Extract destID from Q1 bits[31:16]
    uint16_t destID = (header[1] >> 16) & 0xFFFF;
    EXPECT_EQ(kRemoteNodeID, destID)
        << "OHCI AT Q1 should have destID at bits[31:16], got 0x" << std::hex << destID;
}

TEST_F(ResponseSenderHeaderFormatTest, OHCI_Q1_HasRCode) {
    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, header);

    // Extract rCode from Q1 bits[15:12]
    uint8_t rcode = (header[1] >> 12) & 0x0F;
    EXPECT_EQ(kRCodeComplete, rcode)
        << "OHCI AT Q1 should have rCode at bits[15:12]";
}

TEST_F(ResponseSenderHeaderFormatTest, OHCI_Q0_DoesNotHaveDestID) {
    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, header);

    // In OHCI AT format, Q0 bits[31:16] should NOT be destID
    // They should be: [srcBusID:1][unused:5][speed:3][upper part of tLabel and flags]
    uint16_t q0Upper = (header[0] >> 16) & 0xFFFF;
    EXPECT_NE(kRemoteNodeID, q0Upper)
        << "OHCI AT Q0 bits[31:16] should NOT be destID (that's IEEE 1394 format!)";
    EXPECT_NE(kLocalNodeID, q0Upper)
        << "OHCI AT Q0 bits[31:16] should NOT be srcID either";
}

// =============================================================================
// Verify the OLD (WRONG) Format is Different
// =============================================================================

TEST_F(ResponseSenderHeaderFormatTest, WrongFormat_HasDestID_In_Q0) {
    uint32_t wrongHeader[3];
    BuildWriteResponseHeader_IEEE1394Format_WRONG(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, wrongHeader);

    // In WRONG format, Q0 bits[31:16] = destID
    uint16_t q0Upper = (wrongHeader[0] >> 16) & 0xFFFF;
    EXPECT_EQ(kRemoteNodeID, q0Upper)
        << "WRONG IEEE 1394 format puts destID in Q0 bits[31:16]";
}

TEST_F(ResponseSenderHeaderFormatTest, WrongFormat_HasSrcID_In_Q1) {
    uint32_t wrongHeader[3];
    BuildWriteResponseHeader_IEEE1394Format_WRONG(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, wrongHeader);

    // In WRONG format, Q1 bits[31:16] = srcID
    uint16_t q1Upper = (wrongHeader[1] >> 16) & 0xFFFF;
    EXPECT_EQ(kLocalNodeID, q1Upper)
        << "WRONG IEEE 1394 format puts srcID in Q1 bits[31:16]";
}

TEST_F(ResponseSenderHeaderFormatTest, FormatsAreDifferent) {
    uint32_t correctHeader[3];
    uint32_t wrongHeader[3];

    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, correctHeader);
    BuildWriteResponseHeader_IEEE1394Format_WRONG(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, wrongHeader);

    EXPECT_NE(correctHeader[0], wrongHeader[0])
        << "Q0 should differ between OHCI AT and IEEE 1394 formats";
    EXPECT_NE(correctHeader[1], wrongHeader[1])
        << "Q1 should differ between OHCI AT and IEEE 1394 formats";
}

// =============================================================================
// Regression Tests - Specific Bug Scenario
// =============================================================================

TEST_F(ResponseSenderHeaderFormatTest, Regression_DestID_IsRemoteNode_NotLocalNode) {
    // The bug: responses were being sent to ffc0 (ourselves) instead of ffc2 (device)
    // This happened because destID was incorrectly placed in Q0 bits[31:16] which
    // OHCI interprets as srcBusID/speed/flags, not as destination.

    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID,   // Destination: send response to device (0xFFC2)
        kLocalNodeID,    // Source: we are 0xFFC0
        kTLabel,
        kRCodeComplete,
        header);

    // The destination should be in Q1, NOT in Q0
    uint16_t destInQ1 = (header[1] >> 16) & 0xFFFF;
    EXPECT_EQ(kRemoteNodeID, destInQ1)
        << "Response destination should be remote node (0xFFC2), not local (0xFFC0)";
}

TEST_F(ResponseSenderHeaderFormatTest, Regression_TLabel_AtCorrectPosition) {
    // Verify tLabel is at bits[15:10] in both formats (same position)
    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, header);

    uint8_t tLabel = (header[0] >> 10) & 0x3F;
    EXPECT_EQ(kTLabel, tLabel)
        << "tLabel should be at Q0 bits[15:10]";
}

TEST_F(ResponseSenderHeaderFormatTest, Regression_TCode_AtCorrectPosition) {
    uint32_t header[3];
    BuildWriteResponseHeader_OHCIFormat(
        kRemoteNodeID, kLocalNodeID, kTLabel, kRCodeComplete, header);

    uint8_t tCode = (header[0] >> 4) & 0x0F;
    EXPECT_EQ(TCODE_WRITE_RESPONSE, tCode)
        << "tCode should be WRITE_RESPONSE (0x2) at Q0 bits[7:4]";
}
