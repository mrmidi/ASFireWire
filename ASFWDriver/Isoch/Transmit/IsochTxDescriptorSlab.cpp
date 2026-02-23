// IsochTxDescriptorSlab.cpp

#include "IsochTxDescriptorSlab.hpp"

namespace ASFW::Isoch::Tx {

kern_return_t IsochTxDescriptorSlab::AllocateAndInitialize(Memory::IIsochDMAMemory& dmaMemory) noexcept {
    if (IsValid()) {
        return kIOReturnSuccess;
    }

    // Allocate descriptor ring - request 4K alignment for page gap calculation
    const auto descR = dmaMemory.AllocateDescriptor(Layout::kDescriptorRingSize);
    if (!descR) return kIOReturnNoMemory;

    const auto bufR = dmaMemory.AllocatePayloadBuffer(Layout::kPayloadBufferSize);
    if (!bufR) return kIOReturnNoMemory;
    
    // Only commit to members once we have both regions.
    descRegion_ = *descR;
    bufRegion_ = *bufR;

    if (descRegion_.deviceBase > 0xFFFFFFFFULL || bufRegion_.deviceBase > 0xFFFFFFFFULL) {
        ASFW_LOG(Isoch, "IT: SetupRings - IOVA out of 32-bit range: desc=0x%llx buf=0x%llx",
                 descRegion_.deviceBase, bufRegion_.deviceBase);
        return kIOReturnNoResources;
    }

    // Check 16-byte alignment (minimum for OHCI descriptors)
    if ((descRegion_.deviceBase & 0xFULL) != 0) {
        ASFW_LOG(Isoch, "IT: SetupRings - descriptor base not 16B aligned: 0x%llx",
                 descRegion_.deviceBase);
        return kIOReturnNoResources;
    }

    // CRITICAL: Check 4K alignment for page gap calculation
    // Our GetDescriptorIOVA() assumes base is 4K-aligned so page offsets line up
    const uint64_t pageOffset = descRegion_.deviceBase & (Layout::kOHCIPageSize - 1);
    if (pageOffset != 0) {
        ASFW_LOG(Isoch, "❌ IT: SetupRings - descriptor base NOT 4K aligned! "
                 "IOVA=0x%llx pageOffset=0x%llx - page gap calculation WILL BE WRONG, failing",
                 descRegion_.deviceBase, pageOffset);
        return kIOReturnNoResources;
    }

    // Zero the entire slab (will be filled with 0xDE in Start()).
    std::memset(descRegion_.virtualBase, 0, Layout::kDescriptorRingSize);

    ASFW_LOG(Isoch, "IT: Rings Ready. DescIOVA=0x%llx (pageOff=0x%llx) BufIOVA=0x%llx",
             descRegion_.deviceBase, pageOffset, bufRegion_.deviceBase);
    ASFW_LOG(Isoch, "IT: Layout: %u packets, %u blocks, %u pages, %zu bytes/page usable",
             Layout::kNumPackets, Layout::kRingBlocks, Layout::kTotalPages,
             static_cast<size_t>(Layout::kDescriptorsPerPage * Layout::kDescriptorStride));

    return kIOReturnSuccess;
}

IsochTxDescriptorSlab::OHCIDescriptor* IsochTxDescriptorSlab::GetDescriptorPtr(uint32_t logicalIndex) noexcept {
    // Calculate which 4K page this descriptor is on
    const uint32_t page = logicalIndex / Layout::kDescriptorsPerPage;
    const uint32_t offsetInPage = (logicalIndex % Layout::kDescriptorsPerPage) * Layout::kDescriptorStride;

    uint8_t* base = reinterpret_cast<uint8_t*>(descRegion_.virtualBase);
    return reinterpret_cast<OHCIDescriptor*>(base + (page * Layout::kOHCIPageSize) + offsetInPage);
}

const IsochTxDescriptorSlab::OHCIDescriptor* IsochTxDescriptorSlab::GetDescriptorPtr(uint32_t logicalIndex) const noexcept {
    const uint32_t page = logicalIndex / Layout::kDescriptorsPerPage;
    const uint32_t offsetInPage = (logicalIndex % Layout::kDescriptorsPerPage) * Layout::kDescriptorStride;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(descRegion_.virtualBase);
    return reinterpret_cast<const OHCIDescriptor*>(base + (page * Layout::kOHCIPageSize) + offsetInPage);
}

uint32_t IsochTxDescriptorSlab::GetDescriptorIOVA(uint32_t logicalIndex) const noexcept {
    const uint32_t page = logicalIndex / Layout::kDescriptorsPerPage;
    const uint32_t offsetInPage = (logicalIndex % Layout::kDescriptorsPerPage) * Layout::kDescriptorStride;

    uint32_t baseAddr = static_cast<uint32_t>(descRegion_.deviceBase);
#ifdef ASFW_HOST_TEST
    if (baseAddr == 0 && testDescBaseIOVA32_ != 0) {
        baseAddr = testDescBaseIOVA32_;
    }
#endif

    return baseAddr +
           (page * static_cast<uint32_t>(Layout::kOHCIPageSize)) + offsetInPage;
}

bool IsochTxDescriptorSlab::DecodeCmdAddrToLogicalIndex(uint32_t cmdAddr, uint32_t& outLogicalIndex) const noexcept {
    uint32_t baseAddr = static_cast<uint32_t>(descRegion_.deviceBase);
#ifdef ASFW_HOST_TEST
    if (baseAddr == 0 && testDescBaseIOVA32_ != 0) {
        baseAddr = testDescBaseIOVA32_;
    }
#endif

    // Sanity checks
    if (cmdAddr < baseAddr) return false;
    if ((cmdAddr & 0xFu) != 0) return false;  // Must be 16-byte aligned

    const uint32_t offset = cmdAddr - baseAddr;
    const uint32_t page = offset / static_cast<uint32_t>(Layout::kOHCIPageSize);
    const uint32_t offsetInPage = offset % static_cast<uint32_t>(Layout::kOHCIPageSize);

    if (page >= Layout::kTotalPages) return false;

    // Check if in padding zone (last 64 bytes of page are unused with 252 descs/page)
    const uint32_t usableBytes = Layout::kDescriptorsPerPage * Layout::kDescriptorStride;
    if (offsetInPage >= usableBytes) return false;

    // Must be aligned to descriptor stride
    if ((offsetInPage % Layout::kDescriptorStride) != 0) return false;

    const uint32_t descInPage = offsetInPage / Layout::kDescriptorStride;
    outLogicalIndex = page * Layout::kDescriptorsPerPage + descInPage;

    if (outLogicalIndex >= Layout::kRingBlocks) return false;
    return true;
}

void IsochTxDescriptorSlab::ValidateDescriptorLayout() const noexcept {
#ifndef NDEBUG
    // Verify that no descriptor IOVA falls within the last 32 bytes of a page.
    for (uint32_t i = 0; i < Layout::kRingBlocks; ++i) {
        const uint32_t iova = GetDescriptorIOVA(i);
        const uint32_t pageOffset = iova & (Layout::kOHCIPageSize - 1);
        if (pageOffset >= (Layout::kOHCIPageSize - Layout::kOHCIPrefetchSize)) {
            ASFW_LOG(Isoch, "❌ IT: Layout ERROR: desc %u IOVA=0x%08x pageOffset=0x%x in prefetch zone!",
                     i, iova, pageOffset);
        }
    }

    // Verify packet alignment: each packet's 3 descriptors must be in same page.
    for (uint32_t pkt = 0; pkt < Layout::kNumPackets; ++pkt) {
        const uint32_t base = pkt * Layout::kBlocksPerPacket;
        const uint32_t page0 = GetDescriptorIOVA(base) / Layout::kOHCIPageSize;
        const uint32_t page1 = GetDescriptorIOVA(base + 1) / Layout::kOHCIPageSize;
        const uint32_t page2 = GetDescriptorIOVA(base + 2) / Layout::kOHCIPageSize;
        if (page0 != page1 || page1 != page2) {
            ASFW_LOG(Isoch, "❌ IT: Packet %u spans pages! descBase=%u pages=[%u,%u,%u]",
                     pkt, base, page0, page1, page2);
        }
    }
#endif
}

} // namespace ASFW::Isoch::Tx
