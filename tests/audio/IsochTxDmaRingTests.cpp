// IsochTxDmaRingTests.cpp
// ASFW - Host-safe unit tests for IT DMA ring engine and timing synchronization

#include <gtest/gtest.h>

#include "Isoch/Transmit/IsochTxDmaRing.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/OHCIDescriptors.hpp"

#include <cstddef>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

using namespace ASFW::Isoch;
using namespace ASFW::Isoch::Tx;
using namespace ASFW::Isoch::Memory;

namespace {

std::shared_ptr<IIsochDMAMemory> MakeTestIsochMemory(::ASFW::Driver::HardwareInterface& hw,
                                                     size_t numPackets,
                                                     size_t maxPacketSizeBytes) {
    IsochMemoryConfig config;
    // IT uses 3 descriptors per packet (OMI, OMI, OL).
    config.numDescriptors = numPackets * 3;
    config.packetSizeBytes = maxPacketSizeBytes;
    config.descriptorAlignment = 4096;
    config.payloadPageAlignment = 4096;

    auto concreteMgr = IsochDMAMemoryManager::Create(config);
    EXPECT_TRUE(concreteMgr);
    EXPECT_TRUE(concreteMgr->Initialize(hw));
    return concreteMgr;
}

class DummyPacketProvider : public IIsochTxPacketProvider {
public:
    IsochTxPacket NextTransmitPacket(const TxPacketRequest& request) noexcept override {
        static uint32_t dummyWords[4] = {0};
        IsochTxPacket pkt{};
        pkt.words = dummyWords;
        pkt.sizeBytes = 16;
        pkt.isData = true;
        pkt.dbc = 0;
        return pkt;
    }
};

class TimedSilentPacketProvider final : public IIsochTxPacketProvider {
public:
    IsochTxPacket NextTransmitPacket(const TxPacketRequest&) noexcept override {
        packet_[0] = 0x01020304;
        packet_[1] = 0x05061234;
        packet_[2] = 0;
        packet_[3] = 0;
        const uint64_t firstFrame = nextFrame_;
        nextFrame_ += 8;
        return IsochTxPacket{
            .words = packet_.data(),
            .sizeBytes = static_cast<uint32_t>(packet_.size() * sizeof(uint32_t)),
            .isData = true,
            .dbc = static_cast<uint8_t>(firstFrame),
            .syt = 0x1234,
            .framesPerPacket = 8,
            .audioFrame = firstFrame,
        };
    }

private:
    std::array<uint32_t, 4> packet_{};
    uint64_t nextFrame_{0};
};

class MarkerPayloadPreparer final : public IIsochTxPayloadPreparer {
public:
    PreparedTxPayloadResult PreparePayload(
        const PreparedTxPayloadRequest& request) noexcept override {
        if (!request.writable) {
            return {};
        }
        auto* words = reinterpret_cast<uint32_t*>(request.payloadBytes);
        words[2] = 0xA5000000u | request.packetIndex;
        return PreparedTxPayloadResult{
            .action = PreparedTxAction::Prepared,
        };
    }
};

class FatalDeadlinePreparer final : public IIsochTxPayloadPreparer {
public:
    PreparedTxPayloadResult PreparePayload(
        const PreparedTxPayloadRequest& request) noexcept override {
        return PreparedTxPayloadResult{
            .action = request.deadline ? PreparedTxAction::Fatal
                                       : PreparedTxAction::NoChange,
        };
    }
};

class RecordingCompletionObserver final : public IIsochTxCompletionObserver {
public:
    bool OnTransmitSlotCompleted(
        const CompletedTxSlot& completed) noexcept override {
        completedSlots.push_back(completed);
        return completed.payloadHashMatches;
    }

    std::vector<CompletedTxSlot> completedSlots;
};

class RecordingIsochMemory final : public IIsochDMAMemory {
public:
    enum class EventKind {
        Publish,
        Barrier,
    };

    struct Event {
        EventKind kind;
        const std::byte* address;
        size_t length;
    };

    explicit RecordingIsochMemory(std::shared_ptr<IIsochDMAMemory> inner)
        : inner_(std::move(inner)) {}

    std::optional<ASFW::Shared::DMARegion> AllocateDescriptor(size_t size) override {
        return inner_->AllocateDescriptor(size);
    }

    std::optional<ASFW::Shared::DMARegion> AllocatePayloadBuffer(size_t size) override {
        return inner_->AllocatePayloadBuffer(size);
    }

    std::optional<ASFW::Shared::DMARegion> AllocateRegion(size_t size,
                                                          size_t alignment) override {
        return inner_->AllocateRegion(size, alignment);
    }

    uint64_t VirtToIOVA(const std::byte* virt) const noexcept override {
        return inner_->VirtToIOVA(virt);
    }

    std::byte* IOVAToVirt(uint64_t iova) const noexcept override {
        return inner_->IOVAToVirt(iova);
    }

    void PublishToDevice(const std::byte* address, size_t length) const noexcept override {
        events.push_back(Event{EventKind::Publish, address, length});
        inner_->PublishToDevice(address, length);
    }

    void PublishBarrier() const noexcept override {
        events.push_back(Event{EventKind::Barrier, nullptr, 0});
        inner_->PublishBarrier();
    }

    void FetchFromDevice(const std::byte* address, size_t length) const noexcept override {
        inner_->FetchFromDevice(address, length);
    }

    size_t TotalSize() const noexcept override {
        return inner_->TotalSize();
    }

    size_t AvailableSize() const noexcept override {
        return inner_->AvailableSize();
    }

    mutable std::vector<Event> events;

private:
    std::shared_ptr<IIsochDMAMemory> inner_;
};

bool AddressInRegion(const std::byte* address, const ASFW::Shared::DMARegion& region) {
    const auto* begin = reinterpret_cast<const std::byte*>(region.virtualBase);
    return address >= begin && address < (begin + region.size);
}

} // namespace

TEST(IsochTxDmaRingTests, ResyncCycleTracking_BugDemonstration) {
    ::ASFW::Driver::HardwareInterface hw;
    // Set current hardware cycle timer to:
    // cycleSeconds = 2, cycleCount = 1000, cycleOffset = 0
    // cycleSeconds is bits 31:25, cycleCount is bits 24:12, cycleOffset is bits 11:0
    const uint32_t cycleTimeReg = (2u << 25) | (1000u << 12) | 0u;
    hw.SetTestRegister(ASFW::Driver::Register32::kCycleTimer, cycleTimeReg);

    // IsochTxDmaRing has 192 packets.
    auto mem = MakeTestIsochMemory(hw, 192, 4096);
    ASSERT_TRUE(mem);

    IsochTxDmaRing ring;
    ring.SetChannel(0);
    const auto setupKr = ring.SetupRings(*mem);
    ASSERT_EQ(setupKr, kIOReturnSuccess);

    // Seed cycle tracking: nextTransmitCycle_ will be initialized to (1000 + 4) = 1004.
    ring.SeedCycleTracking(hw);

    // We simulate a packet completion.
    // The packet was actually sent at cycle count 978 (seconds = 2).
    // The OHCI controller writes back the completion status to statusWord.
    // In OHCI 1.1, the timestamp format is [cycleSeconds:3][cycleCount:13].
    // So the lower 16 bits of statusWord will contain:
    // (cycleSeconds << 13) | cycleCount = (2 << 13) | 978 = 16384 + 978 = 17362.
    // If the completion code is 0 (success), statusWord is exactly 17362.
    const uint16_t statusWordTimestamp = (2u << 13) | 978u;
    
    // Set this statusWord in the completed descriptor of packet index 0.
    // The completed descriptor is the last one in the packet (OMI, OMI, OL) -> index 2.
    auto* lastDesc = ring.Slab().GetDescriptorPtr(2);
    ASSERT_NE(lastDesc, nullptr);
    lastDesc->statusWord = statusWordTimestamp;
    mem->PublishToDevice(reinterpret_cast<const std::byte*>(lastDesc), sizeof(*lastDesc));

    // Invoke Refill with hwPacketIndex = 1.
    // In Refill(), ComputeDeltaConsumed(1) is called:
    // Since lastHwPacketIndex_ starts at 0, deltaConsumed = (1 - 0) = 1.
    // Thus lastProcessedPkt = 0.
    // It reads the statusWord of packet index 0 (which has our statusWordTimestamp).
    // Set the command pointer (cmdPtr) for DMA context 0.
    // hwPacketIndex = 1 corresponds to logical descriptor index 3 (packetIndex * 3).
    // The lower 4 bits are Z-count (which is 3 for our layout blocks per packet).
    const uint32_t cmdPtrRegOffset = ::DMAContextHelpers::IsoXmitCommandPtr(0);
    const uint32_t cmdPtrVal = ring.Slab().GetDescriptorIOVA(3) | 3;
    hw.SetTestRegister(static_cast<::ASFW::Driver::Register32>(cmdPtrRegOffset), cmdPtrVal);

    DummyPacketProvider provider;
    
    // Call Refill.
    const auto outcome = ring.Refill(hw, /*contextIndex=*/0, provider, nullptr, nullptr);
    ASSERT_TRUE(outcome.ok);

    // Inspect the outcome's hwTimestamp (which is outputLastTimestamp & 0x1FFF).
    // The outcome.eventGroup.outputLastTimestamp contains 0x8000 | hwCycle.
    const uint32_t decodedHwCycle = outcome.eventGroup.outputLastTimestamp & 0x1FFFu;
    
    // With the bug fixed, we expect the correctly decoded cycle count 978.
    EXPECT_EQ(decodedHwCycle, 978u);
}

TEST(IsochTxDmaRingTests, PrimeAndRefillPublishPayloadThenBarrierThenDescriptor) {
    ::ASFW::Driver::HardwareInterface hw;
    auto concrete = MakeTestIsochMemory(hw, Layout::kNumPackets, Layout::kMaxPacketSize);
    ASSERT_TRUE(concrete);
    RecordingIsochMemory memory(concrete);

    IsochTxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(memory), kIOReturnSuccess);
    const auto descriptorRegion = ring.Slab().DescriptorRegion();
    const auto payloadRegion = ring.Slab().PayloadRegion();
    DummyPacketProvider provider;

    memory.events.clear();
    const auto prime = ring.Prime(provider);
    ASSERT_EQ(prime.packetsAssembled, Layout::kNumPackets);
    ASSERT_EQ(memory.events.size(), Layout::kNumPackets * 3U);
    for (size_t i = 0; i < memory.events.size(); i += 3) {
        EXPECT_EQ(memory.events[i].kind, RecordingIsochMemory::EventKind::Publish);
        EXPECT_TRUE(AddressInRegion(memory.events[i].address, payloadRegion));
        EXPECT_EQ(memory.events[i + 1].kind, RecordingIsochMemory::EventKind::Barrier);
        EXPECT_EQ(memory.events[i + 2].kind, RecordingIsochMemory::EventKind::Publish);
        EXPECT_TRUE(AddressInRegion(memory.events[i + 2].address, descriptorRegion));
    }

    memory.events.clear();
    constexpr uint32_t kHwPacketIndex = 8;
    const uint32_t commandPointer =
        ring.Slab().GetDescriptorIOVA(kHwPacketIndex * Layout::kBlocksPerPacket) |
        Layout::kBlocksPerPacket;
    hw.SetTestRegister(
        static_cast<::ASFW::Driver::Register32>(
            ::DMAContextHelpers::IsoXmitCommandPtr(0)),
        commandPointer);

    const auto refill = ring.Refill(hw, 0, provider, nullptr, nullptr);
    ASSERT_TRUE(refill.ok);
    ASSERT_EQ(refill.refillPacketCount, 4U);
    ASSERT_EQ(memory.events.size(), refill.refillPacketCount * 3U);
    for (size_t i = 0; i < memory.events.size(); i += 3) {
        EXPECT_EQ(memory.events[i].kind, RecordingIsochMemory::EventKind::Publish);
        EXPECT_TRUE(AddressInRegion(memory.events[i].address, payloadRegion));
        EXPECT_EQ(memory.events[i + 1].kind, RecordingIsochMemory::EventKind::Barrier);
        EXPECT_EQ(memory.events[i + 2].kind, RecordingIsochMemory::EventKind::Publish);
        EXPECT_TRUE(AddressInRegion(memory.events[i + 2].address, descriptorRegion));
    }
}

TEST(IsochTxDmaRingTests,
     PreparationWritesOnlyBeyondGuardAndPublishesPayloadThenBarrierWithoutDescriptor) {
    ::ASFW::Driver::HardwareInterface hw;
    auto concrete = MakeTestIsochMemory(hw, Layout::kNumPackets, Layout::kMaxPacketSize);
    ASSERT_TRUE(concrete);
    RecordingIsochMemory memory(concrete);

    IsochTxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(memory), kIOReturnSuccess);
    ring.ResetForStart();
    TimedSilentPacketProvider provider;
    ASSERT_EQ(ring.Prime(provider).packetsAssembled, Layout::kNumPackets);

    const uint32_t commandPointer =
        ring.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket;
    hw.SetTestRegister(
        static_cast<::ASFW::Driver::Register32>(
            ::DMAContextHelpers::IsoXmitCommandPtr(0)),
        commandPointer);

    memory.events.clear();
    std::array<uint32_t, Layout::kPreparationDeadlinePackets + 1>
        prefetchedSnapshot{};
    for (uint32_t depth = 0;
         depth <= Layout::kPreparationDeadlinePackets;
         ++depth) {
        const auto* words =
            reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(depth));
        prefetchedSnapshot[depth] = words[2];
    }
    MarkerPayloadPreparer preparer;
    const auto outcome = ring.PreparePayloads(hw, 0, preparer);
    ASSERT_TRUE(outcome.ok);
    EXPECT_EQ(outcome.preparedCount,
              Layout::kNumPackets - Layout::kPreparationDeadlinePackets - 1);
    EXPECT_EQ(outcome.startupSilenceCount,
              Layout::kPreparationDeadlinePackets + 1);

    for (uint32_t packet = 0;
         packet <= Layout::kPreparationDeadlinePackets;
         ++packet) {
        const auto* words =
            reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(packet));
        EXPECT_EQ(words[2], 0U);
        EXPECT_EQ(words[2], prefetchedSnapshot[packet]);
        EXPECT_EQ(ring.SlotMetadata(packet).state,
                  PreparedTxSlotState::InitialSilence);
    }
    for (uint32_t packet = Layout::kPreparationDeadlinePackets + 1;
         packet < Layout::kNumPackets;
         ++packet) {
        const auto* words =
            reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(packet));
        EXPECT_EQ(words[2], 0xA5000000u | packet);
        EXPECT_EQ(ring.SlotMetadata(packet).state,
                  PreparedTxSlotState::PcmPrepared);
    }

    const auto descriptorRegion = ring.Slab().DescriptorRegion();
    const auto payloadRegion = ring.Slab().PayloadRegion();
    ASSERT_EQ(memory.events.size(), outcome.preparedCount * 2U);
    for (size_t i = 0; i < memory.events.size(); i += 2) {
        EXPECT_EQ(memory.events[i].kind, RecordingIsochMemory::EventKind::Publish);
        EXPECT_TRUE(AddressInRegion(memory.events[i].address, payloadRegion));
        EXPECT_FALSE(AddressInRegion(memory.events[i].address, descriptorRegion));
        EXPECT_EQ(memory.events[i + 1].kind,
                  RecordingIsochMemory::EventKind::Barrier);
    }
}

TEST(IsochTxDmaRingTests,
     CompletionHashDetectsMutationBeforeSlotRecycle) {
    ::ASFW::Driver::HardwareInterface hw;
    auto memory =
        MakeTestIsochMemory(hw, Layout::kNumPackets, Layout::kMaxPacketSize);
    ASSERT_TRUE(memory);
    IsochTxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*memory), kIOReturnSuccess);
    ring.ResetForStart();
    hw.SetTestRegister(::ASFW::Driver::Register32::kCycleTimer, 1000u << 12);
    ring.SeedCycleTracking(hw);
    TimedSilentPacketProvider provider;
    ASSERT_EQ(ring.Prime(provider).packetsAssembled, Layout::kNumPackets);

    hw.SetTestRegister(
        static_cast<::ASFW::Driver::Register32>(
            ::DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket);
    MarkerPayloadPreparer preparer;
    ASSERT_TRUE(ring.PreparePayloads(hw, 0, preparer).ok);

    constexpr uint32_t kMutatedPacket =
        Layout::kPreparationDeadlinePackets + 1;
    auto* words =
        reinterpret_cast<uint32_t*>(ring.Slab().PayloadPtr(kMutatedPacket));
    words[2] ^= 1U;

    constexpr uint32_t kHardwarePacket = kMutatedPacket + 1;
    hw.SetTestRegister(
        static_cast<::ASFW::Driver::Register32>(
            ::DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring.Slab().GetDescriptorIOVA(
            kHardwarePacket * Layout::kBlocksPerPacket) |
            Layout::kBlocksPerPacket);
    RecordingCompletionObserver observer;
    const auto refill =
        ring.Refill(hw, 0, provider, nullptr, nullptr, &observer);

    EXPECT_FALSE(refill.ok);
    ASSERT_FALSE(observer.completedSlots.empty());
    const auto mismatch = std::find_if(
        observer.completedSlots.begin(),
        observer.completedSlots.end(),
        [](const CompletedTxSlot& completed) {
            return !completed.payloadHashMatches;
        });
    ASSERT_NE(mismatch, observer.completedSlots.end());
    EXPECT_EQ(mismatch->packetIndex, kMutatedPacket);
    EXPECT_NE(mismatch->metadata.preparedPayloadHash,
              mismatch->completedPayloadHash);
    EXPECT_EQ(ring.RTCounters().completedPayloadHashMismatches.load(
                  std::memory_order_relaxed),
              1U);
}

TEST(IsochTxDmaRingTests, DeadlineFatalDoesNotModifyOrPublishPayload) {
    ::ASFW::Driver::HardwareInterface hw;
    auto concrete = MakeTestIsochMemory(hw, Layout::kNumPackets, Layout::kMaxPacketSize);
    ASSERT_TRUE(concrete);
    RecordingIsochMemory memory(concrete);

    IsochTxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(memory), kIOReturnSuccess);
    ring.ResetForStart();
    TimedSilentPacketProvider provider;
    ASSERT_EQ(ring.Prime(provider).packetsAssembled, Layout::kNumPackets);
    hw.SetTestRegister(
        static_cast<::ASFW::Driver::Register32>(
            ::DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket);

    memory.events.clear();
    FatalDeadlinePreparer preparer;
    const auto outcome = ring.PreparePayloads(hw, 0, preparer);
    EXPECT_TRUE(outcome.fatal);
    EXPECT_EQ(outcome.faultPacketIndex, 0U);
    EXPECT_EQ(outcome.faultDistance, 0U);
    EXPECT_TRUE(memory.events.empty());
    const auto* words =
        reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(0));
    EXPECT_EQ(words[2], 0U);
}

TEST(IsochTxDmaRingTests, PreparationGuardOwnershipWrapsAtRingEnd) {
    ::ASFW::Driver::HardwareInterface hw;
    auto memory =
        MakeTestIsochMemory(hw, Layout::kNumPackets, Layout::kMaxPacketSize);
    ASSERT_TRUE(memory);
    IsochTxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*memory), kIOReturnSuccess);
    ring.ResetForStart();
    TimedSilentPacketProvider provider;
    ASSERT_EQ(ring.Prime(provider).packetsAssembled, Layout::kNumPackets);

    constexpr uint32_t kHwPacket = Layout::kNumPackets - 2;
    hw.SetTestRegister(
        static_cast<::ASFW::Driver::Register32>(
            ::DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring.Slab().GetDescriptorIOVA(kHwPacket * Layout::kBlocksPerPacket) |
            Layout::kBlocksPerPacket);
    MarkerPayloadPreparer preparer;
    const auto outcome = ring.PreparePayloads(hw, 0, preparer);
    ASSERT_TRUE(outcome.ok);

    for (uint32_t distance = 0;
         distance <= Layout::kPreparationDeadlinePackets;
         ++distance) {
        const uint32_t packet = (kHwPacket + distance) % Layout::kNumPackets;
        const auto* words =
            reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(packet));
        EXPECT_EQ(words[2], 0U);
        EXPECT_EQ(ring.SlotMetadata(packet).state,
                  PreparedTxSlotState::InitialSilence);
    }
    const uint32_t firstWritablePacket =
        (kHwPacket + Layout::kPreparationDeadlinePackets + 1) %
        Layout::kNumPackets;
    const auto* firstWritable = reinterpret_cast<const uint32_t*>(
        ring.Slab().PayloadPtr(firstWritablePacket));
    EXPECT_EQ(firstWritable[2], 0xA5000000U | firstWritablePacket);
    EXPECT_EQ(ring.SlotMetadata(firstWritablePacket).state,
              PreparedTxSlotState::PcmPrepared);
}
