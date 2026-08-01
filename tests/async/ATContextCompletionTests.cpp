#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "ASFWDriver/Async/Contexts/ATRequestContext.hpp"
#include "ASFWDriver/Hardware/OHCIConstants.hpp"
#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"
#include "ASFWDriver/Shared/Memory/DMAMemoryManager.hpp"
#include "ASFWDriver/Shared/Rings/DescriptorRing.hpp"

namespace ASFW::Testing {
namespace {

constexpr size_t kDescriptorCount = 8;
constexpr uint8_t kTLabel = 45;
constexpr uint16_t kTimestamp = 0x1234;

class ATContextCompletionTest : public ::testing::Test {
protected:
    Driver::HardwareInterface hardware_;
    Shared::DMAMemoryManager dma_;
    Shared::DescriptorRing ring_;
    Async::ATRequestContext context_;

    void SetUp() override {
        ASSERT_TRUE(dma_.Initialize(
            hardware_, kDescriptorCount * sizeof(Async::HW::OHCIDescriptor)));

        auto region = dma_.AllocateRegion(
            kDescriptorCount * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(region.has_value());

        auto* descriptors =
            reinterpret_cast<Async::HW::OHCIDescriptor*>(region->virtualBase);
        ASSERT_TRUE(ring_.Initialize(
            std::span<Async::HW::OHCIDescriptor>{descriptors, kDescriptorCount}));
        ASSERT_TRUE(ring_.Finalize(region->deviceBase));
        ASSERT_EQ(context_.Initialize(hardware_, ring_, dma_), kIOReturnSuccess);
    }

    void PrepareBlockWriteChain(
        uint16_t payloadStatus =
            static_cast<uint16_t>(Async::OHCIEventCode::kAckComplete)) {
        auto* header = reinterpret_cast<Async::HW::OHCIDescriptorImmediate*>(ring_.At(0));
        ASSERT_NE(header, nullptr);
        header->common.control = Async::HW::OHCIDescriptor::BuildControl({
            .reqCount = 16,
            .command = Async::HW::OHCIDescriptor::kCmdOutputMore,
            .key = Async::HW::OHCIDescriptor::kKeyImmediate,
            .interruptBits = Async::HW::OHCIDescriptor::kIntNever,
            .branchBits = Async::HW::OHCIDescriptor::kBranchAlways,
        });
        header->immediateData[0] = static_cast<uint32_t>(kTLabel) << 10;

        auto* payload = ring_.At(2);
        ASSERT_NE(payload, nullptr);
        payload->control = Async::HW::OHCIDescriptor::BuildControl({
            .reqCount = 8,
            .command = Async::HW::OHCIDescriptor::kCmdOutputLast,
            .key = Async::HW::OHCIDescriptor::kKeyStandard,
            .interruptBits = Async::HW::OHCIDescriptor::kIntAlways,
            .branchBits = Async::HW::OHCIDescriptor::kBranchNever,
        });
        payload->timeStamp = kTimestamp;
        payload->xferStatus = payloadStatus;

        ring_.SetTail(3);
        hardware_.SetTestRegister(
            Async::ATRequestTag::kControlSetReg,
            Driver::kContextControlRunBit);
        hardware_.SetTestRegister(
            Async::ATRequestTag::kCommandPtrReg,
            ring_.CommandPtrWordTo(ring_.At(0), 3));
    }
};

TEST_F(ATContextCompletionTest,
       UsesOutputLastStatusWhenOutputMorePrecursorHasNoStatus) {
    PrepareBlockWriteChain();

    const auto completion = context_.ScanCompletion();

    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->eventCode, Async::OHCIEventCode::kAckComplete);
    EXPECT_EQ(completion->timeStamp, kTimestamp);
    EXPECT_EQ(completion->tLabel, kTLabel);
    EXPECT_EQ(completion->descriptor, ring_.At(2));
    EXPECT_EQ(ring_.Head(), 3u);
    EXPECT_TRUE(ring_.IsEmpty());
}

TEST_F(ATContextCompletionTest,
       WaitsWhenOutputMoreAndOutputLastBothHaveNoStatus) {
    PrepareBlockWriteChain(0);

    const auto completion = context_.ScanCompletion();

    EXPECT_FALSE(completion.has_value());
    EXPECT_EQ(ring_.Head(), 0u);
    EXPECT_FALSE(ring_.IsEmpty());
}

TEST_F(ATContextCompletionTest,
       PreservesRecognizedChainWhenCommandPtrAdvancesToPendingOutputLast) {
    PrepareBlockWriteChain(0);
    hardware_.SetTestRegister(
        Async::ATRequestTag::kCommandPtrReg,
        ring_.CommandPtrWordTo(ring_.At(2), 1));

    const auto pendingCompletion = context_.ScanCompletion();

    EXPECT_FALSE(pendingCompletion.has_value());
    EXPECT_EQ(ring_.Head(), 0u);
    EXPECT_FALSE(ring_.IsEmpty());

    auto* payload = ring_.At(2);
    ASSERT_NE(payload, nullptr);
    payload->xferStatus =
        static_cast<uint16_t>(Async::OHCIEventCode::kAckComplete);

    const auto completion = context_.ScanCompletion();

    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->eventCode, Async::OHCIEventCode::kAckComplete);
    EXPECT_EQ(completion->timeStamp, kTimestamp);
    EXPECT_EQ(completion->tLabel, kTLabel);
    EXPECT_EQ(completion->descriptor, payload);
    EXPECT_EQ(ring_.Head(), 3u);
    EXPECT_TRUE(ring_.IsEmpty());
}

TEST_F(ATContextCompletionTest, AdvancesTrulyOrphanedPendingDescriptor) {
    auto* descriptor = ring_.At(0);
    ASSERT_NE(descriptor, nullptr);
    descriptor->control = Async::HW::OHCIDescriptor::BuildControl({
        .reqCount = 8,
        .command = Async::HW::OHCIDescriptor::kCmdOutputLast,
        .key = Async::HW::OHCIDescriptor::kKeyStandard,
        .interruptBits = Async::HW::OHCIDescriptor::kIntAlways,
        .branchBits = Async::HW::OHCIDescriptor::kBranchNever,
    });
    descriptor->xferStatus = 0;
    ring_.SetTail(1);
    hardware_.SetTestRegister(Async::ATRequestTag::kControlSetReg, 0);
    hardware_.SetTestRegister(
        Async::ATRequestTag::kCommandPtrReg,
        ring_.CommandPtrWordTo(descriptor, 1));

    const auto completion = context_.ScanCompletion();

    EXPECT_FALSE(completion.has_value());
    EXPECT_EQ(ring_.Head(), 1u);
    EXPECT_TRUE(ring_.IsEmpty());
}

// A quiesced context will never write a status into the OUTPUT_LAST, so the
// recognized-chain path must not hold the head hostage waiting for one. Both
// references drain unconditionally once the context is stopped: Linux gates
// handle_at_packet()'s early return on !ctx->flushing and calls
// at_context_flush() after context_stop() (ohci.c:1364, 2000-2010); AppleFWOHCI
// calls resetDMA() after stopDMA() in handleBusResetInt(). OHCI 1.2 draft
// clause 7.2.3.3 requires software to drain what hardware left unsent.
TEST_F(ATContextCompletionTest, DrainsRecognizedChainWhenContextIsQuiesced) {
    PrepareBlockWriteChain(0);
    hardware_.SetTestRegister(Async::ATRequestTag::kControlSetReg, 0);

    while (!ring_.IsEmpty()) {
        const auto completion = context_.ScanCompletion();
        ASSERT_FALSE(completion.has_value());
        if (ring_.Head() == 0u) {
            FAIL() << "quiesced context wedged the ring head at 0";
        }
    }

    EXPECT_EQ(ring_.Head(), 3u);
    EXPECT_TRUE(ring_.IsEmpty());
}

} // namespace
} // namespace ASFW::Testing
