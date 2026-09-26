#pragma once

#include <cstdint>

namespace ASFW::Isoch {

// Descriptor geometry is an OHCI scheduling decision, not a statement about
// the content carried by a context.  Keep it here so receive/transmit
// transport never takes a dependency on Audio's buffer, frame, or cadence
// policy.
struct IsochDmaGeometry final {
    static constexpr uint32_t kPacketsPerInterrupt = 8;
    static constexpr uint32_t kReceiveDescriptorPackets = 504;
    // Finite IT queue (T5, documentation/TX_OWNERSHIP.md): the hardware stops
    // at the end of what is mapped, so the ring is the refill-stall budget.
    // 504 packets = 63 ms, at RX parity: both directions survive the same
    // dispatch outage.
    static constexpr uint32_t kTransmitDescriptorPackets = 504;
};

static_assert(IsochDmaGeometry::kPacketsPerInterrupt != 0);
static_assert(IsochDmaGeometry::kReceiveDescriptorPackets %
                  IsochDmaGeometry::kPacketsPerInterrupt ==
              0,
              "IR descriptor ring must contain complete interrupt groups");
static_assert(IsochDmaGeometry::kTransmitDescriptorPackets %
                  IsochDmaGeometry::kPacketsPerInterrupt ==
              0,
              "IT descriptor ring must contain complete interrupt groups");

} // namespace ASFW::Isoch
