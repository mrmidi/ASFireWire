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
};

static_assert(IsochQueueGeometry::kPacketsPerCompletionGroup != 0);
static_assert(IsochQueueGeometry::kTransmitInFlightPackets %
                      IsochQueueGeometry::kPacketsPerCompletionGroup ==
                  0,
              "in-flight packet window must contain complete groups");

} // namespace ASFW::Shared::Isoch
