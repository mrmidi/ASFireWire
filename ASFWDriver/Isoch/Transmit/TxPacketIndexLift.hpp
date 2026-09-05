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
/// rule can prefer one on the evidence, so the tie is broken downward. A cycle
/// count is comfortably inside that bound -- it errs by the few cycles between
/// arming the context and the controller fetching its first descriptor, not by
/// twenty-four.
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
