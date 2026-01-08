
#include <gtest/gtest.h>
#include <memory>
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp" // For DMARegion defines

// Stubs needed for IsochDMAMemoryManager
// It uses ASFW_LOG
#include "Logging/Logging.hpp"

// We are linking against the driver sources + stubs.
// The stubs are in HostDriverKitStubs.hpp which is included by implementation files via wrapper.

// IsochDMAMemoryManager uses composition of DMAMemoryManager, which needs hardware interface
// #include "Testing/HardwareInterfaceStub.hpp" // Removed: implementation is linked, header is transitive

using namespace ASFW::Isoch::Memory;

class IsochDMAMemoryManagerTest : public ::testing::Test {
protected:
    ASFW::Driver::HardwareInterface hardware_;
    
    void SetUp() override {
        // Reset log config if needed
    }
};

TEST_F(IsochDMAMemoryManagerTest, AllocateSlabsSuccess) {
    IsochMemoryConfig config;
    config.numDescriptors = 16;
    config.packetSizeBytes = 1024;
    config.descriptorAlignment = 16;
    config.payloadPageAlignment = 4096;

    auto manager = IsochDMAMemoryManager::Create(config);
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->Initialize(hardware_));

    // Verify slabs exist (indirectly via TotalSize or Allocation)
    EXPECT_GT(manager->TotalSize(), 0);
}

TEST_F(IsochDMAMemoryManagerTest, DescriptorSlicing) {
    IsochMemoryConfig config;
    config.numDescriptors = 4;
    config.packetSizeBytes = 1024;
    config.descriptorAlignment = 16;
    config.payloadPageAlignment = 4096;

    auto manager = IsochDMAMemoryManager::Create(config);
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->Initialize(hardware_));

    // Allocate 4 descriptors
    auto d1 = manager->AllocateDescriptor(sizeof(uint64_t) * 4); // 32 bytes
    ASSERT_TRUE(d1.has_value());
    EXPECT_EQ(d1->size, 32);
    // Check alignment
    EXPECT_EQ(reinterpret_cast<uintptr_t>(d1->virtualBase) % 16, 0);

    auto d2 = manager->AllocateDescriptor(32);
    ASSERT_TRUE(d2.has_value());
    EXPECT_NE(d1->virtualBase, d2->virtualBase);
    
    // Check linear allocation
    EXPECT_EQ(d2->virtualBase, d1->virtualBase + 32);
}

TEST_F(IsochDMAMemoryManagerTest, PayloadSlicingAndPageAlignment) {
    IsochMemoryConfig config;
    config.numDescriptors = 2; // small slab
    config.packetSizeBytes = 4096; 
    config.descriptorAlignment = 16;
    config.payloadPageAlignment = 4096; // Request page alignment

    auto manager = IsochDMAMemoryManager::Create(config);
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->Initialize(hardware_));

    // Allocate Buffer 1
    auto b1 = manager->AllocatePayloadBuffer(4096);
    ASSERT_TRUE(b1.has_value());
    EXPECT_EQ(b1->size, 4096);
    
    // Check alignment of the generic slab base (impl detail: stub uses posix_memalign)
    // The slicing should also respect start logic
    // Implementation uses AlignCursorToIOVA, so it should be aligned relative to IOVA.
    // In Host Stub, IOVA = Virtual.
    EXPECT_EQ(reinterpret_cast<uintptr_t>(b1->virtualBase) % 4096, 0);

    // Allocate Buffer 2
    auto b2 = manager->AllocatePayloadBuffer(4096);
    ASSERT_TRUE(b2.has_value());
    EXPECT_EQ(b2->virtualBase, b1->virtualBase + 4096);
}

TEST_F(IsochDMAMemoryManagerTest, AllocationFailureOOM) {
    IsochMemoryConfig config;
    config.numDescriptors = 2; 
    config.packetSizeBytes = 100;
    config.descriptorAlignment = 16;
    config.payloadPageAlignment = 4096;

    auto manager = IsochDMAMemoryManager::Create(config);
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->Initialize(hardware_));

    // Slab size ~2 packets * 100 bytes + padding = 4096 (minimum page roundup in implementation)
    // Implementation uses 4096 minimum rounding logic in DMAMemoryManager.
    
    // Actually implementation calculates:
    // payloadSlabBytes = RoundUp(payloadBytesRaw + (config.payloadPageAlignment - 1), kMinSlabRounding);
    // payloadBytesRaw = 200.
    // 200 + 4095 = 4295 -> rounded to 4096?
    // Wait, RoundUp(4295, 4096) -> 4096. No, 4295 + 4095 ... 
    // RoundUp(v, align) -> (v + a - 1) & ~(a - 1).
    // (4295 + 4095) & ~4095 -> 8192.
    // So slab is 8192.
    
    // Let's allocate huge to force OOM
    auto b1 = manager->AllocatePayloadBuffer(8192);
    // It might succeed if calculated correctly.
    
    // Let's try to allocate MORE than total possible
    auto b3 = manager->AllocatePayloadBuffer(100000);
    EXPECT_FALSE(b3.has_value());
}

TEST_F(IsochDMAMemoryManagerTest, ExplicitApiEnforcement) {
    IsochMemoryConfig config;
    config.numDescriptors = 2;
    config.packetSizeBytes = 1024;
    config.descriptorAlignment = 16;
    config.payloadPageAlignment = 4096;

    auto manager = IsochDMAMemoryManager::Create(config);
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->Initialize(hardware_));

    // Base usage should fail
    auto region = manager->AllocateRegion(100, 16);
    EXPECT_FALSE(region.has_value());
}
