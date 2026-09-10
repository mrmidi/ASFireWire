#pragma once

#include <cstdint>

namespace ASFW::Shared::Isoch {

// Payload-agnostic producer/consumer seam geometry. These values describe how
// far a generic isoch packet producer must stay ahead of transport and how
// completions are grouped. They carry no content format, frame, clock, or
// device semantics and apply to every producer that uses the shared TX queue.
struct IsochQueueGeometry final {
    // Completion grouping, in packets == isoch cycles (125 us each).
    //
    // Was 6 (750 us) until 2026-09-08. Six is not a whole number of AMDTP
    // cadence blocks -- at 48k the blocking cadence repeats D,D,D,N over 4
    // packets, so a 6-packet window straddles the pattern and carries 4 or 5
    // DATA packets depending on phase (32 or 40 frames). Eight is two whole
    // blocks, so every interrupt carries exactly 6 DATA packets at any phase:
    // a constant 48 frames at 1x, 96 at 2x, 192 at 4x. That constant is what
    // lets the HAL zero-timestamp boundary land at a fixed offset inside the
    // group instead of walking, and it costs 25% fewer interrupts (1.0 ms
    // instead of 750 us). kTransmitInFlightPackets stays divisible: 504/8=63.
    static constexpr uint32_t kPacketsPerCompletionGroup = 8;
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
    // Decoupled from completion grouping (kPacketsPerCompletionGroup):
    // In legacy reference (Focusrite Saffire.kext, reversed in IDA Pro at
    // Saffire::UpdateIsochBufferParams 0xf506 and Saffire::PrepareSendDCLs 0x10304),
    // the hardware completion group was 10-12 packets (1.25-1.5 ms), but the
    // TX delay was only 2 packets (16 frames / 333 us). The interrupt frequency
    // governs only descriptor ring recycling, not content finality.
    //
    // Image 0 (silence) is pre-armed in all descriptors during refill.
    // The physical point-of-no-return for repointing to Image 1 is strictly the
    // OHCI 32-byte prefetch horizon (kPayloadRepointGuardPackets = 2) plus 1
    // cycle dispatch slack = 3 packets (375 us).
    // Sealing 10 packets ahead (8-packet group + 2-packet guard) was an artificial
    // software artifact that starved 32-sample CoreAudio buffers (which have only
    // 67 frames / 8.3 packets of wire runway due to the 25-frame IEC 61883-6
    // transfer delay), causing 46 packet substitutions per 3,751 packets on
    // Focusrite Saffire Pro 24 DSP (87% on Phase 2 at the completion boundary).
    static constexpr uint32_t kPayloadFinalityLeadPackets =
        kPayloadRepointGuardPackets + 1;
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
