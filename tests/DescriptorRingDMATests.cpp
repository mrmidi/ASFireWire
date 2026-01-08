#include <gtest/gtest.h>

#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"
#include "ASFWDriver/Shared/Rings/DescriptorRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {

class DescriptorRingDMATest : public ::testing::Test {
protected:
    FakeDMAMemory dma_{512 * 1024};
    Shared::DescriptorRing ring_{};
    uint64_t descBaseIOVA_{0};

    void SetUp() override {
        constexpr size_t kNumDescriptors = 64;
        auto region = dma_.AllocateRegion(kNumDescriptors * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(region.has_value());
        descBaseIOVA_ = region->deviceBase;

        auto* descriptors = reinterpret_cast<Async::HW::OHCIDescriptor*>(region->virtualBase);
        std::span<Async::HW::OHCIDescriptor> descSpan{descriptors, kNumDescriptors};

        ASSERT_TRUE(ring_.Initialize(descSpan));
        ASSERT_TRUE(ring_.Finalize(region->deviceBase));
    }
};

TEST_F(DescriptorRingDMATest, CommandPtrWordEncodesZAndAddress) {
    auto* desc0 = ring_.At(0);
    ASSERT_NE(desc0, nullptr);

    constexpr uint8_t zBlocks = 2;
    const uint32_t cmdPtr = ring_.CommandPtrWordTo(desc0, zBlocks);

    EXPECT_NE(cmdPtr, 0u);
    EXPECT_EQ(cmdPtr & 0xF, zBlocks);
    const uint32_t expectedAddr = static_cast<uint32_t>(descBaseIOVA_ & 0xFFFFFFF0u);
    EXPECT_EQ(cmdPtr & 0xFFFFFFF0u, expectedAddr);
}

TEST_F(DescriptorRingDMATest, RingFullDetectionWraps) {
    const size_t cap = ring_.Capacity();
    for (size_t i = 0; i < cap - 1; ++i) {
        EXPECT_FALSE(ring_.IsFull());
        ring_.SetTail((ring_.Tail() + 1) % cap);
    }
    EXPECT_TRUE(ring_.IsFull());
}

} // namespace ASFW::Testing
