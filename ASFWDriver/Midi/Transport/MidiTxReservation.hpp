//
// MidiTxReservation.hpp
// ASFWDriver
//
// Packet-local reserve / commit / cancel over the host -> device byte rings.
//
// Why this exists at all: AmdtpTxPacketizer rebuilds a refilled packet to be
// byte-for-byte identical to the armed packet wherever PCM does not reach,
// deliberately, "so a fill that loses the race to freeze is indistinguishable
// from never having happened" (AmdtpTxPacketizer.cpp:138-148). Writing a MIDI
// byte breaks that invariant by construction -- a lost race is no longer
// indistinguishable, because a byte left a queue. So the byte must be *chosen*
// before the packet is composed and *retired* only once that packet is known to
// have been published.
//
// A bare Peek/Consume pair is not enough: neither call records which queue
// positions were selected for which packet, so a later Consume cannot know
// whether it is retiring the bytes that actually went out.
//
// Pure logic: no DriverKit dependency, no allocation, no locking. Runs on the
// TX content path, which is latency-sensitive.
//

#pragma once

#include <cstdint>

#include "MidiTransportBlock.hpp"

namespace ASFW::Midi {

/// One packet's selection: at most one byte per port, as Linux and Focusrite
/// both emit (label 0x81). Holding the bytes here rather than re-reading the
/// ring at compose time means the ring cannot change under the packet.
struct MidiPacketReservation final {
    uint64_t epoch{0};
    uint32_t packetIndex{0};
    bool active{false};

    /// Selected byte per port, and whether one was selected at all.
    uint8_t byteForPort[kMidiPortsPerDirection]{};
    bool hasByteForPort[kMidiPortsPerDirection]{};

    [[nodiscard]] bool Any() const noexcept {
        for (const bool has : hasByteForPort) {
            if (has) return true;
        }
        return false;
    }

    [[nodiscard]] bool Get(uint8_t port, uint8_t& outByte) const noexcept {
        if (port >= kMidiPortsPerDirection || !hasByteForPort[port]) return false;
        outByte = byteForPort[port];
        return true;
    }
};

/// Outcome counters for the reservation path. Bounded, and read off the hot
/// path; nothing here logs.
struct MidiReservationCounters final {
    uint64_t reservations{0};
    uint64_t commits{0};
    uint64_t cancels{0};
    uint64_t bytesCommitted{0};
    /// Bytes selected into a packet that was then cancelled. They stay queued;
    /// this counts how often the work was thrown away.
    uint64_t bytesReturned{0};
    /// Commits refused because the epoch moved under the reservation.
    uint64_t staleEpochCommits{0};
    /// Commits refused because they did not match the outstanding reservation.
    uint64_t mismatchedCommits{0};
};

/// Selects, holds and retires the bytes for one TX stream's packets.
///
/// Exactly one reservation may be outstanding at a time, because the packet
/// path composes one candidate image at a time. A second Begin while one is
/// live is a caller bug and is refused rather than silently replacing the
/// first, which would strand its bytes as neither committed nor returned.
class MidiTxReservationScope final {
public:
    /// Select at most one byte per port for `packetIndex`.
    ///
    /// Nothing is consumed: the bytes stay in their rings and remain visible to
    /// the producer's free-space accounting until Commit retires them.
    [[nodiscard]] bool Begin(MidiTransportBlock& block, uint64_t epoch,
                             uint32_t packetIndex,
                             MidiPacketReservation& out) noexcept {
        if (outstanding_) return false;
        out = MidiPacketReservation{};
        if (!block.Usable(epoch)) return false;

        for (uint32_t port = 0; port < kMidiPortsPerDirection; ++port) {
            uint8_t byte = 0;
            uint8_t scratch[1];
            if (block.hostToDevice[port].Peek(scratch) == 1) {
                byte = scratch[0];
                out.byteForPort[port] = byte;
                out.hasByteForPort[port] = true;
            }
        }
        out.epoch = epoch;
        out.packetIndex = packetIndex;
        out.active = true;

        outstanding_ = true;
        epoch_ = epoch;
        packetIndex_ = packetIndex;
        ++counters_.reservations;
        return true;
    }

    /// Retire exactly the bytes this reservation selected.
    ///
    /// Call only once the packet carrying them is known to have been published.
    /// A successful publication is a software retirement point under an
    /// at-most-once policy, not proof of physical delivery -- transport can
    /// still discard an accepted offer, and that is counted as loss elsewhere
    /// rather than replayed here, because replaying would put a byte behind
    /// bytes already on the wire.
    [[nodiscard]] bool Commit(MidiTransportBlock& block,
                              const MidiPacketReservation& reservation) noexcept {
        if (!outstanding_ || !reservation.active) return false;
        if (reservation.epoch != epoch_ ||
            reservation.packetIndex != packetIndex_) {
            ++counters_.mismatchedCommits;
            return false;
        }
        if (!block.Usable(reservation.epoch)) {
            // The stream restarted while this packet was in flight. The bytes
            // belong to a stream that no longer exists; Arm() has already
            // cleared the rings, so there is nothing to retire.
            ++counters_.staleEpochCommits;
            outstanding_ = false;
            return false;
        }

        for (uint32_t port = 0; port < kMidiPortsPerDirection; ++port) {
            if (!reservation.hasByteForPort[port]) continue;
            block.hostToDevice[port].Consume(1);
            ++counters_.bytesCommitted;
        }
        ++counters_.commits;
        outstanding_ = false;
        return true;
    }

    /// Abandon the selection; the bytes stay queued for a later packet.
    ///
    /// This is the normal path for a rejected fill, an unready sibling stream
    /// or a missed freeze frontier. It must not spend the bytes, and it must
    /// not spend their rate-limiter credit either -- but elapsed wire time
    /// still advanced, which the limiter accounts for separately.
    void Cancel(const MidiPacketReservation& reservation) noexcept {
        if (!outstanding_) return;
        for (const bool has : reservation.hasByteForPort) {
            if (has) ++counters_.bytesReturned;
        }
        ++counters_.cancels;
        outstanding_ = false;
    }

    /// Drop any outstanding reservation without retiring anything.
    ///
    /// Packet indices are reused on restart and eventually wrap, so a
    /// reservation that survives an epoch change could otherwise be committed
    /// by a packet that merely shares its index.
    void Reset() noexcept {
        outstanding_ = false;
        epoch_ = 0;
        packetIndex_ = 0;
    }

    [[nodiscard]] bool HasOutstanding() const noexcept { return outstanding_; }
    [[nodiscard]] const MidiReservationCounters& Counters() const noexcept {
        return counters_;
    }

private:
    bool outstanding_{false};
    uint64_t epoch_{0};
    uint32_t packetIndex_{0};
    MidiReservationCounters counters_{};
};

} // namespace ASFW::Midi
