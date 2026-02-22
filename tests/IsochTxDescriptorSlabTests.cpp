// IsochTxDescriptorSlabTests.cpp
// ASFW - Host-safe unit tests for IT descriptor slab page-gap addressing

#include <gtest/gtest.h>

#include "../ASFWDriver/Isoch/Transmit/IsochTxDescriptorSlab.hpp"

using ASFW::Isoch::Tx::IsochTxDescriptorSlab;
using ASFW::Isoch::Tx::Layout;

TEST(IsochTxDescriptorSlab, DescriptorIOVANeverInPrefetchZone) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x10000000u; // 4K-aligned
    slab.AttachDescriptorBaseForTest(kBase);

    for (uint32_t i = 0; i < Layout::kRingBlocks; ++i) {
        const uint32_t iova = slab.GetDescriptorIOVA(i);
        const uint32_t pageOffset = iova & (Layout::kOHCIPageSize - 1);
        EXPECT_LT(pageOffset, (Layout::kOHCIPageSize - Layout::kOHCIPrefetchSize))
            << "desc=" << i << " iova=0x" << std::hex << iova << " offset=0x" << pageOffset;
    }
}

TEST(IsochTxDescriptorSlab, DecodeCmdAddrRoundTripsRepresentativeIndices) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x20000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    const uint32_t reps[] = {
        0u,
        1u,
        Layout::kDescriptorsPerPage - 1u,
        Layout::kDescriptorsPerPage,
        Layout::kRingBlocks - 1u
    };

    for (const uint32_t idx : reps) {
        const uint32_t addr = slab.GetDescriptorIOVA(idx);
        uint32_t decoded = 0;
        ASSERT_TRUE(slab.DecodeCmdAddrToLogicalIndex(addr, decoded))
            << "idx=" << idx << " addr=0x" << std::hex << addr;
        EXPECT_EQ(decoded, idx);
    }
}

TEST(IsochTxDescriptorSlab, DecodeCmdAddrRejectsPaddingZoneAddresses) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x30000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    constexpr uint32_t usableBytes = Layout::kDescriptorsPerPage * Layout::kDescriptorStride;
    static_assert(usableBytes < Layout::kOHCIPageSize, "usableBytes must be within page");

    // Pick an address in the padding region of page 0 (still 16B aligned).
    const uint32_t cmdAddr = kBase + usableBytes + 0x10u;
    uint32_t decoded = 0;
    EXPECT_FALSE(slab.DecodeCmdAddrToLogicalIndex(cmdAddr, decoded));
}

