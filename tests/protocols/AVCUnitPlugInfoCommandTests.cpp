//
// AVCUnitPlugInfoCommandTests.cpp
// Tests for AV/C Unit Plug Info Command (0x02)
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../../ASFWDriver/Protocols/AVC/AVCUnitPlugInfoCommand.hpp"
#include "../../ASFWDriver/Protocols/AVC/IAVCCommandSubmitter.hpp"
#include "../../ASFWDriver/Protocols/AVC/AVCDefs.hpp" // For AVCResponseType

using namespace ASFW::Protocols::AVC;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

// Mock Submitter
class MockAVCCommandSubmitter : public IAVCCommandSubmitter {
public:
    MOCK_METHOD(void, SubmitCommand, (const AVCCdb&, AVCCompletion), (override));
};

class AVCUnitPlugInfoCommandTests : public ::testing::Test {
protected:
    MockAVCCommandSubmitter mockSubmitter;
};

// Test successful parsing of a valid response (e.g. Duet style)
TEST_F(AVCUnitPlugInfoCommandTests, ParseValidResponse) {
    AVCUnitPlugInfoCommand cmd(mockSubmitter);

    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([](const AVCCdb& cdb, AVCCompletion completion) {
            // Check Command
            EXPECT_EQ(cdb.ctype, static_cast<uint8_t>(AVCCommandType::kStatus));
            EXPECT_EQ(cdb.subunit, 0xFF); // Unit
            EXPECT_EQ(cdb.opcode, 0x02);  // Plug Info
            EXPECT_EQ(cdb.operands[0], 0x00); // Subfunction

            // Build Response: [0]=Subfunc, [1]=IsoIn, [2]=IsoOut, [3]=ExtIn, [4]=ExtOut
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kImplementedStable);
            response.operandLength = 5;
            response.operands[0] = 0x00;
            response.operands[1] = 0x02; // 2 Iso Inputs
            response.operands[2] = 0x01; // 1 Iso Output
            response.operands[3] = 0x04; // 4 Ext Inputs
            response.operands[4] = 0x04; // 4 Ext Outputs
            
            completion(AVCResult::kImplementedStable, response);
        }));

    bool callbackCalled = false;
    cmd.Submit([&](AVCResult result, const UnitPlugCounts& counts) {
        callbackCalled = true;
        EXPECT_EQ(result, AVCResult::kImplementedStable);
        EXPECT_EQ(counts.isoInputPlugs, 2);
        EXPECT_EQ(counts.isoOutputPlugs, 1);
        EXPECT_EQ(counts.extInputPlugs, 4);
        EXPECT_EQ(counts.extOutputPlugs, 4);
        EXPECT_TRUE(counts.IsValid());
    });

    EXPECT_TRUE(callbackCalled);
}

// Test parsing of a response with 0 plugs (e.g. pure control unit)
TEST_F(AVCUnitPlugInfoCommandTests, ParseZeroPlugs) {
    AVCUnitPlugInfoCommand cmd(mockSubmitter);

    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([](const AVCCdb& cdb, AVCCompletion completion) {
            AVCCdb response = cdb;
            response.ctype = static_cast<uint8_t>(AVCResponseType::kImplementedStable);
            response.operandLength = 5;
            // All zeros
            for(int i=0; i<5; i++) response.operands[i] = 0;
            
            completion(AVCResult::kImplementedStable, response);
        }));

    bool callbackCalled = false;
    cmd.Submit([&](AVCResult result, const UnitPlugCounts& counts) {
        callbackCalled = true;
        EXPECT_EQ(counts.isoInputPlugs, 0);
        EXPECT_EQ(counts.isoOutputPlugs, 0);
        EXPECT_FALSE(counts.IsValid()); // Should be invalid if no iso plugs
    });

    EXPECT_TRUE(callbackCalled);
}

// Test failure handling
TEST_F(AVCUnitPlugInfoCommandTests, HandleFailure) {
    AVCUnitPlugInfoCommand cmd(mockSubmitter);

    EXPECT_CALL(mockSubmitter, SubmitCommand(_, _))
        .WillOnce(Invoke([](const AVCCdb&, AVCCompletion completion) {
            AVCCdb emptyResponse{};
            completion(AVCResult::kRejected, emptyResponse);
        }));

    bool callbackCalled = false;
    cmd.Submit([&](AVCResult result, const UnitPlugCounts& counts) {
        callbackCalled = true;
        EXPECT_EQ(result, AVCResult::kRejected);
        // Should return zeroed struct
        EXPECT_EQ(counts.isoInputPlugs, 0);
    });

    EXPECT_TRUE(callbackCalled);
}
