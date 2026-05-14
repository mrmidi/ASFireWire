#include <gtest/gtest.h>

#include "Isoch/Receive/IsochRxDmaRing.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp"

#include "Hardware/OHCIDescriptors.hpp"

using namespace ASFW::Isoch;
using namespace ASFW::Isoch::Rx;
using namespace ASFW::Isoch::Memory;

namespace {

std::shared_ptr<IIsochDMAMemory> MakeTestIsochMemory(::ASFW::Driver::HardwareInterface& hw,
                                                     size_t numDescriptors,
                                                     size_t packetSizeBytes) {
    IsochMemoryConfig config;
    config.numDescriptors = numDescriptors;
    config.packetSizeBytes = packetSizeBytes;
    config.descriptorAlignment = 16;
    config.payloadPageAlignment = 4096;

    auto concreteMgr = IsochDMAMemoryManager::Create(config);
    EXPECT_TRUE(concreteMgr);
    EXPECT_TRUE(concreteMgr->Initialize(hw));
    return concreteMgr;
}

} // namespace

TEST(IsochRxDmaRingTests, InitialCommandPtrWord_SetsZBitAndPointsToDesc0) {
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);

    const uint32_t cmdPtr = ring.InitialCommandPtrWord();
    EXPECT_NE(cmdPtr, 0u);
    EXPECT_EQ(cmdPtr & 0x1u, 0x1u);
    EXPECT_EQ(cmdPtr & ~0x1u, ring.Descriptor0IOVA());
}

TEST(IsochRxDmaRingTests, DrainCompleted_ProcessesOneDescriptorAndRearms) {
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);

    auto* d0 = ring.DescriptorAt(0);
    ASSERT_NE(d0, nullptr);

    auto* payload = static_cast<uint8_t*>(ring.PayloadVA(0));
    ASSERT_NE(payload, nullptr);

    payload[0] = 0x11;
    payload[1] = 0x22;
    payload[2] = 0x33;
    payload[3] = 0x44;

    constexpr uint16_t reqCount = 64;
    constexpr uint16_t actualLength = 16;
    d0->statusWord = (0x0000u << 16) | static_cast<uint16_t>(reqCount - actualLength);

    uint32_t calls = 0;
    const uint32_t processed = ring.DrainCompleted(*mem, [&calls, actualLength](const IsochRxDmaRing::CompletedPacket& pkt) {
        calls++;
        EXPECT_EQ(pkt.descriptorIndex, 0u);
        EXPECT_EQ(pkt.actualLength, actualLength);
        ASSERT_NE(pkt.payload, nullptr);
        EXPECT_EQ(pkt.payload[0], 0x11);
        EXPECT_EQ(pkt.payload[1], 0x22);
        EXPECT_EQ(pkt.payload[2], 0x33);
        EXPECT_EQ(pkt.payload[3], 0x44);
    });

    EXPECT_EQ(processed, 1u);
    EXPECT_EQ(calls, 1u);

    // Re-armed back to reqCount with xferStatus=0.
    mem->FetchFromDevice(d0, sizeof(*d0));
    EXPECT_EQ(ASFW::Async::HW::AR_xferStatus(*d0), 0u);
    EXPECT_EQ(ASFW::Async::HW::AR_resCount(*d0), reqCount);
}

// Helper: mark descriptor at index as "done" by setting resCount < reqCount.
static void SimulatePacketArrival(IsochRxDmaRing& ring, size_t index,
                                  uint16_t reqCount, uint16_t actualLength) {
    auto* desc = ring.DescriptorAt(index);
    ASSERT_NE(desc, nullptr);
    desc->statusWord = static_cast<uint16_t>(reqCount - actualLength); // xferStatus=0, resCount<reqCount
}

TEST(IsochRxDmaRingTests, DrainCompleted_EmptyRing_ReturnsZero) {
    // Fresh ring: all descriptors have xferStatus=0 and resCount==reqCount → none are done.
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    ring.ResetForStart();

    uint32_t calls = 0;
    const uint32_t processed = ring.DrainCompleted(*mem, [&calls](const IsochRxDmaRing::CompletedPacket&) {
        calls++;
    });

    EXPECT_EQ(processed, 0u);
    EXPECT_EQ(calls, 0u);
}

TEST(IsochRxDmaRingTests, DrainCompleted_MultipleDescriptors_ProcessedInOrder) {
    // Mark descriptors 0, 1, 2 as done; descriptor 3 stays empty.
    // DrainCompleted must deliver them in order 0→1→2 and stop at 3.
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    ring.ResetForStart();

    constexpr uint16_t reqCount = 64;
    constexpr uint16_t actualLength = 20;

    for (size_t i = 0; i < 3; ++i) {
        ASSERT_NO_FATAL_FAILURE(SimulatePacketArrival(ring, i, reqCount, actualLength));
    }

    std::vector<uint32_t> receivedIndices;
    const uint32_t processed = ring.DrainCompleted(*mem,
        [&receivedIndices, actualLength](const IsochRxDmaRing::CompletedPacket& pkt) {
            receivedIndices.push_back(pkt.descriptorIndex);
            EXPECT_EQ(pkt.actualLength, actualLength);
        });

    EXPECT_EQ(processed, 3u);
    ASSERT_EQ(receivedIndices.size(), 3u);
    EXPECT_EQ(receivedIndices[0], 0u);
    EXPECT_EQ(receivedIndices[1], 1u);
    EXPECT_EQ(receivedIndices[2], 2u);

    // Descriptor 3 must still be un-processed (re-draining returns 0).
    uint32_t second = ring.DrainCompleted(*mem, [](const IsochRxDmaRing::CompletedPacket&) {});
    EXPECT_EQ(second, 0u);
}

TEST(IsochRxDmaRingTests, DrainCompleted_StopsAtFirstIncompleteDescriptor) {
    // Descriptors 0 and 1 are done; 2 is NOT done.
    // DrainCompleted must stop at 2 and return exactly 2.
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    ring.ResetForStart();

    constexpr uint16_t reqCount = 64;
    ASSERT_NO_FATAL_FAILURE(SimulatePacketArrival(ring, 0, reqCount, 8));
    ASSERT_NO_FATAL_FAILURE(SimulatePacketArrival(ring, 1, reqCount, 8));
    // Descriptor 2: leave in fresh state (xferStatus=0, resCount=reqCount → NOT done).

    uint32_t processed = ring.DrainCompleted(*mem, [](const IsochRxDmaRing::CompletedPacket&) {});
    EXPECT_EQ(processed, 2u);
}

TEST(IsochRxDmaRingTests, SetupRings_ReuseSameSize_ReinitializesWithoutOOM) {
    // Calling SetupRings twice with identical parameters must succeed on the second call
    // (reinit path) without exhausting the bump-pointer allocator.
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    // Second call: same size → reinit path, not a new allocation.
    EXPECT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    // Capacity must remain unchanged.
    EXPECT_EQ(ring.Capacity(), 8u);
}

TEST(IsochRxDmaRingTests, SetupRings_DifferentSize_ReturnsUnsupported) {
    // Reconfiguring to a different size after initial setup is not supported.
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    EXPECT_EQ(ring.SetupRings(*mem, 16, 64), kIOReturnUnsupported);
}

TEST(IsochRxDmaRingTests, DrainCompleted_WrapsAroundRingBoundary) {
    // Use a 4-descriptor ring. Process descriptors 0–2, then mark descriptor 3 as done.
    // After the first DrainCompleted returns 3, the internal cursor sits at 3.
    // A second DrainCompleted must pick up descriptor 3 and wrap cursor back to 0.
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 4, 64);
    ASSERT_TRUE(mem);

    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 4, 64), kIOReturnSuccess);
    ring.ResetForStart();

    constexpr uint16_t reqCount = 64;

    // Mark 0, 1, 2 as done.
    for (size_t i = 0; i < 3; ++i) {
        ASSERT_NO_FATAL_FAILURE(SimulatePacketArrival(ring, i, reqCount, 4));
    }
    EXPECT_EQ(ring.DrainCompleted(*mem, [](const IsochRxDmaRing::CompletedPacket&) {}), 3u);

    // Now mark descriptor 3 (last slot) as done.
    ASSERT_NO_FATAL_FAILURE(SimulatePacketArrival(ring, 3, reqCount, 4));

    std::vector<uint32_t> indices;
    const uint32_t processed = ring.DrainCompleted(*mem,
        [&indices](const IsochRxDmaRing::CompletedPacket& pkt) {
            indices.push_back(pkt.descriptorIndex);
        });

    EXPECT_EQ(processed, 1u);
    ASSERT_EQ(indices.size(), 1u);
    EXPECT_EQ(indices[0], 3u);

    // After wrap, ring is empty again.
    EXPECT_EQ(ring.DrainCompleted(*mem, [](const IsochRxDmaRing::CompletedPacket&) {}), 0u);
}

