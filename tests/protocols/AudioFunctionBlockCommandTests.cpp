//
// AudioFunctionBlockCommandTests.cpp
// ASFW Tests
//
// Tests for Audio Function Block Command (0xB8)
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Protocols/AVC/AudioFunctionBlockCommand.hpp"
#include "Protocols/AVC/IAVCCommandSubmitter.hpp"

using namespace ASFW;
using namespace ASFW::Protocols::AVC;
using namespace testing;

// Mock IAVCCommandSubmitter
class MockAVCCommandSubmitter : public IAVCCommandSubmitter {
public:
    MOCK_METHOD(void, SubmitCommand, (const AVCCdb& cdb, AVCCompletion completion), (override));
};

class AudioFunctionBlockCommandTests : public Test {
protected:
    MockAVCCommandSubmitter mockSubmitter;
};

// Test: Set Volume (Control)
TEST_F(AudioFunctionBlockCommandTests, SetVolume_SendsCorrectCDB) {
    uint8_t subunitAddr = 0x08; // Audio Subunit 0
    uint8_t plugId = 0x01;
    int16_t volume = 0x7FFF; // 0dB
    
    std::vector<uint8_t> data;
    data.push_back(0x00); // Channel 0 (Master)
    data.push_back(0x02); // Data length
    data.push_back((volume >> 8) & 0xFF);
    data.push_back(volume & 0xFF);
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        mockSubmitter,
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kVolume,
        data
    );
    
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kControl));
            EXPECT_EQ(cdb.subunit, subunitAddr);
            EXPECT_EQ(cdb.opcode, 0xB8); // FUNCTION BLOCK
            
            // [0]=0x81 (Feature), [1]=PlugID, [2]=0x10 (Current), [3]=Len, [4]=Channel, [5]=Selector, [6+]=Data
            EXPECT_EQ(cdb.operands[0], 0x81);
            EXPECT_EQ(cdb.operands[1], plugId);
            EXPECT_EQ(cdb.operands[2], 0x10);
            EXPECT_EQ(cdb.operands[3], 1 + 4); // Selector + 4 bytes data (channel, len, vol_hi, vol_lo)
            EXPECT_EQ(cdb.operands[4], 0x00); // Channel
            EXPECT_EQ(cdb.operands[5], static_cast<uint8_t>(AudioFunctionBlockCommand::ControlSelector::kVolume));
            EXPECT_EQ(cdb.operands[6], 0x02); // Data length
            EXPECT_EQ(cdb.operands[7], 0x7F);
            EXPECT_EQ(cdb.operands[8], 0xFF);
            
            // Simulate success response
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kAccepted);
            completion(AVCResult::kAccepted, response);
        }));
        
    bool done = false;
    cmd->Submit([&](AVCResult result, const std::vector<uint8_t>& responseData) {
        EXPECT_EQ(result, AVCResult::kAccepted);
        done = true;
    });
    
    EXPECT_TRUE(done);
}

// Test: Set Mute (Control)
TEST_F(AudioFunctionBlockCommandTests, SetMute_SendsCorrectCDB) {
    uint8_t subunitAddr = 0x08; // Audio Subunit 0
    uint8_t plugId = 0x02;
    uint8_t muteVal = 0x70; // On
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        mockSubmitter,
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kMute,
        std::vector<uint8_t>{0x00, 0x01, muteVal} // Channel 0, Data length 1, Mute Value
    );
    
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.operands[4], 0x00); // Channel
            EXPECT_EQ(cdb.operands[5], static_cast<uint8_t>(AudioFunctionBlockCommand::ControlSelector::kMute));
            EXPECT_EQ(cdb.operands[6], 0x01); // Data length
            EXPECT_EQ(cdb.operands[7], 0x70); // Mute value
            
            completion(AVCResult::kAccepted, cdb);
        }));
        
    bool done = false;
    cmd->Submit([&](AVCResult result, const std::vector<uint8_t>&) {
        EXPECT_EQ(result, AVCResult::kAccepted);
        done = true;
    });
    
    EXPECT_TRUE(done);
}

// Test: Set Selector Block (Control) - e.g. Phase 88 Clock Source
TEST_F(AudioFunctionBlockCommandTests, SetClockSourceSelector_SendsCorrectCDB) {
    uint8_t subunitAddr = 0x08; // Audio Subunit 0
    uint8_t plugId = 0x09; // FB 9
    uint8_t sourceValue = 0x00; // Internal
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        mockSubmitter,
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        AudioFunctionBlockCommand::BlockType::kSelector,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kSelectorControl,
        std::vector<uint8_t>{sourceValue}
    );
    
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kControl));
            EXPECT_EQ(cdb.subunit, subunitAddr);
            EXPECT_EQ(cdb.opcode, 0xB8); // FUNCTION BLOCK
            
            // [0]=0x80 (Selector), [1]=PlugID, [2]=0x10 (Current), [3]=Len, [4]=InputPlug, [5]=Selector
            EXPECT_EQ(cdb.operands[0], 0x80); // Selector Block Type
            EXPECT_EQ(cdb.operands[1], plugId);
            EXPECT_EQ(cdb.operands[2], 0x10); // Current
            EXPECT_EQ(cdb.operands[3], 1 + 1); // Selector + 1 byte data
            EXPECT_EQ(cdb.operands[4], sourceValue); // Input Plug ID
            EXPECT_EQ(cdb.operands[5], static_cast<uint8_t>(AudioFunctionBlockCommand::ControlSelector::kSelectorControl));
            
            completion(AVCResult::kAccepted, cdb);
        }));
        
    bool done = false;
    cmd->Submit([&](AVCResult result, const std::vector<uint8_t>&) {
        EXPECT_EQ(result, AVCResult::kAccepted);
        done = true;
    });
    
    EXPECT_TRUE(done);
}
