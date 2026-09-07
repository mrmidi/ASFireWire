// IsochTxLayout.hpp
// ASFW - Isochronous Transmit (IT) layout constants
//
// This file centralizes the IT descriptor layout used by IsochTransmitContext.
// The layout follows Linux-style OHCI page padding constraints (prefetch safety).
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../Hardware/OHCIDescriptors.hpp"
#include "../Core/IsochDmaGeometry.hpp"

namespace ASFW::Isoch::Tx {

struct Layout final {
    // ==========================================================================
    // Descriptor page padding (Linux firewire-ohci strategy)
    //
    // kDescriptorPageStride is NOT a query of the host page size. It is a
    // layout stride applied identically to the virtual address, the IOVA and
    // the command-pointer decode, inside a region AllocateDMA guarantees is one
    // contiguous segment (HardwareInterface.cpp:806 rejects segmentCount != 1).
    // The last kOHCIPrefetchSize bytes of every stride are left empty so a
    // controller that issues an oversized descriptor read cannot walk off the
    // end of a real page -- cross-validated with Linux ohci.c:1221-1227.
    //
    // INVARIANT: the stride must be <= the smallest DMA page size across every
    // architecture we build for, so that padding at stride boundaries also pads
    // at real page boundaries. Finer is safe; coarser is not. Apple Silicon
    // pages are 16 KiB, x86_64 pages are 4 KiB, and project.yml builds both --
    // hence 4096, not 16384. Because the slab base is forced 4 KiB aligned
    // (IsochTxDescriptorSlab.cpp), every real page boundary lands on one of
    // these stride boundaries whatever the base happens to be modulo 16 KiB.
    // ==========================================================================
    static constexpr size_t kDescriptorPageStride = 4096;
    static_assert(kDescriptorPageStride <= 4096,
                  "descriptor page stride must not exceed the smallest DMA page "
                  "size we build for (x86_64 = 4 KiB). Raising it to the Apple "
                  "Silicon 16 KiB page stops the padding from covering real page "
                  "boundaries on the x86_64 slice.");
    static constexpr size_t kOHCIPrefetchSize = 32;
    static constexpr size_t kUsablePerPage = kDescriptorPageStride - kOHCIPrefetchSize;

    // Packet program:
    //   blocks 0-1: OUTPUT_MORE_IMMEDIATE
    //   block 2:    OUTPUT_MORE payload fragment
    //   block 3:    OUTPUT_LAST payload fragment
    //
    // A single contiguous payload is deliberately split between blocks 2 and 3
    // so command pointers and branches always use Z=4. A payload crossing one
    // DMA segment boundary uses the two natural fragments.
    static constexpr uint32_t kBlocksPerPacket = 4;
    static constexpr uint32_t kFirstPayloadBlock = 2;
    static constexpr uint32_t kCompletionBlock = 3;
    static constexpr uint32_t kNumPackets =
        IsochDmaGeometry::kTransmitDescriptorPackets;
    static constexpr uint32_t kRingBlocks = kNumPackets * kBlocksPerPacket;

    static constexpr uint32_t kDescriptorStride = 16;
    static constexpr uint32_t kDescriptorsPerPageRaw =
        static_cast<uint32_t>(kUsablePerPage / kDescriptorStride);
    static constexpr uint32_t kDescriptorsPerPage =
        (kDescriptorsPerPageRaw / kBlocksPerPacket) * kBlocksPerPacket;

    static constexpr uint32_t kTotalPages =
        (kRingBlocks + kDescriptorsPerPage - 1) / kDescriptorsPerPage;

    static constexpr size_t kDescriptorRingSize = kTotalPages * kDescriptorPageStride;

    // Static assertions
    static_assert(kDescriptorsPerPage >= kBlocksPerPacket, "Need at least one packet per page");
    static_assert((kDescriptorsPerPage % kBlocksPerPacket) == 0, "Keep packets within a page");
    static_assert((static_cast<size_t>(kDescriptorsPerPage) * kDescriptorStride) <= kUsablePerPage,
                  "Must fit in usable space");
    static_assert(kBlocksPerPacket == 4, "Z must be 4 for OMI(2)+OM(1)+OL(1)");
    static_assert(sizeof(Async::HW::OHCIDescriptor) == 16, "OHCI descriptor must be 16 bytes");
    static_assert(kDescriptorStride == sizeof(Async::HW::OHCIDescriptor), "Stride must match descriptor size");
};

} // namespace ASFW::Isoch::Tx
