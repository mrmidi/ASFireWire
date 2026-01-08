//
// MusicSubunitTests.cpp
// ASFW Tests
//
// Tests for MusicSubunit integration (Capabilities Discovery)
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Protocols/AVC/Music/MusicSubunit.hpp"
#include "Protocols/AVC/IAVCCommandSubmitter.hpp"
#include "Protocols/AVC/AVCDefs.hpp"
#include "Protocols/AVC/StreamFormats/AVCStreamFormatCommands.hpp"

using namespace ASFW;
using namespace ASFW::Protocols::AVC;
using namespace ASFW::Protocols::AVC::Music;
using namespace ASFW::Protocols::AVC::StreamFormats;
using namespace testing;

// Mock IAVCCommandSubmitter
class MockAVCCommandSubmitter : public IAVCCommandSubmitter {
public:
    MOCK_METHOD(void, SubmitCommand, (const AVCCdb& cdb, AVCCompletion completion), (override));
};

class MusicSubunitTests : public Test {
protected:
    std::shared_ptr<MusicSubunit> subunit;
    MockAVCCommandSubmitter mockSubmitter;

    void SetUp() override {
        // Create Music Subunit (Audio, ID 0)
        subunit = std::make_shared<MusicSubunit>(AVCSubunitType::kMusic0C, 0);
    }

    // Helper to access private plugs_ member (since fixture is friend)
    void AddPlug(ASFW::Protocols::AVC::Music::MusicSubunit& subunit, uint8_t id, ASFW::Protocols::AVC::StreamFormats::PlugDirection dir) {
        ASFW::Protocols::AVC::StreamFormats::PlugInfo plug;
        plug.plugID = id;
        plug.direction = dir;
        subunit.plugs_.push_back(plug);
    }
};

// Test: QuerySupportedFormats should send 0xBF command
TEST_F(MusicSubunitTests, QuerySupportedFormats_Sends0xBF) {
    // Expect SubmitCommand to be called with 0xBF
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillRepeatedly(Invoke([](const AVCCdb& cdb, AVCCompletion completion) {
            // Verify Opcode is 0xBF (Stream Format Support)
            EXPECT_EQ(cdb.opcode, 0xBF);
            
            // Verify Subfunction is 0xC1 (Supported)
            EXPECT_EQ(cdb.operands[0], 0xC1);

            // Simulate a response
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kAccepted); // Accepted
            
            // Add a dummy format to the response so it stops iterating
            // Format: [0]=0x90 (AM824), [1]=0x40 (Compound), [2]=0x02 (48k), [3]=0x00...
            // Offset 7 for subunit plug
            response.operandLength = 7 + 4; 
            response.operands[7] = 0x90;
            response.operands[8] = 0x40;
            response.operands[9] = 0x02;
            response.operands[10] = 0x00;

            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    subunit->QuerySupportedFormats(mockSubmitter, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
}

// Test: SetSampleRate should send 0xBF command with 0xC0 subfunction (Set)
// Note: Set format uses the same opcode (0xBF) but different subfunction/operands
// Actually, to set format, we use 0xC0 (Current) but with WRITE transaction?
// Or is it a CONTROL command?
// Extended Stream Format Spec says:
// To set format: CONTROL command with opcode 0xBF, subfunction 0xC0 (Current)
TEST_F(MusicSubunitTests, SetSampleRate_Sends0xBF_Control) {
    // Expect command submission
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kControl));
            EXPECT_EQ(cdb.opcode, 0xBF); // Output Plug Signal Format
            EXPECT_EQ(cdb.operands[0], 0xC0); // Current
            
            // Verify plug address fields
            // [1]=Direction(1=Output), [2]=Type(1=Subunit), [3]=ID(0), [4]=Label(FF), [5]=Reserved(FF)
            EXPECT_EQ(cdb.operands[1], 0x01); // Output
            EXPECT_EQ(cdb.operands[2], 0x01); // Subunit plug
            EXPECT_EQ(cdb.operands[3], 0x00); // Plug 0
            
            // Verify format in operands (starts at offset 6)
            // [6]=0x90 (AM824), [7]=0x40 (Compound), [8]=0x04 (48k), [9]=0x00, [10]=0x00 (0 channels)
            EXPECT_EQ(cdb.operands[6], 0x90);
            EXPECT_EQ(cdb.operands[7], 0x40);
            EXPECT_EQ(cdb.operands[8], 0x04); // 48kHz
            
            // Simulate a response (ACCEPTED)
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kAccepted);
            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    
    // Populate plugs_ so SetSampleRate has something to work with
    AddPlug(*subunit, 0, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kOutput);

    subunit->SetSampleRate(mockSubmitter, 48000, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
}

// Test: QueryConnections should send 0x1A command for Input plugs
TEST_F(MusicSubunitTests, QueryConnections_Sends0x1A_Status) {
    // Add an Input plug (Destination)
    AddPlug(*subunit, 0, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kInput);
    
    // Add an Output plug (Source) - should NOT be queried
    AddPlug(*subunit, 1, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kOutput);

    // Expect command submission for Input plug only
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kStatus));
            EXPECT_EQ(cdb.opcode, 0x1A); // SIGNAL SOURCE
            
            // Verify operands
            // [0]=0xFF (Output Status), [1]=0xFF, [2]=0xFF (Conv Data)
            // [3]=0x00 (Subunit Plug), [4]=0x00 (Plug ID 0)
            EXPECT_EQ(cdb.operands[0], 0xFF);
            EXPECT_EQ(cdb.operands[3], 0x00);
            EXPECT_EQ(cdb.operands[4], 0x00);
            
            // Simulate response: Connected to Unit Plug 0 (Iso)
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kImplementedStable); // Stable/Implemented
            
            // Response format:
            // [0]=OutputStatus, [1-2]=ConvData
            // [3]=SourcePlugType (0x01=Unit), [4]=SourcePlugID (0x00)
            // [5]=DestPlugType (0x00), [6]=DestPlugID (0x00)
            response.operandLength = 7;
            response.operands[3] = 0x01; // Unit plug
            response.operands[4] = 0x00; // Plug 0
            response.operands[5] = 0x00; // Subunit plug
            response.operands[6] = 0x00; // Plug 0
            
            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    subunit->QueryConnections(mockSubmitter, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
}
// Test: QueryConnections should retry with Unit address if Subunit returns kNotImplemented
TEST_F(MusicSubunitTests, QueryConnections_RetryWithUnit) {
    // Add an Input plug
    AddPlug(*subunit, 0, ASFW::Protocols::AVC::StreamFormats::PlugDirection::kInput);

    // Expect TWO command submissions
    // 1. To Subunit (returns kNotImplemented)
    // 2. To Unit (returns kAccepted)

    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            // First call: To Subunit
            EXPECT_EQ(cdb.subunit, 0x60); // Music Subunit (0x0C << 3) | 0
            EXPECT_EQ(cdb.opcode, 0x1A);
            
            // Return Not Implemented
            completion(AVCResult::kNotImplemented, cdb);
        }))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            // Second call: To Unit
            EXPECT_EQ(cdb.subunit, 0xFF); // Unit Address (0xFF)
            EXPECT_EQ(cdb.opcode, 0x1A);
            
            // Verify operands (asking about Subunit Plug 0)
            // [3]=0x00 (Subunit Plug), [4]=0x00 (Plug ID 0)
            EXPECT_EQ(cdb.operands[3], 0x00);
            EXPECT_EQ(cdb.operands[4], 0x00);

            // Simulate response: Connected to Unit Plug 0
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kImplementedStable);
            response.operandLength = 7;
            response.operands[3] = 0x01; // Unit plug
            response.operands[4] = 0x00; // Plug 0
            
            completion(AVCResult::kAccepted, response);
        }));

    bool done = false;
    subunit->QueryConnections(mockSubmitter, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });

    EXPECT_TRUE(done);
    
    // Verify plug info updated
    auto plugs = subunit->GetPlugs();
    ASSERT_EQ(plugs.size(), 1);
    EXPECT_TRUE(plugs[0].connectionInfo.has_value());
    EXPECT_EQ(plugs[0].connectionInfo->sourceSubunitType, ASFW::Protocols::AVC::StreamFormats::SourceSubunitType::kUnit);
    EXPECT_EQ(plugs[0].connectionInfo->sourcePlugNumber, 0);
}

// Test: SetAudioVolume should send 0xB8 command to Audio Subunit (0x08)
TEST_F(MusicSubunitTests, SetAudioVolume_SendsCorrectCDB) {
    uint8_t plugId = 0x01;
    int16_t volume = 0x7FFF; // 0dB
    
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kControl));
            // Target Audio Subunit 0 (0x01 << 3 | 0 = 0x08)
            EXPECT_EQ(cdb.subunit, 0x08); 
            EXPECT_EQ(cdb.opcode, 0xB8); // FUNCTION BLOCK
            
            // [0]=0x81 (Feature), [1]=PlugID, [2]=0x10 (Current), [3]=Len, [4]=Selector, [5+]=Data
            EXPECT_EQ(cdb.operands[0], 0x81);
            EXPECT_EQ(cdb.operands[1], plugId);
            EXPECT_EQ(cdb.operands[4], 0x02); // Volume
            
            completion(AVCResult::kAccepted, cdb);
        }));
        
    bool done = false;
    subunit->SetAudioVolume(mockSubmitter, plugId, volume, [&](bool success) {
        EXPECT_TRUE(success);
        done = true;
    });
    
    EXPECT_TRUE(done);
}
