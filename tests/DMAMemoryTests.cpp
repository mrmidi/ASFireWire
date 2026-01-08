#include <cstdint>

#include <gtest/gtest.h>

#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {

class FakeDMAMemoryTest : public ::testing::Test {
protected:
    FakeDMAMemory dma_{1024 * 1024};
};

TEST_F(FakeDMAMemoryTest, AllocatesAlignedRegion) {
    auto region = dma_.AllocateRegion(256);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->size, 256u);
    EXPECT_NE(region->virtualBase, nullptr);
    EXPECT_EQ(region->deviceBase, FakeDMAMemory::kBaseIOVA);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(region->virtualBase) % 16, 0u);
    EXPECT_EQ(region->deviceBase % 16, 0u);
}

TEST_F(FakeDMAMemoryTest, RoundsSizeUpTo16Bytes) {
    auto region = dma_.AllocateRegion(3);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->size, 16u);
}

TEST_F(FakeDMAMemoryTest, VirtToIOVATranslation) {
    auto region = dma_.AllocateRegion(64);
    ASSERT_TRUE(region.has_value());

    uint64_t iova = dma_.VirtToIOVA(region->virtualBase);
    EXPECT_EQ(iova, region->deviceBase);

    uint8_t* ptr = region->virtualBase + 32;
    EXPECT_EQ(dma_.VirtToIOVA(ptr), region->deviceBase + 32);
}

TEST_F(FakeDMAMemoryTest, IOVAToVirtRoundTrip) {
    auto region = dma_.AllocateRegion(128);
    ASSERT_TRUE(region.has_value());

    void* virt = dma_.IOVAToVirt(region->deviceBase + 64);
    EXPECT_EQ(virt, region->virtualBase + 64);
}

TEST_F(FakeDMAMemoryTest, OutOfSpaceReturnsNullopt) {
    while (dma_.AllocateRegion(64 * 1024).has_value()) {}

    auto region = dma_.AllocateRegion(64);
    EXPECT_FALSE(region.has_value());
}

TEST_F(FakeDMAMemoryTest, InjectDataWritesIntoSlab) {
    auto region = dma_.AllocateRegion(16);
    ASSERT_TRUE(region.has_value());

    const uint32_t statusWord = 0x00100010;
    dma_.InjectAt(0, &statusWord, sizeof(statusWord));

    EXPECT_EQ(*reinterpret_cast<uint32_t*>(region->virtualBase), statusWord);
}

TEST_F(FakeDMAMemoryTest, ResetClearsSlabAndCursor) {
    dma_.AllocateRegion(1024);
    EXPECT_GT(dma_.Cursor(), 0u);

    dma_.Reset();

    EXPECT_EQ(dma_.Cursor(), 0u);
    EXPECT_EQ(dma_.RawData()[0], 0u);
}

} // namespace ASFW::Testing

