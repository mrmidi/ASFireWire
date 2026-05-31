// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// HardwareInterfaceOrderTests.cpp — Regression tests for CSRControl write order.

#include "Hardware/HardwareInterface.hpp"
#include "Hardware/RegisterMap.hpp"
#include "Testing/HostDriverKitStubs.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>

using namespace ASFW::Driver;
using testing::_;
using testing::Args;
using testing::InSequence;

class MockPCIDevice : public IOPCIDevice {
public:
    virtual ~MockPCIDevice() = default;
    MOCK_METHOD(void, MemoryWrite32, (uint8_t bar, uint64_t offset, uint32_t value), (override));
    MOCK_METHOD(void, MemoryRead32, (uint8_t bar, uint64_t offset, uint32_t* value), (override));
    MOCK_METHOD(kern_return_t, GetBARInfo, (uint8_t bar, uint8_t* index, uint64_t* size, uint8_t* type), (override));
    MOCK_METHOD(kern_return_t, Open, (IOService * owner), (override));
    MOCK_METHOD(void, Close, (IOService * owner), (override));
    
    // Default behaviors to satisfy polling loops
    void SetupDefaultReadBehavior() {
        ON_CALL(*this, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _))
            .WillByDefault([](uint8_t, uint64_t, uint32_t* val) {
                *val = 0x80000000; // Operation done
            });
            
        ON_CALL(*this, Open(_)).WillByDefault(testing::Return(kIOReturnSuccess));
    }
};

class HardwareInterfaceOrderTests : public ::testing::Test {
protected:
    void SetUp() override {
        mockDevice_ = new MockPCIDevice();
        mockDevice_->SetupDefaultReadBehavior();
        
        // We need to trick Attach into succeeding
        EXPECT_CALL(*mockDevice_, GetBARInfo(0, _, _, _))
            .WillOnce([](uint8_t, uint8_t* index, uint64_t* size, uint8_t* type) {
                *index = 0;
                *size = 4096;
                *type = 1; // M32
                return kIOReturnSuccess;
            });
        
        kern_return_t kr = hardware_.Attach(nullptr, mockDevice_);
        ASSERT_EQ(kr, kIOReturnSuccess) << "HardwareInterface::Attach failed";
    }

    HardwareInterface hardware_;
    MockPCIDevice* mockDevice_{nullptr}; // Owned by hardware_ after Attach
};

namespace {

TEST_F(HardwareInterfaceOrderTests, CompareSwapLocalIRMResource_WritesDataCompareControlInOrder) {
    InSequence seq;

    uint32_t selectCode = 0; // BUS_MANAGER_ID
    uint32_t compareValue = 0x3F;
    uint32_t newValue = 0x10;

    // OHCI 1.1 §5.5.1: Write sequence is kCSRData, kCSRCompareData, then kCSRControl.
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRData), newValue));
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRCompareData), compareValue));
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRControl), selectCode));
    // FlushPostedWrites() reads kHCControl
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    // Read kCSRControl until done
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = 0x80000000;
        });
    // Read old value from kCSRData
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRData), _))
        .WillOnce([compareValue](uint8_t, uint64_t, uint32_t* val) {
            *val = compareValue;
        });

    auto result = hardware_.CompareSwapLocalIRMResource(selectCode, compareValue, newValue);
    EXPECT_EQ(result.status, LocalCSRLockResult::Status::Success);
}

TEST_F(HardwareInterfaceOrderTests, WriteLocalIRMResource_WritesDataCompareControlInOrder) {
    InSequence seq;

    uint32_t selectCode = 1; // BANDWIDTH_AVAILABLE
    uint32_t value = 4000;
    uint32_t currentValue = 4915;

    // 1. ReadLocalIRMResource(selectCode) (called by WriteLocalIRMResource)
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRControl), selectCode));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = 0x80000000;
        });
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRData), _))
        .WillOnce([currentValue](uint8_t, uint64_t, uint32_t* val) {
            *val = currentValue;
        });

    // 2. Then WriteLocalIRMResource performs the atomic sequence:
    // OHCI 1.1 §5.5.1: Write sequence is kCSRData, kCSRCompareData, then kCSRControl.
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRData), value));
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRCompareData), currentValue));
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRControl), selectCode));
    // Final FlushPostedWrites()
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    // Final poll
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = 0x80000000;
        });

    auto result = hardware_.WriteLocalIRMResource(selectCode, value);
    EXPECT_EQ(result.status, LocalCSRLockResult::Status::Success);
}

} // namespace
