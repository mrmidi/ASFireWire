//
//  AVCHandlerTests.cpp
//  ASFW Tests
//
//  Tests for AVCHandler using MockAVCDiscovery
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "UserClient/Handlers/AVCHandler.hpp"
#include "Protocols/AVC/IAVCDiscovery.hpp"
#include "Protocols/AVC/AVCUnit.hpp"
#include "Shared/SharedDataModels.hpp"
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

using namespace ASFW;
using namespace ASFW::UserClient;
using namespace ASFW::Protocols::AVC;
using namespace ASFW::Shared;
using namespace testing;

// Mock IAVCDiscovery
class MockAVCDiscovery : public IAVCDiscovery {
public:
    MOCK_METHOD(std::shared_ptr<AVCUnit>, Unit, (uint64_t guid), (override));
    MOCK_METHOD(std::vector<std::shared_ptr<AVCUnit>>, Units, (), (override));
    MOCK_METHOD(void, ReScanAllUnits, (), (override));
    MOCK_METHOD(FCPTransport*, GetFCPTransportForNodeID, (uint16_t nodeID), (override));
    MOCK_METHOD(std::shared_ptr<FCPTransport>, AcquireFCPTransportForNodeID, (uint16_t nodeID), (override));
};

// Test Fixture
class AVCHandlerTests : public Test {
protected:
    MockAVCDiscovery mockDiscovery;
    std::unique_ptr<AVCHandler> handler;
    
    // Helper to create IOUserClientMethodArguments
    IOUserClientMethodArguments args{};
    
    void SetUp() override {
        handler = std::make_unique<AVCHandler>(&mockDiscovery);
        // Reset args
        std::memset(&args, 0, sizeof(args));
    }
    
    void TearDown() override {
        if (args.structureOutput) {
            args.structureOutput->release();
            args.structureOutput = nullptr;
        }
    }
};

// Test: GetAVCUnits with no units
TEST_F(AVCHandlerTests, GetAVCUnits_NoUnits) {
    EXPECT_CALL(mockDiscovery, Units())
        .WillOnce(Return(std::vector<std::shared_ptr<AVCUnit>>{}));
    
    kern_return_t ret = handler->GetAVCUnits(&args);
    
    EXPECT_EQ(ret, kIOReturnSuccess);
    ASSERT_NE(args.structureOutput, nullptr);
    
    // Verify data: should contain just the count (0)
    EXPECT_EQ(args.structureOutput->getLength(), sizeof(uint32_t));
    
    const uint32_t* countPtr = static_cast<const uint32_t*>(args.structureOutput->getBytesNoCopy());
    EXPECT_EQ(*countPtr, 0);
}

// Test: ReScanAVCUnits calls discovery
TEST_F(AVCHandlerTests, ReScanAVCUnits_CallsDiscovery) {
    EXPECT_CALL(mockDiscovery, ReScanAllUnits()).Times(1);
    
    kern_return_t ret = handler->ReScanAVCUnits(&args);
    EXPECT_EQ(ret, kIOReturnSuccess);
}

// Test: GetSubunitDescriptor with missing inputs returns kIOReturnBadArgument
TEST_F(AVCHandlerTests, GetSubunitDescriptor_MissingInputs_ReturnsBadArgument) {
    uint64_t scalarInputs[2] = {0, 0};
    args.scalarInput = scalarInputs;
    args.scalarInputCount = 2;

    kern_return_t ret = handler->GetSubunitDescriptor(&args);
    EXPECT_EQ(ret, kIOReturnBadArgument);
}

// Test: GetSubunitDescriptor with subunit not found returns kIOReturnNotFound
TEST_F(AVCHandlerTests, GetSubunitDescriptor_SubunitNotFound_ReturnsNotFound) {
    uint64_t scalarInputs[4] = {0x0003, 0xDB000001, 0x01, 0x00};
    args.scalarInput = scalarInputs;
    args.scalarInputCount = 4;

    EXPECT_CALL(mockDiscovery, Units()).WillOnce(Return(std::vector<std::shared_ptr<AVCUnit>>{}));

    kern_return_t ret = handler->GetSubunitDescriptor(&args);
    EXPECT_EQ(ret, kIOReturnNotFound);
}
