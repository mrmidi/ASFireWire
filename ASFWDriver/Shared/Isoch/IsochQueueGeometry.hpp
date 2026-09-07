#pragma once

#include <cstdint>

namespace ASFW::Shared::Isoch {

// Payload-agnostic producer/consumer seam geometry. These values describe how
// far a generic isoch packet producer must stay ahead of transport and how
// completions are grouped. They carry no content format, frame, clock, or
// device semantics and apply to every producer that uses the shared TX queue.
struct IsochQueueGeometry final {
    static constexpr uint32_t kPacketsPerCompletionGroup = 6;
    // Transmit descriptor ring depth, and therefore the DMA runway: transport
    // keeps mappedEnd at completionCursor + this, the queue is zero-terminated,
    // and a consumer that finishes everything mapped before refill runs takes a
    // fatal MappedRegionExhausted (IsochTxDmaRing.cpp, the observedCompleted ==
    // mappedLimit branch). One packet is one isoch cycle, so this is the number
    // in units of 125 us -- 504 packets = 63 ms.
    //
    // Was 48 (6.0 ms) until 2026-09-07. The driver measures DriverKit dispatch
    // stalls of 40-42 packets (5.0-5.25 ms), so the fatal boundary carried 1.14x
    // margin, and the 1 ms watchdog fallback that carries TX through interrupt
    // silence lives on the same queue as the interrupt source -- one stall
    // delays both. Raised to RX parity so both directions survive the same
    // outage: RX has always had 63 ms here and is drained unconditionally by
    // that watchdog, which is the whole reason only TX ever died.
    //
    // Depth is not delay. A producer arms every slot with a complete, valid
    // image up front and may replace it until kPayloadFinalityLeadPackets
    // below, so that lead -- not this one -- is the only depth a producer's
    // own delay accounting has to carry. Deepening the ring buys runway
    // without moving the finality frontier.
    static constexpr uint32_t kTransmitInFlightPackets = 504;
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
