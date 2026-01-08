//
// ExtendedStreamFormatCommandTests.cpp
// ASFW Tests
//
// Tests for Extended Stream Format Information command (Opcode 0xBF)
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Protocols/AVC/ExtendedStreamFormatCommand.hpp"
#include "Protocols/AVC/AVCAddress.hpp"
#include "Protocols/AVC/AVCDefs.hpp"

using namespace ASFW;
using namespace ASFW::Protocols::AVC;
using namespace testing;

class ExtendedStreamFormatCommandTests : public Test {
protected:
    void SetUp() override {
        // Setup code
    }
};

// Test 1: Verify Command Structure for GetSupported
TEST_F(ExtendedStreamFormatCommandTests, BuildGetSupportedCommand) {
    // Target: Unit Plug 0 (Output)
    // Structure: [Opcode(BF), Subfunc(C0), PlugAddr(Unit, Out, 0), Status(FF)]
    
    ExtendedStreamFormatCommand cmd(
        ExtendedStreamFormatCommand::CommandType::kGetSupported,
        AVCAddress::UnitPlugAddress(PlugType::kOutput, 0)
    );

    std::vector<uint8_t> payload = cmd.BuildCommand();

    ASSERT_GE(payload.size(), 4);
    EXPECT_EQ(payload[0], 0xBF); // Opcode
    EXPECT_EQ(payload[1], 0xC0); // Subfunction: Single Plug
    // Plug Address: Unit(00), Output(01), Plug 0(00) -> But wait, structure depends on addressing mode
    // Let's assume standard Unit Plug Address format:
    // [0]: 0xBF
    // [1]: 0xC0
    // [2]: Plug Address Byte 1
    // [3]: Plug Address Byte 2
    // [4]: Status (0xFF)
    
    // We expect the implementation to handle the addressing details.
    // For now, let's verify the Opcode and Subfunc.
}

// Test 2: Verify Command Structure for GetCurrent
TEST_F(ExtendedStreamFormatCommandTests, BuildGetCurrentCommand) {
    ExtendedStreamFormatCommand cmd(
        ExtendedStreamFormatCommand::CommandType::kGetCurrent,
        AVCAddress::SubunitPlugAddress(0, PlugType::kInput, 2) // Subunit 0, Input Plug 2
    );

    std::vector<uint8_t> payload = cmd.BuildCommand();

    EXPECT_EQ(payload[0], 0xBF);
    EXPECT_EQ(payload[1], 0xC0);
}

// Test 3: Parse Supported Formats Response
TEST_F(ExtendedStreamFormatCommandTests, ParseSupportedFormats) {
    ExtendedStreamFormatCommand cmd(
        ExtendedStreamFormatCommand::CommandType::kGetSupported,
        AVCAddress::UnitPlugAddress(PlugType::kOutput, 0)
    );

    // Mock Response: [ResponseCode, Subfunc, PlugAddr..., Status(00), FormatInfo...]
    // Format Info: [Root(90), Level1(40), Count(2), Rate1(02=48k), Rate2(03=96k)]
    std::vector<uint8_t> response = {
        0x09, // ACCEPTED
        0xBF, 0xC0, 0x00, 0x00, 0x00, // Header + Address + Status(00=Supported)
        0x90, 0x40, // AM824 Compound
        0x02,       // Count = 2
        0x02, 0x00, // 48kHz
        0x03, 0x00  // 96kHz
    };

    bool success = cmd.ParseResponse(response);
    EXPECT_TRUE(success);

    auto formats = cmd.GetSupportedFormats();
    ASSERT_EQ(formats.size(), 2);
    EXPECT_EQ(formats[0].sampleRate, 48000);
    EXPECT_EQ(formats[1].sampleRate, 96000);
}
