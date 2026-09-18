// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuTxTiming.hpp - Transmit-side SPH stamping for MOTU protocol-v2 streams.
//
// MOTU is duplex-always: the device recovers its media clock from the host replaying the
// device's own timing, and it needs BOTH replays -- the data-blocks-per-packet sequence
// and the per-data-block source packet header as presentation time
// (motu-stream.c:205-207). This header covers the second one.
//
// The algorithm is Linux write_sph() (amdtp-motu.c:373-393):
//
//     base_tick = rx_cycle_count * TICKS_PER_CYCLE
//     for each data block:
//         tick = (base_tick + event_offsets[head]) % TICKS_PER_SECOND
//         block SPH = (tick / TICKS_PER_CYCLE) << 12 | (tick % TICKS_PER_CYCLE)
//         head = (head + 1) % cache_size          <-- advances PER DATA BLOCK
//     rx_cycle_count = (rx_cycle_count + 1) % CYCLES_PER_SECOND
//
// GRANULARITY NOTE -- read before wiring this to RxSequenceReplay.
//
// Linux caches one presentation offset per *data block*. ASFW's RxSequenceReplay caches
// one sytOffset per *packet* (DirectAudioReceiveConsumer publishes a single
// RxSequenceEntry per received packet, carrying its dataBlocks count). A MOTU stream at
// 48 kHz runs 8 data blocks per packet, so a packet-granular cache is 8x coarser than
// the replay this device expects.
//
// That gap is deliberately NOT papered over here by interpolating offsets within a
// packet: the wire-compatibility bar is "behaves like the reference stack", and evenly
// spacing blocks the device timed unevenly is a different stream. Feeding this function
// therefore needs a per-block offset source -- either a MOTU-specific capture cache or a
// per-block extension of the existing one. The function takes the offsets explicitly so
// that decision stays with the caller and the arithmetic stays testable meanwhile.

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuSph.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

/// Base tick for a packet, from its whole-cycle count (amdtp-motu.c:379).
[[nodiscard]] constexpr uint32_t BaseTickForCycle(uint32_t cycleCount) noexcept {
    return (cycleCount % kCyclesPerSecond) * kTicksPerCycle;
}

/// Next packet's cycle count (amdtp-motu.c:391).
[[nodiscard]] constexpr uint32_t AdvanceCycleCount(uint32_t cycleCount) noexcept {
    return (cycleCount + 1U) % kCyclesPerSecond;
}

struct SphStampResult final {
    /// Data blocks actually stamped. Short of `dataBlocks` only when the payload or the
    /// offset sequence could not cover them, which the caller should treat as a fault
    /// rather than a partial success -- an unstamped block carries a stale SPH.
    uint32_t blocksStamped{0};
    /// Cycle count to use for the next packet.
    uint32_t nextCycleCount{0};
};

/// Stamp one SPH quadlet at the head of every data block in a prepared packet.
///
/// `payload` is the whole isochronous payload including the CIP header; `dbs` is the data
/// block size in quadlets, so blocks are `dbs * 4` bytes apart. `offsets` supplies one
/// cached presentation offset per data block, consumed in order -- see the granularity
/// note above.
[[nodiscard]] inline SphStampResult WritePacketSph(std::span<uint8_t> payload,
                                                   uint32_t dbs,
                                                   uint32_t dataBlocks,
                                                   uint32_t cycleCount,
                                                   std::span<const uint32_t> offsets,
                                                   uint32_t cipHeaderBytes = 8U) noexcept {
    SphStampResult result{};
    result.nextCycleCount = AdvanceCycleCount(cycleCount);

    if (dbs == 0 || dataBlocks == 0) {
        return result;
    }

    const uint32_t baseTick = BaseTickForCycle(cycleCount);
    const uint64_t blockBytes = static_cast<uint64_t>(dbs) * 4ULL;

    for (uint32_t block = 0; block < dataBlocks; ++block) {
        if (block >= offsets.size()) {
            break; // caller under-supplied the replay sequence
        }
        const uint64_t blockStart =
            static_cast<uint64_t>(cipHeaderBytes) + static_cast<uint64_t>(block) * blockBytes;
        if (blockStart + 4ULL > payload.size()) {
            break; // payload cannot hold this block's SPH quadlet
        }

        WriteSph(payload.subspan(static_cast<size_t>(blockStart), 4),
                 ReplaySph(offsets[block], baseTick));
        ++result.blocksStamped;
    }

    return result;
}

} // namespace ASFW::Encoding::Motu
