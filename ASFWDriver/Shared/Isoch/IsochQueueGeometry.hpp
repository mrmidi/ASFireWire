#pragma once

#include <cstdint>

namespace ASFW::Shared::Isoch {

// Payload-agnostic producer/consumer seam geometry. These values describe how
// far a generic isoch packet producer must stay ahead of transport and how
// completions are grouped. They carry no content format, frame, clock, or
// device semantics and apply to every producer that uses the shared TX queue.
struct IsochQueueGeometry final {
    static constexpr uint32_t kPacketsPerCompletionGroup = 6;
    static constexpr uint32_t kTransmitInFlightPackets = 48;
    // Receive descriptor ring depth. It lives here rather than in
    // Isoch/Core/IsochDmaGeometry.hpp so that layers above the transport seam
    // can report the ring without including OHCI descriptor geometry, which is
    // the same reason kTransmitInFlightPackets is here.
    static constexpr uint32_t kReceiveInFlightPackets = 504;
    // A mutable-tail rebind never targets the command-pointer packet or its
    // immediate successor. The two-packet guard corresponds to the 32-byte
    // OHCI descriptor prefetch quantum; the tail address itself is one aligned
    // 32-bit store owned by transport.
    static constexpr uint32_t kPayloadRepointGuardPackets = 2;
    static constexpr uint32_t kPayloadFinalityLeadPackets =
        kPacketsPerCompletionGroup + kPayloadRepointGuardPackets;
};

static_assert(IsochQueueGeometry::kPacketsPerCompletionGroup != 0);
static_assert(IsochQueueGeometry::kTransmitInFlightPackets %
                      IsochQueueGeometry::kPacketsPerCompletionGroup ==
                  0,
              "in-flight packet window must contain complete groups");
static_assert(IsochQueueGeometry::kPayloadFinalityLeadPackets <
              IsochQueueGeometry::kTransmitInFlightPackets);
static_assert(IsochQueueGeometry::kReceiveInFlightPackets %
                      IsochQueueGeometry::kPacketsPerCompletionGroup ==
                  0,
              "receive packet window must contain complete groups");

} // namespace ASFW::Shared::Isoch
