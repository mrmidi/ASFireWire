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
#include "../../Shared/Isoch/AudioTimingGeometry.hpp"

namespace ASFW::Isoch::Tx {

struct Layout final {
    // ==========================================================================
    // Linux-style OHCI page padding constants
    // ==========================================================================
    static constexpr size_t kOHCIPageSize = 4096;
    static constexpr size_t kOHCIPrefetchSize = 32;
    static constexpr size_t kUsablePerPage = kOHCIPageSize - kOHCIPrefetchSize;  // 4064

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
        ASFW::IsochTransport::AudioTimingGeometry::
            kTxHardwareRingPackets;  // ~24ms @ 8000 pkts/sec
    static constexpr uint32_t kRingBlocks = kNumPackets * kBlocksPerPacket;

    static constexpr uint32_t kDescriptorStride = 16;
    static constexpr uint32_t kDescriptorsPerPageRaw =
        static_cast<uint32_t>(kUsablePerPage / kDescriptorStride);  // 254
    static constexpr uint32_t kDescriptorsPerPage =
        (kDescriptorsPerPageRaw / kBlocksPerPacket) * kBlocksPerPacket;  // 252

    static constexpr uint32_t kTotalPages =
        (kRingBlocks + kDescriptorsPerPage - 1) / kDescriptorsPerPage;  // 4

    static constexpr size_t kDescriptorRingSize = kTotalPages * kOHCIPageSize;  // 16384

    // The command-pointer slot and the next four packets are treated as
    // hardware-owned. Payload preparation uses a much earlier deadline so the
    // controller cannot observe a packet while it is being patched.
    static constexpr uint32_t kHardwareOwnedGuardPackets = 4;
    static constexpr uint32_t kPreparationDeadlinePackets = 64;
    static constexpr uint32_t kGuardBandPackets = kHardwareOwnedGuardPackets;

    // Metadata exposure window for inspecting recently refilled packet metadata/payloads.
    static constexpr uint32_t kMetadataWriteAhead = 16;
    static constexpr uint32_t kMaxWriteAhead =
        kNumPackets - kHardwareOwnedGuardPackets;  // 188

    static_assert(kPreparationDeadlinePackets > kHardwareOwnedGuardPackets);
    static_assert(kPreparationDeadlinePackets < kMaxWriteAhead);

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
