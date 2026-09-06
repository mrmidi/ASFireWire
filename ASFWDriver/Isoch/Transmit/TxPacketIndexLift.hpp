// TxPacketIndexLift.hpp
// ASFW - Recovering the lap a CommandPtr cannot carry.
//
// The OHCI IT CommandPtr names a descriptor within the ring and nothing else.
// Divided down it gives a packet index modulo the ring size, so absolute packet
// position has to come from somewhere. Accumulating deltas supplies it only
// while every observation lands within one lap of the previous one: a first
// observation taken more than a lap after the context started, or a callback
// gap longer than a lap, silently loses whole laps, and because every later
// delta is relative the loss never corrects itself.
//
// The cycle timer supplies what the pointer cannot. An IT context transmits one
// packet per isochronous cycle, so cycles elapsed since the context started is
// the number of packets it has begun, and the lap follows from that.

#pragma once

#include <cstdint>

namespace ASFW::Isoch::Tx {

inline constexpr uint32_t kIsochCyclesPerSecond = 8'000;
/// The cycle timer's seconds field is three bits, so the whole thing repeats
/// every eight seconds.
inline constexpr uint32_t kCycleTimerWrapCycles = 8U * kIsochCyclesPerSecond;

[[nodiscard]] constexpr uint32_t CycleTimerToCycles(uint32_t cycleTimer) noexcept {
    const uint32_t seconds = (cycleTimer >> 25) & 0x7U;
    const uint32_t cycle = (cycleTimer >> 12) & 0x1FFFU;
    return seconds * kIsochCyclesPerSecond + cycle;
}

/// Cycles from `start` to `now`, correct across one wrap of the three-bit
/// seconds field. Beyond eight seconds the answer is not recoverable from the
/// timer alone and the caller must not ask.
[[nodiscard]] constexpr uint32_t CyclesBetween(uint32_t startCycleTimer,
                                               uint32_t nowCycleTimer) noexcept {
    const uint32_t start = CycleTimerToCycles(startCycleTimer);
    const uint32_t now = CycleTimerToCycles(nowCycleTimer);
    return now >= start ? now - start
                        : now + kCycleTimerWrapCycles - start;
}

/// Absolute packet index for a ring slot, given how many packets the controller
/// is believed to have begun.
///
/// Returns the index congruent to `ringSlot` that is nearest `expectedIndex`.
/// `expectedIndex` does not need to be exact, but it must be closer to the truth
/// than half a ring: at exactly half the two candidates are equidistant and no
/// rule can prefer one on the evidence, so the tie is broken downward.
///
/// A cycle count satisfies that bound only conditionally, and the caller owns
/// the condition. Cycles equal descriptor advances while every cycle the
/// context is scheduled in transmits a packet, but ASFW self-links each
/// packet's skip address (IsochTxDmaRing.cpp, following Linux
/// `queue_iso_transmit`), so a lost cycle or FIFO overrun skips a cycle
/// *without* advancing past the packet -- see
/// references/linux-ohci-firewire-low-level-stack/ohci.c:3250-3256. The cycle
/// timer keeps running through those, so elapsed cycles is an upper bound on
/// descriptor progress, not an equality. Launch delay biases it the same way.
///
/// Below half a ring of accumulated skips the lift is still exact, because the
/// nearest-congruent rule absorbs the error. At or beyond it the result is off
/// by a whole lap in the direction that invents transmitted laps. Skips are
/// most likely exactly when the context is starved, which is the condition a
/// lap-loss diagnostic exists to measure, so callers must report the result as
/// an estimate under this assumption and not as established progress.
[[nodiscard]] constexpr uint64_t LiftRingSlotToAbsolute(
    uint32_t ringSlot, uint64_t expectedIndex, uint32_t ringPackets) noexcept {
    if (ringPackets == 0) return expectedIndex;
    const uint64_t ring = ringPackets;
    uint64_t candidate = (expectedIndex / ring) * ring + ringSlot;
    const uint64_t half = ring / 2;
    if (candidate + half < expectedIndex) {
        candidate += ring;
    } else if (candidate > expectedIndex + half && candidate >= ring) {
        candidate -= ring;
    }
    return candidate;
}

/// Packets the controller completed between two CommandPtr observations.
///
/// The slot difference alone is bounded by `ringPackets - 1`, so it cannot
/// express a lap: if the controller completed a whole ring or more since the
/// previous reading, that lap is absent from the difference rather than large in
/// it. Accumulating such differences into an absolute cursor loses the lap
/// permanently and re-bases every later reading on the wrong origin.
///
/// `elapsedCycles` adjudicates. One packet begins per isochronous cycle, so it
/// bounds the true advance, and the nearest-congruent lift recovers the laps the
/// slot could not carry.
///
/// Never returns less than the naive difference: elapsed cycles is an upper
/// bound on descriptor progress (self-linked skip addresses mean a lost cycle
/// need not advance the context), so it may only ever add laps, never remove
/// packets the slots already prove were consumed. Exact while accumulated
/// skipped cycles stay under half a ring; at or beyond that it over-counts in
/// the direction that invents laps, so callers owe the same caveat
/// LiftRingSlotToAbsolute carries.
[[nodiscard]] constexpr uint64_t RecoverConsumedDelta(uint32_t prevSlot,
                                                      uint32_t nowSlot,
                                                      uint32_t elapsedCycles,
                                                      uint32_t ringPackets) noexcept {
    if (ringPackets == 0) return 0;
    const uint32_t naive = nowSlot >= prevSlot
                               ? nowSlot - prevSlot
                               : (ringPackets - prevSlot) + nowSlot;
    const uint64_t lifted = LiftRingSlotToAbsolute(
        nowSlot, static_cast<uint64_t>(prevSlot) + elapsedCycles, ringPackets);
    const uint64_t delta = lifted > prevSlot ? lifted - prevSlot : 0;
    return delta > naive ? delta : naive;
}

} // namespace ASFW::Isoch::Tx
