//
// AVCStreamFormatCommandTests.cpp
// ASFW Tests
//
// Tests for AVCStreamFormatCommand response parsing (opcode 0xBF)
// Validates the format offset fix for bug where subunit plugs had wrong offset
//
// Reference: FWA/discovery.txt captures from actual Apogee Duet device
// Reference: TA Document 2001002 - AV/C Stream Format Information Specification
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Protocols/AVC/StreamFormats/AVCStreamFormatCommands.hpp"
#include "Protocols/AVC/AVCCommand.hpp"
#include "Protocols/AVC/AVCDefs.hpp"

using namespace ASFW::Protocols::AVC;
using namespace ASFW::Protocols::AVC::StreamFormats;
using namespace testing;

//==============================================================================
// Mock Command Submitter for Testing
//==============================================================================

class MockAVCCommandSubmitter : public IAVCCommandSubmitter {
public:
    MOCK_METHOD(void, SubmitCommand, 
                (const AVCCdb& cdb, std::function<void(AVCResult, const AVCCdb&)> completion),
                (override));
};

//==============================================================================
// Test Fixtures
//==============================================================================

class AVCStreamFormatCommandTests : public Test {
protected:
    MockAVCCommandSubmitter mockSubmitter_;

    // Helper to build a mock response CDB from raw wire bytes
    // Wire format: [ctype][subunit][opcode][operands...]
    static AVCCdb BuildResponseCdb(const std::vector<uint8_t>& wireBytes) {
        AVCCdb cdb;
        if (wireBytes.size() >= 3) {
            cdb.ctype = wireBytes[0];
            cdb.subunit = wireBytes[1];
            cdb.opcode = wireBytes[2];
            cdb.operandLength = std::min(wireBytes.size() - 3, kAVCOperandMaxLength);
            for (size_t i = 0; i < cdb.operandLength; ++i) {
                cdb.operands[i] = wireBytes[3 + i];
            }
        }
        return cdb;
    }
};

//==============================================================================
// Unit Plug Format Query Tests (subunit = 0xFF)
//==============================================================================

// Real data from discovery.txt line 138:
// RSP: 0x0C 0xFF 0xBF 0xC0 0x00 0x00 0x00 0x00 0xFF 0x01 0x90 0x40 0x03 0x02 0x01 0x02 0x06
// C0 (current format), unit plug, format: Compound AM824 44.1kHz 2ch MBLA
TEST_F(AVCStreamFormatCommandTests, ParsesUnitPlugCurrentFormat_C0) {
    std::vector<uint8_t> response = {
        0x0C, 0xFF, 0xBF,       // STABLE response (0x0C = ImplementedStable), unit, opcode
        0xC0,                    // operands[0]: subfunction = current
        0x00, 0x00, 0x00, 0x00,  // operands[1-4]: plug addressing
        0xFF,                    // operands[5]: format_info_label
        0x01,                    // operands[6]: channel count
        0x90, 0x40, 0x03, 0x02, 0x01, 0x02, 0x06  // operands[7+]: format block
    };
    
    auto responseCdb = BuildResponseCdb(response);
    std::optional<AudioStreamFormat> parsedFormat;
    
    // Setup mock to capture the parsed response
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([&](const AVCCdb& cdb, auto completion) {
            completion(AVCResult::kImplementedStable, responseCdb);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0), true  // Unit input plug 0
    );
    
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kImplementedStable);
        parsedFormat = format;
    });
    
    ASSERT_TRUE(parsedFormat.has_value());
    EXPECT_EQ(parsedFormat->formatHierarchy, FormatHierarchy::kCompoundAM824);
    EXPECT_EQ(parsedFormat->sampleRate, SampleRate::k44100Hz);
}

// Real data from discovery.txt line 154:
// RSP: 0x0C 0xFF 0xBF 0xC1 0x00 0x00 0x00 0x00 0xFF 0x00 0x00 0x90 0x40 0x03 0x02 0x01 0x02 0x06
// C1 (supported format), unit plug, format starts at operands[8]
TEST_F(AVCStreamFormatCommandTests, ParsesUnitPlugSupportedFormat_C1) {
    std::vector<uint8_t> response = {
        0x0C, 0xFF, 0xBF,        // STABLE response, unit, opcode
        0xC1,                     // operands[0]: subfunction = supported
        0x00, 0x00, 0x00, 0x00,   // operands[1-4]: plug addressing
        0xFF,                     // operands[5]: format_info_label
        0x00, 0x00,               // operands[6-7]: reserved + list_index echo
        0x90, 0x40, 0x03, 0x02, 0x01, 0x02, 0x06  // operands[8+]: format block
    };
    
    auto responseCdb = BuildResponseCdb(response);
    std::optional<AudioStreamFormat> parsedFormat;
    
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([&](const AVCCdb& cdb, auto completion) {
            completion(AVCResult::kImplementedStable, responseCdb);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0), true, static_cast<uint8_t>(0)  // Unit input plug 0, list index 0
    );
    
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kImplementedStable);
        parsedFormat = format;
    });
    
    ASSERT_TRUE(parsedFormat.has_value());
    EXPECT_EQ(parsedFormat->formatHierarchy, FormatHierarchy::kCompoundAM824);
    EXPECT_EQ(parsedFormat->sampleRate, SampleRate::k44100Hz);
}

//==============================================================================
// Subunit Plug Format Query Tests (Music Subunit 0x60)
//==============================================================================

// Real data from discovery.txt line 387:
// RSP: 0x0C 0x60 0xBF 0xC0 0x00 0x01 0x00 0xFF 0xFF 0x01 0x90 0x40 0x03 0x02 0x01 0x02 0x06
// C0 (current format), music subunit plug
// BUG FIX: Format was being parsed starting at operands[6] instead of operands[7]
TEST_F(AVCStreamFormatCommandTests, ParsesSubunitPlugCurrentFormat_C0) {
    std::vector<uint8_t> response = {
        0x0C, 0x60, 0xBF,        // STABLE response, music subunit (0x60), opcode
        0xC0,                     // operands[0]: subfunction = current
        0x00,                     // operands[1]: plug_direction
        0x01, 0x00,               // operands[2-3]: plug_type, plug_num
        0xFF, 0xFF,               // operands[4-5]: format_info_label, reserved
        0x01,                     // operands[6]: ??? (this was being parsed as format!)
        0x90, 0x40, 0x03, 0x02, 0x01, 0x02, 0x06  // operands[7+]: format block
    };
    
    auto responseCdb = BuildResponseCdb(response);
    std::optional<AudioStreamFormat> parsedFormat;
    
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([&](const AVCCdb& cdb, auto completion) {
            completion(AVCResult::kImplementedStable, responseCdb);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0x60), static_cast<uint8_t>(0), true  // Music subunit input plug 0
    );
    
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kImplementedStable);
        parsedFormat = format;
    });
    
    ASSERT_TRUE(parsedFormat.has_value());
    // The critical assertion: format should be 0x90 0x40 (AM824 Compound), NOT 0x01 0x90
    EXPECT_EQ(parsedFormat->formatHierarchy, FormatHierarchy::kCompoundAM824);
    EXPECT_EQ(parsedFormat->sampleRate, SampleRate::k44100Hz);
}

// Subunit plug C1 (supported formats) - this was the main bug!
// The format offset was 7 for subunit but should be 8 (same as unit plugs)
TEST_F(AVCStreamFormatCommandTests, ParsesSubunitPlugSupportedFormat_C1) {
    // Simulated response for music subunit plug supported format query
    std::vector<uint8_t> response = {
        0x0C, 0x60, 0xBF,         // STABLE response, music subunit (0x60), opcode
        0xC1,                      // operands[0]: subfunction = supported
        0x00,                      // operands[1]: plug_direction
        0x01, 0x00,                // operands[2-3]: plug_type, plug_num
        0xFF, 0xFF,                // operands[4-5]: format_info_label, reserved
        0x00, 0xFF,                // operands[6-7]: reserved, list_index echo
        0x90, 0x40, 0x04, 0x02, 0x01, 0x02, 0x06  // operands[8+]: format block (48kHz)
    };
    
    auto responseCdb = BuildResponseCdb(response);
    std::optional<AudioStreamFormat> parsedFormat;
    
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([&](const AVCCdb& cdb, auto completion) {
            completion(AVCResult::kImplementedStable, responseCdb);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0x60), static_cast<uint8_t>(0), true, static_cast<uint8_t>(0)  // Music subunit input plug 0, list index 0
    );
    
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kImplementedStable);
        parsedFormat = format;
    });
    
    ASSERT_TRUE(parsedFormat.has_value());
    // BUG FIX VERIFICATION: Before fix, this would parse 0xFF 0x90 as the format
    // which would fail validation. After fix, correctly parses 0x90 0x40.
    EXPECT_EQ(parsedFormat->formatHierarchy, FormatHierarchy::kCompoundAM824);
    EXPECT_EQ(parsedFormat->sampleRate, SampleRate::k48000Hz);
}

//==============================================================================
// Simple Format Tests (Sync Stream)
//==============================================================================

// Real data from discovery.txt line 465: 3-byte simple format (sync stream)
// RSP: 0x0C 0x60 0xBF 0xC0 0x00 0x01 0x02 0xFF 0xFF 0x01 0x90 0x00 0x40
TEST_F(AVCStreamFormatCommandTests, ParsesSubunitPlug_SyncStream_3ByteFormat) {
    std::vector<uint8_t> response = {
        0x0C, 0x60, 0xBF,        // STABLE response, music subunit, opcode
        0xC0,                     // operands[0]: subfunction = current
        0x00,                     // operands[1]: plug_direction (input)
        0x01, 0x02,               // operands[2-3]: plug_type, plug_num (plug 2)
        0xFF, 0xFF,               // operands[4-5]: format_info_label, reserved
        0x01,                     // operands[6]: ???
        0x90, 0x00, 0x40          // operands[7-9]: 3-byte simple format
    };
    
    auto responseCdb = BuildResponseCdb(response);
    std::optional<AudioStreamFormat> parsedFormat;
    
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([&](const AVCCdb& cdb, auto completion) {
            completion(AVCResult::kImplementedStable, responseCdb);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0x60), static_cast<uint8_t>(2), true  // Music subunit input plug 2
    );
    
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kImplementedStable);
        parsedFormat = format;
    });
    
    ASSERT_TRUE(parsedFormat.has_value());
    EXPECT_EQ(parsedFormat->formatHierarchy, FormatHierarchy::kAM824);
    EXPECT_EQ(parsedFormat->subtype, AM824Subtype::kSimple);
    EXPECT_EQ(parsedFormat->sampleRate, SampleRate::kDontCare);
}

//==============================================================================
// Error Handling Tests
//==============================================================================

TEST_F(AVCStreamFormatCommandTests, ReturnsNulloptOnRejectedResponse) {
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([](const AVCCdb& cdb, auto completion) {
            AVCCdb response;
            completion(AVCResult::kRejected, response);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0), true
    );
    
    std::optional<AudioStreamFormat> parsedFormat;
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kRejected);
        parsedFormat = format;
    });
    
    EXPECT_FALSE(parsedFormat.has_value());
}

TEST_F(AVCStreamFormatCommandTests, ReturnsNulloptOnNotImplemented) {
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([](const AVCCdb& cdb, auto completion) {
            AVCCdb response;
            completion(AVCResult::kNotImplemented, response);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0), true
    );
    
    std::optional<AudioStreamFormat> parsedFormat;
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        EXPECT_EQ(result, AVCResult::kNotImplemented);
        parsedFormat = format;
    });
    
    EXPECT_FALSE(parsedFormat.has_value());
}

TEST_F(AVCStreamFormatCommandTests, ReturnsNulloptOnShortResponse) {
    // Response too short to contain format block
    std::vector<uint8_t> response = {
        0x0C, 0xFF, 0xBF,
        0xC0, 0x00
    };
    
    auto responseCdb = BuildResponseCdb(response);
    
    EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
        .WillOnce([&](const AVCCdb& cdb, auto completion) {
            completion(AVCResult::kImplementedStable, responseCdb);
        });
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(
        mockSubmitter_, static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0), true
    );
    
    std::optional<AudioStreamFormat> parsedFormat;
    cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
        parsedFormat = format;
    });
    
    EXPECT_FALSE(parsedFormat.has_value());
}

//==============================================================================
// Multi-Format Sample Rate Tests
//==============================================================================

// Test parsing all 4 sample rates that Apogee Duet supports
TEST_F(AVCStreamFormatCommandTests, ParsesAllApogeeDuetSampleRates) {
    const std::vector<std::pair<uint8_t, SampleRate>> rateTestCases = {
        {0x03, SampleRate::k44100Hz},
        {0x04, SampleRate::k48000Hz},
        {0x0A, SampleRate::k88200Hz},
        {0x05, SampleRate::k96000Hz}
    };
    
    for (const auto& [rateCode, expectedRate] : rateTestCases) {
        std::vector<uint8_t> response = {
            0x0C, 0xFF, 0xBF,
            0xC1,
            0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
            0x90, 0x40, rateCode, 0x02, 0x01, 0x02, 0x06
        };
        
        auto responseCdb = BuildResponseCdb(response);
        
        EXPECT_CALL(mockSubmitter_, SubmitCommand(_, _))
            .WillOnce([&](const AVCCdb& cdb, auto completion) {
                completion(AVCResult::kImplementedStable, responseCdb);
            });
        
        auto cmd = std::make_shared<AVCStreamFormatCommand>(
            mockSubmitter_, static_cast<uint8_t>(0xFF), static_cast<uint8_t>(0), true, static_cast<uint8_t>(0)
        );
        
        std::optional<AudioStreamFormat> parsedFormat;
        cmd->Submit([&](AVCResult result, const std::optional<AudioStreamFormat>& format) {
            parsedFormat = format;
        });
        
        ASSERT_TRUE(parsedFormat.has_value()) 
            << "Failed for rate code 0x" << std::hex << static_cast<int>(rateCode);
        EXPECT_EQ(parsedFormat->sampleRate, expectedRate)
            << "Wrong rate for code 0x" << std::hex << static_cast<int>(rateCode);
    }
}
