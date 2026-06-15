// IsochTxDmaRingTests.cpp
// ASFW - Host-safe unit tests for IT DMA ring engine

#include <gtest/gtest.h>
#include <array>
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
using ASFW::Isoch::Tx::TxPayloadDmaMap;
using ASFW::Isoch::Tx::TxPayloadDmaSegment;
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
    static constexpr uint32_t kSharedPayloadSlots =
        ASFW::IsochTransport::AudioTimingGeometry::kTxSharedSlotPackets;
    static constexpr uint32_t kSharedPayloadStride = 512;

    ASFW::Driver::HardwareInterface hardware_;
    std::shared_ptr<IsochDMAMemoryManager> dmaMemory_;
    IsochTxDmaRing ring_;
    TxPayloadDmaMap payloadDmaMap_;
    std::vector<uint8_t> sharedPayload_ =
        std::vector<uint8_t>(kSharedPayloadSlots * kSharedPayloadStride);

    [[nodiscard]] static std::vector<TxPacketMeta> MakeMetadataRing() {
        std::vector<TxPacketMeta> metadataRing(kSharedPayloadSlots);
        for (uint32_t packetIndex = 0;
             packetIndex < metadataRing.size();
             ++packetIndex) {
            auto& meta = metadataRing[packetIndex];
            meta.packetIndex = packetIndex;
            meta.payloadLength = 8;
            meta.commitGen.store(1, std::memory_order_release);
        }
        return metadataRing;
    }

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

        const TxPayloadDmaSegment payloadSegment{
            .deviceAddress = kSharedPayloadIOVA,
            .length = sharedPayload_.size(),
        };
        ASSERT_TRUE(payloadDmaMap_.Configure(
            std::span<const TxPayloadDmaSegment>(&payloadSegment, 1),
            sharedPayload_.size()));
    }
};

TEST_F(IsochTxDmaRingTest, PrimeInitializesStaticDescriptorChain) {
    auto metadataRing = MakeMetadataRing();
    auto stats = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
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

        // Descriptor 2 (standard OUTPUT_MORE, first half of payload)
        auto* desc2 = ring_.Slab().GetDescriptorPtr(
            descBase + Layout::kFirstPayloadBlock);
        const uint32_t expectedControl2 = OHCIDescriptor::BuildControl({
            .reqCount = 4,
            .command = OHCIDescriptor::kCmdOutputMore,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = OHCIDescriptor::kIntNever,
            .branchBits = OHCIDescriptor::kBranchNever,
        });
        EXPECT_EQ(desc2->control, expectedControl2);
        EXPECT_EQ(desc2->dataAddress,
                  kSharedPayloadIOVA +
                      (pktIdx % kSharedPayloadSlots) * kSharedPayloadStride);
        EXPECT_EQ(desc2->branchWord, 0u);
        EXPECT_EQ(desc2->statusWord, 0u);

        // Descriptor 3 (OUTPUT_LAST, second half of payload)
        auto* desc3 = ring_.Slab().GetDescriptorPtr(
            descBase + Layout::kCompletionBlock);
        const uint8_t expectedInterrupt =
            ASFW::Isoch::Core::IsTimingGroupBoundary(pktIdx)
                ? OHCIDescriptor::kIntAlways
                : OHCIDescriptor::kIntNever;
        const uint32_t expectedControl3 = OHCIDescriptor::BuildControl({
            .reqCount = 4,
            .command = OHCIDescriptor::kCmdOutputLast,
            .key = OHCIDescriptor::kKeyStandard,
            .interruptBits = expectedInterrupt,
            .branchBits = OHCIDescriptor::kBranchAlways,
        }) | (1u << (OHCIDescriptor::kStatusShift + OHCIDescriptor::kControlHighShift));
        
        EXPECT_EQ(desc3->control, expectedControl3);
        EXPECT_EQ(desc3->dataAddress,
                  kSharedPayloadIOVA +
                      (pktIdx % kSharedPayloadSlots) * kSharedPayloadStride +
                      4);

        const uint32_t nextPktIdx = (pktIdx + 1) % Layout::kNumPackets;
        const uint32_t nextDescIOVA = ring_.Slab().GetDescriptorIOVA(nextPktIdx * Layout::kBlocksPerPacket);
        EXPECT_EQ(desc3->branchWord, (nextDescIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket);
    }
}

TEST_F(IsochTxDmaRingTest, PrimeRejectsMissingSharedPayloadGeometry) {
    auto metadataRing = MakeMetadataRing();
    TxPayloadDmaMap invalidMap;
    EXPECT_EQ(ring_.Prime(invalidMap, kSharedPayloadSlots, kSharedPayloadStride, metadataRing.data(), Layout::kNumPackets).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, 0, kSharedPayloadStride, metadataRing.data(), Layout::kNumPackets).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, 0, metadataRing.data(), Layout::kNumPackets).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride, nullptr, Layout::kNumPackets).packetsAssembled, 0u);
}

TEST_F(IsochTxDmaRingTest, PrimeRejectsPrefillShorterThanHardwareRing) {
    auto metadataRing = MakeMetadataRing();
    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        Layout::kNumPackets - 1);
    EXPECT_EQ(prime.packetsAssembled, 0U);
}

TEST_F(IsochTxDmaRingTest, PrimeUsesMappedIOVAOnBothSidesOfPageBoundary) {
    constexpr uint64_t kFirstPageIOVA = 0x71000000u;
    constexpr uint64_t kRemainingPagesIOVA = 0x72000000u;
    constexpr uint64_t kPageBytes = 4096;
    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = kFirstPageIOVA, .length = kPageBytes},
        {
            .deviceAddress = kRemainingPagesIOVA,
            .length = sharedPayload_.size() - kPageBytes,
        },
    }};
    TxPayloadDmaMap segmentedMap;
    ASSERT_TRUE(segmentedMap.Configure(segments, sharedPayload_.size()));

    auto metadataRing = MakeMetadataRing();
    for (auto& meta : metadataRing) {
        meta.payloadLength = 296;
    }

    const auto prime = ring_.Prime(
        segmentedMap,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);

    const auto* lastPacketOnFirstPage =
        ring_.Slab().GetDescriptorPtr(
            7 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    const auto* firstPacketOnSecondPage =
        ring_.Slab().GetDescriptorPtr(
            8 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    EXPECT_EQ(lastPacketOnFirstPage->dataAddress,
              kFirstPageIOVA + 7 * kSharedPayloadStride);
    EXPECT_EQ(firstPacketOnSecondPage->dataAddress, kRemainingPagesIOVA);
}

TEST_F(IsochTxDmaRingTest, PrimeProgramsPayloadCrossingDmaSegment) {
    constexpr uint64_t kBoundaryOffset = 3840;
    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = 0x73000000u, .length = kBoundaryOffset},
        {
            .deviceAddress = 0x74000000u,
            .length = sharedPayload_.size() - kBoundaryOffset,
        },
    }};
    TxPayloadDmaMap segmentedMap;
    ASSERT_TRUE(segmentedMap.Configure(segments, sharedPayload_.size()));

    auto metadataRing = MakeMetadataRing();
    metadataRing[7].payloadLength = 296;

    const auto prime = ring_.Prime(
        segmentedMap,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);

    const uint32_t descBase = 7 * Layout::kBlocksPerPacket;
    const auto* desc2 = ring_.Slab().GetDescriptorPtr(
        descBase + Layout::kFirstPayloadBlock);
    const auto* desc3 = ring_.Slab().GetDescriptorPtr(
        descBase + Layout::kCompletionBlock);
    EXPECT_EQ(desc2->control & 0xffffu, 256u);
    EXPECT_EQ(desc2->dataAddress, 0x73000000u + 7 * kSharedPayloadStride);
    EXPECT_EQ(desc3->control & 0xffffu, 40u);
    EXPECT_EQ(desc3->dataAddress, 0x74000000u);
}

TEST_F(IsochTxDmaRingTest, PrimeRejectsPayloadSpanningThreeDmaSegments) {
    const std::array<TxPayloadDmaSegment, 3> segments{{
        {.deviceAddress = 0x74100000u, .length = 100},
        {.deviceAddress = 0x74200000u, .length = 100},
        {
            .deviceAddress = 0x74300000u,
            .length = sharedPayload_.size() - 200,
        },
    }};
    TxPayloadDmaMap segmentedMap;
    ASSERT_TRUE(segmentedMap.Configure(segments, sharedPayload_.size()));

    auto metadataRing = MakeMetadataRing();
    metadataRing[0].payloadLength = 296;

    const auto prime = ring_.Prime(
        segmentedMap,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        Layout::kNumPackets);
    EXPECT_EQ(prime.packetsAssembled, 0u);
}

TEST_F(IsochTxDmaRingTest, RefillUsesMappedIOVAAfterPageBoundary) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    constexpr uint64_t kFirstPageIOVA = 0x75000000u;
    constexpr uint64_t kRemainingPagesIOVA = 0x76000000u;
    constexpr uint64_t kPageBytes = 4096;
    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = kFirstPageIOVA, .length = kPageBytes},
        {
            .deviceAddress = kRemainingPagesIOVA,
            .length = sharedPayload_.size() - kPageBytes,
        },
    }};
    TxPayloadDmaMap segmentedMap;
    ASSERT_TRUE(segmentedMap.Configure(segments, sharedPayload_.size()));

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    for (uint32_t i = 0; i < 9; ++i) {
        metadataRing[i].payloadLength = 296;
        metadataRing[i].commitGen.store(1, std::memory_order_release);
    }

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(9 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto outcome = ring_.Refill(
        hardware_,
        0,
        metadataRing.data(),
        &controlBlock,
        kSharedPayloadSlots,
        sharedPayload_.data(),
        segmentedMap);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 9u);

    const auto* firstPacketOnSecondPage =
        ring_.Slab().GetDescriptorPtr(
            8 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    EXPECT_EQ(firstPacketOnSecondPage->dataAddress, kRemainingPagesIOVA);
}

TEST_F(IsochTxDmaRingTest, RefillProgramsPayloadCrossingDmaSegment) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = 0x77000000u, .length = 128},
        {
            .deviceAddress = 0x78000000u,
            .length = sharedPayload_.size() - 128,
        },
    }};
    TxPayloadDmaMap crossingMap;
    ASSERT_TRUE(crossingMap.Configure(segments, sharedPayload_.size()));

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    metadataRing[0].payloadLength = 296;
    metadataRing[0].commitGen.store(1, std::memory_order_release);

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto outcome = ring_.Refill(
        hardware_,
        0,
        metadataRing.data(),
        &controlBlock,
        kSharedPayloadSlots,
        sharedPayload_.data(),
        crossingMap);

    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 1u);

    const auto* desc2 = ring_.Slab().GetDescriptorPtr(
        Layout::kFirstPayloadBlock);
    const auto* desc3 = ring_.Slab().GetDescriptorPtr(
        Layout::kCompletionBlock);
    EXPECT_EQ(desc2->control & 0xffffu, 128u);
    EXPECT_EQ(desc2->dataAddress, 0x77000000u);
    EXPECT_EQ(desc3->control & 0xffffu, 168u);
    EXPECT_EQ(desc3->dataAddress, 0x78000000u);
}

TEST_F(IsochTxDmaRingTest, RefillConsumesMetadataAndPushesStamps) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    // Allocate host buffers for metadata ring and control block
    TxStreamControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();

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
    hardware_.SetTestRegister(
        Register32::kCycleTimer,
        (5u << 25) | (1234u << 12) | 0x06B0u);

    // Write mock hw timestamp values into retired OL status words
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc2 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        const uint16_t timestamp =
            static_cast<uint16_t>((3u << 13) | (3000u + i));
        desc2->statusWord = (0x8000u << 16) | timestamp;
    }

    // Run Refill
    auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock, numSlots,
        payloadBase, payloadDmaMap_);

    EXPECT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 8);
    EXPECT_EQ(outcome.hwPacketIndex, 8);

    // Verify completed stamps
    EXPECT_EQ(controlBlock.completionStampCount.load(), 8);
    EXPECT_EQ(controlBlock.completionCursor.load(), 8);
    EXPECT_EQ(outcome.preparationRequestGeneration, 1U);
    EXPECT_EQ(
        controlBlock.preparationRequestGeneration.load(
            std::memory_order_acquire),
        1U);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t pktIdx = 0;
        uint32_t ts = 0;
        EXPECT_TRUE(controlBlock.ReadCompletionStamp(i, pktIdx, ts));
        EXPECT_EQ(pktIdx, i);
        EXPECT_EQ(ts,
                  (3u << 25) |
                      (static_cast<uint32_t>(3000 + i) << 12));
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

        auto* desc2 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
        auto* desc3 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        const uint32_t firstLength = (100 + i * 4) / 2;
        EXPECT_EQ(desc2->control & 0xFFFF, firstLength);
        EXPECT_EQ(desc2->dataAddress,
                  kSharedPayloadIOVA + i * kSharedPayloadStride);
        EXPECT_EQ(desc3->control & 0xFFFF, firstLength);
        EXPECT_EQ(desc3->dataAddress,
                  kSharedPayloadIOVA + i * kSharedPayloadStride +
                      firstLength);
    }
}

TEST_F(IsochTxDmaRingTest, CompletionNotificationCoalescesUntilHandled) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    for (uint32_t i = 0; i < 24; ++i) {
        metadataRing[i].payloadLength = 8;
        metadataRing[i].commitGen.store(1, std::memory_order_release);
    }
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto refillTo = [&](uint32_t packetIndex) {
        const uint32_t iova = ring_.Slab().GetDescriptorIOVA(
            packetIndex * Layout::kBlocksPerPacket);
        hardware_.SetTestRegister(
            static_cast<Register32>(
                DMAContextHelpers::IsoXmitCommandPtr(0)),
            iova | Layout::kBlocksPerPacket);
        return ring_.Refill(
            hardware_,
            0,
            metadataRing.data(),
            &controlBlock,
            kSharedPayloadSlots,
            sharedPayload_.data(),
            payloadDmaMap_);
    };

    const auto first = refillTo(8);
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.preparationRequestGeneration, 1U);

    const auto coalesced = refillTo(16);
    ASSERT_TRUE(coalesced.ok);
    EXPECT_EQ(coalesced.preparationRequestGeneration, 0U);

    controlBlock.preparationHandledGeneration.store(
        1, std::memory_order_release);
    const auto next = refillTo(24);
    ASSERT_TRUE(next.ok);
    EXPECT_EQ(next.preparationRequestGeneration, 2U);
}

TEST_F(IsochTxDmaRingTest, PreparationAcknowledgementNeverMovesBackward) {
    TxStreamControl controlBlock{};

    controlBlock.MarkPreparationHandled(2);
    controlBlock.MarkPreparationHandled(1);

    EXPECT_EQ(
        controlBlock.preparationHandledGeneration.load(
            std::memory_order_acquire),
        2U);
}

TEST_F(IsochTxDmaRingTest, RefillMapsWrappedHardwareSlotsToAbsoluteProducerSlots) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;

    // Commit enough producer slots to cross the 48-entry hardware-ring wrap.
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
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
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
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
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

TEST_F(IsochTxDmaRingTest, RefillRejectsStaleGenerationAtFirstSharedRingWrap) {
    auto metadataRing = MakeMetadataRing();
    for (uint32_t packetIndex = 0;
         packetIndex < kSharedPayloadSlots;
         ++packetIndex) {
        auto& meta = metadataRing[packetIndex];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGen.store(1, std::memory_order_release);
    }

    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        kSharedPayloadSlots);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto refillTo = [&](uint32_t hardwarePacketIndex) {
        const uint32_t iova = ring_.Slab().GetDescriptorIOVA(
            hardwarePacketIndex * Layout::kBlocksPerPacket);
        hardware_.SetTestRegister(
            static_cast<Register32>(
                DMAContextHelpers::IsoXmitCommandPtr(0)),
            iova | Layout::kBlocksPerPacket);
        return ring_.Refill(
            hardware_,
            0,
            metadataRing.data(),
            &controlBlock,
            kSharedPayloadSlots,
            sharedPayload_.data(),
            payloadDmaMap_);
    };

    constexpr uint32_t kGroupsToSharedRingWrap =
        (kSharedPayloadSlots - Layout::kNumPackets) /
        ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
    for (uint32_t refill = 1;
         refill <= kGroupsToSharedRingWrap;
         ++refill) {
        const uint32_t hardwarePacketIndex =
            (refill *
             ASFW::IsochTransport::AudioTimingGeometry::
                 kTxPacketsPerGroup) %
            Layout::kNumPackets;
        ASSERT_TRUE(refillTo(hardwarePacketIndex).ok);
    }

    const auto commitBefore =
        metadataRing[0].commitGen.load(std::memory_order_acquire);
    const auto packetBefore = metadataRing[0].packetIndex;
    const std::array<uint8_t, 8> payloadBefore{
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
    std::memcpy(sharedPayload_.data(), payloadBefore.data(),
                payloadBefore.size());

    const auto firstSecondLapRefill =
        refillTo(
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPacketsPerGroup);
    EXPECT_FALSE(firstSecondLapRefill.ok);
    EXPECT_EQ(firstSecondLapRefill.packetsFilled, 0U);
    EXPECT_EQ(metadataRing[0].commitGen.load(std::memory_order_acquire),
              commitBefore);
    EXPECT_EQ(metadataRing[0].packetIndex, packetBefore);
    EXPECT_EQ(
        std::memcmp(sharedPayload_.data(), payloadBefore.data(),
                    payloadBefore.size()),
        0);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              TxStreamStatus::kUnderrunFatal);
    EXPECT_EQ(controlBlock.streamGeneration.load(std::memory_order_acquire),
              1U);
    EXPECT_EQ(
        ring_.RTCounters().txUnderruns.load(std::memory_order_relaxed),
        1U);
}

TEST_F(IsochTxDmaRingTest, RefillAcceptsGenerationTwoAtFirstSharedRingWrap) {
    auto metadataRing = MakeMetadataRing();
    for (uint32_t packetIndex = 0;
         packetIndex < kSharedPayloadSlots;
         ++packetIndex) {
        auto& meta = metadataRing[packetIndex];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGen.store(1, std::memory_order_release);
    }

    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        kSharedPayloadSlots);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto refillTo = [&](uint32_t hardwarePacketIndex) {
        const uint32_t iova = ring_.Slab().GetDescriptorIOVA(
            hardwarePacketIndex * Layout::kBlocksPerPacket);
        hardware_.SetTestRegister(
            static_cast<Register32>(
                DMAContextHelpers::IsoXmitCommandPtr(0)),
            iova | Layout::kBlocksPerPacket);
        return ring_.Refill(
            hardware_,
            0,
            metadataRing.data(),
            &controlBlock,
            kSharedPayloadSlots,
            sharedPayload_.data(),
            payloadDmaMap_);
    };

    constexpr uint32_t kGroupsToSharedRingWrap =
        (kSharedPayloadSlots - Layout::kNumPackets) /
        ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
    for (uint32_t refill = 1;
         refill <= kGroupsToSharedRingWrap;
         ++refill) {
        const uint32_t hardwarePacketIndex =
            (refill *
             ASFW::IsochTransport::AudioTimingGeometry::
                 kTxPacketsPerGroup) %
            Layout::kNumPackets;
        ASSERT_TRUE(refillTo(hardwarePacketIndex).ok);
    }

    for (uint64_t packetIndex = kSharedPayloadSlots;
         packetIndex <
         kSharedPayloadSlots +
             ASFW::IsochTransport::AudioTimingGeometry::
                 kTxPacketsPerGroup;
         ++packetIndex) {
        auto& meta = metadataRing[packetIndex % kSharedPayloadSlots];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGen.store(
            ExpectedCommitGen(packetIndex, kSharedPayloadSlots),
            std::memory_order_release);
    }

    const auto firstSecondLapRefill =
        refillTo(
            ASFW::IsochTransport::AudioTimingGeometry::
                kTxPacketsPerGroup);
    EXPECT_TRUE(firstSecondLapRefill.ok);
    EXPECT_EQ(
        firstSecondLapRefill.packetsFilled,
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxPacketsPerGroup);
    EXPECT_EQ(metadataRing[0].packetIndex, kSharedPayloadSlots);
    EXPECT_EQ(metadataRing[0].commitGen.load(std::memory_order_acquire), 2U);
}

TEST_F(IsochTxDmaRingTest,
       SixtyCommittedPacketsFailOnThirdUnservicedCompletionGroup) {
    using Geometry = ASFW::IsochTransport::AudioTimingGeometry;
    constexpr uint32_t kHistoricalCommittedPackets =
        Geometry::kTxHardwareRingPackets +
        2 * Geometry::kTxPacketsPerGroup;
    static_assert(kHistoricalCommittedPackets == 60);

    auto metadataRing = MakeMetadataRing();
    for (uint32_t packetIndex = 0;
         packetIndex < kSharedPayloadSlots;
         ++packetIndex) {
        auto& meta = metadataRing[packetIndex];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGen.store(
            packetIndex < kHistoricalCommittedPackets ? 1U : 0U,
            std::memory_order_release);
    }

    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        kSharedPayloadSlots);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto refillTo = [&](uint32_t hardwarePacketIndex) {
        const uint32_t iova = ring_.Slab().GetDescriptorIOVA(
            hardwarePacketIndex * Layout::kBlocksPerPacket);
        hardware_.SetTestRegister(
            static_cast<Register32>(
                DMAContextHelpers::IsoXmitCommandPtr(0)),
            iova | Layout::kBlocksPerPacket);
        return ring_.Refill(
            hardware_,
            0,
            metadataRing.data(),
            &controlBlock,
            kSharedPayloadSlots,
            sharedPayload_.data(),
            payloadDmaMap_);
    };

    EXPECT_TRUE(refillTo(Geometry::kTxPacketsPerGroup).ok);
    EXPECT_TRUE(refillTo(2 * Geometry::kTxPacketsPerGroup).ok);

    const auto third =
        refillTo(3 * Geometry::kTxPacketsPerGroup);
    EXPECT_FALSE(third.ok);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              TxStreamStatus::kUnderrunFatal);
    EXPECT_EQ(
        ring_.RTCounters().txUnderruns.load(std::memory_order_relaxed),
        1U);
}

TEST_F(IsochTxDmaRingTest, RefillRejectsPayloadLargerThanSharedSlot) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

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
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(
        outcome.failureReason,
        IsochTxDmaRing::RefillFailureReason::InvalidPacketSize);
    EXPECT_EQ(outcome.failurePacketAbs, 0U);
    EXPECT_EQ(outcome.failureSlot, 0U);
    EXPECT_EQ(outcome.failurePayloadLength, kSharedPayloadStride + 1);
    EXPECT_EQ(ring_.RTCounters().fatalPacketSize.load(std::memory_order_relaxed), 1u);
}

TEST_F(IsochTxDmaRingTest, RefillHonorsProducerFatalStatusImmediately) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    const ASFW::IsochTransport::TxProducerFailureRecord producerFailure{
        .stage =
            ASFW::IsochTransport::TxProducerStage::kReplayRead,
        .reason =
            ASFW::IsochTransport::TxProducerFailureReason::
                kReplayUnavailable,
        .packetIndex = 192,
        .rangeStart = 192,
        .rangeTarget = 194,
        .preparedCount = 0,
        .completionCursor = 50,
        .exposeCursor = 192,
        .replayProducerCursor = 513,
        .replayEpoch = 4,
    };
    EXPECT_EQ(
        controlBlock.producerFailure.Publish(producerFailure), 1U);
    controlBlock.statusWord.store(
        TxStreamStatus::kUnderrunFatal, std::memory_order_release);

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);

    const auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(
        outcome.failureReason,
        IsochTxDmaRing::RefillFailureReason::ProducerFatalStatus);
    EXPECT_EQ(outcome.packetsFilled, 0U);
    ASSERT_TRUE(outcome.producerFailureAvailable);
    EXPECT_EQ(outcome.producerFailure.generation, 1U);
    EXPECT_EQ(
        outcome.producerFailure.stage,
        ASFW::IsochTransport::TxProducerStage::kReplayRead);
    EXPECT_EQ(
        outcome.producerFailure.reason,
        ASFW::IsochTransport::TxProducerFailureReason::
            kReplayUnavailable);
    EXPECT_EQ(outcome.producerFailure.packetIndex, 192U);
    EXPECT_EQ(outcome.producerFailure.rangeStart, 192U);
    EXPECT_EQ(outcome.producerFailure.rangeTarget, 194U);
    EXPECT_EQ(outcome.producerFailure.preparedCount, 0U);
    EXPECT_EQ(outcome.producerFailure.completionCursor, 50U);
    EXPECT_EQ(outcome.producerFailure.exposeCursor, 192U);
    EXPECT_EQ(outcome.producerFailure.replayProducerCursor, 513U);
    EXPECT_EQ(outcome.producerFailure.replayEpoch, 4U);
}

TEST(TxProducerFailureSnapshotTests, ResetClearsRecordAndNamesArePublic) {
    ASFW::IsochTransport::TxProducerFailureSnapshot snapshot{};
    const ASFW::IsochTransport::TxProducerFailureRecord failure{
        .stage =
            ASFW::IsochTransport::TxProducerStage::
                kReplaySytValidation,
        .reason =
            ASFW::IsochTransport::TxProducerFailureReason::
                kInvalidReplaySyt,
        .packetIndex = 192,
    };

    EXPECT_EQ(snapshot.Publish(failure), 1U);
    ASFW::IsochTransport::TxProducerFailureRecord observed{};
    ASSERT_TRUE(snapshot.TryRead(observed));
    EXPECT_STREQ(
        ASFW::IsochTransport::TxProducerStageName(observed.stage),
        "replay-syt-validation");
    EXPECT_STREQ(
        ASFW::IsochTransport::TxProducerFailureReasonName(
            observed.reason),
        "invalid-replay-syt");

    snapshot.Reset();
    EXPECT_FALSE(snapshot.TryRead(observed));
}

TEST_F(IsochTxDmaRingTest,
       RefillUnderrunIsFatalAndDoesNotInventPacketState) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    TxStreamControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();

    metadataRing[0].commitGen.store(0, std::memory_order_release);
    metadataRing[0].immediateHeader[0] = 0x11110000;
    metadataRing[0].immediateHeader[1] = 0x22220000;
    std::array<uint8_t, 8> payloadBefore{
        0x87, 0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10};
    std::memcpy(payloadBase, payloadBefore.data(), payloadBefore.size());

    controlBlock.numSlots = numSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    // Mock hardware registers: cmdPtr points to packet 8
    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)), nextPktDescIOVA | Layout::kBlocksPerPacket);

    // Run Refill
    auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock, numSlots,
        payloadBase, payloadDmaMap_);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 0);
    EXPECT_EQ(controlBlock.statusWord.load(),
              TxStreamStatus::kUnderrunFatal);
    EXPECT_EQ(controlBlock.streamGeneration.load(), 1);
    EXPECT_EQ(metadataRing[0].commitGen.load(), 0);

    auto* desc0 = ring_.Slab().GetDescriptorPtr(0);
    auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
    EXPECT_EQ(immDesc->immediateData[0], 0U);
    EXPECT_EQ(immDesc->immediateData[1], 0U);
    EXPECT_EQ(
        std::memcmp(payloadBase, payloadBefore.data(),
                    payloadBefore.size()),
        0);
}
