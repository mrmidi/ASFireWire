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
#include "Isoch/Core/IsochTxQueue.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

using ASFW::Isoch::Tx::IsochTxDmaRing;
using ASFW::Isoch::Tx::Layout;
using ASFW::Isoch::Tx::TxPayloadDmaMap;
using ASFW::Isoch::Tx::TxPayloadDmaSegment;
using ASFW::Isoch::Memory::IsochDMAMemoryManager;
using ASFW::Isoch::Memory::IsochMemoryConfig;
using ASFW::Isoch::IsochTxPacketMeta;
using ASFW::Isoch::IsochTxOperation;
using ASFW::Isoch::IsochTxQueueControl;
using ASFW::Isoch::IsochTxQueueStatus;
using ASFW::Isoch::ExpectedTxCommitGeneration;
using ASFW::Async::HW::OHCIDescriptor;
using ASFW::Async::HW::OHCIDescriptorImmediate;
using ASFW::Driver::Register32;

namespace {

constexpr uint32_t kIsochChannelMask = 0x3fu << 8;
constexpr uint32_t kIsochSpeedMask = 0x7u << 16;

// Transport owns both fields in the transmitted header: the channel the IRM
// granted and the speed the link was charged at. The producer's placeholder
// values for both are overwritten.
[[nodiscard]] uint32_t WithIsochChannelAndSpeed(uint32_t leHeader, uint8_t channel,
                                                ASFW::FW::FwSpeed speed) noexcept {
    uint32_t hostHeader = OSSwapLittleToHostInt32(leHeader);
    hostHeader = (hostHeader & ~kIsochChannelMask) |
                 (static_cast<uint32_t>(channel & 0x3fu) << 8);
    hostHeader = (hostHeader & ~kIsochSpeedMask) |
                 ((static_cast<uint32_t>(speed) & 0x7u) << 16);
    return OSSwapHostToLittleInt32(hostHeader);
}

} // namespace

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

    [[nodiscard]] static std::vector<IsochTxPacketMeta> MakeMetadataRing() {
        std::vector<IsochTxPacketMeta> metadataRing(kSharedPayloadSlots);
        for (uint32_t packetIndex = 0;
             packetIndex < metadataRing.size();
             ++packetIndex) {
            auto& meta = metadataRing[packetIndex];
            meta.packetIndex = packetIndex;
            meta.payloadLength = 8;
            meta.commitGeneration.store(1, std::memory_order_release);
        }
        return metadataRing;
    }

    // The controller finished absolute packets [first, end): non-zero
    // OUTPUT_LAST xferStatus, keeping whatever timestamp a test wrote. The
    // ring reads completion from this, not from the CommandPtr (T5).
    void MarkSent(uint64_t first, uint64_t end) {
        for (uint64_t packet = first; packet < end; ++packet) {
            auto* completion = ring_.Slab().GetDescriptorPtr(
                static_cast<uint32_t>(packet % Layout::kNumPackets) * Layout::kBlocksPerPacket +
                Layout::kCompletionBlock);
            completion->statusWord = (0x8011u << 16) | (completion->statusWord & 0xFFFFu);
        }
    }

    // Production order (IsochTransmitContext::Start) on a fresh ring: prime,
    // then seed cycle tracking. The first refill maps packet kNumPackets.
    void PrimeForTest(std::vector<IsochTxPacketMeta>& metadataRing,
                      uint64_t prefill = Layout::kNumPackets) {
        (void)ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                          metadataRing.data(), prefill);
        ring_.SeedCycleTracking(hardware_);
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);
    }
    static void ConfigureControl(IsochTxQueueControl& controlBlock) {
        controlBlock.numSlots = kSharedPayloadSlots;
        controlBlock.slotStrideBytes = kSharedPayloadStride;
        controlBlock.maxPacketBytes = kSharedPayloadStride;
    }
    // Stand-in for the controller: it sent every packet below absolute `end`
    // and points at the next one; then the refill runs. Since the newest
    // completion is kept as the anchor, the first refill after N sent maps
    // N - 1 packets.
    IsochTxDmaRing::RefillOutcome SendThrough(uint64_t end,
                                              std::vector<IsochTxPacketMeta>& metadataRing,
                                              IsochTxQueueControl& controlBlock,
                                              const TxPayloadDmaMap* map = nullptr) {
        MarkSent(controlBlock.completionCursor.load(std::memory_order_acquire), end);
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
            ring_.Slab().GetDescriptorIOVA(
                static_cast<uint32_t>(end % Layout::kNumPackets) * Layout::kBlocksPerPacket) |
                Layout::kBlocksPerPacket);
        return ring_.Refill(hardware_, 0, metadataRing.data(), &controlBlock,
                            kSharedPayloadSlots, sharedPayload_.data(),
                            map ? *map : payloadDmaMap_);
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

        // Finite queue: the primed chain ends at the last packet (T5).
        if (pktIdx + 1 == Layout::kNumPackets) {
            EXPECT_EQ(desc3->branchWord, 0u);
        } else {
            const uint32_t nextDescIOVA =
                ring_.Slab().GetDescriptorIOVA((pktIdx + 1) * Layout::kBlocksPerPacket);
            EXPECT_EQ(desc3->branchWord, (nextDescIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket);
        }
    }
}

TEST_F(IsochTxDmaRingTest, PrimeStartsWithSkipAndBranchesToNextOperationEntry) {
    auto metadataRing = MakeMetadataRing();
    metadataRing[0].operation = IsochTxOperation::SkipCycle;
    metadataRing[0].payloadLength = 0;
    const auto stats = ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
                                   kSharedPayloadStride, metadataRing.data(),
                                   Layout::kNumPackets);
    ASSERT_EQ(stats.packetsAssembled, Layout::kNumPackets);
    const auto* firstTail = ring_.Slab().GetDescriptorPtr(Layout::kCompletionBlock);
    EXPECT_EQ(stats.firstCommandPointer,
              static_cast<uint32_t>(ring_.Slab().GetDescriptorIOVA(Layout::kCompletionBlock)) | 1U);
        EXPECT_EQ(firstTail->control & 0xF0000000U,
              OHCIDescriptor::kCmdOutputLast <<
                  (OHCIDescriptor::kCmdShift + OHCIDescriptor::kControlHighShift));
    EXPECT_EQ(firstTail->control & 0xFFFFU, 0U);
    EXPECT_EQ(firstTail->dataAddress, 0U);
    EXPECT_EQ(firstTail->branchWord,
              (ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket) & 0xFFFFFFF0u) |
                  Layout::kBlocksPerPacket);
}

TEST_F(IsochTxDmaRingTest, PrimeRejectsInvalidOperationOrSkipPayload) {
    auto metadataRing = MakeMetadataRing();
    metadataRing[0].operation = IsochTxOperation::SkipCycle;
    metadataRing[0].payloadLength = 8;
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                          metadataRing.data(), Layout::kNumPackets).packetsAssembled, 0U);
    metadataRing = MakeMetadataRing();
    metadataRing[0].operation = static_cast<IsochTxOperation>(99);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                          metadataRing.data(), Layout::kNumPackets).packetsAssembled, 0U);
}

TEST_F(IsochTxDmaRingTest, ZeroLengthPacketIsNotASkipCycle) {
    auto metadataRing = MakeMetadataRing();
    metadataRing[0].payloadLength = 0;
    metadataRing[0].immediateHeader[0] = OSSwapHostToLittleInt32(0x000080A0U);
    metadataRing[1].operation = IsochTxOperation::SkipCycle;
    metadataRing[1].payloadLength = 0;
    const auto stats = ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
                                   kSharedPayloadStride, metadataRing.data(),
                                   Layout::kNumPackets);
    ASSERT_EQ(stats.packetsAssembled, Layout::kNumPackets);
    EXPECT_EQ(stats.firstCommandPointer,
              (ring_.Slab().GetDescriptorIOVA(0) & 0xFFFFFFF0u) |
                  Layout::kBlocksPerPacket);
    const auto* emptyPacket = ring_.Slab().GetDescriptorPtr(Layout::kCompletionBlock);
    const auto* skip = ring_.Slab().GetDescriptorPtr(
        Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    EXPECT_EQ(emptyPacket->control & 0xFFFFU, 0U);
    EXPECT_EQ(reinterpret_cast<const OHCIDescriptorImmediate*>(
                  ring_.Slab().GetDescriptorPtr(0))->common.control & 0xFFFFU, 8U);
    EXPECT_EQ(emptyPacket->branchWord,
              static_cast<uint32_t>(ring_.Slab().GetDescriptorIOVA(
                  Layout::kBlocksPerPacket + Layout::kCompletionBlock)) | 1U);
    EXPECT_EQ(skip->control & 0xFFFFU, 0U);
}

TEST_F(IsochTxDmaRingTest,
       PrimeOverridesProducerChannelWithConfiguredTransportChannel) {
    constexpr uint8_t kConfiguredChannel = 37;
    constexpr uint32_t kProducerHostHeader =
        (2u << 16) | (1u << 14) | (0u << 8) | (0xau << 4) | 5u;
    const uint32_t producerHeader =
        OSSwapHostToLittleInt32(kProducerHostHeader);

    auto metadataRing = MakeMetadataRing();
    metadataRing[0].immediateHeader[0] = producerHeader;
    ring_.SetChannel(kConfiguredChannel);

    const auto stats = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ASSERT_EQ(stats.packetsAssembled, Layout::kNumPackets);

    const auto* immediate = reinterpret_cast<const OHCIDescriptorImmediate*>(
        ring_.Slab().GetDescriptorPtr(0));
    const uint32_t actualHostHeader =
        OSSwapLittleToHostInt32(immediate->immediateData[0]);
    EXPECT_EQ((actualHostHeader & kIsochChannelMask) >> 8,
              kConfiguredChannel);
    EXPECT_EQ(actualHostHeader & ~kIsochChannelMask,
              kProducerHostHeader & ~kIsochChannelMask);
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

// The first refill maps packets kN.. into hardware slots 0.. (T5 finite
// queue, production order). Their payloads live in shared slots kN.., so the
// DMA-segment layouts below are placed around those slots.
constexpr uint32_t kN = Layout::kNumPackets;

TEST_F(IsochTxDmaRingTest, RefillUsesMappedIOVAAfterPageBoundary) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing);

    constexpr uint64_t kFirstSegmentIOVA = 0x75000000u;
    constexpr uint64_t kSecondSegmentIOVA = 0x76000000u;
    // Packet kN + 8 is the first whose payload sits in the second segment.
    const uint64_t firstSegmentBytes = uint64_t{kN + 8} * kSharedPayloadStride;
    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = kFirstSegmentIOVA, .length = firstSegmentBytes},
        {.deviceAddress = kSecondSegmentIOVA, .length = sharedPayload_.size() - firstSegmentBytes},
    }};
    TxPayloadDmaMap segmentedMap;
    ASSERT_TRUE(segmentedMap.Configure(segments, sharedPayload_.size()));

    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    for (uint32_t i = 0; i < 9; ++i) {
        metadataRing[kN + i].payloadLength = 296;
    }

    const auto outcome = SendThrough(10, metadataRing, controlBlock, &segmentedMap);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 9u);

    const auto* packetInSecondSegment =
        ring_.Slab().GetDescriptorPtr(8 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    EXPECT_EQ(packetInSecondSegment->dataAddress, kSecondSegmentIOVA);
}

TEST_F(IsochTxDmaRingTest,
       RefillOverridesProducerChannelWithConfiguredTransportChannel) {
    constexpr uint8_t kConfiguredChannel = 37;
    constexpr uint32_t kProducerHostHeader =
        (2u << 16) | (1u << 14) | (0u << 8) | (0xau << 4) | 5u;

    auto metadataRing = MakeMetadataRing();
    ring_.SetChannel(kConfiguredChannel);
    PrimeForTest(metadataRing);
    metadataRing[kN].immediateHeader[0] = OSSwapHostToLittleInt32(kProducerHostHeader);

    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    const auto outcome = SendThrough(2, metadataRing, controlBlock);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 1u);

    const auto* immediate = reinterpret_cast<const OHCIDescriptorImmediate*>(
        ring_.Slab().GetDescriptorPtr(0));
    const uint32_t actualHostHeader = OSSwapLittleToHostInt32(immediate->immediateData[0]);
    EXPECT_EQ((actualHostHeader & kIsochChannelMask) >> 8, kConfiguredChannel);
    EXPECT_EQ(actualHostHeader & ~kIsochChannelMask,
              kProducerHostHeader & ~kIsochChannelMask);
}

TEST_F(IsochTxDmaRingTest, RefillProgramsPayloadCrossingDmaSegment) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing);

    // Packet kN's payload starts 128 bytes before the segment boundary.
    const uint64_t firstSegmentBytes = uint64_t{kN} * kSharedPayloadStride + 128;
    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = 0x77000000u, .length = firstSegmentBytes},
        {.deviceAddress = 0x78000000u, .length = sharedPayload_.size() - firstSegmentBytes},
    }};
    TxPayloadDmaMap crossingMap;
    ASSERT_TRUE(crossingMap.Configure(segments, sharedPayload_.size()));

    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    metadataRing[kN].payloadLength = 296;

    const auto outcome = SendThrough(2, metadataRing, controlBlock, &crossingMap);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 1u);

    const auto* desc2 = ring_.Slab().GetDescriptorPtr(Layout::kFirstPayloadBlock);
    const auto* desc3 = ring_.Slab().GetDescriptorPtr(Layout::kCompletionBlock);
    EXPECT_EQ(desc2->control & 0xffffu, 128u);
    EXPECT_EQ(desc2->dataAddress, 0x77000000u + uint64_t{kN} * kSharedPayloadStride);
    EXPECT_EQ(desc3->control & 0xffffu, 168u);
    EXPECT_EQ(desc3->dataAddress, 0x78000000u);
}

// The wire speed is the transport's to decide, exactly like the channel. A
// device whose link is only good for S200 must not be transmitted to at S400
// just because the audio producer wrote that into its placeholder header.
TEST_F(IsochTxDmaRingTest, RefillStampsTheConfiguredSpeedOverTheProducerPlaceholder) {
    auto metadataRing = MakeMetadataRing();
    ring_.SetSpeed(ASFW::FW::FwSpeed::S200);
    PrimeForTest(metadataRing);

    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    // The producer writes S400 into the placeholder; transport must overwrite it.
    constexpr uint32_t kProducerHeaderS400 = (2u << 16) | (1u << 14) | (0xAu << 4);
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[kN + i].immediateHeader[0] = OSSwapHostToLittleInt32(kProducerHeaderS400);
        metadataRing[kN + i].payloadLength = 100 + i * 4;
    }

    const auto outcome = SendThrough(9, metadataRing, controlBlock);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 8U);

    for (uint32_t i = 0; i < 8; ++i) {
        const auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(
            ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket));
        const uint32_t header = OSSwapLittleToHostInt32(immDesc->immediateData[0]);
        EXPECT_EQ((header >> 16) & 0x7u, static_cast<uint32_t>(ASFW::FW::FwSpeed::S200))
            << "packet " << i;
        // The ring's channel is still stamped, and nothing else moved.
        EXPECT_EQ((header >> 8) & 0x3Fu, 1U) << "packet " << i;
        EXPECT_EQ((header >> 14) & 0x3u, 1U) << "packet " << i;  // tag
        EXPECT_EQ((header >> 4) & 0xFu, 0xAU) << "packet " << i; // tcode
    }
}

// An all-zero header is the underrun sentinel: transport must not turn it into
// a well-formed packet by stamping fields into it.
TEST_F(IsochTxDmaRingTest, RefillLeavesTheNoPacketSentinelUnstamped) {
    auto metadataRing = MakeMetadataRing();
    ring_.SetSpeed(ASFW::FW::FwSpeed::S200);
    PrimeForTest(metadataRing);

    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[kN + i].immediateHeader[0] = 0;
        metadataRing[kN + i].immediateHeader[1] = 0;
        metadataRing[kN + i].payloadLength = 100 + i * 4;
    }

    const auto outcome = SendThrough(9, metadataRing, controlBlock);
    ASSERT_TRUE(outcome.ok);
    for (uint32_t i = 0; i < 8; ++i) {
        const auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(
            ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket));
        EXPECT_EQ(immDesc->immediateData[0], 0U) << "packet " << i;
    }
}

TEST_F(IsochTxDmaRingTest, RefillConsumesMetadataAndPushesStamps) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing);

    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[kN + i].immediateHeader[0] = 0x11110000 + i;
        metadataRing[kN + i].immediateHeader[1] = 0x22220000 + i;
        metadataRing[kN + i].payloadLength = 100 + i * 4;
    }
    hardware_.SetTestRegister(Register32::kCycleTimer, (5u << 25) | (1234u << 12) | 0x06B0u);

    // Packets 0..8 went out in cycles 3000..3008 of second 3.
    for (uint32_t i = 0; i < 9; ++i) {
        auto* completion = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        completion->statusWord = static_cast<uint16_t>((3u << 13) | (3000u + i));
    }
    const auto outcome = SendThrough(9, metadataRing, controlBlock);

    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 8u);
    EXPECT_EQ(controlBlock.completionStampCount.load(), 9u);
    EXPECT_EQ(controlBlock.completionCursor.load(), 9u);
    EXPECT_EQ(outcome.refillRequestGeneration, 1U);
    EXPECT_EQ(controlBlock.refillRequestGeneration.load(std::memory_order_acquire), 1U);
    for (uint32_t i = 0; i < 9; ++i) {
        uint64_t pktIdx = 0;
        uint32_t ts = 0;
        EXPECT_TRUE(controlBlock.ReadCompletionStamp(i, pktIdx, ts));
        EXPECT_EQ(pktIdx, i);
        EXPECT_EQ(ts, (3u << 25) | (static_cast<uint32_t>(3000 + i) << 12));
    }

    // Packets kN..kN+7 went into hardware slots 0..7, reading shared slots kN+i.
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc0 = ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket);
        auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
        EXPECT_EQ(desc0->branchWord,
                  (ring_.Slab().GetDescriptorIOVA(i * Layout::kBlocksPerPacket) & 0xFFFFFFF0u) |
                      Layout::kBlocksPerPacket);
        EXPECT_EQ(desc0->statusWord, 0u);
        EXPECT_EQ(immDesc->immediateData[0],
                  WithIsochChannelAndSpeed(0x11110000 + i, 1, ASFW::FW::FwSpeed::S400));
        EXPECT_EQ(immDesc->immediateData[1], 0x22220000 + i);

        auto* desc2 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
        auto* desc3 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        const uint32_t firstLength = (100 + i * 4) / 2;
        const uint64_t payload = kSharedPayloadIOVA + uint64_t{kN + i} * kSharedPayloadStride;
        EXPECT_EQ(desc2->control & 0xFFFF, firstLength);
        EXPECT_EQ(desc2->dataAddress, payload);
        EXPECT_EQ(desc3->control & 0xFFFF, firstLength);
        EXPECT_EQ(desc3->dataAddress, payload + firstLength);
    }
}

TEST_F(IsochTxDmaRingTest, CompletionNotificationCoalescesUntilHandled) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);

    const auto first = SendThrough(8, metadataRing, controlBlock);
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.refillRequestGeneration, 1U);

    const auto coalesced = SendThrough(16, metadataRing, controlBlock);
    ASSERT_TRUE(coalesced.ok);
    EXPECT_EQ(coalesced.refillRequestGeneration, 0U);

    controlBlock.refillHandledGeneration.store(1, std::memory_order_release);
    const auto next = SendThrough(24, metadataRing, controlBlock);
    ASSERT_TRUE(next.ok);
    EXPECT_EQ(next.refillRequestGeneration, 2U);
}

TEST_F(IsochTxDmaRingTest, PreparationAcknowledgementNeverMovesBackward) {
    IsochTxQueueControl controlBlock{};

    controlBlock.MarkRefillHandled(2);
    controlBlock.MarkRefillHandled(1);

    EXPECT_EQ(
        controlBlock.refillHandledGeneration.load(
            std::memory_order_acquire),
        2U);
}

TEST_F(IsochTxDmaRingTest, RefillMapsWrappedHardwareSlotsToAbsoluteProducerSlots) {
    auto metadataRing = MakeMetadataRing();
    for (uint32_t packetIndex = 0; packetIndex < kN + 8; ++packetIndex) {
        auto& meta = metadataRing[packetIndex];
        meta.immediateHeader[0] = 0x11000000u + packetIndex;
        meta.immediateHeader[1] = 0x22000000u + packetIndex;
        meta.payloadLength = 64;
    }
    PrimeForTest(metadataRing);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);

    const auto outcome = SendThrough(9, metadataRing, controlBlock);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 8u);

    // Absolute packet kN reuses hardware slot 0, but it must read producer
    // slot kN rather than producer slot 0.
    auto* wrappedDesc = ring_.Slab().GetDescriptorPtr(Layout::kFirstPayloadBlock);
    EXPECT_EQ(wrappedDesc->dataAddress, kSharedPayloadIOVA + uint64_t{kN} * kSharedPayloadStride);
    auto* wrappedImmediate = reinterpret_cast<OHCIDescriptorImmediate*>(ring_.Slab().GetDescriptorPtr(0));
    EXPECT_EQ(wrappedImmediate->immediateData[0],
              WithIsochChannelAndSpeed(0x11000000u + kN, 1, ASFW::FW::FwSpeed::S400));
    EXPECT_EQ(wrappedImmediate->immediateData[1], 0x22000000u + kN);
}

namespace {
// Packets sent, in whole groups, just before the refill that would map the
// first packet of the second shared-slot lap (absolute kSharedPayloadSlots):
// mapped end = sent - 1 + kN, so it stays below the lap while
// sent <= kSharedPayloadSlots - kN + 1.
constexpr uint64_t kGroup = ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup;
}  // namespace

TEST_F(IsochTxDmaRingTest, RefillRejectsStaleGenerationAtFirstSharedRingWrap) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing, kSharedPayloadSlots);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);

    const uint64_t lastSafe = ((kSharedPayloadSlots - kN + 1) / kGroup) * kGroup;
    for (uint64_t sent = kGroup; sent <= lastSafe; sent += kGroup) {
        ASSERT_TRUE(SendThrough(sent, metadataRing, controlBlock).ok) << sent;
    }

    const auto commitBefore = metadataRing[0].commitGeneration.load(std::memory_order_acquire);
    const auto packetBefore = metadataRing[0].packetIndex;
    const std::array<uint8_t, 8> payloadBefore{0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
    std::memcpy(sharedPayload_.data(), payloadBefore.data(), payloadBefore.size());

    // Shared slot 0 still holds generation 1: its second-lap packet is not
    // committed, so the refill that reaches it fails without touching it.
    const auto firstSecondLapRefill = SendThrough(lastSafe + kGroup, metadataRing, controlBlock);
    EXPECT_FALSE(firstSecondLapRefill.ok);
    EXPECT_EQ(firstSecondLapRefill.failureReason,
              IsochTxDmaRing::RefillFailureReason::UncommittedSlot);
    EXPECT_EQ(firstSecondLapRefill.failurePacketAbs, kSharedPayloadSlots);
    EXPECT_EQ(firstSecondLapRefill.packetsFilled, 0U);
    EXPECT_EQ(metadataRing[0].commitGeneration.load(std::memory_order_acquire), commitBefore);
    EXPECT_EQ(metadataRing[0].packetIndex, packetBefore);
    EXPECT_EQ(std::memcmp(sharedPayload_.data(), payloadBefore.data(), payloadBefore.size()), 0);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(controlBlock.streamGeneration.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(ring_.RTCounters().txUnderruns.load(std::memory_order_relaxed), 1U);
}

TEST_F(IsochTxDmaRingTest, RefillAcceptsGenerationTwoAtFirstSharedRingWrap) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing, kSharedPayloadSlots);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);

    const uint64_t lastSafe = ((kSharedPayloadSlots - kN + 1) / kGroup) * kGroup;
    for (uint64_t sent = kGroup; sent <= lastSafe; sent += kGroup) {
        ASSERT_TRUE(SendThrough(sent, metadataRing, controlBlock).ok) << sent;
    }
    for (uint64_t packetIndex = kSharedPayloadSlots;
         packetIndex < kSharedPayloadSlots + 2 * kGroup; ++packetIndex) {
        auto& meta = metadataRing[packetIndex % kSharedPayloadSlots];
        meta.packetIndex = packetIndex;
        meta.payloadLength = 8;
        meta.commitGeneration.store(ExpectedTxCommitGeneration(packetIndex, kSharedPayloadSlots),
                                    std::memory_order_release);
    }

    const auto firstSecondLapRefill = SendThrough(lastSafe + kGroup, metadataRing, controlBlock);
    EXPECT_TRUE(firstSecondLapRefill.ok);
    EXPECT_EQ(firstSecondLapRefill.packetsFilled, kGroup);
    EXPECT_EQ(metadataRing[0].packetIndex, kSharedPayloadSlots);
    EXPECT_EQ(metadataRing[0].commitGeneration.load(std::memory_order_acquire), 2U);
}

TEST_F(IsochTxDmaRingTest, CommittedPacketsFailOnThirdUnservicedCompletionGroup) {
    // Pinned at the six-packet completion group that was live when this
    // failure was captured; the ring mechanics are group-size independent.
    constexpr uint32_t kHistoricalGroupPackets = 6;
    // Committed: the primed ring plus two groups. The refill after the third
    // group maps past them.
    constexpr uint32_t kCommittedPackets = kN + 2 * kHistoricalGroupPackets;

    auto metadataRing = MakeMetadataRing();
    for (uint32_t packetIndex = 0; packetIndex < kSharedPayloadSlots; ++packetIndex) {
        metadataRing[packetIndex].commitGeneration.store(
            packetIndex < kCommittedPackets ? 1U : 0U, std::memory_order_release);
    }
    PrimeForTest(metadataRing, kSharedPayloadSlots);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);

    EXPECT_TRUE(SendThrough(kHistoricalGroupPackets, metadataRing, controlBlock).ok);
    EXPECT_TRUE(SendThrough(2 * kHistoricalGroupPackets, metadataRing, controlBlock).ok);
    const auto third = SendThrough(3 * kHistoricalGroupPackets, metadataRing, controlBlock);
    EXPECT_FALSE(third.ok);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(ring_.RTCounters().txUnderruns.load(std::memory_order_relaxed), 1U);
}

TEST_F(IsochTxDmaRingTest, RefillRejectsPayloadLargerThanSharedSlot) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);
    metadataRing[kN].payloadLength = kSharedPayloadStride + 1;

    const auto outcome = SendThrough(2, metadataRing, controlBlock);
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureReason, IsochTxDmaRing::RefillFailureReason::InvalidPacketSize);
    EXPECT_EQ(outcome.failurePacketAbs, kN);
    EXPECT_EQ(outcome.failureSlot, kN);
    EXPECT_EQ(outcome.failurePayloadLength, kSharedPayloadStride + 1);
    EXPECT_EQ(ring_.RTCounters().fatalPacketSize.load(std::memory_order_relaxed), 1u);
}

TEST_F(IsochTxDmaRingTest, RefillHonorsProducerFaultStatusImmediately) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.statusWord.store(
        IsochTxQueueStatus::kProducerFault, std::memory_order_release);

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
        IsochTxDmaRing::RefillFailureReason::ProducerFaultStatus);
    EXPECT_EQ(outcome.packetsFilled, 0U);
}

TEST_F(IsochTxDmaRingTest,
       RefillUnderrunIsFatalAndDoesNotInventPacketState) {
    auto metadataRing = MakeMetadataRing();
    PrimeForTest(metadataRing);
    IsochTxQueueControl controlBlock{};
    ConfigureControl(controlBlock);

    // Packet kN is not committed.
    metadataRing[kN].commitGeneration.store(0, std::memory_order_release);
    metadataRing[kN].immediateHeader[0] = 0x11110000;
    metadataRing[kN].immediateHeader[1] = 0x22220000;
    uint8_t* payloadBase = sharedPayload_.data() + uint64_t{kN} * kSharedPayloadStride;
    const std::array<uint8_t, 8> payloadBefore{0x87, 0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10};
    std::memcpy(payloadBase, payloadBefore.data(), payloadBefore.size());
    const uint32_t headerBefore =
        reinterpret_cast<OHCIDescriptorImmediate*>(ring_.Slab().GetDescriptorPtr(0))->immediateData[0];

    const auto outcome = SendThrough(9, metadataRing, controlBlock);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 0);
    EXPECT_EQ(controlBlock.statusWord.load(), IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(controlBlock.streamGeneration.load(), 1);
    EXPECT_EQ(metadataRing[kN].commitGeneration.load(), 0);
    // Hardware slot 0 still carries the primed packet 0, not a made-up one.
    EXPECT_EQ(reinterpret_cast<OHCIDescriptorImmediate*>(ring_.Slab().GetDescriptorPtr(0))
                  ->immediateData[0],
              headerBefore);
    EXPECT_EQ(std::memcmp(payloadBase, payloadBefore.data(), payloadBefore.size()), 0);
}


// ---------------------------------------------------------------------------
// Finite IT queue (T5, documentation/TX_OWNERSHIP.md). Completion is read from
// OUTPUT_LAST status, the newest completed descriptor is kept as the resume
// anchor, each refill maps a zero-terminated batch linked from the old tail,
// and a queue that ran dry is a fault, never a replay. Each test below fails
// on one of the mutations midi's c4dd1700 named: a cyclic tail, recycling the
// newest completion, waking only when idle -- or on completion inferred from
// the command pointer.
// ---------------------------------------------------------------------------
class IsochTxDmaRingFiniteTest : public IsochTxDmaRingTest {
protected:
    static constexpr uint32_t kHw = Layout::kNumPackets;

    std::vector<IsochTxPacketMeta> metadataRing_ = MakeMetadataRing();
    IsochTxQueueControl controlBlock_{};

    void SetUp() override {
        IsochTxDmaRingTest::SetUp();
        for (uint32_t packetIndex = 0; packetIndex < kSharedPayloadSlots; ++packetIndex) {
            auto& meta = metadataRing_[packetIndex];
            meta.packetIndex = packetIndex;
            meta.immediateHeader[0] = 0x11000000u + packetIndex;
            meta.immediateHeader[1] = 0x22000000u + packetIndex;
            meta.payloadLength = 64;
            meta.commitGeneration.store(
                ExpectedTxCommitGeneration(packetIndex, kSharedPayloadSlots),
                std::memory_order_release);
        }
        // Production order (IsochTransmitContext::Start): reset, seed, prime.
        ring_.ResetForStart();
        ring_.SeedCycleTracking(hardware_);
        (void)ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                          metadataRing_.data(), kHw);
        controlBlock_.numSlots = kSharedPayloadSlots;
        controlBlock_.slotStrideBytes = kSharedPayloadStride;
        controlBlock_.maxPacketBytes = kSharedPayloadStride;
        controlBlock_.committedEnd.store(kSharedPayloadSlots, std::memory_order_release);
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);
        PointerAt(0);
    }

    OHCIDescriptor* CompletionDescriptor(uint32_t slot) {
        return ring_.Slab().GetDescriptorPtr(
            slot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    }
    // The hardware finished absolute packets [first, end): non-zero xferStatus
    // with a timeStamp = sec[2:0]:cycle[12:0], one packet per cycle from 3000.
    void Complete(uint64_t first, uint64_t end) {
        for (uint64_t packet = first; packet < end; ++packet) {
            const uint32_t cycle = static_cast<uint32_t>(3000 + packet);
            const uint16_t stamp = static_cast<uint16_t>(
                (((cycle / 8000u) & 0x7u) << 13) | (cycle % 8000u));
            CompletionDescriptor(static_cast<uint32_t>(packet % kHw))->statusWord =
                (0x8000u << 16) | stamp;
        }
    }
    // Where the CommandPtr points. Completion must not depend on it.
    void PointerAt(uint32_t slot) {
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
            ring_.Slab().GetDescriptorIOVA(slot * Layout::kBlocksPerPacket) |
                Layout::kBlocksPerPacket);
    }
    IsochTxDmaRing::RefillOutcome Refill() {
        return ring_.Refill(hardware_, 0, metadataRing_.data(), &controlBlock_,
                            kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    }
    uint32_t HeaderInSlot(uint32_t slot) const {
        auto* immediate = reinterpret_cast<const OHCIDescriptorImmediate*>(
            ring_.Slab().GetDescriptorPtr(slot * Layout::kBlocksPerPacket));
        return immediate->immediateData[0];
    }
    uint32_t PacketHeader(uint64_t packetIndex) const {
        return WithIsochChannelAndSpeed(0x11000000u + static_cast<uint32_t>(packetIndex), 1,
                                        ASFW::FW::FwSpeed::S400);
    }
    uint32_t BranchToSlot(uint32_t slot) const {
        return ASFW::Async::HW::MakeBranchWordAT(ring_.Slab().GetDescriptorIOVA(slot * Layout::kBlocksPerPacket),
                                Layout::kBlocksPerPacket);
    }
};

TEST_F(IsochTxDmaRingFiniteTest, PrimeEndsTheChainWithAZeroBranch) {
    for (uint32_t slot = 0; slot + 1 < kHw; ++slot) {
        EXPECT_EQ(CompletionDescriptor(slot)->branchWord, BranchToSlot(slot + 1)) << slot;
    }
    // A cyclic tail would let the hardware replay the head after a late refill.
    EXPECT_EQ(CompletionDescriptor(kHw - 1)->branchWord, 0u);
}

TEST_F(IsochTxDmaRingFiniteTest, CompletionComesFromDescriptorStatusNotTheCommandPointer) {
    Complete(0, 8);
    PointerAt(3);  // an arbitrary, disagreeing pointer
    const auto outcome = Refill();
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u);
    EXPECT_EQ(controlBlock_.completionStampCount.load(), 8u);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t packet = 0;
        uint32_t stamp = 0;
        ASSERT_TRUE(controlBlock_.ReadCompletionStamp(i, packet, stamp));
        EXPECT_EQ(packet, i);
        EXPECT_EQ(stamp, (3000u + i) << 12);
    }
}

TEST_F(IsochTxDmaRingFiniteTest, ALateRefillCountsEveryCompletedPacket) {
    // Far more than the old 48-packet ring: no lap to guess, just more status.
    Complete(0, 300);
    PointerAt(300);
    const auto outcome = Refill();
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 300u);
    EXPECT_EQ(outcome.completedPacketCount, 300u);
    // Everything older than the newest completion is re-mapped.
    EXPECT_EQ(outcome.packetsFilled, 299u);
    for (uint32_t slot = 0; slot < 299; ++slot) {
        EXPECT_EQ(HeaderInSlot(slot), PacketHeader(kHw + slot)) << slot;
    }
}

TEST_F(IsochTxDmaRingFiniteTest, TheNewestCompletedDescriptorIsRetainedAsTheAnchor) {
    Complete(0, 8);
    PointerAt(8);
    const auto outcome = Refill();
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 7u);
    for (uint32_t slot = 0; slot < 7; ++slot) {
        EXPECT_EQ(HeaderInSlot(slot), PacketHeader(kHw + slot)) << slot;
    }
    // Packet 7 finished last: its descriptor and its branch to packet 8 stay
    // until packet 8 completes, because the controller may still follow it.
    EXPECT_EQ(HeaderInSlot(7), PacketHeader(7));
    EXPECT_EQ(CompletionDescriptor(7)->branchWord, BranchToSlot(8));
    EXPECT_NE(CompletionDescriptor(7)->statusWord >> 16, 0u);

    // Once packet 8 completes, packet 7's slot is recycled.
    Complete(8, 9);
    const auto next = Refill();
    ASSERT_TRUE(next.ok);
    EXPECT_EQ(next.packetsFilled, 1u);
    EXPECT_EQ(HeaderInSlot(7), PacketHeader(kHw + 7));
}

TEST_F(IsochTxDmaRingFiniteTest, EachBatchIsZeroTerminatedAndLinkedFromTheOldTail) {
    Complete(0, 8);
    const auto outcome = Refill();
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 7u);
    // The old tail (packet kHw-1) now branches to the first new packet.
    EXPECT_EQ(CompletionDescriptor(kHw - 1)->branchWord, BranchToSlot(0));
    for (uint32_t slot = 0; slot < 6; ++slot) {
        EXPECT_EQ(CompletionDescriptor(slot)->branchWord, BranchToSlot(slot + 1)) << slot;
    }
    // The new tail ends the chain.
    EXPECT_EQ(CompletionDescriptor(6)->branchWord, 0u);
    // New descriptors start with zero status, so they are never counted
    // before the hardware sends them.
    for (uint32_t slot = 0; slot < 7; ++slot) {
        EXPECT_EQ(CompletionDescriptor(slot)->statusWord >> 16, 0u) << slot;
    }
}

TEST_F(IsochTxDmaRingFiniteTest, MixedOperationsRefillAndReuseKeepCompletionAccounting) {
    metadataRing_[kHw].operation = IsochTxOperation::SkipCycle;
    metadataRing_[kHw].payloadLength = 0;
    metadataRing_[kHw + 1].operation = IsochTxOperation::Packet;
    metadataRing_[kHw + 1].payloadLength = 0; // empty packet, still an OMI packet
    metadataRing_[kHw + 7].operation = IsochTxOperation::SkipCycle;
    metadataRing_[kHw + 7].payloadLength = 0;

    Complete(0, 8);
    const auto first = Refill();
    ASSERT_TRUE(first.ok);
    EXPECT_EQ(first.completedPacketCount, 8U);
    EXPECT_EQ(controlBlock_.completionStampCount.load(), 8U);
    const uint32_t firstEntry = static_cast<uint32_t>(
        ring_.Slab().GetDescriptorIOVA(Layout::kCompletionBlock)) | 1U;
    EXPECT_EQ(CompletionDescriptor(kHw - 1)->branchWord, firstEntry);
    EXPECT_EQ(CompletionDescriptor(0)->control & 0xFFFFU, 0U);
    EXPECT_EQ(CompletionDescriptor(0)->dataAddress, 0U);
    EXPECT_EQ(HeaderInSlot(1), PacketHeader(kHw + 1));
    EXPECT_EQ(reinterpret_cast<const OHCIDescriptorImmediate*>(
                  ring_.Slab().GetDescriptorPtr(Layout::kBlocksPerPacket))
                  ->common.control & 0xFFFFU, 8U);

    Complete(8, 9);
    const auto reused = Refill();
    ASSERT_TRUE(reused.ok);
    EXPECT_EQ(reused.completedPacketCount, 1U);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 9U);
    EXPECT_EQ(controlBlock_.completionStampCount.load(), 9U);
    const uint32_t reusedSkipEntry = static_cast<uint32_t>(
        ring_.Slab().GetDescriptorIOVA(7 * Layout::kBlocksPerPacket +
                                       Layout::kCompletionBlock)) | 1U;
    EXPECT_EQ(CompletionDescriptor(6)->branchWord, reusedSkipEntry);
    EXPECT_EQ(CompletionDescriptor(7)->control & 0xFFFFU, 0U);
    EXPECT_EQ(CompletionDescriptor(7)->dataAddress, 0U);

    // Let the finite chain advance through the rest of the original ring and
    // the first mixed batch. The next packet in slot 1 retires slot 0's skip,
    // allowing that exact hardware entry to be rebuilt as a normal packet.
    for (uint64_t absolute = kHw + 8; absolute <= 2 * kHw; ++absolute) {
        auto& meta = metadataRing_[absolute % kSharedPayloadSlots];
        meta.operation = IsochTxOperation::Packet;
        meta.payloadLength = 64;
        meta.immediateHeader[0] = 0x11000000u + static_cast<uint32_t>(absolute);
        meta.immediateHeader[1] = 0x22000000u + static_cast<uint32_t>(absolute);
        meta.commitGeneration.store(
            ExpectedTxCommitGeneration(absolute, kSharedPayloadSlots),
            std::memory_order_release);
    }
    Complete(9, kHw + 1);
    const auto restOfRing = Refill();
    ASSERT_TRUE(restOfRing.ok);
    EXPECT_EQ(controlBlock_.completionCursor.load(), kHw + 1);

    Complete(kHw + 1, kHw + 2);
    const auto skipToPacket = Refill();
    ASSERT_TRUE(skipToPacket.ok);
    EXPECT_EQ(skipToPacket.packetsFilled, 1U);
    EXPECT_EQ(HeaderInSlot(0), PacketHeader(2 * kHw));
    const auto* reusedPacketHead = reinterpret_cast<const OHCIDescriptorImmediate*>(
        ring_.Slab().GetDescriptorPtr(0));
    EXPECT_EQ(reusedPacketHead->common.branchWord, BranchToSlot(0));
    EXPECT_EQ(reusedPacketHead->common.control & 0xFFFFU, 8U);
}

TEST_F(IsochTxDmaRingFiniteTest, AQueueThatRanDryIsAFaultNotAReplay) {
    Complete(0, 8);
    ASSERT_TRUE(Refill().ok);  // mapped: packets [8, kHw + 7)
    const uint32_t headerBefore = HeaderInSlot(0);
    // The hardware finished everything mapped. Packet 7's slot still carries
    // its old, non-zero status; the walk must stop at the mapped end anyway.
    Complete(8, kHw + 7);
    const auto outcome = Refill();
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureReason, IsochTxDmaRing::RefillFailureReason::MappedRegionExhausted);
    EXPECT_EQ(ring_.RTCounters().mappedRegionExhausted.load(), 1u);
    // Nothing moved: no descriptor re-mapped, no completion published.
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u);
    EXPECT_EQ(HeaderInSlot(0), headerBefore);

    // The count belongs to that stream: the next start begins at zero.
    ring_.ResetForStart();
    EXPECT_EQ(ring_.RTCounters().mappedRegionExhausted.load(), 0u);
}

TEST_F(IsochTxDmaRingFiniteTest, AFailedBatchStaysUnreachable) {
    Complete(0, 8);
    // Packet kHw + 3 is not committed: the refill stops mid-batch.
    const uint32_t slot = (kHw + 3) % kSharedPayloadSlots;
    metadataRing_[slot].commitGeneration.store(0, std::memory_order_release);
    const auto outcome = Refill();
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureReason, IsochTxDmaRing::RefillFailureReason::UncommittedSlot);
    // The old tail still ends the chain: the partial batch is never linked.
    EXPECT_EQ(CompletionDescriptor(kHw - 1)->branchWord, 0u);
}

TEST_F(IsochTxDmaRingFiniteTest, AnAppendWakesEvenAnActiveContext) {
    constexpr uint32_t kActive = ASFW::Driver::ContextControl::kActive;
    constexpr uint32_t kRun = ASFW::Driver::ContextControl::kRun;
    constexpr uint32_t kWake = ASFW::Driver::ContextControl::kWake;
    // ContextControlSet and ContextControl share one address (a read of the
    // set register returns the control value), so a WAKE shows up as a write
    // there; the host stub keeps the last value written.
    const auto ctrlReg = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(0));

    // After an append the context may already have fetched the old zero
    // branch, so it is woken even though it reads active.
    hardware_.SetTestRegister(ctrlReg, kRun | kActive);
    EXPECT_TRUE(ring_.WakeHardware(hardware_, 0));
    EXPECT_EQ(hardware_.GetTestRegister(ctrlReg), kWake);

    // A stopped context is never woken.
    hardware_.SetTestRegister(ctrlReg, 0);
    EXPECT_FALSE(ring_.WakeHardware(hardware_, 0));
    EXPECT_EQ(hardware_.GetTestRegister(ctrlReg), 0u);
}

TEST(IsochTxQueueControlTests, ProducerAndConsumerResetsHaveDisjointOwnership) {
    IsochTxQueueControl queue{};
    queue.abiVersion = ASFW::Isoch::kTxQueueAbiVersion;
    queue.committedEnd.store(408, std::memory_order_release);
    queue.completionCursor.store(144, std::memory_order_release);
    queue.statusWord.store(IsochTxQueueStatus::kRunning,
                           std::memory_order_release);

    queue.ResetConsumerForArm();
    EXPECT_EQ(queue.abiVersion, ASFW::Isoch::kTxQueueAbiVersion);
    EXPECT_EQ(queue.committedEnd.load(std::memory_order_acquire), 408U);
    EXPECT_EQ(queue.completionCursor.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(queue.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kStopped);

    queue.statusWord.store(IsochTxQueueStatus::kRunning,
                           std::memory_order_release);
    queue.completionCursor.store(12, std::memory_order_release);
    queue.ResetProducerForStart();
    EXPECT_EQ(queue.committedEnd.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(queue.completionCursor.load(std::memory_order_acquire), 12U);
    EXPECT_EQ(queue.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kRunning);
}
