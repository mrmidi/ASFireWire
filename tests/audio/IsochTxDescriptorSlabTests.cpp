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
        const uint32_t pageOffset = iova & (Layout::kDescriptorPageStride - 1);
        EXPECT_LT(pageOffset, (Layout::kDescriptorPageStride - Layout::kOHCIPrefetchSize))
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
        Layout::kRingBlocks / 2u,
        Layout::kRingBlocks - 2u,
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
    static_assert(usableBytes < Layout::kDescriptorPageStride, "usableBytes must be within page");

    // Pick an address in the padding region of page 0 (still 16B aligned).
    const uint32_t cmdAddr = kBase + usableBytes + 0x10u;
    uint32_t decoded = 0;
    EXPECT_FALSE(slab.DecodeCmdAddrToLogicalIndex(cmdAddr, decoded));
}

// ---------------------------------------------------------------------------
// Multi-page coverage.
//
// The page-gap arithmetic in GetDescriptorIOVA / DecodeCmdAddrToLogicalIndex was
// written page-aware from the start, but while the TX ring was 48 packets
// kRingBlocks (192) fitted inside one 252-descriptor page, so kTotalPages was 1
// and every test index above landed on page 0 -- the crossing arithmetic had
// never executed. The ring is now 504 packets (8 pages). These tests exist so a
// future ring change cannot quietly take that coverage away again.
// ---------------------------------------------------------------------------

TEST(IsochTxDescriptorSlab, LayoutActuallySpansMultiplePages) {
    static_assert(Layout::kTotalPages > 1,
                  "descriptor page-crossing arithmetic is only exercised when "
                  "the ring spans more than one page; if the TX ring shrinks "
                  "back under 63 packets these tests stop testing anything");
    EXPECT_GT(Layout::kRingBlocks, Layout::kDescriptorsPerPage);
}

TEST(IsochTxDescriptorSlab, DecodeCmdAddrRoundTripsEveryIndex) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x40000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    for (uint32_t idx = 0; idx < Layout::kRingBlocks; ++idx) {
        const uint32_t addr = slab.GetDescriptorIOVA(idx);
        uint32_t decoded = 0;
        ASSERT_TRUE(slab.DecodeCmdAddrToLogicalIndex(addr, decoded))
            << "idx=" << idx << " addr=0x" << std::hex << addr;
        EXPECT_EQ(decoded, idx) << "idx=" << idx;
    }
}

TEST(IsochTxDescriptorSlab, IOVAsAreMonotonicAndJumpOnlyAtPageBoundaries) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x50000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    for (uint32_t idx = 1; idx < Layout::kRingBlocks; ++idx) {
        const uint32_t prev = slab.GetDescriptorIOVA(idx - 1);
        const uint32_t here = slab.GetDescriptorIOVA(idx);
        ASSERT_GT(here, prev) << "idx=" << idx;
        const bool crossesPage = (idx % Layout::kDescriptorsPerPage) == 0;
        // Within a page descriptors are adjacent; at a boundary the gap is the
        // padding the prefetch guard requires, and nothing else.
        EXPECT_EQ(here - prev,
                  crossesPage
                      ? Layout::kDescriptorPageStride -
                            (Layout::kDescriptorsPerPage - 1) *
                                Layout::kDescriptorStride
                      : Layout::kDescriptorStride)
            << "idx=" << idx << " crossesPage=" << crossesPage;
    }
}

TEST(IsochTxDescriptorSlab, PacketProgramsNeverStraddleAPage) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x60000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    // A Z=4 command pointer addresses one block and the controller walks the
    // next three; splitting them across a page would break that walk.
    for (uint32_t pkt = 0; pkt < Layout::kNumPackets; ++pkt) {
        const uint32_t first = pkt * Layout::kBlocksPerPacket;
        const uint32_t firstPage =
            slab.GetDescriptorIOVA(first) / Layout::kDescriptorPageStride;
        const uint32_t lastPage =
            slab.GetDescriptorIOVA(first + Layout::kBlocksPerPacket - 1) /
            Layout::kDescriptorPageStride;
        EXPECT_EQ(firstPage, lastPage) << "packet=" << pkt;
    }
}

TEST(IsochTxDescriptorSlab, DecodeCmdAddrRejectsPaddingZoneOnEveryPage) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x70000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    constexpr uint32_t usableBytes =
        Layout::kDescriptorsPerPage * Layout::kDescriptorStride;
    for (uint32_t page = 0; page < Layout::kTotalPages; ++page) {
        const uint32_t cmdAddr =
            kBase + page * Layout::kDescriptorPageStride + usableBytes;
        uint32_t decoded = 0;
        EXPECT_FALSE(slab.DecodeCmdAddrToLogicalIndex(cmdAddr, decoded))
            << "page=" << page;
    }
}

TEST(IsochTxDescriptorSlab, DecodeCmdAddrRejectsAddressesPastTheRing) {
    IsochTxDescriptorSlab slab;
    constexpr uint32_t kBase = 0x80000000u;
    slab.AttachDescriptorBaseForTest(kBase);

    uint32_t decoded = 0;
    EXPECT_FALSE(slab.DecodeCmdAddrToLogicalIndex(
        kBase + Layout::kTotalPages * Layout::kDescriptorPageStride, decoded));
    EXPECT_FALSE(slab.DecodeCmdAddrToLogicalIndex(kBase - 16u, decoded));
}

// Every real page boundary must land on one of our stride boundaries, so the
// padding covers it. This holds because the slab base is forced 4 KiB aligned;
// it is what makes a 4 KiB stride correct on a 16 KiB-page host.
TEST(IsochTxDescriptorSlab, StrideBoundariesCoverLargerHostPages) {
    for (size_t hostPage : {size_t{4096}, size_t{16384}}) {
        ASSERT_EQ(hostPage % Layout::kDescriptorPageStride, 0u)
            << "stride must divide the host page, or padding misses boundaries";
    }
    // A base that is 4 KiB but not 16 KiB aligned is still fine: every 16 KiB
    // boundary above it is an integer number of strides away.
    constexpr uint32_t kAwkwardBase = 0x90001000u;
    ASSERT_EQ(kAwkwardBase % Layout::kDescriptorPageStride, 0u);
    IsochTxDescriptorSlab slab;
    slab.AttachDescriptorBaseForTest(kAwkwardBase);
    for (uint32_t idx = 0; idx < Layout::kRingBlocks; ++idx) {
        const uint32_t iova = slab.GetDescriptorIOVA(idx);
        const uint32_t offset16k = iova & (16384u - 1u);
        EXPECT_LE(offset16k + Layout::kOHCIPrefetchSize, 16384u)
            << "idx=" << idx << " would prefetch across a 16 KiB page";
    }
}
