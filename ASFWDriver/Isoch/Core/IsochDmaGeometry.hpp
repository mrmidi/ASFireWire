#pragma once

#include "../../Shared/Isoch/IsochQueueGeometry.hpp"

#include <cstdint>

namespace ASFW::Isoch {

// Descriptor geometry is an OHCI scheduling decision, not a statement about
// the content carried by a context.  Keep it here so receive/transmit
// transport never takes a dependency on a content buffer, frame, or cadence
// policy.
struct IsochDmaGeometry final {
    static constexpr uint32_t kPacketsPerInterrupt =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::kPacketsPerCompletionGroup;
    static constexpr uint32_t kReceiveDescriptorPackets = 504;
    static constexpr uint32_t kTransmitDescriptorPackets =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::kTransmitInFlightPackets;
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
