#include <cstring>
#include <gtest/gtest.h>

#include "ASFWDriver/Async/Contexts/ATRequestContext.hpp"
#include "ASFWDriver/Async/Engine/ATManager.hpp"
#include "ASFWDriver/Async/Tx/DescriptorBuilder.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/OHCIConstants.hpp"
#include "ASFWDriver/Shared/Memory/DMAMemoryManager.hpp"
#include "ASFWDriver/Shared/Rings/DescriptorRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

// Regression for the LS-9000 AT Response wedge (2026-09-24): back-to-back
// packets must hot-append (PATH2) onto the running context. A PATH2 failure
// fell back to PATH1, which re-programmed CommandPtr while the previous
// packet was still in flight and left the FW643 ACTIVE=1 forever.

namespace ASFW::Testing {

using ASFW::Async::ATRequestContext;
using ASFW::Async::ATRequestTag;
using ASFW::Async::DescriptorBuilder;
using ASFW::Async::Engine::ATManager;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::kContextControlActiveBit;
using ASFW::Shared::DescriptorRing;
using ASFW::Shared::DMAMemoryManager;
namespace HW = ASFW::Async::HW;

class ATManagerHotAppendTest : public ::testing::Test {
protected:
    FakeDMAMemory dma_{64 * 1024};
    DescriptorRing ring_{};
    DMAMemoryManager dmaManager_{};  // no slab: PublishRange degrades to a barrier
    HardwareInterface hw_{};
    ATRequestContext ctx_{};
    DescriptorBuilder builder_{ring_, dmaManager_};
    ATManager<ATRequestContext, DescriptorRing, ATRequestTag> manager_{ctx_, ring_, builder_};

    void SetUp() override {
        constexpr size_t kNumDescriptors = 64;
        auto region = dma_.AllocateRegion(kNumDescriptors * sizeof(HW::OHCIDescriptor));
        ASSERT_TRUE(region.has_value());
        auto* descriptors = reinterpret_cast<HW::OHCIDescriptor*>(region->virtualBase);
        ASSERT_TRUE(ring_.Initialize(std::span<HW::OHCIDescriptor>{descriptors, kNumDescriptors}));
        ASSERT_TRUE(ring_.Finalize(region->deviceBase));
        ASSERT_EQ(ctx_.Initialize(hw_, ring_, dmaManager_), kIOReturnSuccess);
    }

    // Z=3 packet at the ring tail, the ORB/page-table response shape captured
    // in the wedge snapshot: OUTPUT_MORE_IMMEDIATE (2 blocks) + OUTPUT_LAST.
    DescriptorBuilder::DescriptorChain PlacePayloadChain() {
        const size_t cap = ring_.Capacity();
        const size_t idx = ring_.Tail();
        auto* first = ring_.At(idx);
        auto* last = ring_.At((idx + 2) % cap);
        std::memset(first, 0, 2 * sizeof(HW::OHCIDescriptor));
        std::memset(last, 0, sizeof(HW::OHCIDescriptor));
        first->control = 0x02000010u;  // key=immediate, reqCount=16
        last->control = 0x103c0020u;   // OUTPUT_LAST, b=always, i=always, reqCount=32
        DescriptorBuilder::DescriptorChain chain{};
        chain.first = first;
        chain.last = last;
        chain.firstIOVA32 = ring_.CommandPtrWordTo(first, 0) & 0xFFFFFFF0u;
        chain.lastIOVA32 = ring_.CommandPtrWordTo(last, 0) & 0xFFFFFFF0u;
        chain.firstBlocks = 2;
        chain.lastBlocks = 1;
        chain.firstRingIndex = idx;
        chain.lastRingIndex = (idx + 2) % cap;
        chain.txid = ++txid_;
        return chain;
    }

    uint32_t CommandPtr() const { return hw_.GetTestRegister(ATRequestTag::kCommandPtrReg); }

    uint32_t txid_{0};
};

TEST_F(ATManagerHotAppendTest, SecondPayloadPacketHotAppendsInsteadOfRearming) {
    auto first = PlacePayloadChain();
    ASSERT_EQ(first.TotalBlocks(), 3u);
    auto* firstLast = first.last;
    ASSERT_EQ(manager_.Submit(std::move(first), {}), kIOReturnSuccess);
    const uint32_t armedCommandPtr = CommandPtr();
    ASSERT_NE(armedCommandPtr, 0u);
    ASSERT_TRUE(ctx_.IsRunning());  // stub keeps the RUN write visible on ControlSet

    HW::OHCIDescriptor* prevLast = nullptr;
    size_t prevLastIndex = 0;
    uint8_t prevBlocks = 0;
    ASSERT_TRUE(ring_.LocatePreviousLast(ring_.Tail(), prevLast, prevLastIndex, prevBlocks))
        << "PrevLastBlocks must describe the whole previous packet (2 or 3 blocks)";
    ASSERT_EQ(prevLast, firstLast);
    ASSERT_EQ(prevLast->branchWord, 0u);

    auto second = PlacePayloadChain();
    const uint32_t secondIOVA = second.firstIOVA32;
    ASSERT_EQ(manager_.Submit(std::move(second), {}), kIOReturnSuccess);

    EXPECT_EQ(firstLast->branchWord, secondIOVA | 3u) << "PATH2 must link the tail";
    EXPECT_EQ(CommandPtr(), armedCommandPtr) << "PATH1 must not re-arm over an in-flight packet";
}

TEST_F(ATManagerHotAppendTest, Path1RefusesToArmWhileContextStaysActive) {
    hw_.SetTestRegister(ATRequestTag::kControlSetReg, kContextControlActiveBit);

    EXPECT_EQ(manager_.Submit(PlacePayloadChain(), {}), kIOReturnBusy);
    EXPECT_EQ(CommandPtr(), 0u) << "CommandPtr must never be written on an ACTIVE context (OHCI §3.1.1)";
}

} // namespace ASFW::Testing
