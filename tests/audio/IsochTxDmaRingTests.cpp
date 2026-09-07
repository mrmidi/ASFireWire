// IsochTxDmaRingTests.cpp
// ASFW - Host-safe unit tests for IT DMA ring engine

#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "Isoch/Transmit/IsochTxDmaRing.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/OHCIConstants.hpp"
#include "Isoch/Core/IsochTxQueue.hpp"
#include "Shared/Isoch/IsochQueueGeometry.hpp"
#include "Shared/Isoch/TxPayloadSeal.hpp"

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

[[nodiscard]] uint32_t WithIsochChannel(uint32_t leHeader,
                                        uint8_t channel) noexcept {
    uint32_t hostHeader = OSSwapLittleToHostInt32(leHeader);
    hostHeader = (hostHeader & ~kIsochChannelMask) |
                 (static_cast<uint32_t>(channel & 0x3fu) << 8);
    return OSSwapHostToLittleInt32(hostHeader);
}

} // namespace

class IsochTxDmaRingTest : public ::testing::Test {
protected:
    ASFWTestMachTime::ScopedClock clock_{1'000'000'000};
    static constexpr uint64_t kSharedPayloadIOVA = 0x70000000u;
    // An arbitrary non-power-of-two producer queue depth. Transport must not
    // depend on a content producer's chosen retention geometry.
    //
    // Derived rather than written down, because the tests below need three
    // properties this must not silently lose when the ring moves: it must not
    // be a whole number of hardware laps (so the shared-ring wrap does not land
    // on descriptor 0), the gap to one lap must be a whole number of completion
    // groups (so the wrap tests can step to it), and it must exceed the two
    // laps the wrapped-slot test commits across. It was the literal 912, which
    // satisfied all three at a 48-packet ring and none of them at 504 -- the
    // wrapped-slot test wrote past the end of the metadata ring.
    static constexpr uint32_t kSharedPayloadSlots = 3 * Layout::kNumPackets + 6;
    static_assert(kSharedPayloadSlots % Layout::kNumPackets != 0);
    static_assert((kSharedPayloadSlots - Layout::kNumPackets) %
                      ASFW::Shared::Isoch::IsochQueueGeometry::
                          kPacketsPerCompletionGroup ==
                  0);
    static_assert(kSharedPayloadSlots > 2 * Layout::kNumPackets + 6);
    static constexpr uint32_t kSharedPayloadStride = 512;

    ASFW::Driver::HardwareInterface hardware_;
    std::shared_ptr<IsochDMAMemoryManager> dmaMemory_;
    IsochTxDmaRing ring_;
    TxPayloadDmaMap payloadDmaMap_;
    // Two payload images per slot: transport binds one at map time.
    std::vector<uint8_t> sharedPayload_ =
        std::vector<uint8_t>(kSharedPayloadSlots *
                             ASFW::Isoch::kTxPayloadImagesPerSlot *
                             kSharedPayloadStride);
    IsochTxQueueControl primeControl_{};

    [[nodiscard]] uint8_t* ImageBytes(uint32_t slot, uint32_t image = 0) {
        return sharedPayload_.data() + ASFW::Isoch::TxPayloadImageOffset(
                                           slot, image, kSharedPayloadStride);
    }
    [[nodiscard]] static uint64_t ImageIOVA(uint32_t slot, uint32_t image = 0) {
        return kSharedPayloadIOVA + ASFW::Isoch::TxPayloadImageOffset(
                                        slot, image, kSharedPayloadStride);
    }

    /// Simulate the controller retiring packets. OHCI writes xferStatus into
    /// the packet's OUTPUT_LAST as it finishes with it, and a refill zeroes the
    /// field again (AR_init_status). The completion walk reads exactly that, so
    /// a test that only moves the CommandPtr is not describing hardware.
    void MarkCompletedByHardware(uint64_t firstAbs, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot =
                static_cast<uint32_t>((firstAbs + i) % Layout::kNumPackets);
            auto* completion = ring_.Slab().GetDescriptorPtr(
                slot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
            // Any non-zero xferStatus means retired; 0x11 is a plausible
            // ack/complete event code and the value itself is not inspected.
            completion->statusWord =
                (0x0011u << 16) | (completion->statusWord & 0xFFFFu);
        }
    }

    /// Mark enough completed descriptors to retire through the requested
    /// cursor, including the successor retained as the continuation anchor.
    /// The pointer may still name that successor's OUTPUT_LAST.
    ///
    /// Hardware does two things as it advances: it moves the pointer AND it
    /// writes xferStatus into each OUTPUT_LAST it passes. Completion is read
    /// from the second of those, so a test that moves only the pointer is
    /// describing a controller that finishes nothing. This keeps the tests that
    /// predate the completion walk honest without restating each one.
    void RetireToCommandPtr(IsochTxQueueControl& control) {
        const uint32_t cmdPtr = hardware_.GetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)));
        const uint32_t addr = cmdPtr & 0xFFFFFFF0u;
        // Decode through the slab, not with flat (addr - base) / stride
        // arithmetic. The two agree only while the ring fits in one descriptor
        // page; past that the padding at each page boundary makes the flat form
        // over-count, and this helper silently retired nothing because the slot
        // it computed was out of range. Production always used the slab
        // decoder -- this was a harness-only defect, but it hid a real test.
        uint32_t logicalIndex = 0;
        if (!ring_.Slab().DecodeCmdAddrToLogicalIndex(addr, logicalIndex)) return;
        const uint32_t slot = logicalIndex / Layout::kBlocksPerPacket;
        if (slot >= Layout::kNumPackets) return;
        const uint32_t delta =
            (slot + Layout::kNumPackets - mockHwSlot_) % Layout::kNumPackets;
        MarkCompletedByHardware(control.completionCursor.load(), delta + 1);
        mockHwSlot_ = slot;
    }

    /// Where this fixture last observed the mock controller's CommandPtr.
    uint32_t mockHwSlot_{0};

    [[nodiscard]] std::vector<IsochTxPacketMeta> MakeMetadataRing() {
        std::vector<IsochTxPacketMeta> metadataRing(kSharedPayloadSlots);
        for (uint32_t packetIndex = 0;
             packetIndex < metadataRing.size();
             ++packetIndex) {
            auto& meta = metadataRing[packetIndex];
            meta.packetIndex = packetIndex;
            meta.payloadLength = 8;
            // The seal is written by transport when it binds the slot; this
            // only has to be something other than the value it will compute.
            meta.payloadSeal = 0;
            // Armed for lap 1, exactly as the producer's commit leaves it.
            meta.payloadArbitration.store(
                ASFW::Isoch::MakeTxPayloadArbitration(
                    1, ASFW::Isoch::TxPayloadArbitration::kNoAlternative),
                std::memory_order_relaxed);
            meta.commitGeneration.store(1, std::memory_order_release);
        }
        return metadataRing;
    }

    /// Offer image 1 through the real producer entry point, so a test cannot
    /// place a slot in a state the producer could not have reached.
    [[nodiscard]] static bool OfferLateImage(
        std::vector<IsochTxPacketMeta>& metadataRing, uint32_t slot) {
        return ASFW::Isoch::OfferLateTxPayload(
            metadataRing[slot],
            ASFW::Isoch::ExpectedTxCommitGeneration(slot, kSharedPayloadSlots));
    }

    void RefreshPayloadSeal(
        std::vector<IsochTxPacketMeta>& metadataRing,
        uint32_t slot) {
        ASSERT_LT(slot, metadataRing.size());
        auto& meta = metadataRing[slot];
        ASSERT_LE(meta.payloadLength, kSharedPayloadStride);
        meta.payloadSeal = ASFW::Shared::Isoch::SealTxPayload(
            ImageBytes(slot, meta.selectedPayloadImage), meta.payloadLength);
    }

    void RefreshAllPayloadSeals(
        std::vector<IsochTxPacketMeta>& metadataRing) {
        for (uint32_t slot = 0; slot < metadataRing.size(); ++slot) {
            RefreshPayloadSeal(metadataRing, slot);
        }
    }

    void SetUp() override {
        IsochMemoryConfig config;
        config.numDescriptors = Layout::kRingBlocks;
        config.packetSizeBytes = 0;
        config.descriptorAlignment = Layout::kDescriptorPageStride;
        config.payloadPageAlignment = 16384;
        config.allocatePayloadSlab = false;

        dmaMemory_ = IsochDMAMemoryManager::Create(config);
        ASSERT_NE(dmaMemory_, nullptr);
        ASSERT_TRUE(dmaMemory_->Initialize(hardware_));

        ring_.SetChannel(1);
        ASSERT_EQ(ring_.SetupRings(*dmaMemory_), kIOReturnSuccess);
        ring_.SeedCycleTracking(hardware_);

        const TxPayloadDmaSegment payloadSegment{
            .deviceAddress = kSharedPayloadIOVA,
            .length = sharedPayload_.size(),
        };
        ASSERT_TRUE(payloadDmaMap_.Configure(
            std::span<const TxPayloadDmaSegment>(&payloadSegment, 1),
            sharedPayload_.size()));
    }
};

TEST_F(IsochTxDmaRingTest, PrimeInitializesTerminatedDescriptorChain) {
    auto metadataRing = MakeMetadataRing();
    auto stats = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
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
                  ImageIOVA(pktIdx % kSharedPayloadSlots));
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
                  ImageIOVA(pktIdx % kSharedPayloadSlots) + 4);

        const uint32_t nextPktIdx = (pktIdx + 1) % Layout::kNumPackets;
        const uint32_t nextDescIOVA = ring_.Slab().GetDescriptorIOVA(nextPktIdx * Layout::kBlocksPerPacket);
        EXPECT_EQ(desc3->branchWord, pktIdx + 1 < Layout::kNumPackets
            ? (nextDescIOVA & 0xFFFFFFF0u) | Layout::kBlocksPerPacket : 0U);
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
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
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
    EXPECT_EQ(ring_.Prime(invalidMap, kSharedPayloadSlots, kSharedPayloadStride, metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, 0, kSharedPayloadStride, metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, 0, metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets).packetsAssembled, 0u);
    EXPECT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride, nullptr, &primeControl_, sharedPayload_.data(), Layout::kNumPackets).packetsAssembled, 0u);
}

TEST_F(IsochTxDmaRingTest, PrimeRejectsPrefillShorterThanHardwareRing) {
    auto metadataRing = MakeMetadataRing();
    const auto prime = ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets - 1);
    EXPECT_EQ(prime.packetsAssembled, 0U);
}

TEST_F(IsochTxDmaRingTest, PrimeUsesMappedIOVAOnBothSidesOfPageBoundary) {
    constexpr uint64_t kFirstPageIOVA = 0x71000000u;
    constexpr uint64_t kRemainingPagesIOVA = 0x72000000u;
    // The boundary must land exactly on slot 8's bound image so the assertions
    // below still read "last slot before the split" and "first slot after it".
    const uint64_t kPageBytes =
        ASFW::Isoch::TxPayloadImageOffset(8, 0, kSharedPayloadStride);
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
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);

    const auto* lastPacketOnFirstPage =
        ring_.Slab().GetDescriptorPtr(
            7 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    const auto* firstPacketOnSecondPage =
        ring_.Slab().GetDescriptorPtr(
            8 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    EXPECT_EQ(lastPacketOnFirstPage->dataAddress,
              kFirstPageIOVA +
                  ASFW::Isoch::TxPayloadImageOffset(7, 0, kSharedPayloadStride));
    EXPECT_EQ(firstPacketOnSecondPage->dataAddress, kRemainingPagesIOVA);
}

TEST_F(IsochTxDmaRingTest, PrefixSplitPutsTheMutableTailInOneDescriptorField) {
    // With a prefix declared, the first entry addresses exactly the invariant
    // bytes and the second addresses the rest. That is what lets an image swap
    // be a single aligned store to desc3->dataAddress instead of two stores the
    // hardware could catch half-done.
    auto metadataRing = MakeMetadataRing();
    for (auto& meta : metadataRing) {
        meta.payloadLength = 72;        // 8-byte prefix + 64 bytes of samples
        meta.payloadPrefixBytes = 8;
    }
    const auto prime = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);

    const uint32_t descBase = 3 * Layout::kBlocksPerPacket;
    const auto* desc2 =
        ring_.Slab().GetDescriptorPtr(descBase + Layout::kFirstPayloadBlock);
    const auto* desc3 =
        ring_.Slab().GetDescriptorPtr(descBase + Layout::kCompletionBlock);
    EXPECT_EQ(desc2->control & 0xffffu, 8u);
    EXPECT_EQ(desc2->dataAddress, ImageIOVA(3));
    EXPECT_EQ(desc3->control & 0xffffu, 64u);
    EXPECT_EQ(desc3->dataAddress, ImageIOVA(3) + 8);
}

TEST_F(IsochTxDmaRingTest, PrefixIsIgnoredWhenItWouldEmptyTheSecondEntry) {
    // A cadence NO-DATA packet is nothing but the prefix. Both descriptor
    // entries must still carry bytes, so the split falls back to halving.
    auto metadataRing = MakeMetadataRing();
    for (auto& meta : metadataRing) {
        meta.payloadLength = 8;
        meta.payloadPrefixBytes = 8;
    }
    const auto prime = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);

    const uint32_t descBase = 3 * Layout::kBlocksPerPacket;
    const auto* desc2 =
        ring_.Slab().GetDescriptorPtr(descBase + Layout::kFirstPayloadBlock);
    const auto* desc3 =
        ring_.Slab().GetDescriptorPtr(descBase + Layout::kCompletionBlock);
    EXPECT_EQ(desc2->control & 0xffffu, 4u);
    EXPECT_EQ(desc3->control & 0xffffu, 4u);
}

TEST_F(IsochTxDmaRingTest,
       RefillRepointsOnlyTheMutableTailOutsideTheLiveCommandGuard) {
    auto metadataRing = MakeMetadataRing();
    for (auto& meta : metadataRing) {
        meta.payloadLength = 72;
        meta.payloadPrefixBytes = 8;
    }
    primeControl_.numSlots = kSharedPayloadSlots;
    primeControl_.slotStrideBytes = kSharedPayloadStride;
    primeControl_.maxPacketBytes = kSharedPayloadStride;
    const auto prime = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ASSERT_EQ(primeControl_.mappedEnd.load(std::memory_order_acquire),
              Layout::kNumPackets);
    ASSERT_EQ(
        primeControl_.finalizedEnd.load(std::memory_order_acquire),
        ASFW::Shared::Isoch::IsochQueueGeometry::
            kPayloadFinalityLeadPackets);

    // Packet 7 is inside the live guard when CommandPtr reaches packet 6.
    // Packet 8 is exactly two packets away and is the first legal rebind.
    ImageBytes(7, 1)[8] = 0x77;
    ImageBytes(8, 1)[8] = 0x88;
    // Packet 7 is already final: Prime sealed everything below the finality
    // frontier, so the producer is correctly refused rather than left believing
    // it placed content into a packet whose choice was made.
    EXPECT_FALSE(OfferLateImage(metadataRing, 7));
    EXPECT_TRUE(OfferLateImage(metadataRing, 8));

    const uint32_t commandPtr = ring_.Slab().GetDescriptorIOVA(
        6 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(0)),
        commandPtr | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    RetireToCommandPtr(primeControl_);
    const auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.latePayloadRebinds, 1U);
    EXPECT_EQ(outcome.latePayloadRebindRejected, 0U);
    EXPECT_EQ(outcome.finalizedEnd, 14U);

    const auto* packet7Prefix = ring_.Slab().GetDescriptorPtr(
        7 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    const auto* packet7Tail = ring_.Slab().GetDescriptorPtr(
        7 * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    EXPECT_EQ(packet7Prefix->dataAddress, ImageIOVA(7, 0));
    EXPECT_EQ(packet7Tail->dataAddress, ImageIOVA(7, 0) + 8);
    EXPECT_EQ(metadataRing[7].selectedPayloadImage, 0U);

    const auto* packet8Prefix = ring_.Slab().GetDescriptorPtr(
        8 * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
    const auto* packet8Tail = ring_.Slab().GetDescriptorPtr(
        8 * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    EXPECT_EQ(packet8Prefix->dataAddress, ImageIOVA(8, 0));
    EXPECT_EQ(packet8Tail->dataAddress, ImageIOVA(8, 1) + 8);
    EXPECT_EQ(metadataRing[8].selectedPayloadImage, 1U);
    EXPECT_EQ(
        primeControl_.minimumLatePayloadRebindDistance.load(
            std::memory_order_relaxed),
        2U);
}

TEST_F(IsochTxDmaRingTest,
       RefillKeepsArmedImageWhenNoInvariantPrefixWasDeclared) {
    auto metadataRing = MakeMetadataRing();
    for (auto& meta : metadataRing) {
        meta.payloadLength = 72;
        meta.payloadPrefixBytes = 0;
    }
    primeControl_.numSlots = kSharedPayloadSlots;
    primeControl_.slotStrideBytes = kSharedPayloadStride;
    primeControl_.maxPacketBytes = kSharedPayloadStride;
    ASSERT_EQ(
        ring_.Prime(
            payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
            metadataRing.data(), &primeControl_, sharedPayload_.data(),
            Layout::kNumPackets).packetsAssembled,
        Layout::kNumPackets);
    ASSERT_TRUE(OfferLateImage(metadataRing, 8));

    const uint32_t commandPtr = ring_.Slab().GetDescriptorIOVA(
        6 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(0)),
        commandPtr | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(
            DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    RetireToCommandPtr(primeControl_);
    const auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.latePayloadRebinds, 0U);
    EXPECT_EQ(outcome.latePayloadRebindRejected, 1U);
    EXPECT_EQ(metadataRing[8].selectedPayloadImage, 0U);
}

TEST_F(IsochTxDmaRingTest, PrimeProgramsPayloadCrossingDmaSegment) {
    const uint64_t kBoundaryOffset =
        ASFW::Isoch::TxPayloadImageOffset(7, 0, kSharedPayloadStride) + 256;
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
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);

    const uint32_t descBase = 7 * Layout::kBlocksPerPacket;
    const auto* desc2 = ring_.Slab().GetDescriptorPtr(
        descBase + Layout::kFirstPayloadBlock);
    const auto* desc3 = ring_.Slab().GetDescriptorPtr(
        descBase + Layout::kCompletionBlock);
    EXPECT_EQ(desc2->control & 0xffffu, 256u);
    EXPECT_EQ(desc2->dataAddress,
              0x73000000u +
                  ASFW::Isoch::TxPayloadImageOffset(7, 0, kSharedPayloadStride));
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
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
    EXPECT_EQ(prime.packetsAssembled, 0u);
}

TEST_F(IsochTxDmaRingTest, RefillUsesMappedIOVAAfterPageBoundary) {
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    constexpr uint64_t kFirstPageIOVA = 0x75000000u;
    constexpr uint64_t kRemainingPagesIOVA = 0x76000000u;
    // The boundary must land exactly on slot 8's bound image so the assertions
    // below still read "last slot before the split" and "first slot after it".
    const uint64_t kPageBytes =
        ASFW::Isoch::TxPayloadImageOffset(Layout::kNumPackets + 8, 0, kSharedPayloadStride);
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
        metadataRing[Layout::kNumPackets + i].payloadLength = 296;
        metadataRing[i].commitGeneration.store(1, std::memory_order_release);
    }
    RefreshAllPayloadSeals(metadataRing);

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(9 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    RetireToCommandPtr(controlBlock);
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

    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ring_.SetChannel(kConfiguredChannel);
    ring_.SeedCycleTracking(hardware_);

    metadataRing[Layout::kNumPackets].immediateHeader[0] =
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

    RetireToCommandPtr(controlBlock);
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
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    const std::array<TxPayloadDmaSegment, 2> segments{{
        {.deviceAddress = 0x77000000u, .length = ASFW::Isoch::TxPayloadImageOffset(Layout::kNumPackets, 0, kSharedPayloadStride) + 128},
        {
            .deviceAddress = 0x78000000u,
            .length = sharedPayload_.size() - ASFW::Isoch::TxPayloadImageOffset(Layout::kNumPackets, 0, kSharedPayloadStride) - 128,
        },
    }};
    TxPayloadDmaMap crossingMap;
    ASSERT_TRUE(crossingMap.Configure(segments, sharedPayload_.size()));

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    metadataRing[Layout::kNumPackets].payloadLength = 296;
    metadataRing[Layout::kNumPackets].commitGeneration.store(1, std::memory_order_release);
    RefreshPayloadSeal(metadataRing, Layout::kNumPackets);

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    RetireToCommandPtr(controlBlock);
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
    EXPECT_EQ(desc2->dataAddress, 0x77000000u + ASFW::Isoch::TxPayloadImageOffset(Layout::kNumPackets, 0, kSharedPayloadStride));
    EXPECT_EQ(desc3->control & 0xffffu, 168u);
    EXPECT_EQ(desc3->dataAddress, 0x78000000u);
}

TEST_F(IsochTxDmaRingTest, RefillConsumesMetadataAndPushesStamps) {
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    // Allocate host buffers for metadata ring and control block
    IsochTxQueueControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();

    // Pre-populate metadata ring for first lap (8 packets)
    for (uint32_t i = 0; i < 8; ++i) {
        metadataRing[Layout::kNumPackets + i].packetIndex = i;
        metadataRing[Layout::kNumPackets + i].immediateHeader[0] = 0x11110000 + i;
        metadataRing[Layout::kNumPackets + i].immediateHeader[1] = 0x22220000 + i;
        metadataRing[Layout::kNumPackets + i].payloadLength = 100 + i * 4;
        metadataRing[Layout::kNumPackets + i].commitGeneration.store(1, std::memory_order_release); // Lap 1
    }
    RefreshAllPayloadSeals(metadataRing);

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

    ring_.SeedCycleTracking(hardware_);

    // Write mock hw timestamp values into retired OL status words
    for (uint32_t i = 0; i < 8; ++i) {
        auto* desc2 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        const uint16_t timestamp =
            static_cast<uint16_t>((3u << 13) | (3000u + i));
        desc2->statusWord = (0x8000u << 16) | timestamp;
    }

    // Run Refill
    RetireToCommandPtr(controlBlock);
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
                  WithIsochChannel(0x11110000 + i, 1));
        EXPECT_EQ(immDesc->immediateData[1], 0x22220000 + i);

        auto* desc2 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
        auto* desc3 = ring_.Slab().GetDescriptorPtr(
            i * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
        const uint32_t firstLength = (100 + i * 4) / 2;
        EXPECT_EQ(desc2->control & 0xFFFF, firstLength);
        EXPECT_EQ(desc2->dataAddress,
                  ImageIOVA(Layout::kNumPackets + i));
        EXPECT_EQ(desc3->control & 0xFFFF, firstLength);
        EXPECT_EQ(desc3->dataAddress,
                  ImageIOVA(Layout::kNumPackets + i) +
                      firstLength);
    }
}

TEST_F(IsochTxDmaRingTest, CompletionNotificationCoalescesUntilHandled) {
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_,
        kSharedPayloadSlots,
        kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
        Layout::kNumPackets);
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
        RetireToCommandPtr(controlBlock);
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
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;

    // Commit enough producer slots to cross the 48-entry hardware-ring wrap.
    constexpr uint32_t kLastProducerPacket = 2 * Layout::kNumPackets + 6;
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
    RefreshAllPayloadSeals(metadataRing);

    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)), 0);

    // Retire 46 primed packets and map [48, 94); then retire nine more
    // and map [94, 103), including absolute packet 96 into descriptor slot 0.
    const uint32_t beforeWrapIOVA =
        ring_.Slab().GetDescriptorIOVA((Layout::kNumPackets - 2) *
                                       Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        beforeWrapIOVA | Layout::kBlocksPerPacket);
    RetireToCommandPtr(controlBlock);
    const auto beforeWrap = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(beforeWrap.ok);
    ASSERT_EQ(beforeWrap.packetsFilled, Layout::kNumPackets - 2);

    constexpr uint32_t kHardwarePacketAfterWrap = 7;
    const uint32_t afterWrapIOVA =
        ring_.Slab().GetDescriptorIOVA(kHardwarePacketAfterWrap *
                                       Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        afterWrapIOVA | Layout::kBlocksPerPacket);
    RetireToCommandPtr(controlBlock);
    const auto afterWrap = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(afterWrap.ok);
    ASSERT_EQ(afterWrap.packetsFilled, 9u);

    // Absolute packet 96 reuses hardware slot 0, but it must read producer
    // slot 96 rather than producer slot 0. This is the ownership distinction
    // that the removed private payload path obscured.
    auto* wrappedDesc =
        ring_.Slab().GetDescriptorPtr(2);
    EXPECT_EQ(wrappedDesc->dataAddress, ImageIOVA(2 * Layout::kNumPackets));

    auto* wrappedImmediate = reinterpret_cast<OHCIDescriptorImmediate*>(
        ring_.Slab().GetDescriptorPtr(0));
    EXPECT_EQ(wrappedImmediate->immediateData[0],
              WithIsochChannel(
                  0x11000000u + 2 * Layout::kNumPackets, 1));
    EXPECT_EQ(wrappedImmediate->immediateData[1],
              0x22000000u + 2 * Layout::kNumPackets);
}

TEST_F(IsochTxDmaRingTest, RefillRejectsStaleGenerationAtFirstSharedRingWrap) {
    const std::array<uint8_t, 8> payloadBefore{
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
    std::memcpy(sharedPayload_.data(), payloadBefore.data(),
                payloadBefore.size());
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
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
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
        RetireToCommandPtr(controlBlock);
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
        ASFW::Shared::Isoch::IsochQueueGeometry::kPacketsPerCompletionGroup;
    for (uint32_t refill = 1;
         refill <= kGroupsToSharedRingWrap;
         ++refill) {
        const uint32_t hardwarePacketIndex =
            (refill *
             ASFW::Shared::Isoch::IsochQueueGeometry::
                 kPacketsPerCompletionGroup) %
            Layout::kNumPackets;
        ASSERT_TRUE(refillTo(hardwarePacketIndex).ok);
    }

    const auto commitBefore =
        metadataRing[0].commitGeneration.load(std::memory_order_acquire);
    const auto packetBefore = metadataRing[0].packetIndex;

    // Hardware descriptor index of the first group past the shared-ring wrap.
    // The shared ring need not be a whole number of 48-packet hardware laps,
    // so derive it instead of assuming the wrap lands on descriptor 0.
    const auto firstSecondLapRefill =
        refillTo(((kGroupsToSharedRingWrap + 1) *
                  ASFW::Shared::Isoch::IsochQueueGeometry::
                      kPacketsPerCompletionGroup) %
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
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
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
        RetireToCommandPtr(controlBlock);
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
        ASFW::Shared::Isoch::IsochQueueGeometry::kPacketsPerCompletionGroup;
    for (uint32_t refill = 1;
         refill <= kGroupsToSharedRingWrap;
         ++refill) {
        const uint32_t hardwarePacketIndex =
            (refill *
             ASFW::Shared::Isoch::IsochQueueGeometry::
                 kPacketsPerCompletionGroup) %
            Layout::kNumPackets;
        ASSERT_TRUE(refillTo(hardwarePacketIndex).ok);
    }

    for (uint64_t packetIndex = kSharedPayloadSlots;
         packetIndex <
         kSharedPayloadSlots +
             ASFW::Shared::Isoch::IsochQueueGeometry::
                 kPacketsPerCompletionGroup;
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
                  ASFW::Shared::Isoch::IsochQueueGeometry::
                      kPacketsPerCompletionGroup) %
                 Layout::kNumPackets);
    EXPECT_TRUE(firstSecondLapRefill.ok);
    EXPECT_EQ(
        firstSecondLapRefill.packetsFilled,
        ASFW::Shared::Isoch::IsochQueueGeometry::
            kPacketsPerCompletionGroup);
    EXPECT_EQ(metadataRing[0].packetIndex, kSharedPayloadSlots);
    EXPECT_EQ(metadataRing[0].commitGeneration.load(std::memory_order_acquire), 2U);
}

// Coverage runs out exactly two completion groups past the mapped window: the
// first two refills consume committed packets, the third reaches slots the
// producer never committed and must fault rather than transmit a stale lap.
//
// Captured on hardware as "60 committed packets" when the TX ring was 48; the
// number is ring + 2 groups, so it is written that way and follows the ring.
TEST_F(IsochTxDmaRingTest,
       CommittedRingPlusTwoGroupsFailsOnThirdUnservicedCompletionGroup) {
    using Geometry = ASFW::Shared::Isoch::IsochQueueGeometry;
    constexpr uint32_t kHistoricalCommittedPackets =
        Geometry::kTransmitInFlightPackets +
        2 * Geometry::kPacketsPerCompletionGroup;
    // The scenario needs room for the committed region plus the third,
    // uncommitted group inside the test's shared slab.
    static_assert(kHistoricalCommittedPackets +
                      Geometry::kPacketsPerCompletionGroup <=
                  kSharedPayloadSlots);

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
        metadataRing.data(), &primeControl_, sharedPayload_.data(),
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
        RetireToCommandPtr(controlBlock);
        return ring_.Refill(
            hardware_,
            0,
            metadataRing.data(),
            &controlBlock,
            kSharedPayloadSlots,
            sharedPayload_.data(),
            payloadDmaMap_);
    };

    EXPECT_TRUE(refillTo(Geometry::kPacketsPerCompletionGroup).ok);
    EXPECT_TRUE(refillTo(2 * Geometry::kPacketsPerCompletionGroup).ok);

    const auto third =
        refillTo(3 * Geometry::kPacketsPerCompletionGroup);
    EXPECT_FALSE(third.ok);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(
        ring_.RTCounters().txUnderruns.load(std::memory_order_relaxed),
        1U);
}

TEST_F(IsochTxDmaRingTest, RefillRejectsPayloadLargerThanSharedSlot) {
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
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

    RetireToCommandPtr(controlBlock);
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
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
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

    RetireToCommandPtr(controlBlock);
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
       RefillDetectsPayloadMutationAfterReleaseCommitBeforeCompletion) {
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    const auto prime = ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};
    controlBlock.numSlots = kSharedPayloadSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.committedEnd.store(Layout::kNumPackets,
                                    std::memory_order_release);

    // The metadata seal is the release-commit boundary. Any subsequent writer
    // touching this slot must be detected before completion returns ownership.
    sharedPayload_[3] ^= 0x5a;

    const uint32_t nextPacketIOVA =
        ring_.Slab().GetDescriptorIOVA(Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        nextPacketIOVA | Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(
        static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
        0);

    RetireToCommandPtr(controlBlock);
    const auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failureReason,
              IsochTxDmaRing::RefillFailureReason::PayloadSealMismatch);
    EXPECT_EQ(outcome.failurePacketAbs, 0U);
    EXPECT_EQ(outcome.failureSlot, 0U);
    EXPECT_EQ(outcome.failurePayloadLength, 8U);
    EXPECT_NE(outcome.failureExpectedPayloadSeal,
              outcome.failureObservedPayloadSeal);
    EXPECT_EQ(controlBlock.completionCursor.load(std::memory_order_acquire),
              0U);
    EXPECT_EQ(controlBlock.statusWord.load(std::memory_order_acquire),
              IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(controlBlock.streamGeneration.load(std::memory_order_acquire),
              1U);
    EXPECT_EQ(
        ring_.RTCounters().fatalPayloadSealMismatch.load(
            std::memory_order_relaxed),
        1U);
    // Exercise the production exit guard: it must freeze the failing outcome
    // before a caller resets the ring for recovery, including seal identities.
    ring_.ResetForStart();
    bool visited = false;
    EXPECT_TRUE(ring_.FlightRecorderForTest().ExportOnce(
        [&](uint32_t index, uint32_t count, const ASFW::Isoch::Tx::TxRefillRecord& r) {
            visited = true;
            EXPECT_EQ(index, 0U);
            EXPECT_EQ(count, 1U);
            EXPECT_EQ(r.failure, static_cast<uint32_t>(outcome.failureReason));
            EXPECT_EQ(r.failedPacket, outcome.failurePacketAbs);
            EXPECT_EQ(r.expectedSeal, outcome.failureExpectedPayloadSeal);
            EXPECT_EQ(r.observedSeal, outcome.failureObservedPayloadSeal);
            EXPECT_EQ(r.completionBefore, 0U);
            EXPECT_EQ(r.committedBefore, Layout::kNumPackets);
            EXPECT_EQ(r.command, nextPacketIOVA | Layout::kBlocksPerPacket);
            EXPECT_EQ(r.inferredDelta, 1U);
            EXPECT_EQ(r.filled, 0U);
            EXPECT_EQ(r.epoch, 1U); // restart advanced the live epoch to two
        }));
    EXPECT_TRUE(visited);

}

TEST_F(IsochTxDmaRingTest,
       RefillUnderrunIsFatalAndDoesNotInventPacketState) {
    ring_.ResetForStart();
    auto metadataRing = MakeMetadataRing();
    (void)ring_.Prime(
        payloadDmaMap_, kSharedPayloadSlots, kSharedPayloadStride,
        metadataRing.data(), &primeControl_, sharedPayload_.data(), Layout::kNumPackets);
    ring_.SeedCycleTracking(hardware_);

    IsochTxQueueControl controlBlock{};

    const uint32_t numSlots = kSharedPayloadSlots;
    uint8_t* payloadBase = sharedPayload_.data();

    metadataRing[Layout::kNumPackets].commitGeneration.store(0, std::memory_order_release);
    metadataRing[Layout::kNumPackets].immediateHeader[0] = 0x11110000;
    metadataRing[Layout::kNumPackets].immediateHeader[1] = 0x22220000;
    std::array<uint8_t, 8> payloadBefore{
        0x87, 0x76, 0x65, 0x54, 0x43, 0x32, 0x21, 0x10};
    std::memcpy(payloadBase, payloadBefore.data(), payloadBefore.size());
    RefreshPayloadSeal(metadataRing, 0);

    controlBlock.numSlots = numSlots;
    controlBlock.slotStrideBytes = kSharedPayloadStride;
    controlBlock.maxPacketBytes = kSharedPayloadStride;
    controlBlock.completionCursor.store(0, std::memory_order_relaxed);

    // Mock hardware registers: cmdPtr points to packet 8
    const uint32_t nextPktDescIOVA = ring_.Slab().GetDescriptorIOVA(8 * Layout::kBlocksPerPacket);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)), nextPktDescIOVA | Layout::kBlocksPerPacket);

    // Run Refill
    RetireToCommandPtr(controlBlock);
    auto outcome = ring_.Refill(
        hardware_, 0, metadataRing.data(), &controlBlock, numSlots,
        payloadBase, payloadDmaMap_);

    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.packetsFilled, 0);
    EXPECT_EQ(controlBlock.statusWord.load(),
              IsochTxQueueStatus::kProducerFault);
    EXPECT_EQ(controlBlock.streamGeneration.load(), 1);
    EXPECT_EQ(metadataRing[Layout::kNumPackets].commitGeneration.load(), 0);

    auto* desc0 = ring_.Slab().GetDescriptorPtr(0);
    auto* immDesc = reinterpret_cast<OHCIDescriptorImmediate*>(desc0);
    EXPECT_EQ(immDesc->immediateData[0], 0U);
    EXPECT_EQ(immDesc->immediateData[1], 0U);
    EXPECT_EQ(
        std::memcmp(payloadBase, payloadBefore.data(),
                    payloadBefore.size()),
        0);
}

TEST(IsochTxQueueControlTests, ProducerAndConsumerResetsHaveDisjointOwnership) {
    IsochTxQueueControl queue{};
    queue.abiVersion = ASFW::Isoch::kTxQueueAbiVersion;
    queue.committedEnd.store(408, std::memory_order_release);
    queue.completionCursor.store(144, std::memory_order_release);
    queue.mappedEnd.store(192, std::memory_order_release);
    queue.finalizedEnd.store(152, std::memory_order_release);
    queue.statusWord.store(IsochTxQueueStatus::kRunning,
                           std::memory_order_release);

    queue.ResetConsumerForArm();
    EXPECT_EQ(queue.abiVersion, ASFW::Isoch::kTxQueueAbiVersion);
    EXPECT_EQ(queue.committedEnd.load(std::memory_order_acquire), 408U);
    EXPECT_EQ(queue.completionCursor.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(queue.mappedEnd.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(queue.finalizedEnd.load(std::memory_order_acquire), 0U);
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

TEST(IsochTxQueueOwnershipTests, ProducerAcquiresOnlyAppendCursorOwnedSlot) {
    using ASFW::Isoch::CanAcquireTxProducerSlot;

    EXPECT_TRUE(CanAcquireTxProducerSlot(100, 100, 80, 64));
    EXPECT_TRUE(CanAcquireTxProducerSlot(0, 0, 800, 64));
    EXPECT_TRUE(CanAcquireTxProducerSlot(63, 63, 800, 64));
    EXPECT_FALSE(CanAcquireTxProducerSlot(99, 100, 80, 64));
    EXPECT_FALSE(CanAcquireTxProducerSlot(101, 100, 80, 64));
    EXPECT_FALSE(CanAcquireTxProducerSlot(79, 79, 80, 64));
    EXPECT_FALSE(CanAcquireTxProducerSlot(144, 144, 80, 64));
    EXPECT_FALSE(CanAcquireTxProducerSlot(100, 100, 80, 0));
}


// --- Producer/transport arbitration ------------------------------------------
//
// Regression cover for the lost-publication race: transport used to decide a
// packet's payload by skipping it in a scan and advancing a separate frontier
// afterwards, so a producer publishing between those two steps read a frontier
// that had not yet caught up and was told it had won a packet already decided.
// The arbitration word makes offering and deciding one atomic event per packet.

namespace {

/// Runs a hook inside transport's DMA publish, which is where a real producer
/// thread would interleave with the completion pass.
class InterposingDma final : public ASFW::Isoch::Memory::IIsochDMAMemory {
public:
    explicit InterposingDma(ASFW::Isoch::Memory::IIsochDMAMemory& inner)
        : inner_(inner) {}
    mutable std::function<void(const std::byte*)> onPublish;
    mutable std::function<void(const std::byte*)> onFetch;
    mutable std::function<void()> onBarrier;

    std::optional<ASFW::Shared::DMARegion> AllocateDescriptor(size_t n) override {
        return inner_.AllocateDescriptor(n);
    }
    std::optional<ASFW::Shared::DMARegion> AllocatePayloadBuffer(size_t n) override {
        return inner_.AllocatePayloadBuffer(n);
    }
    std::optional<ASFW::Shared::DMARegion> AllocateRegion(size_t n, size_t a) override {
        return inner_.AllocateRegion(n, a);
    }
    uint64_t VirtToIOVA(const std::byte* p) const noexcept override {
        return inner_.VirtToIOVA(p);
    }
    std::byte* IOVAToVirt(uint64_t p) const noexcept override {
        return inner_.IOVAToVirt(p);
    }
    void PublishToDevice(const std::byte* p, size_t n) const noexcept override {
        if (onPublish) onPublish(p);
        inner_.PublishToDevice(p, n);
    }
    void FetchFromDevice(const std::byte* p, size_t n) const noexcept override {
        inner_.FetchFromDevice(p, n);
        if (onFetch) onFetch(p);
    }
    void PublishBarrier() const noexcept override {
        inner_.PublishBarrier();
        if (onBarrier) onBarrier();
    }
    size_t TotalSize() const noexcept override { return inner_.TotalSize(); }
    size_t AvailableSize() const noexcept override { return inner_.AvailableSize(); }

private:
    ASFW::Isoch::Memory::IIsochDMAMemory& inner_;
};

} // namespace

class IsochTxPayloadArbitrationTest : public IsochTxDmaRingTest {
protected:
    std::unique_ptr<InterposingDma> interposed_;

    void SetUp() override {
        IsochMemoryConfig config;
        config.numDescriptors = Layout::kRingBlocks;
        config.packetSizeBytes = 0;
        config.descriptorAlignment = Layout::kDescriptorPageStride;
        config.payloadPageAlignment = 16384;
        config.allocatePayloadSlab = false;

        dmaMemory_ = IsochDMAMemoryManager::Create(config);
        ASSERT_NE(dmaMemory_, nullptr);
        ASSERT_TRUE(dmaMemory_->Initialize(hardware_));
        interposed_ = std::make_unique<InterposingDma>(*dmaMemory_);
        ring_.SetChannel(1);
        ASSERT_EQ(ring_.SetupRings(*interposed_), kIOReturnSuccess);
        ring_.SeedCycleTracking(hardware_);

        const TxPayloadDmaSegment payloadSegment{
            .deviceAddress = kSharedPayloadIOVA,
            .length = sharedPayload_.size(),
        };
        ASSERT_TRUE(payloadDmaMap_.Configure(
            std::span<const TxPayloadDmaSegment>(&payloadSegment, 1),
            sharedPayload_.size()));
    }

    /// Place the live command pointer on a packet, as the controller would.
    void PointAt(uint32_t packet) {
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
            ring_.Slab().GetDescriptorIOVA(packet * Layout::kBlocksPerPacket) |
                Layout::kBlocksPerPacket);
        hardware_.SetTestRegister(
            static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),
            0);
    }

    /// Packets need a real invariant prefix and a two-fragment payload before a
    /// late rebind is legal at all.
    void PrimeRebindable(std::vector<IsochTxPacketMeta>& metadataRing) {
        for (auto& meta : metadataRing) {
            meta.payloadLength = 72;
            meta.payloadPrefixBytes = 8;
        }
        primeControl_.numSlots = kSharedPayloadSlots;
        primeControl_.slotStrideBytes = kSharedPayloadStride;
        primeControl_.maxPacketBytes = kSharedPayloadStride;
        primeControl_.committedEnd.store(Layout::kNumPackets);
        ASSERT_EQ(
            ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
                        kSharedPayloadStride, metadataRing.data(),
                        &primeControl_, sharedPayload_.data(), Layout::kNumPackets)
                .packetsAssembled,
            Layout::kNumPackets);
    }

    auto Refill(std::vector<IsochTxPacketMeta>& metadataRing) {
        RetireToCommandPtr(primeControl_);
        return ring_.Refill(hardware_, 0, metadataRing.data(), &primeControl_,
                            kSharedPayloadSlots, sharedPayload_.data(),
                            payloadDmaMap_);
    }
};

// The reproduced schedule from the V3 review: the scan passes packet 8 with
// nothing on offer, and the producer publishes packet 8 while transport is
// still working on packet 9. Transport now revisits the packet immediately
// before sealing it, so the content the producer was told it placed is the
// content that goes on the wire.
TEST_F(IsochTxPayloadArbitrationTest,
       OfferLandingDuringTheScanIsBoundNotSilentlyDropped) {
    auto metadataRing = MakeMetadataRing();
    PrimeRebindable(metadataRing);
    PointAt(6);

    ASSERT_TRUE(OfferLateImage(metadataRing, 9));

    bool producerAccepted = false;
    bool interleaved = false;
    interposed_->onPublish = [&](const std::byte* published) {
        if (published != reinterpret_cast<const std::byte*>(ImageBytes(9, 1))) {
            return;
        }
        interleaved = true;
        producerAccepted = OfferLateImage(metadataRing, 8);
    };

    const auto outcome = Refill(metadataRing);
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(interleaved);

    EXPECT_TRUE(producerAccepted);
    EXPECT_EQ(metadataRing[8].selectedPayloadImage, 1U);
    EXPECT_EQ(metadataRing[9].selectedPayloadImage, 1U);
    EXPECT_EQ(outcome.latePayloadLostPublications, 0U);
}

// The same interleaving against a packet transport has already sealed. The
// producer must be refused. Under the old frontier comparison this returned
// success -- the frontier still read 8 while packet 8's fate was already
// decided -- and the engine counted silence as content.
//
// Reaching that state takes two interleavings: an offer must land after the
// selection pass has gone past its packet, so that the packet is only bound
// during the finality pass, which is the one that runs after packet 8 is
// sealed.
TEST_F(IsochTxPayloadArbitrationTest,
       OfferForAnAlreadySealedPacketIsRefused) {
    auto metadataRing = MakeMetadataRing();
    PrimeRebindable(metadataRing);
    PointAt(6);

    ASSERT_TRUE(OfferLateImage(metadataRing, 20));

    bool lateOfferPlaced = false;
    bool interleaved = false;
    bool producerAccepted = true;
    interposed_->onPublish = [&](const std::byte* published) {
        if (published == reinterpret_cast<const std::byte*>(ImageBytes(20, 1))) {
            // The selection pass is past packet 13 by now, so this offer can
            // only be taken up by the finality pass.
            lateOfferPlaced = OfferLateImage(metadataRing, 13);
            return;
        }
        if (published == reinterpret_cast<const std::byte*>(ImageBytes(13, 1))) {
            // Packet 8 was sealed at the top of that same finality pass.
            interleaved = true;
            producerAccepted = OfferLateImage(metadataRing, 8);
        }
    };

    const auto outcome = Refill(metadataRing);
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(lateOfferPlaced);
    ASSERT_TRUE(interleaved);
    ASSERT_EQ(outcome.finalizedEnd, 14U);

    EXPECT_FALSE(producerAccepted);
    EXPECT_EQ(metadataRing[8].selectedPayloadImage, 0U);
    EXPECT_EQ(metadataRing[13].selectedPayloadImage, 1U);
}

// A packet inside the live-command guard cannot be repointed, so an image
// published for it is genuinely discarded. That is a real outcome, not an
// error, and it has to be counted where transport decides it.
TEST_F(IsochTxPayloadArbitrationTest,
       ImageDiscardedInsideTheGuardIsCountedAsALostPublication) {
    auto metadataRing = MakeMetadataRing();
    PrimeRebindable(metadataRing);
    PointAt(6);
    ASSERT_TRUE(Refill(metadataRing).ok);
    ASSERT_EQ(primeControl_.finalizedEnd.load(), 14U);

    // Packet 16 is open: beyond the frontier and outside the guard.
    ASSERT_TRUE(OfferLateImage(metadataRing, 16));

    // The controller then jumps a full completion group, putting packet 16
    // inside the guard before transport ever gets to bind it.
    PointAt(15);
    const auto outcome = Refill(metadataRing);
    ASSERT_TRUE(outcome.ok);

    EXPECT_EQ(metadataRing[16].selectedPayloadImage, 0U);
    EXPECT_EQ(outcome.latePayloadLostPublications, 1U);
    EXPECT_EQ(primeControl_.latePayloadLostPublicationCount.load(), 1U);
    // And the packet stays refused from now on.
    EXPECT_FALSE(OfferLateImage(metadataRing, 16));
}

// The controller can advance while transport is publishing the alternate image.
// The position that authorised the rebind was sampled before completion
// processing and before every packet examined ahead of this one, so trusting it
// let the descriptor address be changed on a packet the controller had already
// reached, while reporting a two-packet margin that no longer existed.
TEST_F(IsochTxPayloadArbitrationTest,
       RebindIsAbandonedWhenTheControllerReachesThePacketDuringPublication) {
    auto metadataRing = MakeMetadataRing();
    PrimeRebindable(metadataRing);
    PointAt(6);
    ASSERT_TRUE(OfferLateImage(metadataRing, 8));

    bool advanced = false;
    interposed_->onPublish = [&](const std::byte* published) {
        if (published != reinterpret_cast<const std::byte*>(ImageBytes(8, 1))) {
            return;
        }
        // Hardware proceeds during the delay after the MMIO snapshot.
        PointAt(8);
        advanced = true;
    };

    const auto outcome = Refill(metadataRing);
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(advanced);

    // Counted once: the miss is permanent, so the packet is sealed rather than
    // retried and re-counted by the finality pass.
    EXPECT_EQ(outcome.latePayloadRebindMissedDeadline, 1U);
    EXPECT_EQ(outcome.latePayloadRebinds, 0U);
    EXPECT_EQ(metadataRing[8].selectedPayloadImage, 0U);
    // And the producer's image is booked as the lost publication it is.
    EXPECT_EQ(outcome.latePayloadLostPublications, 1U);
    // No margin was claimed, because none was ever verified.
    EXPECT_EQ(primeControl_.minimumLatePayloadRebindDistance.load(),
              ~uint32_t{0});
}

// A rebind that keeps its distance is still accepted, and the margin it reports
// is measured against the position actually checked.
TEST_F(IsochTxPayloadArbitrationTest,
       AcceptedRebindReportsTheMarginItVerified) {
    auto metadataRing = MakeMetadataRing();
    PrimeRebindable(metadataRing);
    PointAt(6);
    ASSERT_TRUE(OfferLateImage(metadataRing, 8));

    const auto outcome = Refill(metadataRing);
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.latePayloadRebinds, 1U);
    EXPECT_EQ(outcome.latePayloadRebindMissedDeadline, 0U);
    EXPECT_EQ(metadataRing[8].selectedPayloadImage, 1U);
    EXPECT_EQ(primeControl_.minimumLatePayloadRebindDistance.load(), 2U);
}

// These cases drive production retirement, not a model of packet ownership.
// Completion is read from OUTPUT_LAST descriptor status, so a late observation
// is not ambiguous -- it simply finds more finished descriptors. This is the
// September 7 Instruments gap (7.865 ms, 63 cycles, modulo advance 15), which
// the estimator turned into 63 retired packets and a seal failure at the mapped
// frontier. Here it retires exactly what hardware reported and nothing more.
TEST_F(IsochTxDmaRingTest, LateObservationCountsFinishedDescriptorsInsteadOfGuessing) {
    auto metadata = MakeMetadataRing();
    ring_.ResetForStart();
    hardware_.SetTestRegister(Register32::kCycleTimer, 100u << 12);
    ring_.SeedCycleTracking(hardware_);
    ASSERT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
        kSharedPayloadStride, metadata.data(), &primeControl_,
        sharedPayload_.data(), Layout::kNumPackets).packetsAssembled,
        Layout::kNumPackets);
    primeControl_.numSlots = kSharedPayloadSlots;
    primeControl_.slotStrideBytes = kSharedPayloadStride;
    primeControl_.maxPacketBytes = kSharedPayloadStride;

    // 63 cycles on, and the CommandPtr has moved only 15 slots -- exactly the
    // observation the estimator lifted to a lap. Hardware finished 15.
    MarkCompletedByHardware(0, 15);
    // Poison the packet the estimator would have invented. If the walk ever
    // reaches past what hardware reported, this fires.
    metadata[Layout::kNumPackets + 15].payloadLength = UINT32_MAX;
    hardware_.SetTestRegister(Register32::kCycleTimer, 163u << 12);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring_.Slab().GetDescriptorIOVA(15 * Layout::kBlocksPerPacket) | Layout::kBlocksPerPacket);

    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.completedPacketCount, 14U);
    EXPECT_EQ(primeControl_.completionCursor.load(), 14U);
    EXPECT_EQ(ring_.RTCounters().fatalPayloadSealMismatch.load(), 0U);
    EXPECT_EQ(ring_.RTCounters().lapUnresolvable.load(), 0U);
}

// The old admission gate refused any observation more than 47 packet-times
// apart, which stopped the stream on a 5.9 ms scheduling stall -- barely above
// the 40-42 packet DriverKit stalls this driver already measures. Elapsed time
// is no longer consulted at all.
TEST_F(IsochTxDmaRingTest, AnEightSecondGapIsNotAmbiguousWhenDescriptorsReportCompletion) {
    auto metadata = MakeMetadataRing();
    ring_.ResetForStart();
    ring_.SeedCycleTracking(hardware_);
    ASSERT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
        kSharedPayloadStride, metadata.data(), &primeControl_,
        sharedPayload_.data(), Layout::kNumPackets).packetsAssembled,
        Layout::kNumPackets);
    primeControl_.numSlots = kSharedPayloadSlots;
    primeControl_.slotStrideBytes = kSharedPayloadStride;
    primeControl_.maxPacketBytes = kSharedPayloadStride;

    MarkCompletedByHardware(0, 30);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring_.Slab().GetDescriptorIOVA(30 * Layout::kBlocksPerPacket) | Layout::kBlocksPerPacket);
    clock_.Advance(8'000'000'000ULL); // the short cycle-timer representation wraps

    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.completedPacketCount, 29U);
    EXPECT_EQ(primeControl_.completionCursor.load(), 29U);
    EXPECT_EQ(ring_.RTCounters().lapUnresolvable.load(), 0U);
}

// Cycles are not consulted, so they cannot invent a lap however many elapse.
TEST_F(IsochTxDmaRingTest, SkippedCyclesCannotInventACompletedLap) {
    auto metadata = MakeMetadataRing();
    ring_.ResetForStart();
    hardware_.SetTestRegister(Register32::kCycleTimer, 100u << 12);
    ring_.SeedCycleTracking(hardware_);
    ASSERT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
        kSharedPayloadStride, metadata.data(), &primeControl_,
        sharedPayload_.data(), Layout::kNumPackets).packetsAssembled,
        Layout::kNumPackets);
    primeControl_.numSlots = kSharedPayloadSlots;
    primeControl_.slotStrideBytes = kSharedPayloadStride;
    primeControl_.maxPacketBytes = kSharedPayloadStride;

    // 27 cycles elapse; the controller finished two packets and skipped the
    // rest. Descriptor status says two, and two is what is retired.
    MarkCompletedByHardware(0, 2);
    hardware_.SetTestRegister(Register32::kCycleTimer, 127u << 12);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring_.Slab().GetDescriptorIOVA(2 * Layout::kBlocksPerPacket) | Layout::kBlocksPerPacket);
    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.completedPacketCount, 1U);
    EXPECT_EQ(primeControl_.completionCursor.load(), 1U);
    EXPECT_EQ(primeControl_.mappedEnd.load(), Layout::kNumPackets + 1U);
    EXPECT_EQ(ring_.RTCounters().lastDmaGapPackets.load(), Layout::kNumPackets - 2U);
}

// Exhaustion retains the terminal descriptor as the hardware wake anchor.
// Until coordinated restart can reclaim that anchor, the whole mapped region
// is refused, with no ownership returned. The zero tail prevents replay.
TEST_F(IsochTxDmaRingTest, ConsumingTheWholeMappedRegionIsRefused) {
    auto metadata = MakeMetadataRing();
    ring_.ResetForStart();
    hardware_.SetTestRegister(Register32::kCycleTimer, 100u << 12);
    ring_.SeedCycleTracking(hardware_);
    ASSERT_EQ(ring_.Prime(payloadDmaMap_, kSharedPayloadSlots,
        kSharedPayloadStride, metadata.data(), &primeControl_,
        sharedPayload_.data(), Layout::kNumPackets).packetsAssembled,
        Layout::kNumPackets);
    primeControl_.numSlots = kSharedPayloadSlots;
    primeControl_.slotStrideBytes = kSharedPayloadStride;
    primeControl_.maxPacketBytes = kSharedPayloadStride;
    ASSERT_EQ(primeControl_.mappedEnd.load(), Layout::kNumPackets);

    MarkCompletedByHardware(0, Layout::kNumPackets);
    hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring_.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket);

    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(out.failureReason,
              IsochTxDmaRing::RefillFailureReason::MappedRegionExhausted);
    EXPECT_EQ(out.completedPacketCount, 0U);
    // Refused means refused: nothing retired, nothing mapped, no ownership back.
    EXPECT_EQ(primeControl_.completionCursor.load(), 0U);
    EXPECT_EQ(primeControl_.mappedEnd.load(), Layout::kNumPackets);
    EXPECT_EQ(ring_.RTCounters().lapUnresolvable.load(), 1U);
    EXPECT_EQ(ring_.RTCounters().fatalPayloadSealMismatch.load(), 0U);
}

// Execute the actual DMA links during a refill stall. Merely setting a modulo
// pointer would assume the bug: the hardware must be able to REACH that slot.
TEST_F(IsochTxPayloadArbitrationTest, StallAfterCompletionScanCannotReplayRetiredPayload) {
    auto metadata = MakeMetadataRing();
    PrimeRebindable(metadata);
    PointAt(2);
    MarkCompletedByHardware(0, 2);
    std::vector<uint32_t> payloadAddresses;
    auto drain = [&](uint32_t start) {
        uint32_t slot = start;
        for (uint32_t n = 0; n <= Layout::kNumPackets; ++n) {
            auto* payload = ring_.Slab().GetDescriptorPtr(
                slot * Layout::kBlocksPerPacket + Layout::kFirstPayloadBlock);
            payloadAddresses.push_back(payload->dataAddress);
            MarkCompletedByHardware(slot, 1);
            PointAt(slot);
            auto* tail = ring_.Slab().GetDescriptorPtr(
                slot * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
            if ((tail->branchWord & 0xf) == 0) return;
            uint32_t logical = 0;
            ASSERT_TRUE(ring_.Slab().DecodeCmdAddrToLogicalIndex(
                tail->branchWord & ~0xfU, logical));
            slot = logical / Layout::kBlocksPerPacket;
        }
        FAIL() << "DMA queue cycles instead of stopping at its unpublished tail";
    };
    unsigned zeroReads = 0;
    bool injected = false;
    const auto* zero = reinterpret_cast<const std::byte*>(
        ring_.Slab().GetDescriptorPtr(Layout::kCompletionBlock));
    interposed_->onFetch = [&](const std::byte* p) {
        if (p == zero && ++zeroReads == 2) {
            // The scan saw packets 0/1 done and packet 2 pending. Pause software
            // now; let DMA consume every remaining reachable packet.
            drain(2);
            clock_.Advance(6'000'000);
            injected = true;
        }
    };
    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(injected);
    ASSERT_TRUE(out.ok);
    ASSERT_EQ(payloadAddresses.size(), Layout::kNumPackets - 2);
    for (uint32_t i = 0; i < payloadAddresses.size(); ++i) {
        EXPECT_EQ(payloadAddresses[i], ImageIOVA(i + 2));
    }
    // Publication links the retained old tail to the newly bound packet 48.
    const auto* oldTail = ring_.Slab().GetDescriptorPtr(
        (Layout::kNumPackets - 1) * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    EXPECT_EQ(oldTail->branchWord,
        ring_.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket);
    payloadAddresses.clear();
    drain(0); // what WAKE follows from the old terminal branch
    ASSERT_EQ(payloadAddresses.size(), 1U);
    EXPECT_EQ(payloadAddresses[0], ImageIOVA(Layout::kNumPackets));
    EXPECT_EQ(primeControl_.completionCursor.load(), 1U);
    EXPECT_EQ(primeControl_.mappedEnd.load(), Layout::kNumPackets + 1U);
}

TEST_F(IsochTxPayloadArbitrationTest, AppendPublishesDetachedBatchBeforeOpeningOldTail) {
    auto metadata = MakeMetadataRing();
    PrimeRebindable(metadata);
    PointAt(3);
    MarkCompletedByHardware(0, 4);
    auto* oldTail = ring_.Slab().GetDescriptorPtr(
        (Layout::kNumPackets - 1) * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    uint32_t publishedCompletions = 0;
    uint32_t barriers = 0;
    uint32_t barrierAtLastDescriptor = 0;
    bool linked = false;
    interposed_->onBarrier = [&] { ++barriers; };
    interposed_->onPublish = [&](const std::byte* p) {
        for (uint32_t slot = 0; slot < 3; ++slot) {
            if (p == reinterpret_cast<const std::byte*>(ring_.Slab().GetDescriptorPtr(
                    slot * Layout::kBlocksPerPacket + Layout::kCompletionBlock))) {
                EXPECT_EQ(oldTail->branchWord, 0U);
                ++publishedCompletions;
                barrierAtLastDescriptor = barriers;
            }
        }
        if (p == reinterpret_cast<const std::byte*>(&oldTail->branchWord)) {
            EXPECT_EQ(publishedCompletions, 3U);
            EXPECT_GT(barriers, barrierAtLastDescriptor);
            auto* newTail = ring_.Slab().GetDescriptorPtr(
                2 * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
            EXPECT_EQ(newTail->branchWord, 0U);
            EXPECT_EQ(newTail->statusWord >> 16, 0U);
            linked = true;
        }
    };
    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    ASSERT_TRUE(out.ok);
    EXPECT_TRUE(linked);
}

TEST_F(IsochTxDmaRingTest, QueueAppendWakesEvenWhenActiveWasObserved) {
    const auto control = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0));
    const auto controlSet = static_cast<Register32>(DMAContextHelpers::IsoXmitContextControlSet(0));
    hardware_.SetTestRegister(control, ASFW::Driver::ContextControl::kRun |
        ASFW::Driver::ContextControl::kActive);
    EXPECT_FALSE(ring_.WakeHardware(hardware_, 0)); // idle-only probe
    EXPECT_TRUE(ring_.WakeHardware(hardware_, 0, true)); // branch may be prefetched
    EXPECT_EQ(hardware_.GetTestRegister(controlSet), ASFW::Driver::ContextControl::kWake);
    hardware_.SetTestRegister(control, ASFW::Driver::ContextControl::kDead);
    EXPECT_FALSE(ring_.WakeHardware(hardware_, 0, true));
    hardware_.SetTestRegister(control, 0);
    EXPECT_FALSE(ring_.WakeHardware(hardware_, 0, true));
}

TEST_F(IsochTxPayloadArbitrationTest, CompletedTerminalStaysIntactUntilSuccessorCompletes) {
    auto metadata = MakeMetadataRing();
    PrimeRebindable(metadata);
    PointAt(2);
    MarkCompletedByHardware(0, 2);
    auto refill = [&] {
        return ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
            kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    };
    ASSERT_TRUE(refill().ok); // retire 0, append 48, retain completed 1
    auto* terminal = ring_.Slab().GetDescriptorPtr(
        (Layout::kNumPackets - 1) * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    const auto oldTerminal = *terminal;
    MarkCompletedByHardware(2, Layout::kNumPackets - 2);
    PointAt(Layout::kNumPackets - 1); // parked at old terminal, WAKE pending
    ASSERT_TRUE(refill().ok);
    EXPECT_EQ(primeControl_.completionCursor.load(), Layout::kNumPackets - 1);
    EXPECT_EQ(terminal->branchWord, oldTerminal.branchWord);
    EXPECT_EQ(terminal->dataAddress, oldTerminal.dataAddress);
    EXPECT_EQ(primeControl_.mappedEnd.load(), 2U * Layout::kNumPackets - 1);
    // Only after successor 48 finishes may descriptor 47 be rebound as 95.
    MarkCompletedByHardware(Layout::kNumPackets, 1);
    PointAt(0);
    ASSERT_TRUE(refill().ok);
    EXPECT_EQ(primeControl_.completionCursor.load(), Layout::kNumPackets);
    EXPECT_EQ(primeControl_.mappedEnd.load(), 2U * Layout::kNumPackets);
    EXPECT_NE(terminal->dataAddress, oldTerminal.dataAddress);
    EXPECT_EQ(terminal->branchWord, 0U);
}

TEST_F(IsochTxPayloadArbitrationTest, FailedDetachedBatchNeverOpensTheLiveTail) {
    auto metadata = MakeMetadataRing();
    PrimeRebindable(metadata);
    MarkCompletedByHardware(0, 4);
    PointAt(4);
    metadata[Layout::kNumPackets + 1].commitGeneration.store(0);
    const auto out = ring_.Refill(hardware_, 0, metadata.data(), &primeControl_,
        kSharedPayloadSlots, sharedPayload_.data(), payloadDmaMap_);
    EXPECT_EQ(out.failureReason, IsochTxDmaRing::RefillFailureReason::UncommittedSlot);
    const auto* tail = ring_.Slab().GetDescriptorPtr(
        (Layout::kNumPackets - 1) * Layout::kBlocksPerPacket + Layout::kCompletionBlock);
    EXPECT_EQ(tail->branchWord, 0U);
    EXPECT_EQ(primeControl_.mappedEnd.load(), Layout::kNumPackets);
}
