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
// Cycle timing can bound an observation gap; it cannot establish a lap, and
// nothing in the transmit path asks it to any more: completion is read from
// OUTPUT_LAST descriptor status (IsochTxDmaRing::CountCompletedPackets), which
// carries the lap the pointer cannot. The lift below is a diagnostic estimate,
// never an ownership proof.

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

} // namespace ASFW::Isoch::Tx
