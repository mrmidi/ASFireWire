// IsochTxDmaRingTests.cpp
// ASFW - Host-safe unit tests for IT DMA ring engine

#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

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
using ASFW::Async::HW::OHCIDescriptor;
using ASFW::Async::HW::OHCIDescriptorImmediate;
using ASFW::Driver::Register32;

class IsochTxDmaRingTest : public ::testing::Test {
protected:
    static constexpr uint64_t kSharedPayloadIOVA = 0x70000000u;
    static constexpr uint32_t kSharedPayloadSlots = 512;
    static constexpr uint32_t kSharedPayloadStride = 512;

    ASFW::Driver::HardwareInterface hardware_;
    std::shared_ptr<IsochDMAMemoryManager> dmaMemory_;
    IsochTxDmaRing ring_;
    std::vector<uint8_t> sharedPayload_ =
        std::vector<uint8_t>(kSharedPayloadSlots * kSharedPayloadStride);

    void SetUp() override {
        IsochMemoryConfig config;
        config.numDescriptors = Layout::kRingBlocks;
        config.packetSizeBytes = 0;
        config.descriptorAlignment = Layout::kOHCIPageSize;
        config.payloadPageAlignment = 16384;
        config.allocatePayloadSlab = false;

        dmaMemory_ = IsochDMAMemoryManager::Create(config);
        ASSERT_NE(dmaMemory_, nullptr);
        ASSERT_TRUE(dmaMemory_->Initialize(hardware_));

        ring_.SetChannel(1);
        ASSERT_EQ(ring_.SetupRings(*dmaMemory_), kIOReturnSuccess);
    }
};

TEST_F(IsochTxDmaRingTest, PrimeInitializesStaticDescriptorChain) {
    auto stats = ring_.Prime(
        kSharedPayloadIOVA, kSharedPayloadSlots, kSharedPayloadStride);
    EXPECT_EQ(stats.packetsAssembled, Layout::kNumPackets);

    // Verify a few static descriptors in the slab
    for (uint32_t pktIdx = 0; pktIdx < Layout::kNumPackets; ++pktIdx) {
        const uint32_t descBase = pktIdx * Layout::kBlocksPerPacket;
        
        // Descriptor 0 (OMI)
        auto* desc0 = ring_.Slab().GetDescriptorPtr(descBase);
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
        
        const uint32_t expectedControl0 = OHCIDescriptor::BuildControl({
            .reqCount = 8,
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyImmediate,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });
        EXPECT_EQ(desc0->control, expectedControl0);
        EXPECT_EQ(desc0->dataAddress, 0);
        // Cross-validated with Linux: firewire/ohci.c:3364-3375.
        EXPECT_EQ(desc0->branchWord,
                  (ring_.Slab().GetDescriptorIOVA(descBase) & 0xFFFFFFF0u) |
                      Layout::kBlocksPerPacket);
        EXPECT_EQ(desc0->statusWord, 0u);
        EXPECT_EQ(immDesc->immediateData[0], 0u);
        EXPECT_EQ(immDesc->immediateData[1], 0u);

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
        EXPECT_EQ(desc2->dataAddress,
                  kSharedPayloadIOVA +
                      (pktIdx % kSharedPayloadSlots) * kSharedPayloadStride);

        const uint32_t nextPktIdx = (pktIdx + 1) % Layout::kNumPackets;
        const uint32_t nextDescIOVA = ring_.Slab().GetDescriptorIOVA(nextPktIdx * Layout::kBlocksPerPacket);
        EXPECT_EQ(desc2->branchWord, (nextDescIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket);
    }
}

TEST_F(IsochTxDmaRingTest, PrimeRejectsMissingSharedPayloadGeometry) {
    EXPECT_EQ(ring_.Prime(0, kSharedPayloadSlots, kSharedPayloadStride).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(kSharedPayloadIOVA, 0, kSharedPayloadStride).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(kSharedPayloadIOVA, kSharedPayloadSlots, 0).packetsAssembled, 0u);
}

TEST_F(IsochTxDmaRingTest, RefillConsumesMetadataAndPushesStamps) {
    (void)ring_.Prime(
        kSharedPayloadIOVA, kSharedPayloadSlots, kSharedPayloadStride);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    // Allocate host buffers for metadata ring and control block
    std::vector<TxPacketMeta> metadataRing(kSharedPayloadSlots);
    TxStreamControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();
    const uint64_t payloadIOVA = kSharedPayloadIOVA;

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
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    // Mock hardware registers: cmdPtr points to packet 8 descriptor
    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    const uint32_t cmdPtrVal = nextPktDescIOVA | Layout::kBlocksPerPacket;
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)), cmdPtrVal);

    // Set status word on hardware control to running
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);
    constexpr uint32_t kRefillSubcycle = 0x06B0;
    hardware_.SetTestRegister(
        Register32::kCycleTimer,
        (5u << 25) | (1234u << 12) | kRefillSubcycle);

    // Write mock hw timestamp values into retired OL status words
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc2 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket + 2);
        const uint16_t timestamp =
            static_cast<uint16_t>((3u << 13) | (3000u + i));
        desc2->statusWord = (0x8000u << 16) | timestamp;
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
        EXPECT_EQ(ts,
                  (3u << 25) |
                      (static_cast<uint32_t>(3000 + i) << 12) |
                      kRefillSubcycle);
    }

    // Verify descriptors were patched
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc0 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket);
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
        EXPECT_EQ(desc0->branchWord,
                  (ring_.Slab().GetDescriptorIOVA(i * Layout::kBlocksPerPacket) &
                   0xFFFFFFF0u) |
                      Layout::kBlocksPerPacket);
        EXPECT_EQ(desc0->statusWord, 0u);
        EXPECT_EQ(immDesc->immediateData[0], 0x11110000 + i);
        EXPECT_EQ(immDesc->immediateData[1], 0x22220000 + i);

        auto* desc2 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket + 2);
        EXPECT_EQ(desc2->control & 0xFFFF, 100 + i * 4);
        EXPECT_EQ(desc2->dataAddress,
                  kSharedPayloadIOVA + i * kSharedPayloadStride);
    }
}

TEST_F(IsochTxDmaRingTest, RefillMapsWrappedHardwareSlotsToAbsoluteProducerSlots) {
    (void)ring_.Prime(
        kSharedPayloadIOVA, kSharedPayloadSlots, kSharedPayloadStride);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    std::vector<TxPacketMeta> metadataRing(kSharedPayloadSlots);
    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;

    // Commit enough producer slots to cross the 192-entry hardware-ring wrap.
    constexpr uint32_t kLastProducerPacket = Layout::kNumPackets + 6;
    for (uint32_t packetIndex = 0; packetIndex <= kLastProducerPacket; ++packetIndex) {
        auto& meta = metadataRing[packetIndex];
        meta.packetIndex = packetIndex;
        meta.immediateHeader[0] = 0x11000000u + packetIndex;
        meta.immediateHeader[1] = 0x22000000u + packetIndex;
        meta.payloadLength = 64;
        meta.commitGen.store(
            ExpectedCommitGen(packetIndex, kSharedPayloadSlots),
            std::memory_order_release);
    }

    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);

    // First advance to hardware packet 191, filling absolute producer slots
    // [0, 191). Then wrap the command pointer to packet 7, filling [191, 199).
    const uint32_t beforeWrapIOVA =
        ring_.Slab().GetDescriptorIOVA((Layout::kNumPackets - 1) *
                                       Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        beforeWrapIOVA | Layout::kBlocksPerPacket);
    const auto beforeWrap = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), kSharedPayloadIOVA);
    ASSERT_TRUE(beforeWrap.ok);
    ASSERT_EQ(beforeWrap.packetsFilled, Layout::kNumPackets - 1);

    constexpr uint32_t kHardwarePacketAfterWrap = 7;
    const uint32_t afterWrapIOVA =
        ring_.Slab().GetDescriptorIOVA(kHardwarePacketAfterWrap *
                                       Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        afterWrapIOVA | Layout::kBlocksPerPacket);
    const auto afterWrap = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), kSharedPayloadIOVA);
    ASSERT_TRUE(afterWrap.ok);
    ASSERT_EQ(afterWrap.packetsFilled, 8u);

    // Absolute packet 192 reuses hardware slot 0, but it must read producer
    // slot 192 rather than producer slot 0. This is the ownership distinction
    // that the removed private payload path obscured.
    auto* wrappedDesc =
        ring_.Slab().GetDescriptorPtr(2);
    EXPECT_EQ(wrappedDesc->dataAddress,
              kSharedPayloadIOVA +
                  Layout::kNumPackets * kSharedPayloadStride);

    auto* wrappedImmediate = reinterpret_cast<OHCIDescriptorImmediate*>(
        ring_.Slab().GetDescriptorPtr(0));
    EXPECT_EQ(wrappedImmediate->immediateData[0],
              0x11000000u + Layout::kNumPackets);
    EXPECT_EQ(wrappedImmediate->immediateData[1],
              0x22000000u + Layout::kNumPackets);
}

TEST_F(IsochTxDmaRingTest, RefillRejectsPayloadLargerThanSharedSlot) {
    (void)ring_.Prime(
        kSharedPayloadIOVA, kSharedPayloadSlots, kSharedPayloadStride);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    std::vector<TxPacketMeta> metadataRing(kSharedPayloadSlots);
    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;

    metadataRing[0].packetIndex = 0;
    metadataRing[0].payloadLength = kSharedPayloadStride + 1;
    metadataRing[0].commitGen.store(1, std::memory_order_release);

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);

    const auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), kSharedPayloadIOVA);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(ring_.RTCounters().fatalPacketSize.load(std::memory_order_relaxed), 1u);
}

TEST_F(IsochTxDmaRingTest, RefillUnderrunHaltsContextAndFlagsFatal) {
    (void)ring_.Prime(
        kSharedPayloadIOVA, kSharedPayloadSlots, kSharedPayloadStride);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    std::vector<TxPacketMeta> metadataRing(kSharedPayloadSlots);
    TxStreamControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();
    const uint64_t payloadIOVA = kSharedPayloadIOVA;

    // Pre-populate metadata ring for first 7 packets, leave 8th uncommitted
    for (uint32_t i = 0; i < 7; ++i) {
        metadataRing[i].packetIndex = i;
        metadataRing[i].commitGen.store(1, std::memory_order_release);
    }
    // metadataRing[7] has commitGen = 0

    controlBlock.numSlots = numSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
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
