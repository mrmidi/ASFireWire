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
    const uint32_t processed = ring.DrainCompleted(*mem, [&](const IsochRxDmaRing::CompletedPacket& pkt) {
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

