// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuEventOffsetCache.hpp - Per-data-block presentation offsets for MOTU SPH replay.
//
// MOTU is duplex-always: the device recovers its media clock from the host replaying the
// device's own timing, and it needs the source packet header of *every data block*, not
// one presentation time per packet (motu-stream.c:205-207).
//
// This is the MOTU-specific counterpart to RxSequenceReplay. That cache stores one
// sytOffset per received packet, which is the right granularity for DICE and Apogee
// (their presentation time lives in the CIP SYT field, once per packet) but 8x too
// coarse for MOTU at 48 kHz. Rather than widening the shared cache -- which carries the
// TX reclamp/self-heal logic every working family depends on -- MOTU keeps its own ring,
// so the blast radius of getting this wrong is confined to MOTU.
//
// Mirrors the amdtp_motu_cache pair (motu.h:41-48):
//     capture  <- cache_event_offsets() (amdtp-motu.c:303-329)
//     playback -> write_sph()           (amdtp-motu.c:373-393)
// Both walk one ring slot per data block and advance their own whole-cycle counter by
// one cycle per packet.
//
// Naming: Linux names the counters from the *device's* point of view (tx_cycle_count
// fills from the device's transmit stream). These are named from the host's, because
// every other cursor in this driver is: Capture* is device->host, Playback* is
// host->device.

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuSph.hpp"
#include "MotuTxTiming.hpp"

#include <atomic>
#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

class MotuEventOffsetCache final {
public:
    /// 512 packets of 8 data blocks: the same history depth RxSequenceReplayState keeps,
    /// expressed per block. A power of two so the modulo folds to a mask.
    static constexpr uint32_t kCapacity = 4096;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

    /// Offsets are ticks on the 24.576 MHz timeline, always < kTicksPerSecond.
    static constexpr uint32_t kNoOffset = UINT32_MAX;

    void Reset() noexcept {
        captureCursor_.store(0, std::memory_order_relaxed);
        playbackCursor_.store(0, std::memory_order_relaxed);
        captureCycleCount_.store(0, std::memory_order_relaxed);
        playbackCycleCount_.store(0, std::memory_order_relaxed);
        established_.store(false, std::memory_order_release);
    }

    /// Cache one offset per data block from a received packet.
    ///
    /// `payload` is the whole isochronous payload including the CIP header. Returns the
    /// number of blocks cached, which is short of `dataBlocks` only when the payload
    /// cannot hold them -- a truncated packet must not contribute invented timing.
    ///
    /// Mirrors cache_event_offsets (amdtp-motu.c:303-329): the base is this packet's
    /// whole-cycle count, and the counter advances one cycle per packet regardless of
    /// how many blocks were present.
    uint32_t Capture(std::span<const uint8_t> payload,
                     uint32_t dbs,
                     uint32_t dataBlocks,
                     uint32_t cipHeaderBytes = 8U) noexcept {
        if (dbs == 0 || dataBlocks == 0) {
            return 0;
        }
        const uint32_t baseTick = BaseTickForCycle(captureCycleCount_.load(std::memory_order_relaxed));
        const uint64_t blockBytes = static_cast<uint64_t>(dbs) * 4ULL;

        uint32_t cached = 0;
        uint64_t cursor = captureCursor_.load(std::memory_order_relaxed);

        for (uint32_t block = 0; block < dataBlocks; ++block) {
            const uint64_t blockStart =
                static_cast<uint64_t>(cipHeaderBytes) + static_cast<uint64_t>(block) * blockBytes;
            if (blockStart + 4ULL > payload.size()) {
                break;
            }
            const uint32_t sph = ReadSph(payload.subspan(static_cast<size_t>(blockStart), 4));
            slots_[(cursor + block) & (kCapacity - 1)].store(TickOffsetFromBase(sph, baseTick),
                                                             std::memory_order_relaxed);
            ++cached;
        }

        // Publish the new cursor last: a reader that sees it is guaranteed to see the
        // slot writes above (release pairs with the acquire in Take).
        captureCursor_.store(cursor + cached, std::memory_order_release);
        captureCycleCount_.store(AdvanceCycleCount(captureCycleCount_.load(std::memory_order_relaxed)),
                                 std::memory_order_relaxed);
        if (cached > 0) {
            established_.store(true, std::memory_order_release);
        }
        return cached;
    }

    /// Fill `out` with the next `out.size()` cached offsets and advance the playback
    /// cursor past them.
    ///
    /// Returns false without consuming anything when fewer are available or when the
    /// requested run would read history already overwritten by the capture side. A
    /// partial fill is never returned: an unstamped block would go out carrying a stale
    /// SPH, so the caller must be able to treat this as all-or-nothing.
    [[nodiscard]] bool Take(std::span<uint32_t> out) noexcept {
        if (out.empty()) {
            return false;
        }
        const uint64_t producer = captureCursor_.load(std::memory_order_acquire);
        const uint64_t cursor = playbackCursor_.load(std::memory_order_relaxed);

        if (cursor + out.size() > producer) {
            return false; // playback has caught up with capture
        }
        if (producer - cursor > kCapacity) {
            return false; // the run we want has been overwritten
        }

        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = slots_[(cursor + i) & (kCapacity - 1)].load(std::memory_order_relaxed);
        }
        playbackCursor_.store(cursor + out.size(), std::memory_order_release);
        return true;
    }

    /// Cycle count the next transmitted packet should be based on, and its advance.
    /// Kept here so the pair of counters stays together, matching the reference.
    [[nodiscard]] uint32_t PlaybackCycleCount() const noexcept {
        return playbackCycleCount_.load(std::memory_order_relaxed);
    }
    void AdvancePlaybackCycle() noexcept {
        playbackCycleCount_.store(AdvanceCycleCount(playbackCycleCount_.load(std::memory_order_relaxed)),
                                  std::memory_order_relaxed);
    }

    /// True once at least one block has been captured, so the transmit side knows the
    /// device's timing has actually been observed rather than assumed.
    [[nodiscard]] bool IsEstablished() const noexcept {
        return established_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t Available() const noexcept {
        const uint64_t producer = captureCursor_.load(std::memory_order_acquire);
        const uint64_t cursor = playbackCursor_.load(std::memory_order_relaxed);
        return (producer > cursor) ? (producer - cursor) : 0ULL;
    }

    [[nodiscard]] uint64_t CaptureCursor() const noexcept {
        return captureCursor_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t PlaybackCursor() const noexcept {
        return playbackCursor_.load(std::memory_order_relaxed);
    }

private:
    // Capture and playback run on separate queues, so the ring is atomic like
    // RxSequenceReplayState's. Cursors are monotonic 64-bit counts folded to an index,
    // which keeps "how far behind am I" answerable without wrap ambiguity.
    std::atomic<uint32_t> slots_[kCapacity]{};
    std::atomic<uint64_t> captureCursor_{0};
    std::atomic<uint64_t> playbackCursor_{0};
    std::atomic<uint32_t> captureCycleCount_{0};
    std::atomic<uint32_t> playbackCycleCount_{0};
    std::atomic<bool> established_{false};
};

} // namespace ASFW::Encoding::Motu
