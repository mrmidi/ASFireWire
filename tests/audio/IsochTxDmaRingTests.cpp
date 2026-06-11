// IsochTxDmaRingTests.cpp
// ASFW - Host-safe unit tests for IT DMA ring engine

#include <gtest/gtest.h>
#include <memory>
#include <cstring>

#include "Isoch/Transmit/IsochTxDmaRing.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/OHCIConstants.hpp"
#include "Shared/Isoch/IsochAudioTransport.hpp"

using ASFW::Isoch::Tx::IsochTxDmaRing;
using ASFW::Isoch::Tx::Layout;
using ASFW::Isoch::Memory::IsochDMAMemoryManager;
using ASFW::Isoch::Memory::IsochMemoryConfig;
using ASFW::IsochTransport::TxPacketMeta;
using ASFW::IsochTransport::TxStreamControl;
using ASFW::IsochTransport::TxStreamStatus;
using ASFW::IsochTransport::ExpectedCommitGen;
using ASFW::IsochTransport::SlotIndexFor;
using ASFW::Async::HW::OHCIDescriptor;
using ASFW::Async::HW::OHCIDescriptorImmediate;
using ASFW::Driver::Register32;

class IsochTxDmaRingTest : public ::testing::Test {
protected:
    ASFW::Driver::HardwareInterface hardware_;
    std::shared_ptr<IsochDMAMemoryManager> dmaMemory_;
    IsochTxDmaRing ring_;

    void SetUp() override {
        IsochMemoryConfig config;
        config.numDescriptors = Layout::kRingBlocks;
        config.packetSizeBytes = Layout::kMaxPacketSize;
        config.descriptorAlignment = Layout::kOHCIPageSize;
        config.payloadPageAlignment = 16384;

        dmaMemory_ = IsochDMAMemoryManager::Create(config);
        ASSERT_NE(dmaMemory_, nullptr);
        ASSERT_TRUE(dmaMemory_->Initialize(hardware_));

        ring_.SetChannel(1);
        ASSERT_EQ(ring_.SetupRings(*dmaMemory_), kIOReturnSuccess);
    }
};

TEST_F(IsochTxDmaRingTest, PrimeInitializesStaticDescriptorChain) {
    auto stats = ring_.Prime();
    EXPECT_EQ(stats.packetsAssembled, Layout::kNumPackets);

    // Verify a few static descriptors in the slab
    for (uint32_t pktIdx = 0; pktIdx < Layout::kNumPackets; ++pktIdx) {
        const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;
        
        // Descriptor 0 (OMI)
        auto* desc0 = ring_.Slab().GetDescriptorPtr(descBase);
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
        
        const uint32_t expectedControl0 = OHCIDescriptor::BuildControl({
            .reqCount = 4,
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyImmediate,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });
        EXPECT_EQ(desc0->control, expectedControl0);
        EXPECT_EQ(desc0->dataAddress, 0);

        // Descriptor 2 (OL)
        auto* desc2 = ring_.Slab().GetDescriptorPtr(descBase + 2);
        
        const uint8_t expectedInterrupt = ((pktIdx + 1) % 8 == 0) ? OHCIDescriptor::kIntAlways : OHCIDescriptor::kIntNever;
        const uint32_t expectedControl2 = OHCIDescriptor::BuildControl({
            .reqCount = 0,
            .command = OHCIDescriptor::kCmdOutputLast,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = expectedInterrupt,
            .branchBits = OHCIDescriptor::kBranchAlways,
        }) | (1u << (OHCIDescriptor::kStatusShift + OHCIDescriptor::kControlHighShift));
        
        EXPECT_EQ(desc2->control, expectedControl2);
        EXPECT_EQ(desc2->dataAddress, ring_.Slab().PayloadRegion().deviceBase + pktIdx * Layout::kMaxPacketSize);

        const uint32_t nextPktIdx = (pktIdx + 1) % Layout::kNumPackets;
        const uint32_t nextDescIOVA = ring_.Slab().GetDescriptorIOVA(nextPktIdx * Layout::kBlocksPerPacket);
        EXPECT_EQ(desc2->branchWord, (nextDescIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket);
    }
}

TEST_F(IsochTxDmaRingTest, RefillConsumesMetadataAndPushesStamps) {
    (void)ring_.Prime();
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    // Allocate host buffers for metadata ring and control block
    std::vector<TxPacketMeta> metadataRing(Layout::kNumPackets);
    TxStreamControl controlBlock{};

    const uint32_t numSlots = Layout::kNumPackets;
    uint8_t* payloadBase = reinterpret_cast<uint8_t*>(ring_.Slab().PayloadRegion().virtualBase);
    uint64_t payloadIOVA = ring_.Slab().PayloadRegion().deviceBase;

    // Pre-populate metadata ring for first lap (8 packets)
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[i].packetIndex = i;
        metadataRing[i].immediateHeader[0] = 0x11110000 + i;
        metadataRing[i].immediateHeader[1] = 0x22220000 + i;
        metadataRing[i].payloadLength = 100 + i * 4;
        metadataRing[i].commitGen.store(1, std::memory_order_release); // Lap 1
    }

    // Set control block structure
    controlBlock.numSlots = numSlots;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    // Mock hardware registers: cmdPtr points to packet 8 descriptor
    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    const uint32_t cmdPtrVal = nextPktDescIOVA | Layout::kBlocksPerPacket;
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)), cmdPtrVal);

    // Set status word on hardware control to running
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);

    // Write mock hw timestamp values into retired OL status words
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc2 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket + 2);
        desc2->statusWord = (0x8000u << 16) | (3000 + i); // timeStamp = 3000 + i
    }

    // Run Refill
    auto outcome = ring_.Refill(hardware_, 0, metadataRing.data(), &controlBlock, numSlots, payloadBase, payloadIOVA);

    EXPECT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 8);
    EXPECT_EQ(outcome.hwPacketIndex, 8);

    // Verify completed stamps
    EXPECT_EQ(controlBlock.completionStampCount.load(), 8);
    EXPECT_EQ(controlBlock.completionCursor.load(), 8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t pktIdx = 0;
        uint32_t ts = 0;
        EXPECT_TRUE(controlBlock.ReadCompletionStamp(i, pktIdx, ts));
        EXPECT_EQ(pktIdx, i);
        EXPECT_EQ(ts, 3000 + i);
    }

    // Verify descriptors were patched
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc0 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket);
        EXPECT_EQ(desc0->branchWord, 0x11110000 + i);
        EXPECT_EQ(desc0->statusWord, 0x22220000 + i);

        auto* desc2 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket + 2);
        EXPECT_EQ(desc2->control & 0xFFFF, 100 + i * 4);
    }
}

TEST_F(IsochTxDmaRingTest, RefillUnderrunHaltsContextAndFlagsFatal) {
    (void)ring_.Prime();
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    std::vector<TxPacketMeta> metadataRing(Layout::kNumPackets);
    TxStreamControl controlBlock{};

    const uint32_t numSlots = Layout::kNumPackets;
    uint8_t* payloadBase = reinterpret_cast<uint8_t*>(ring_.Slab().PayloadRegion().virtualBase);
    uint64_t payloadIOVA = ring_.Slab().PayloadRegion().deviceBase;

    // Pre-populate metadata ring for first 7 packets, leave 8th uncommitted
    for (uint32_t i = 0; i < 7; ++i) {
        metadataRing[i].packetIndex = i;
        metadataRing[i].commitGen.store(1, std::memory_order_release);
    }
    // metadataRing[7] has commitGen = 0

    controlBlock.numSlots = numSlots;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    // Mock hardware registers: cmdPtr points to packet 8
    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)), nextPktDescIOVA | Layout::kBlocksPerPacket);

    // Run Refill
    auto outcome = ring_.Refill(hardware_, 0, metadataRing.data(), &controlBlock, numSlots, payloadBase, payloadIOVA);

    // Refill should fail because packet 7 is not committed
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(controlBlock.statusWord.load(), TxStreamStatus::kUnderrunFatal);
    EXPECT_EQ(controlBlock.streamGeneration.load(), 1);

    // Verify context was stopped: run bit cleared in IsoXmitContextControlClear
    const uint32_t cleared = hardware_.GetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlClear(0)));
    EXPECT_NE((cleared & ASFW::Driver::ContextControl::kRun), 0);
}
