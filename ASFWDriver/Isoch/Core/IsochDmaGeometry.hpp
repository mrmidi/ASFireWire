#pragma once

#include <cstdint>

namespace ASFW::Isoch {

// Descriptor geometry is an OHCI scheduling decision, not a statement about
// the content carried by a context.  Keep it here so receive/transmit
// transport never takes a dependency on Audio's buffer, frame, or cadence
// policy.
struct IsochDmaGeometry final {
    static constexpr uint32_t kPacketsPerInterrupt = 6;
    static constexpr uint32_t kReceiveDescriptorPackets = 504;
};

static_assert(IsochDmaGeometry::kPacketsPerInterrupt != 0);
static_assert(IsochDmaGeometry::kReceiveDescriptorPackets %
                  IsochDmaGeometry::kPacketsPerInterrupt ==
              0,
              "IR descriptor ring must contain complete interrupt groups");

} // namespace ASFW::Isoch
