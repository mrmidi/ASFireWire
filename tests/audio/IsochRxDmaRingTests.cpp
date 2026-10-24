#include <gtest/gtest.h>
#include <functional>

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

// Keep the production allocator/IOVA layout; intercept only visibility events.
// This models ordering obligations, not ARM caches or a physical DMA engine.
class ObservedReceiveMemory final : public IIsochDMAMemory {
public:
    explicit ObservedReceiveMemory(IIsochDMAMemory& backing) : backing_(backing) {}
    std::function<void()> onOwnership;
    std::function<void(const std::byte*, size_t)> onFetch;
    std::function<void(const std::byte*, size_t)> onPublish;

    std::optional<ASFW::Shared::DMARegion> AllocateDescriptor(size_t size) override {
        return backing_.AllocateDescriptor(size);
    }
    std::optional<ASFW::Shared::DMARegion> AllocatePayloadBuffer(size_t size) override {
        return backing_.AllocatePayloadBuffer(size);
    }
    std::optional<ASFW::Shared::DMARegion> AllocateRegion(size_t size, size_t alignment) override {
        return backing_.AllocateRegion(size, alignment);
    }
    uint64_t VirtToIOVA(const std::byte* p) const noexcept override {
        return backing_.VirtToIOVA(p);
    }
    std::byte* IOVAToVirt(uint64_t address) const noexcept override {
        return backing_.IOVAToVirt(address);
    }
    void FetchFromDevice(const std::byte* p, size_t size) const noexcept override {
        if (onFetch) onFetch(p, size);
        backing_.FetchFromDevice(p, size);
    }
    void PublishToDevice(const std::byte* p, size_t size) const noexcept override {
        if (onPublish) onPublish(p, size);
        backing_.PublishToDevice(p, size);
    }
    void ReceiveOwnershipBarrier() const noexcept override {
        if (onOwnership) onOwnership();
        backing_.ReceiveOwnershipBarrier();
    }
    size_t TotalSize() const noexcept override { return backing_.TotalSize(); }
    size_t AvailableSize() const noexcept override { return backing_.AvailableSize(); }
private:
    IIsochDMAMemory& backing_;
};

} // namespace

TEST(IsochRxDmaRingTests, OrdersCompletedPayloadReadsAndRearmAcrossWraps) {
    ::ASFW::Driver::HardwareInterface hw;
    auto backing = MakeTestIsochMemory(hw, 8, 64);
    ASSERT_TRUE(backing);
    ObservedReceiveMemory mem(*backing);
    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(mem, 8, 64), kIOReturnSuccess);

    // Follow the descriptor program built by SetupRings, including its wrap.
    // Fixed-size INPUT_LAST slots have no allocator padding to skip.
    uint32_t command = ring.InitialCommandPtrWord();
    for (uint32_t packet = 0; packet < 40; ++packet) {
        const uint32_t index = packet % 8;
        auto* desc = backing->IOVAToPtr<ASFW::Async::HW::OHCIDescriptor>(command & ~0xfu);
        ASSERT_EQ(desc, ring.DescriptorAt(index));
        ASSERT_EQ(command & 0xfu, 1u);
        auto* payload = ring.PayloadVA(index);
        // Delay modeled payload visibility until the ownership acquire. The
        // production FetchFromDevice currently supplies only a store barrier.
        payload[0] = 0xee;
        desc->statusWord = (0x8451u << 16) | 48u;
        uint32_t barriers = 0;
        bool consumed = false;
        bool published = false;
        mem.onOwnership = [&] {
            EXPECT_EQ(desc->statusWord, (0x8451u << 16) | 48u);
            if (barriers == 0) {
                EXPECT_FALSE(consumed);
                payload[0] = static_cast<uint8_t>(packet);
            } else {
                EXPECT_TRUE(consumed);
            }
            ++barriers;
        };
        mem.onFetch = [&](const std::byte* p, size_t) {
            if (p == reinterpret_cast<const std::byte*>(payload)) EXPECT_EQ(barriers, 1u);
        };
        mem.onPublish = [&](const std::byte* p, size_t) {
            ASSERT_EQ(p, reinterpret_cast<const std::byte*>(desc));
            EXPECT_TRUE(consumed);
            EXPECT_EQ(barriers, 2u);
            EXPECT_EQ(desc->statusWord, 64u);
            published = true;
        };
        EXPECT_EQ(ring.DrainCompleted(mem, [&](const auto& completion) {
            EXPECT_EQ(completion.descriptorIndex, index);
            EXPECT_EQ(completion.payload[0], static_cast<uint8_t>(packet));
            EXPECT_EQ(desc->statusWord, (0x8451u << 16) | 48u);
            EXPECT_FALSE(published);
            consumed = true;
        }), 1u);
        EXPECT_EQ(barriers, 2u);
        EXPECT_TRUE(published);
        command = desc->branchWord;
        mem.onOwnership = {};
        mem.onFetch = {};
        mem.onPublish = {};
        EXPECT_EQ(ring.DrainCompleted(mem, [](const auto&) { ADD_FAILURE(); }), 0u);
    }
}

TEST(IsochRxDmaRingTests, DoesNotAcquireOrReuseWhileOnlyResidualHasChanged) {
    ::ASFW::Driver::HardwareInterface hw;
    auto backing = MakeTestIsochMemory(hw, 8, 64);
    ObservedReceiveMemory mem(*backing);
    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(mem, 8, 64), kIOReturnSuccess);
    ring.DescriptorAt(0)->statusWord = 48;
    mem.onOwnership = [] { ADD_FAILURE() << "incomplete packet has no ownership transfer"; };
    mem.onPublish = [](const auto*, auto) { ADD_FAILURE() << "must not rearm in-flight DMA"; };
    mem.onFetch = [&](const std::byte* p, size_t size) {
        EXPECT_EQ(p, reinterpret_cast<const std::byte*>(ring.DescriptorAt(0)));
        EXPECT_EQ(size, sizeof(ASFW::Async::HW::OHCIDescriptor));
    };
    EXPECT_EQ(ring.DrainCompleted(mem, [](const auto&) { ADD_FAILURE(); }), 0u);
    EXPECT_EQ(ring.DescriptorAt(0)->statusWord, 48u);
}

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

    auto* payload = ring.PayloadVA(0);
    ASSERT_NE(payload, nullptr);

    payload[0] = 0x11;
    payload[1] = 0x22;
    payload[2] = 0x33;
    payload[3] = 0x44;

    constexpr uint16_t reqCount = 64;
    constexpr uint16_t actualLength = 16;
    d0->statusWord = (0x8451u << 16) | static_cast<uint16_t>(reqCount - actualLength);

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

TEST(IsochRxDmaRingTests, ResidualProgressDoesNotRetireAnIncompletePacket) {
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    auto* first = ring.DescriptorAt(0);
    first->statusWord = 48;
    ring.DescriptorAt(1)->statusWord = (0x8451u << 16) | 48u;
    uint32_t calls = 0;
    EXPECT_EQ(ring.DrainCompleted(*mem, [&](const auto&) { ++calls; }), 0u);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(first->statusWord, 48u);
    EXPECT_EQ(ring.DrainCompleted(*mem, [&](const auto&) { ++calls; }), 0u);
    EXPECT_EQ(first->statusWord, 48u);
    first->statusWord = (0x8451u << 16) | 48u;
    EXPECT_EQ(ring.DrainCompleted(*mem, [&](const auto& packet) {
        EXPECT_EQ(packet.descriptorIndex, calls++);
        EXPECT_EQ(packet.actualLength, 16u);
        EXPECT_EQ(packet.xferStatus, 0x8451u);
    }), 2u);
    EXPECT_EQ(calls, 2u);
}

TEST(IsochRxDmaRingTests, FullRingWrapEmptyTailAndHotAppendPreserveOrder) {
    ::ASFW::Driver::HardwareInterface hw;
    auto mem = MakeTestIsochMemory(hw, 8, 64);
    IsochRxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*mem, 8, 64), kIOReturnSuccess);
    // This production receive allocator has fixed-size slots and no wrap
    // padding. Exercise its branch from the tail back to descriptor zero.
    uint32_t expected = 0;
    auto consume = [&](const auto& packet) {
        EXPECT_EQ(packet.descriptorIndex, expected++ % 8);
        EXPECT_EQ(packet.actualLength, 16u);
    };
    for (unsigned lap = 0; lap < 4; ++lap) {
        for (unsigned i = 0; i < 8; ++i)
            ring.DescriptorAt(i)->statusWord = (0x8451u << 16) | 48u;
        EXPECT_EQ(ring.DrainCompleted(*mem, consume), 8u);
        EXPECT_EQ(ring.DrainCompleted(*mem, consume), 0u);
    }
    for (unsigned i = 0; i < 7; ++i)
        ring.DescriptorAt(i)->statusWord = (0x8451u << 16) | 48u;
    EXPECT_EQ(ring.DrainCompleted(*mem, consume), 7u);
    EXPECT_EQ(ring.DrainCompleted(*mem, consume), 0u); // empty at tail
    ring.DescriptorAt(7)->statusWord = (0x8451u << 16) | 48u;
    EXPECT_EQ(ring.DrainCompleted(*mem, [&](const auto& packet) {
        consume(packet);
        if (packet.descriptorIndex == 7)
            ring.DescriptorAt(0)->statusWord = (0x8451u << 16) | 48u;
    }), 2u); // completion arrives across the wrap while draining
    EXPECT_EQ(expected, 41u);
}
