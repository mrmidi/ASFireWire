#include <gtest/gtest.h>

#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"
#include "ASFWDriver/Shared/Rings/BufferRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {

class BufferRingDMATest : public ::testing::Test {
protected:
    FakeDMAMemory dma_{512 * 1024};
    Shared::BufferRing ring_{};
    uint64_t descBaseIOVA_{0};
    uint64_t bufBaseIOVA_{0};

    void SetUp() override {
        constexpr size_t kNum = 32;
        constexpr size_t kBufSize = 256;

        auto descRegion = dma_.AllocateRegion(kNum * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(descRegion.has_value());
        descBaseIOVA_ = descRegion->deviceBase;

        auto bufRegion = dma_.AllocateRegion(kNum * kBufSize);
        ASSERT_TRUE(bufRegion.has_value());
        bufBaseIOVA_ = bufRegion->deviceBase;

        auto* descs = reinterpret_cast<Async::HW::OHCIDescriptor*>(descRegion->virtualBase);
        std::span<Async::HW::OHCIDescriptor> descSpan{descs, kNum};
        std::span<uint8_t> bufSpan{bufRegion->virtualBase, kNum * kBufSize};

        ASSERT_TRUE(ring_.Initialize(descSpan, bufSpan, kNum, kBufSize));
        ring_.BindDma(&dma_);
        ASSERT_TRUE(ring_.Finalize(descBaseIOVA_, bufBaseIOVA_));
    }
};

TEST_F(BufferRingDMATest, FinalizeProgramsDataAddressAndBranchWords) {
    constexpr size_t kNum = 32;
    constexpr size_t kBufSize = 256;

    for (size_t i = 0; i < kNum; ++i) {
        auto* desc = ring_.GetDescriptor(i);
        ASSERT_NE(desc, nullptr);

        const uint32_t expectedData =
            static_cast<uint32_t>((bufBaseIOVA_ + i * kBufSize) & 0xFFFFFFFFu);
        EXPECT_EQ(desc->dataAddress, expectedData);

        const size_t nextIndex = (i + 1) % kNum;
        const uint32_t expectedNextDescAddr =
            static_cast<uint32_t>((descBaseIOVA_ + nextIndex * sizeof(Async::HW::OHCIDescriptor)) & 0xFFFFFFF0u);
        const uint32_t branchAddr = Async::HW::DecodeBranchPhys32_AR(desc->branchWord);
        EXPECT_EQ(branchAddr, expectedNextDescAddr);
        EXPECT_NE(desc->branchWord, 0u);
    }
}

} // namespace ASFW::Testing

