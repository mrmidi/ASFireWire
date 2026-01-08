#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Isoch/IsochReceiveContext.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/OHCIConstants.hpp"

// Use a mock or stub for HardwareInterface
namespace ASFW::Driver {
// We can use the Stub linked from HardwareInterfaceStub.cpp
// Or define a mock here if we need to verify register writes
}

using namespace ASFW::Isoch;
using namespace ASFW::Shared;
using namespace ASFW::Async;
using namespace ASFW::Isoch::Memory;
using namespace testing;

class IsochReceiveContextTest : public Test {
protected:
    void SetUp() override {
        // Create dependencies
        hardware_ = new ::ASFW::Driver::HardwareInterface(); // Using the Stub
        
        IsochMemoryConfig config;
        config.numDescriptors = 512;
        config.packetSizeBytes = 4096;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 4096;
        
        // Use concrete class for testing explicit init, but pass as interface
        auto concreteMgr = IsochDMAMemoryManager::Create(config);
        ASSERT_TRUE(concreteMgr->Initialize(*hardware_));
        dmaMemory_ = concreteMgr;
        
        context_ = IsochReceiveContext::Create(hardware_, dmaMemory_);
        ASSERT_TRUE(context_);
    }

    void TearDown() override {
        if (context_) {
            context_->Stop();
            // context_->release(); // OSSharedPtr manages refcount? No, it's a smart pointer wrapper.
            // If Create returns OSSharedPtr, we should just let it destruct or reset.
            context_.reset();
        }
        if (hardware_) {
            delete hardware_;
            hardware_ = nullptr;
        }
    }

    ::ASFW::Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<IIsochDMAMemory> dmaMemory_;
    OSSharedPtr<IsochReceiveContext> context_;
};

TEST_F(IsochReceiveContextTest, Initialization) {
    EXPECT_TRUE(context_);
}

TEST_F(IsochReceiveContextTest, ConfigurationAllocatesRings) {
    // Configure Channel 0, Context 0
    auto kr = context_->Configure(0, 0);
    EXPECT_EQ(kr, kIOReturnSuccess);
    
    // Verify rings were allocated in Fake Memory
    // 512 descriptors * 16 bytes = 8192
    // 512 buffers * 4096 bytes = 2MB
    // Total should be > 2MB
    // Verify rings were allocated
    // Check Available logic? 
    // Total size should be allocated blocks.
    // Available should decrease?
    // IsochDMAMemoryManager uses cursors. 
    // Allocation happens at Configure->SetupRings time.
    
    // We can check TotalSize() which is sum of slabs.
    EXPECT_GT(dmaMemory_->TotalSize(), 2000000);
}

TEST_F(IsochReceiveContextTest, StartProgramsRegisters) {
    context_->Configure(0, 0);
    auto kr = context_->Start();
    EXPECT_EQ(kr, kIOReturnSuccess);
    
    // We would need to inspect HardwareInterface stub state to verify writes
    // Since we are using the simple Stub, we trust it returns success.
    // Ideally we'd use a MockHardwareInterface to verify ::Write calls.
}

TEST_F(IsochReceiveContextTest, PollProcessesPackets) {
    context_->Configure(0, 0);
    context_->Start();
    
    // Inject a packet into the buffer ring
    // 1. Get the descriptor ring allocation
    // 2. Modify descriptor 0 to have status != 0
    
    // For now, testing that Poll returns 0 on empty rings
    EXPECT_EQ(context_->Poll(), 0);
    
    // TODO: Advanced test - Write to FakeMemory to simulate packet arrival
    // This requires knowing the addresses allocated.
    // IsochReceiveContext doesn't expose rings directly.
}
