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
            
            // [0]=0x81 (Feature), [1]=PlugID, [2]=0x10 (Current), [3]=Len, [4]=Selector, [5+]=Data
            EXPECT_EQ(cdb.operands[0], 0x81);
            EXPECT_EQ(cdb.operands[1], plugId);
            EXPECT_EQ(cdb.operands[2], 0x10);
            EXPECT_EQ(cdb.operands[3], 1 + 2); // Selector + 2 bytes data
            EXPECT_EQ(cdb.operands[4], static_cast<uint8_t>(AudioFunctionBlockCommand::ControlSelector::kVolume));
            EXPECT_EQ(cdb.operands[5], 0x7F);
            EXPECT_EQ(cdb.operands[6], 0xFF);
            
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
        std::vector<uint8_t>{muteVal}
    );
    
    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([&](const AVCCdb& cdb, AVCCompletion completion) {
            EXPECT_EQ(cdb.operands[4], static_cast<uint8_t>(AudioFunctionBlockCommand::ControlSelector::kMute));
            EXPECT_EQ(cdb.operands[5], 0x70);
            
            completion(AVCResult::kAccepted, cdb);
        }));
        
    bool done = false;
    cmd->Submit([&](AVCResult result, const std::vector<uint8_t>&) {
        EXPECT_EQ(result, AVCResult::kAccepted);
        done = true;
    });
    
    EXPECT_TRUE(done);
}
