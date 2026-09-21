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

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    for (uint32_t i = 0; i < 9; ++i) {
        metadataRing[i].payloadLength = 296;
        metadataRing[i].commitGeneration.store(1, std::memory_order_release);
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

TEST_F(IsochTxDmaRingTest,
       RefillOverridesProducerChannelWithConfiguredTransportChannel) {
    constexpr uint8_t kConfiguredChannel = 37;
    constexpr uint32_t kProducerHostHeader =
        (2u << 16) | (1u << 14) | (0u << 8) | (0xau << 4) | 5u;

    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SetChannel(kConfiguredChannel);
    ring_.SeedCycleTracking(hardware_);

    metadataRing[0].immediateHeader[0] =
        OSSwapHostToLittleInt32(kProducerHostHeader);

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    const auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(outcome.ok);
    ASSERT_EQ(outcome.packetsFilled, 1u);

    const auto* immediate = reinterpret_cast<const OHCIDescriptorImmediate*>(
        ring_.Slab().GetDescriptorPtr(0));
    const uint32_t actualHostHeader =
        OSSwapLittleToHostInt32(immediate->immediateData[0]);
    EXPECT_EQ((actualHostHeader & kIsochChannelMask) >> 8,
              kConfiguredChannel);
    EXPECT_EQ(actualHostHeader & ~kIsochChannelMask,
              kProducerHostHeader & ~kIsochChannelMask);
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

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    metadataRing[0].payloadLength = 296;
    metadataRing[0].commitGeneration.store(1, std::memory_order_release);

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

// The wire speed is the transport's to decide, exactly like the channel. A
// device whose link is only good for S200 must not be transmitted to at S400
// just because the audio producer wrote that into its placeholder header.
TEST_F(IsochTxDmaRingTest, RefillStampsTheConfiguredSpeedOverTheProducerPlaceholder) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                      metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);
    ring_.SetSpeed(ASFW::FW::FwSpeed::S200);

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    // The producer writes S400 into the placeholder; transport must overwrite it.
    constexpr uint32_t kProducerHeaderS400 = (2u << 16) | (1u << 14) | (0xAu << 4);
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[i].packetIndex = i;
        metadataRing[i].immediateHeader[0] = OSSwapHostToLittleInt32(kProducerHeaderS400);
        metadataRing[i].immediateHeader[1] = 0x22220000 + i;
        metadataRing[i].payloadLength = 100 + i * 4;
        metadataRing[i].commitGeneration.store(1, std::memory_order_release);
    }

    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
                              nextPktDescIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);
    hardware_.SetTestRegister(Register32::kCycleTimer, (5u << 25) | (1234u << 12) | 0x06B0u);

    const auto outcome = ring_.Refill(hardware_, 0, metadataRing.data(), &controlBlock,
                                      kSharedPayloadSlots, sharedPayload_.data(),
                                      payloadDmaMap_);
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
    (void)ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                      metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);
    ring_.SetSpeed(ASFW::FW::FwSpeed::S200);

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[i].packetIndex = i;
        metadataRing[i].immediateHeader[0] = 0;
        metadataRing[i].immediateHeader[1] = 0;
        metadataRing[i].payloadLength = 100 + i * 4;
        metadataRing[i].commitGeneration.store(1, std::memory_order_release);
    }

    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
                              nextPktDescIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);
    hardware_.SetTestRegister(Register32::kCycleTimer, (5u << 25) | (1234u << 12) | 0x06B0u);

    const auto outcome = ring_.Refill(hardware_, 0, metadataRing.data(), &controlBlock,
                                      kSharedPayloadSlots, sharedPayload_.data(),
                                      payloadDmaMap_);
    ASSERT_TRUE(outcome.ok);

    for (uint32_t i = 0; i < 8; ++i) {
        const auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(
            ring_.Slab().GetDescriptorPtr(i * Layout::kBlocksPerPacket));
        EXPECT_EQ(immDesc->immediateData[0], 0U) << "packet " << i;
    }
}

TEST_F(IsochTxDmaRingTest, RefillConsumesMetadataAndPushesStamps) {
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    // Allocate host buffers for metadata ring and control block
    IsochTxQueueControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();

    // Pre-populate metadata ring for first lap (8 packets)
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[i].packetIndex = i;
        metadataRing[i].immediateHeader[0] = 0x11110000 + i;
        metadataRing[i].immediateHeader[1] = 0x22220000 + i;
        metadataRing[i].payloadLength = 100 + i * 4;
        metadataRing[i].commitGeneration.store(1, std::memory_order_release); // Lap 1
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
    EXPECT_EQ(outcome.refillRequestGeneration, 1U);
    EXPECT_EQ(
        controlBlock.refillRequestGeneration.load(
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
        EXPECT_EQ(immDesc->immediateData[0],
                  WithIsochChannelAndSpeed(0x11110000 + i, 1, ASFW::FW::FwSpeed::S400));
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

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    for (uint32_t i = 0; i < 24; ++i) {
        metadataRing[i].payloadLength = 8;
        metadataRing[i].commitGeneration.store(1, std::memory_order_release);
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
    EXPECT_EQ(first.refillRequestGeneration, 1U);

    const auto coalesced = refillTo(16);
    ASSERT_TRUE(coalesced.ok);
    EXPECT_EQ(coalesced.refillRequestGeneration, 0U);

    controlBlock.refillHandledGeneration.store(
        1, std::memory_order_release);
    const auto next = refillTo(24);
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
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};
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
        meta.commitGeneration.store(
            ExpectedTxCommitGeneration(packetIndex, kSharedPayloadSlots),
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
              WithIsochChannelAndSpeed(
                  0x11000000u + Layout::kNumPackets, 1, ASFW::FW::FwSpeed::S400));
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
        meta.commitGeneration.store(1, std::memory_order_release);
    }

    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        kSharedPayloadSlots);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};
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
        metadataRing[0].commitGeneration.load(std::memory_order_acquire);
    const auto packetBefore = metadataRing[0].packetIndex;
    const std::array<uint8_t, 8> payloadBefore{
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
    std::memcpy(sharedPayload_.data(), payloadBefore.data(),
                payloadBefore.size());

    // Hardware descriptor index of the first group past the shared-ring wrap.
    // The shared ring need not be a whole number of 48-packet hardware laps,
    // so derive it instead of assuming the wrap lands on descriptor 0.
    const auto firstSecondLapRefill =
        refillTo(((kGroupsToSharedRingWrap + 1) *
                  ASFW::IsochTransport::AudioTimingGeometry::
                      kTxPacketsPerGroup) %
                 Layout::kNumPackets);
    EXPECT_FALSE(firstSecondLapRefill.ok);
    EXPECT_EQ(firstSecondLapRefill.packetsFilled, 0U);
    EXPECT_EQ(metadataRing[0].commitGeneration.load(std::memory_order_acquire),
              commitBefore);
    EXPECT_EQ(metadataRing[0].packetIndex, packetBefore);
    EXPECT_EQ(
        std::memcmp(sharedPayload_.data(), payloadBefore.data(),
                    payloadBefore.size()),
        0);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kProducerFault);
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
        meta.commitGeneration.store(1, std::memory_order_release);
    }

    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(),
        kSharedPayloadSlots);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};
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
        meta.commitGeneration.store(
            ExpectedTxCommitGeneration(packetIndex, kSharedPayloadSlots),
            std::memory_order_release);
    }

    // See RefillRejectsStaleGenerationAtFirstSharedRingWrap: the wrap does not
    // necessarily land on hardware descriptor 0, so derive the index.
    const auto firstSecondLapRefill =
        refillTo(((kGroupsToSharedRingWrap + 1) *
                  ASFW::IsochTransport::AudioTimingGeometry::
                      kTxPacketsPerGroup) %
                 Layout::kNumPackets);
    EXPECT_TRUE(firstSecondLapRefill.ok);
    EXPECT_EQ(
        firstSecondLapRefill.packetsFilled,
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxPacketsPerGroup);
    EXPECT_EQ(metadataRing[0].packetIndex, kSharedPayloadSlots);
    EXPECT_EQ(metadataRing[0].commitGeneration.load(std::memory_order_acquire), 2U);
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
        meta.commitGeneration.store(
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

    IsochTxQueueControl controlBlock{};
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
              IsochTxQueueStatus::kProducerFault);
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

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;

    metadataRing[0].packetIndex = 0;
    metadataRing[0].payloadLength = kSharedPayloadStride + 1;
    metadataRing[0].commitGeneration.store(1, std::memory_order_release);

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
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), Layout::kNumPackets);
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();

    metadataRing[0].commitGeneration.store(0, std::memory_order_release);
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
              IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(controlBlock.streamGeneration.load(), 1);
    EXPECT_EQ(metadataRing[0].commitGeneration.load(), 0);

    auto* desc0 = ring_.Slab().GetDescriptorPtr(0);
    auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
    EXPECT_EQ(immDesc->immediateData[0], 0U);
    EXPECT_EQ(immDesc->immediateData[1], 0U);
    EXPECT_EQ(
        std::memcmp(payloadBase, payloadBefore.data(),
                    payloadBefore.size()),
        0);
}


// ---------------------------------------------------------------------------
// Hardware-ring lap detection. The command pointer measures packets modulo
// Layout::kNumPackets; the completed descriptors' timeStamps measure cycles.
// A refill that arrives a full ring late must realign by skipping the packets
// the lap should have carried, not slip the whole stream a ring later, and a
// single odd stamp must never move anything.
// ---------------------------------------------------------------------------
class IsochTxDmaRingLapTest : public IsochTxDmaRingTest {
protected:
    static constexpr uint32_t kHw = Layout::kNumPackets;  // 48

    std::vector<IsochTxPacketMeta> metadataRing_ = MakeMetadataRing();
    IsochTxQueueControl controlBlock_{};

    void SetUp() override {
        IsochTxDmaRingTest::SetUp();
        for (uint32_t packetIndex = 0; packetIndex < 8 * kHw; ++packetIndex) {
            auto& meta = metadataRing_[packetIndex];
            meta.packetIndex = packetIndex;
            meta.immediateHeader[0] = 0x11000000u + packetIndex;
            meta.immediateHeader[1] = 0x22000000u + packetIndex;
            meta.payloadLength = 64;
            meta.commitGeneration.store(
                ExpectedTxCommitGeneration(packetIndex, kSharedPayloadSlots),
                std::memory_order_release);
        }
        // Production order (IsochTransmitContext::Start): reset, seed, prime,
        // so the primed ring counts as 48 packets ahead and the first refill
        // continues at packet 48. The producer has committed 8 rings ahead.
        ring_.ResetForStart();
        ring_.SeedCycleTracking(hardware_);
        (void)ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
                          metadataRing_.data(), kHw);
        controlBlock_.numSlots = kSharedPayloadSlots;
        controlBlock_.slotStrideBytes = kSharedPayloadStride;
        controlBlock_.maxPacketBytes = kSharedPayloadStride;
        controlBlock_.committedEnd.store(8 * kHw, std::memory_order_release);
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);
    }

    // The hardware finished `slot` in bus cycle `cycle`: OUTPUT_LAST status
    // word = xferStatus:timeStamp, timeStamp = sec[2:0]:cycle[12:0].
    void StampCompleted(uint32_t slot, uint32_t cycle) {
        auto* desc3 = ring_.Slab().GetDescriptorPtr(
            slot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        const uint16_t stamp = static_cast<uint16_t>(
            (((cycle / 8000u) & 0x7u) << 13) | (cycle % 8000u));
        desc3->statusWord = (0x8000u << 16) | stamp;
    }
    // Hardware is now at `slot`, and the cycle timer reads `nowCycle`
    // (seconds in bits 31:25, cycle in 24:12).
    void HardwareAt(uint32_t slot, uint32_t nowCycle) {
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
            ring_.Slab().GetDescriptorIOVA(slot * Layout::kBlocksPerPacket) |
                Layout::kBlocksPerPacket);
        hardware_.SetTestRegister(
            Register32::kCycleTimer,
            (((nowCycle / 8000u) & 0x7Fu) << 25) | ((nowCycle % 8000u) << 12));
    }
    IsochTxDmaRing::RefillOutcome Refill() {
        return ring_.Refill(hardware_, 0, metadataRing_.data(), &controlBlock_,
                            kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    }
    // First refill: slots 0..7 went out in cycles 3000..3007, hardware is at
    // slot 8 in cycle 3008. Establishes the stamp baseline (3007) and fills
    // packets 48..55 into slots 0..7.
    void FirstRefillAtSlot8() {
        for (uint32_t slot = 0; slot < 8; ++slot) StampCompleted(slot, 3000 + slot);
        HardwareAt(8, 3008);
        const auto first = Refill();
        ASSERT_TRUE(first.ok);
        ASSERT_EQ(first.packetsFilled, 8u);
        ASSERT_EQ(controlBlock_.completionCursor.load(), 8u);
        ASSERT_EQ(first.ringLaps, 0u);
    }
    // Then a normal advance: slots 8..13 in cycles 3008..3013, hardware at 14.
    void NormalAdvanceToSlot14() {
        for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, 3000 + slot);
        HardwareAt(14, 3014);
    }
    // A lap: the refill missed 54 cycles. Slots 8..47 went out in 3008..3047,
    // the ring wrapped, slots 0..7 were RE-SENT (stale) in 3048..3055 and
    // slots 8..13 again in 3056..3061; the pointer is back at slot 14.
    void LapToSlot14() {
        for (uint32_t slot = 14; slot < kHw; ++slot) StampCompleted(slot, 3000 + slot);
        for (uint32_t slot = 0; slot < 14; ++slot) StampCompleted(slot, 3000 + kHw + slot);
        HardwareAt(14, 3000 + kHw + 14);
    }
    uint32_t HeaderInSlot(uint32_t slot) const {
        auto* immediate = reinterpret_cast<const OHCIDescriptorImmediate*>(
            ring_.Slab().GetDescriptorPtr(slot * Layout::kBlocksPerPacket));
        return immediate->immediateData[0];
    }
    uint32_t PacketHeader(uint32_t packetIndex) const {
        return WithIsochChannelAndSpeed(0x11000000u + packetIndex, 1, ASFW::FW::FwSpeed::S400);
    }
};

TEST_F(IsochTxDmaRingLapTest, ConsecutiveRefillsWithOnePacketPerCycleSeeNoLap) {
    FirstRefillAtSlot8();
    NormalAdvanceToSlot14();
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.packetsFilled, 6u);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
    EXPECT_EQ(HeaderInSlot(8), PacketHeader(56));  // packet 56 -> slot 8
    EXPECT_EQ(ring_.RTCounters().ringLaps.load(), 0u);
    EXPECT_EQ(controlBlock_.ringLaps.load(), 0u);
}

TEST_F(IsochTxDmaRingLapTest, LapIsActedOnOnlyWhenASecondReadingAgrees) {
    FirstRefillAtSlot8();
    LapToSlot14();
    // First sighting: nothing moves yet, the six packets behind the pointer
    // are refilled as usual.
    const auto sighting = Refill();
    ASSERT_TRUE(sighting.ok);
    EXPECT_EQ(sighting.ringLaps, 0u);
    EXPECT_EQ(sighting.packetsFilled, 6u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
    EXPECT_EQ(HeaderInSlot(8), PacketHeader(56));
    // Four more packets, still one ring late: confirmed.
    for (uint32_t slot = 14; slot < 18; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(18, 3000 + kHw + 18);
    const auto confirmed = Refill();
    ASSERT_TRUE(confirmed.ok);
    EXPECT_EQ(confirmed.ringLaps, 1u);
    EXPECT_EQ(confirmed.lapPacketsSkipped, kHw);
    EXPECT_EQ(confirmed.lostCycles, 0u);
    EXPECT_EQ(confirmed.packetsFilled, 4u);
    // Cursor: 8 + 6 (sighting) + 4 + one skipped ring = the truth, 8 + 58.
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u + 6u + 4u + kHw);
    // Packets 62..109 are skipped; slot 14 holds packet 110, whose nominal
    // cycle (3000 + 110) is exactly when slot 14 next goes out
    // (3000 + 48 + 18 + 44).
    EXPECT_EQ(HeaderInSlot(14), PacketHeader(62 + kHw));
    EXPECT_EQ(HeaderInSlot(17), PacketHeader(65 + kHw));
    // Stamps cover only the four packets that really went out this refill.
    EXPECT_EQ(controlBlock_.completionStampCount.load(), 8u + 6u + 4u);
    uint64_t pktIdx = 0; uint32_t ts = 0;
    ASSERT_TRUE(controlBlock_.ReadCompletionStamp(14, pktIdx, ts));
    EXPECT_EQ(pktIdx, 14u + kHw);
    EXPECT_EQ(ts, static_cast<uint32_t>((3000u + kHw + 14u) << 12));
    EXPECT_EQ(ring_.RTCounters().ringLaps.load(), 1u);
    EXPECT_EQ(ring_.RTCounters().ringLapPacketsSkipped.load(), kHw);
    EXPECT_EQ(controlBlock_.ringLaps.load(), 1u);
    EXPECT_EQ(controlBlock_.ringLapPacketsSkipped.load(), kHw);
    EXPECT_EQ(controlBlock_.maxCompletionDelta.load(), kHw + 4u);
    // Afterwards the ring is in step again.
    for (uint32_t slot = 18; slot < 24; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(24, 3000 + kHw + 24);
    const auto later = Refill();
    ASSERT_TRUE(later.ok);
    EXPECT_EQ(later.ringLaps, 0u);
    EXPECT_EQ(later.lostCycles, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u + 6u + 4u + kHw + 6u);
    EXPECT_EQ(HeaderInSlot(18), PacketHeader(66 + kHw));
    // The reading after a realignment must be accepted, not refused because
    // the skipped packets were counted as expected cycles (field-found on
    // v25: one refusal after the first confirmed lap).
    EXPECT_EQ(ring_.RTCounters().inconsistentStampReads.load(), 0u);
    EXPECT_EQ(ring_.RTCounters().staleStampReads.load(), 0u);
    // And a second lap later on is still caught from a consistent baseline.
    for (uint32_t slot = 24; slot < kHw; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    for (uint32_t slot = 0; slot < 30; ++slot) StampCompleted(slot, 3000 + 2 * kHw + slot);
    HardwareAt(30, 3000 + 2 * kHw + 30);
    ASSERT_TRUE(Refill().ok);  // sighting
    for (uint32_t slot = 30; slot < 36; ++slot) StampCompleted(slot, 3000 + 2 * kHw + slot);
    HardwareAt(36, 3000 + 2 * kHw + 36);
    const auto secondLap = Refill();
    ASSERT_TRUE(secondLap.ok);
    EXPECT_EQ(secondLap.ringLaps, 1u);
    EXPECT_EQ(ring_.RTCounters().ringLaps.load(), 2u);
    EXPECT_EQ(ring_.RTCounters().inconsistentStampReads.load(), 0u);
}

TEST_F(IsochTxDmaRingLapTest, ExactLapWithUnchangedPointerIsStillDetected) {
    FirstRefillAtSlot8();
    // Exactly one ring of cycles passed: every slot was re-sent once and the
    // pointer is back where it was. The raw delta is zero.
    for (uint32_t slot = 8; slot < kHw; ++slot) StampCompleted(slot, 3000 + slot);
    for (uint32_t slot = 0; slot < 8; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(8, 3000 + kHw + 8);
    const auto sighting = Refill();
    ASSERT_TRUE(sighting.ok);
    EXPECT_EQ(sighting.ringLaps, 0u);
    EXPECT_EQ(sighting.packetsFilled, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u);
    for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(14, 3000 + kHw + 14);
    const auto confirmed = Refill();
    ASSERT_TRUE(confirmed.ok);
    EXPECT_EQ(confirmed.ringLaps, 1u);
    EXPECT_EQ(confirmed.packetsFilled, 6u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u + 6u + kHw);
    EXPECT_EQ(HeaderInSlot(8), PacketHeader(56 + kHw));
    for (uint32_t slot = 14; slot < 20; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(20, 3000 + kHw + 20);
    const auto later = Refill();
    ASSERT_TRUE(later.ok);
    EXPECT_EQ(later.ringLaps, 0u);
    EXPECT_EQ(ring_.RTCounters().inconsistentStampReads.load(), 0u);
}

TEST_F(IsochTxDmaRingLapTest, LostCyclesShortOfARingAreCountedNotRealigned) {
    FirstRefillAtSlot8();
    // Six packets went out but they took eight cycles (two lost cycles, each
    // re-sent through the OMI skip address).
    for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, 3002 + slot);
    HardwareAt(14, 3016);
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 2u);
    EXPECT_EQ(second.packetsFilled, 6u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
    EXPECT_EQ(HeaderInSlot(8), PacketHeader(56));
    EXPECT_EQ(ring_.RTCounters().lostCycles.load(), 2u);
    EXPECT_EQ(controlBlock_.lostCycles.load(), 2u);
}

TEST_F(IsochTxDmaRingLapTest, StampSecondsFieldWrapsWithoutAFalseLap) {
    // Baseline just below the 3-bit seconds wrap: cycle 7*8000+7999 = 63999.
    for (uint32_t slot = 0; slot < 8; ++slot) StampCompleted(slot, 63992 + slot);
    HardwareAt(8, 0);
    ASSERT_TRUE(Refill().ok);
    for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, slot - 8);  // 0..5 after wrap
    HardwareAt(14, 6);
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
}

TEST_F(IsochTxDmaRingLapTest, StaleStampIsRefusedAndLeavesTheBaselineAlone) {
    FirstRefillAtSlot8();
    // The words in slots 13 and 12 are 1.6 s old: not this transmission's
    // stamps (the fallback descriptor is stale too).
    for (uint32_t slot = 8; slot < 12; ++slot) StampCompleted(slot, 3000 + slot);
    StampCompleted(12, 3012 + 64000 - 12823);
    StampCompleted(13, 3013 + 64000 - 12823);
    HardwareAt(14, 3014);
    const auto refused = Refill();
    ASSERT_TRUE(refused.ok);
    EXPECT_EQ(refused.ringLaps, 0u);
    EXPECT_EQ(refused.packetsFilled, 6u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
    EXPECT_EQ(ring_.RTCounters().staleStampReads.load(), 1u);
    // The next honest reading compares against the original baseline with
    // all ten packets counted: no lap, nothing lost.
    for (uint32_t slot = 14; slot < 18; ++slot) StampCompleted(slot, 3000 + slot);
    HardwareAt(18, 3018);
    const auto honest = Refill();
    ASSERT_TRUE(honest.ok);
    EXPECT_EQ(honest.ringLaps, 0u);
    EXPECT_EQ(honest.lostCycles, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 18u);
    EXPECT_EQ(ring_.RTCounters().ringLaps.load(), 0u);
}

TEST_F(IsochTxDmaRingLapTest, StampOlderThanThePacketsSinceBaselineIsRefused) {
    FirstRefillAtSlot8();
    // Slots 13 and 12 carry words newer than the baseline but older than the
    // packets counted since it (torn words): refused.
    for (uint32_t slot = 8; slot < 12; ++slot) StampCompleted(slot, 3000 + slot);
    StampCompleted(12, 3009);
    StampCompleted(13, 3010);
    HardwareAt(14, 3014);
    const auto refused = Refill();
    ASSERT_TRUE(refused.ok);
    EXPECT_EQ(refused.ringLaps, 0u);
    EXPECT_EQ(ring_.RTCounters().inconsistentStampReads.load(), 1u);
    // A fresh reading afterwards must not turn into a lap.
    for (uint32_t slot = 14; slot < 18; ++slot) StampCompleted(slot, 3000 + slot);
    HardwareAt(18, 3018);
    const auto honest = Refill();
    ASSERT_TRUE(honest.ok);
    EXPECT_EQ(honest.ringLaps, 0u);
    EXPECT_EQ(honest.lostCycles, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 18u);
}

TEST_F(IsochTxDmaRingLapTest, LapSeenOnceThenContradictedIsDropped) {
    FirstRefillAtSlot8();
    // One reading claims a ring of extra cycles...
    for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(14, 3000 + kHw + 14);
    const auto sighting = Refill();
    ASSERT_TRUE(sighting.ok);
    EXPECT_EQ(sighting.ringLaps, 0u);
    // ...and the next one is consistent with the packets counted: forget it.
    for (uint32_t slot = 14; slot < 18; ++slot) StampCompleted(slot, 3000 + slot);
    HardwareAt(18, 3018);
    const auto contradiction = Refill();
    ASSERT_TRUE(contradiction.ok);
    EXPECT_EQ(contradiction.ringLaps, 0u);
    EXPECT_EQ(contradiction.lostCycles, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 18u);
    EXPECT_EQ(ring_.RTCounters().ringLaps.load(), 0u);
    EXPECT_EQ(HeaderInSlot(14), PacketHeader(62));
}

TEST_F(IsochTxDmaRingLapTest, ImplausiblyManyLapsAreRefused) {
    FirstRefillAtSlot8();
    // A word from a previous session: 1066 rings "ago", but fresh-looking
    // against a cycle timer that happens to sit just past it.
    for (uint32_t slot = 8; slot < 12; ++slot) StampCompleted(slot, 3000 + slot);
    StampCompleted(12, 3012 + 1066 * kHw);
    StampCompleted(13, 3013 + 1066 * kHw);
    HardwareAt(14, 3014 + 1066 * kHw);
    const auto refused = Refill();
    ASSERT_TRUE(refused.ok);
    EXPECT_EQ(refused.ringLaps, 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
    EXPECT_EQ(ring_.RTCounters().implausibleLapReads.load(), 1u);
    EXPECT_EQ(ring_.RTCounters().unrealignableLaps.load(), 0u);
}

TEST_F(IsochTxDmaRingLapTest, UpperStampBitsAreIgnored) {
    // Whatever a controller puts in bits 15:13 must not skew the reading:
    // baseline with them clear, next stamps with them set.
    FirstRefillAtSlot8();
    for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, 5 * 8000 + 3000 + slot);
    HardwareAt(14, 3014);
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 0u);
    EXPECT_EQ(ring_.RTCounters().staleStampReads.load(), 0u);
    EXPECT_EQ(ring_.RTCounters().inconsistentStampReads.load(), 0u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 14u);
}

TEST_F(IsochTxDmaRingLapTest, InFlightLastDescriptorFallsBackToThePreviousOne) {
    FirstRefillAtSlot8();
    // Slot 13 is still in flight: its word is from its previous transmission
    // one ring ago. Slot 12 is complete and fresh.
    for (uint32_t slot = 8; slot < 13; ++slot) StampCompleted(slot, 3000 + slot);
    StampCompleted(13, 3013 + 8000 - kHw);
    HardwareAt(14, 3014);
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 0u);
    EXPECT_EQ(ring_.RTCounters().inFlightFallbacks.load(), 1u);
    EXPECT_EQ(ring_.RTCounters().staleStampReads.load(), 0u);
    EXPECT_EQ(ring_.RTCounters().inconsistentStampReads.load(), 0u);
    // A lap is still measured correctly through the fallback: 54 cycles for
    // six packets, with slot 17 in flight.
    for (uint32_t slot = 14; slot < 17; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    StampCompleted(17, 3017);  // previous transmission's word
    HardwareAt(18, 3000 + kHw + 18);
    const auto sighting = Refill();
    ASSERT_TRUE(sighting.ok);
    EXPECT_EQ(sighting.ringLaps, 0u);  // seen once
    for (uint32_t slot = 17; slot < 22; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(22, 3000 + kHw + 22);
    const auto confirmed = Refill();
    ASSERT_TRUE(confirmed.ok);
    EXPECT_EQ(confirmed.ringLaps, 1u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u + 6u + 4u + 4u + kHw);
}

TEST_F(IsochTxDmaRingLapTest, StampLeadingTheCycleTimerByTwoIsFresh) {
    // The Apple Thunderbolt adapter stamps two cycles ahead of the register
    // read: every reading must still be accepted, and a lap still found.
    for (uint32_t slot = 0; slot < 8; ++slot) StampCompleted(slot, 3000 + slot);
    HardwareAt(8, 3005);  // register reads two behind the last stamp
    ASSERT_TRUE(Refill().ok);
    for (uint32_t slot = 8; slot < 14; ++slot) StampCompleted(slot, 3000 + slot);
    HardwareAt(14, 3011);
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 0u);
    EXPECT_EQ(ring_.RTCounters().staleStampReads.load(), 0u);
    EXPECT_EQ(ring_.RTCounters().inFlightFallbacks.load(), 0u);
    // A lap with the same lead: sighted, then confirmed.
    for (uint32_t slot = 14; slot < kHw; ++slot) StampCompleted(slot, 3000 + slot);
    for (uint32_t slot = 0; slot < 18; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(18, 3000 + kHw + 15);
    ASSERT_TRUE(Refill().ok);
    for (uint32_t slot = 18; slot < 22; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(22, 3000 + kHw + 19);
    const auto confirmed = Refill();
    ASSERT_TRUE(confirmed.ok);
    EXPECT_EQ(confirmed.ringLaps, 1u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 8u + 6u + 4u + 4u + kHw);
}

TEST_F(IsochTxDmaRingLapTest, PreviousRingWordIsNotFreshEvenWithTheLead) {
    // A word one ring old reads as 46 behind a register that trails by two:
    // outside the freshness window, so the previous descriptor is used.
    FirstRefillAtSlot8();
    for (uint32_t slot = 8; slot < 13; ++slot) StampCompleted(slot, 3000 + slot);
    StampCompleted(13, 3013 + 8000 - kHw);
    HardwareAt(14, 3011);
    const auto second = Refill();
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(second.ringLaps, 0u);
    EXPECT_EQ(second.lostCycles, 0u);
    EXPECT_EQ(ring_.RTCounters().inFlightFallbacks.load(), 1u);
    EXPECT_EQ(ring_.RTCounters().staleStampReads.load(), 0u);
}

TEST_F(IsochTxDmaRingLapTest, LapBeyondTheCommittedCursorIsNotRealigned) {
    // The producer is only 66 packets ahead: a one-ring skip would run the
    // fill past it, so the lap is counted but the stream keeps its slip.
    controlBlock_.committedEnd.store(66, std::memory_order_release);
    FirstRefillAtSlot8();
    LapToSlot14();
    ASSERT_TRUE(Refill().ok);
    for (uint32_t slot = 14; slot < 18; ++slot) StampCompleted(slot, 3000 + kHw + slot);
    HardwareAt(18, 3000 + kHw + 18);
    const auto confirmed = Refill();
    ASSERT_TRUE(confirmed.ok);
    EXPECT_EQ(confirmed.ringLaps, 0u);
    EXPECT_EQ(confirmed.lapPacketsSkipped, 0u);
    EXPECT_EQ(confirmed.packetsFilled, 4u);
    EXPECT_EQ(controlBlock_.completionCursor.load(), 18u);
    EXPECT_EQ(HeaderInSlot(14), PacketHeader(62));
    EXPECT_EQ(ring_.RTCounters().unrealignableLaps.load(), 1u);
    EXPECT_EQ(ring_.RTCounters().ringLaps.load(), 0u);
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
