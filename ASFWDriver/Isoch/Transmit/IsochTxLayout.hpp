// IsochTxLayout.hpp
// ASFW - Isochronous Transmit (IT) layout constants
//
// This file centralizes the IT descriptor/payload layout used by IsochTransmitContext.
// The layout follows Linux-style OHCI page padding constraints (prefetch safety).
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../Hardware/OHCIDescriptors.hpp"

namespace ASFW::Isoch::Tx {

struct Layout final {
    // ==========================================================================
    // Linux-style OHCI page padding constants
    // ==========================================================================
    static constexpr size_t kOHCIPageSize = 4096;
    static constexpr size_t kOHCIPrefetchSize = 32;
    static constexpr size_t kUsablePerPage = kOHCIPageSize - kOHCIPrefetchSize;  // 4064

    // We program packets as: OUTPUT_MORE_IMMEDIATE (Isoch header) + OUTPUT_LAST.
    static constexpr uint32_t kBlocksPerPacket = 3;
    static constexpr uint32_t kNumPackets = 200;  // ~25ms @ 8000 pkts/sec
    static constexpr uint32_t kRingBlocks = kNumPackets * kBlocksPerPacket;

    static constexpr uint32_t kDescriptorStride = 16;
    static constexpr uint32_t kDescriptorsPerPageRaw =
        static_cast<uint32_t>(kUsablePerPage / kDescriptorStride);  // 254
    static constexpr uint32_t kDescriptorsPerPage =
        (kDescriptorsPerPageRaw / kBlocksPerPacket) * kBlocksPerPacket;  // 252

    static constexpr uint32_t kTotalPages =
        (kRingBlocks + kDescriptorsPerPage - 1) / kDescriptorsPerPage;  // 3

    static constexpr size_t kDescriptorRingSize = kTotalPages * kOHCIPageSize;  // 12288

    // Worst-case packet size we reserve per slot (kept simple: fixed stride per packet).
    static constexpr uint32_t kMaxPacketSize = 4096;
    static constexpr size_t kPayloadBufferSize = kNumPackets * kMaxPacketSize;

    // Guard band in packets used by verifier mismatch checks.
    static constexpr uint32_t kGuardBandPackets = 4;

    // Audio injection window (latency control) — used by audio pipeline.
    static constexpr uint32_t kAudioWriteAhead = 16;
    static constexpr uint32_t kMaxWriteAhead = kNumPackets - kGuardBandPackets;  // 196

    // Static assertions
    static_assert(kDescriptorsPerPage >= kBlocksPerPacket, "Need at least one packet per page");
    static_assert((kDescriptorsPerPage % kBlocksPerPacket) == 0, "Keep packets within a page");
    static_assert((kDescriptorsPerPage * kDescriptorStride) <= kUsablePerPage, "Must fit in usable space");
    static_assert(kBlocksPerPacket == 3, "Z must be 3 for OMI(2)+OL(1)");
    static_assert(sizeof(Async::HW::OHCIDescriptor) == 16, "OHCI descriptor must be 16 bytes");
    static_assert(kDescriptorStride == sizeof(Async::HW::OHCIDescriptor), "Stride must match descriptor size");
};

} // namespace ASFW::Isoch::Tx

