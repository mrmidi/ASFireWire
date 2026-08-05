// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// HardwareInterfaceOrderTests.cpp — Regression tests for CSRControl write order and cycle master.

#include "Hardware/HardwareInterface.hpp"
#include "Hardware/IEEE1394.hpp"
#include "Hardware/RegisterMap.hpp"
#include "Bus/IRM/IRMCSRConstants.hpp"
#include "Testing/HostDriverKitStubs.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace ASFW::Driver;
using testing::_;
using testing::Args;
using testing::InSequence;
using testing::Return;

class MockPCIDevice : public IOPCIDevice {
public:
    virtual ~MockPCIDevice() = default;
    MOCK_METHOD(void, MemoryWrite32, (uint8_t bar, uint64_t offset, uint32_t value), (override));
    MOCK_METHOD(void, MemoryRead32, (uint8_t bar, uint64_t offset, uint32_t* value), (override));
    MOCK_METHOD(kern_return_t, GetBARInfo, (uint8_t bar, uint8_t* index, uint64_t* size, uint8_t* type), (override));
    MOCK_METHOD(kern_return_t, Open, (IOService * owner), (override));
    MOCK_METHOD(void, Close, (IOService * owner), (override));
};

class HardwareInterfaceOrderTests : public ::testing::Test {
protected:
    void SetUp() override {
        mockDevice_ = new MockPCIDevice();
        
        // Default behaviors
        ON_CALL(*mockDevice_, Open(_)).WillByDefault(Return(kIOReturnSuccess));
        ON_CALL(*mockDevice_, GetBARInfo(0, _, _, _))
            .WillByDefault([](uint8_t, uint8_t* index, uint64_t* size, uint8_t* type) {
                *index = 0;
                *size = 4096;
                *type = 1; // M32
                return kIOReturnSuccess;
            });

        // Default done bit for polling loops
        ON_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _))
            .WillByDefault([](uint8_t, uint64_t, uint32_t* val) {
                *val = 0x80000000;
            });

        hardware_.Attach(nullptr, mockDevice_);
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
    
    // Flush
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    
    // Poll loop
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _));
    
    // Read old value
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRData), _))
        .WillOnce([compareValue](uint8_t, uint64_t, uint32_t* val) {
            *val = compareValue;
        });

    auto result = hardware_.CompareSwapLocalIRMResource(selectCode, compareValue, newValue);
    EXPECT_EQ(result.status, LocalCSRLockResult::Status::Success);
    EXPECT_TRUE(result.compareMatched);
}

TEST_F(HardwareInterfaceOrderTests, WriteLocalIRMResource_WritesDataCompareControlInOrder) {
    InSequence seq;

    uint32_t selectCode = 1; // BANDWIDTH_AVAILABLE
    uint32_t value = 4000;
    uint32_t currentValue = 4915;

    // 1. ReadLocalIRMResource (pre-read)
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRControl), selectCode));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRData), _))
        .WillOnce([currentValue](uint8_t, uint64_t, uint32_t* val) {
            *val = currentValue;
        });

    // 2. Atomic Swap
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRData), value));
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRCompareData), currentValue));
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRControl), selectCode));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRData), _))
        .WillOnce([currentValue](uint8_t, uint64_t, uint32_t* val) {
            *val = currentValue;
        });

    // 3. Verification Read
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kCSRControl), selectCode));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kCSRData), _))
        .WillOnce([value](uint8_t, uint64_t, uint32_t* val) {
            *val = value;
        });

    auto result = hardware_.WriteLocalIRMResource(selectCode, value);
    EXPECT_EQ(result.status, LocalCSRLockResult::Status::Success);
}

TEST_F(HardwareInterfaceOrderTests, SetLocalCycleMasterEnabled_InOrder) {
    InSequence seq;

    // Set path
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kLinkControlSet), LinkControlBits::kCycleMaster));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    
    // Readback verification
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kLinkControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = LinkControlBits::kCycleMaster;
        });

    EXPECT_TRUE(hardware_.SetLocalCycleMasterEnabled(true));

    // Clear path
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kLinkControlClear), LinkControlBits::kCycleMaster));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    
    // Readback verification
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kLinkControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = 0;
        });

    EXPECT_TRUE(hardware_.SetLocalCycleMasterEnabled(false));
}

TEST_F(HardwareInterfaceOrderTests, RevokeBlocksAllFurtherBarAccess) {
    hardware_.RevokeAndDrain();

    EXPECT_CALL(*mockDevice_, MemoryRead32(_, _, _)).Times(0);
    EXPECT_CALL(*mockDevice_, MemoryWrite32(_, _, _)).Times(0);
    EXPECT_FALSE(hardware_.IsAvailable());
    auto access = hardware_.TryBeginAccess();
    EXPECT_FALSE(access);
    hardware_.SetInterruptMask(0xFFFFFFFFu, false);
}

TEST_F(HardwareInterfaceOrderTests, ProviderRevocationLatchesHardwareGoneAndBlocksAllBarAccess) {
    hardware_.LatchProviderRevokedAndDrain();

    EXPECT_CALL(*mockDevice_, MemoryRead32(_, _, _)).Times(0);
    EXPECT_CALL(*mockDevice_, MemoryWrite32(_, _, _)).Times(0);
    EXPECT_TRUE(hardware_.HardwareGone());
    EXPECT_EQ(hardware_.GoneReason(), HardwareGoneReason::kProviderRevoked);
    EXPECT_FALSE(hardware_.TryBeginAccess());
}

TEST_F(HardwareInterfaceOrderTests, RevokeWaitsForActiveAccessScope) {
    std::atomic<bool> scopeEntered{false};
    std::atomic<bool> releaseScope{false};

    std::thread worker([&] {
        auto access = hardware_.TryBeginAccess();
        ASSERT_TRUE(access);
        scopeEntered.store(true, std::memory_order_release);
        while (!releaseScope.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    while (!scopeEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto revoke = std::async(std::launch::async, [&] { hardware_.RevokeAndDrain(); });
    EXPECT_EQ(revoke.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);

    releaseScope.store(true, std::memory_order_release);
    EXPECT_EQ(revoke.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    worker.join();
    EXPECT_FALSE(hardware_.TryBeginAccess());
}

TEST_F(HardwareInterfaceOrderTests, AllOnesPresenceProbeLatchesHardwareGoneAndFencesLaterMmio) {
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* value) { *value = 0xFFFFFFFFu; });
    EXPECT_CALL(*mockDevice_, MemoryWrite32(_, _, _)).Times(0);

    auto access = hardware_.TryBeginAccess();
    ASSERT_TRUE(access);
    EXPECT_EQ(access.Read(Register32::kHCControl), 0xFFFFFFFFu);
    EXPECT_TRUE(hardware_.HardwareGone());
    EXPECT_EQ(hardware_.GoneReason(), HardwareGoneReason::kMmioPresenceProbeAllOnes);

    // The current scope is still responsible for releasing the gate lock, but
    // it cannot submit another BAR operation after the faulting read.
    access.Write(Register32::kIntMaskClear, 0xFFFFFFFFu);
    access = {};
    EXPECT_FALSE(hardware_.TryBeginAccess());
}

TEST_F(HardwareInterfaceOrderTests, InitialChannelsLowAllOnesIsNotProviderRemoval) {
    using namespace ASFW::Driver::IRMCSR;

    InSequence seq;
    const auto expectFlush = [this] {
        EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _))
            .WillOnce([](uint8_t, uint64_t, uint32_t* value) { *value = 0; });
    };

    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kInitialBandwidthAvailable),
                                             kInitialBandwidthAvailable));
    expectFlush();
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kInitialChannelsAvailableHi),
                                             kInitialChannelsAvailableHi));
    expectFlush();
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kInitialChannelsAvailableLo),
                                             kInitialChannelsAvailableLo));
    expectFlush();
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kInitialBandwidthAvailable), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* value) { *value = kInitialBandwidthAvailable; });
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kInitialChannelsAvailableHi), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* value) { *value = kInitialChannelsAvailableHi; });
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kInitialChannelsAvailableLo), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* value) { *value = kInitialChannelsAvailableLo; });

    EXPECT_EQ(hardware_.ProgramInitialIRMResourceRegisters(), kIOReturnSuccess);
    EXPECT_FALSE(hardware_.HardwareGone());
    EXPECT_TRUE(hardware_.TryBeginAccess());
}

TEST_F(HardwareInterfaceOrderTests, ScopeBatchesMultipleRegisterOperations) {
    InSequence seq;
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kIntEventClear), 0x1U));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kIntEvent), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* value) { *value = 0x2U; });
    EXPECT_CALL(*mockDevice_, MemoryWrite32(0, static_cast<uint64_t>(Register32::kIntMaskSet), 0x4U));

    auto access = hardware_.TryBeginAccess();
    ASSERT_TRUE(access);
    access.Write(Register32::kIntEventClear, 0x1U);
    EXPECT_EQ(access.Read(Register32::kIntEvent), 0x2U);
    access.Write(Register32::kIntMaskSet, 0x4U);
}

TEST_F(HardwareInterfaceOrderTests, SetRootHoldOffFalseClearsRhbWithoutIssuingBusReset) {
    InSequence seq;

    constexpr uint8_t reg1WithRhb = kPhyRootHoldOff | kPhyGapCountMask;
    constexpr uint8_t reg1Cleared = kPhyGapCountMask;

    // Read PHY register 1.
    EXPECT_CALL(*mockDevice_,
                MemoryWrite32(0, static_cast<uint64_t>(Register32::kPhyControl), 0x8100u));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kPhyControl), _))
        .WillOnce([=](uint8_t, uint64_t, uint32_t* val) {
            *val = 0x80000000u | (static_cast<uint32_t>(reg1WithRhb) << 16);
        });

    // Clear RHB. This must not set IBR (bit 6), which would request another reset.
    EXPECT_CALL(*mockDevice_,
                MemoryWrite32(0, static_cast<uint64_t>(Register32::kPhyControl),
                              0x4000u | (static_cast<uint32_t>(kPhyReg1Address) << 8) |
                                  reg1Cleared));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kPhyControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = 0;
        });

    hardware_.SetRootHoldOff(false);
}

TEST_F(HardwareInterfaceOrderTests, SetRootHoldOffTrueSetsRhbPreservingGap) {
    InSequence seq;

    constexpr uint8_t reg1WithoutRhb = kPhyGapCountMask;
    constexpr uint8_t reg1WithRhb = kPhyRootHoldOff | kPhyGapCountMask;

    EXPECT_CALL(*mockDevice_,
                MemoryWrite32(0, static_cast<uint64_t>(Register32::kPhyControl), 0x8100u));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kPhyControl), _))
        .WillOnce([=](uint8_t, uint64_t, uint32_t* val) {
            *val = 0x80000000u | (static_cast<uint32_t>(reg1WithoutRhb) << 16);
        });

    EXPECT_CALL(*mockDevice_,
                MemoryWrite32(0, static_cast<uint64_t>(Register32::kPhyControl),
                              0x4000u | (static_cast<uint32_t>(kPhyReg1Address) << 8) |
                                  reg1WithRhb));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kHCControl), _));
    EXPECT_CALL(*mockDevice_, MemoryRead32(0, static_cast<uint64_t>(Register32::kPhyControl), _))
        .WillOnce([](uint8_t, uint64_t, uint32_t* val) {
            *val = 0;
        });

    hardware_.SetRootHoldOff(true);
}

} // namespace
