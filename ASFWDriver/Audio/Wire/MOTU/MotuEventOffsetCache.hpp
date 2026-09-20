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
// Concurrency contract:
// - Capture and playback operate on separate queues.
// - Cursors and epochs are packed into 64-bit atomic positions: bits [63:32] hold epoch,
//   bits [31:0] hold the monotonic 32-bit slot cursor.
// - An atomic commit protocol via CAS eliminates TOCTOU races on Reset().
// - Per-slot odd/even sequence counters validate that Take() copies coherent runs
//   without concurrent write tears.

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuSph.hpp"
#include "MotuTxTiming.hpp"

#include <atomic>
#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

enum class OffsetCacheTakeResult : uint8_t {
    Success = 0,
    SuccessOverrunResync,
    SuccessUnderrunResync,
    NotEstablished,
    NotEnoughHistory,
    ConcurrentReset,
    ConcurrentRewrite,
    InvalidRequest,
};

class MotuEventOffsetCache final {
public:
    /// 512 packets of 8 data blocks: the same history depth RxSequenceReplayState keeps,
    /// expressed per block. A power of two so the modulo folds to a mask.
    static constexpr uint32_t kCapacity = 4096;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "kCapacity must be a power of two");

    /// Offsets are ticks on the 24.576 MHz timeline, always < kTicksPerSecond.
    static constexpr uint32_t kNoOffset = UINT32_MAX;

    static constexpr uint64_t Pack(uint32_t epoch, uint32_t cursor) noexcept {
        return (static_cast<uint64_t>(epoch) << 32) | static_cast<uint64_t>(cursor);
    }
    static constexpr uint32_t UnpackEpoch(uint64_t packed) noexcept {
        return static_cast<uint32_t>(packed >> 32);
    }
    static constexpr uint32_t UnpackCursor(uint64_t packed) noexcept {
        return static_cast<uint32_t>(packed & 0xFFFFFFFFU);
    }

    /// Reset cursors and increment the epoch to invalidate any in-flight capture or take.
    void Reset(uint32_t explicitEpoch = 0) noexcept {
        epochTransitionSequence_.fetch_add(1, std::memory_order_acq_rel);
        uint32_t nextEpoch = explicitEpoch;
        if (nextEpoch == 0) {
            nextEpoch = UnpackEpoch(capturePosition_.load(std::memory_order_relaxed)) + 1U;
            if (nextEpoch == 0) {
                nextEpoch = 1U;
            }
        }
        capturePosition_.store(Pack(nextEpoch, 0U), std::memory_order_release);
        playbackPosition_.store(Pack(nextEpoch, 0U), std::memory_order_release);
        established_.store(false, std::memory_order_release);
        epochTransitionSequence_.fetch_add(1, std::memory_order_release);
    }

    /// Cache one offset per data block from a received packet.
    ///
    /// `payload` is the whole isochronous payload including the CIP header. Returns the
    /// number of blocks cached, which is short of `dataBlocks` only when the payload
    /// cannot hold them -- a truncated packet must not contribute invented timing.
    ///
    /// Mirrors cache_event_offsets (amdtp-motu.c:303-329): each offset is the block's
    /// SPH tick minus the whole-cycle base of the cycle the packet was received in.
    ///
    /// `receiveCycle` is that cycle, taken from the packet's own receive timestamp.
    uint32_t Capture(std::span<const uint8_t> payload,
                     uint32_t dbs,
                     uint32_t dataBlocks,
                     uint32_t receiveCycle,
                     uint32_t cipHeaderBytes = 8U) noexcept {
        if (dbs == 0 || dataBlocks == 0) {
            return 0;
        }

        const uint64_t transBefore = epochTransitionSequence_.load(std::memory_order_acquire);
        if ((transBefore & 1U) != 0U) {
            return 0; // Transition in progress
        }

        uint64_t capPos = capturePosition_.load(std::memory_order_acquire);
        const uint32_t epoch = UnpackEpoch(capPos);
        const uint32_t cursor = UnpackCursor(capPos);

        const uint32_t baseTick = BaseTickForCycle(receiveCycle);
        const uint64_t blockBytes = static_cast<uint64_t>(dbs) * 4ULL;

        uint32_t cached = 0;
        for (uint32_t block = 0; block < dataBlocks; ++block) {
            const uint64_t blockStart =
                static_cast<uint64_t>(cipHeaderBytes) + static_cast<uint64_t>(block) * blockBytes;
            if (blockStart + 4ULL > payload.size()) {
                break;
            }
            const uint32_t sph = ReadSph(payload.subspan(static_cast<size_t>(blockStart), 4));
            const uint32_t slotIdx = (cursor + block) & (kCapacity - 1);

            const uint32_t oldSeq = slotSequences_[slotIdx].load(std::memory_order_relaxed);
            const uint32_t writingSeq = oldSeq | 1U;
            slotSequences_[slotIdx].store(writingSeq, std::memory_order_release);
            slots_[slotIdx].store(TickOffsetFromBase(sph, baseTick), std::memory_order_relaxed);
            slotSequences_[slotIdx].store(writingSeq + 1U, std::memory_order_release);
            ++cached;
        }

        if (cached == 0) {
            return 0;
        }

        if (epochTransitionSequence_.load(std::memory_order_acquire) != transBefore) {
            return 0; // Reset occurred while capturing
        }

        const uint64_t desiredPos = Pack(epoch, cursor + cached);
        if (!capturePosition_.compare_exchange_strong(capPos, desiredPos,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed)) {
            // Concurrent reset clobbered our epoch; discard pre-reset work.
            return 0;
        }

        established_.store(true, std::memory_order_release);
        return cached;
    }

    /// Fill `out` with the next `out.size()` cached offsets and advance the playback
    /// cursor past them.
    ///
    /// Returns false without consuming anything when fewer are available or when timing
    /// has not been established. A partial fill is never returned.
    ///
    /// On overrun (playback fell behind ring capacity) or underrun (playback caught up
    /// with capture), resyncs to the newest complete run.
    [[nodiscard]] bool Take(std::span<uint32_t> out,
                            OffsetCacheTakeResult* outResult = nullptr) noexcept {
        auto setResult = [outResult](OffsetCacheTakeResult r) {
            if (outResult) {
                *outResult = r;
            }
        };

        if (out.empty() || out.size() > kCapacity) {
            setResult(OffsetCacheTakeResult::InvalidRequest);
            return false;
        }
        if (!IsEstablished()) {
            setResult(OffsetCacheTakeResult::NotEstablished);
            return false;
        }

        constexpr uint32_t kMaxAttempts = 3;
        for (uint32_t attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const uint64_t transBefore = epochTransitionSequence_.load(std::memory_order_acquire);
            if ((transBefore & 1U) != 0U) {
                continue;
            }

            const uint64_t capPos = capturePosition_.load(std::memory_order_acquire);
            uint64_t playPos = playbackPosition_.load(std::memory_order_acquire);

            const uint32_t capEpoch = UnpackEpoch(capPos);
            const uint32_t playEpoch = UnpackEpoch(playPos);
            if (capEpoch != playEpoch) {
                setResult(OffsetCacheTakeResult::ConcurrentReset);
                return false;
            }

            const uint32_t producer = UnpackCursor(capPos);
            const uint32_t cursor = UnpackCursor(playPos);
            const uint32_t requested = static_cast<uint32_t>(out.size());

            // Check if capture has ever received enough blocks to fill one run.
            if (producer < requested) {
                setResult(OffsetCacheTakeResult::NotEnoughHistory);
                return false;
            }

            uint32_t start = cursor;
            OffsetCacheTakeResult result = OffsetCacheTakeResult::Success;

            const int32_t diff = static_cast<int32_t>(producer - cursor);
            if (diff < static_cast<int32_t>(requested)) {
                // Underrun: playback caught up with capture.
                // Resync to newest complete run (marked unverified on MOTU hardware).
                start = producer - requested;
                result = OffsetCacheTakeResult::SuccessUnderrunResync;
            } else if (diff > static_cast<int32_t>(kCapacity)) {
                // Overrun: playback fell behind the ring window.
                // Resync to newest complete run.
                start = producer - requested;
                result = OffsetCacheTakeResult::SuccessOverrunResync;
            }

            // Copy with coherent run sequence check.
            bool stable = true;
            for (uint32_t i = 0; i < requested; ++i) {
                const uint32_t slotIdx = (start + i) & (kCapacity - 1);
                const uint32_t seqBefore = slotSequences_[slotIdx].load(std::memory_order_acquire);
                if ((seqBefore & 1U) != 0U) {
                    stable = false;
                    break;
                }
                out[i] = slots_[slotIdx].load(std::memory_order_relaxed);
                const uint32_t seqAfter = slotSequences_[slotIdx].load(std::memory_order_acquire);
                if (seqBefore != seqAfter) {
                    stable = false;
                    break;
                }
            }

            if (!stable) {
                continue; // Retry copy
            }

            if (epochTransitionSequence_.load(std::memory_order_acquire) != transBefore) {
                continue; // Reset intervened
            }

            const uint64_t desiredPlayPos = Pack(playEpoch, start + requested);
            if (!playbackPosition_.compare_exchange_strong(playPos, desiredPlayPos,
                                                           std::memory_order_release,
                                                           std::memory_order_relaxed)) {
                // Concurrent reset or take intervened; retry
                continue;
            }

            setResult(result);
            return true;
        }

        setResult(OffsetCacheTakeResult::ConcurrentRewrite);
        return false;
    }

    /// True once at least one block has been captured, so the transmit side knows the
    /// device's timing has actually been observed rather than assumed.
    [[nodiscard]] bool IsEstablished() const noexcept {
        return established_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t Available() const noexcept {
        const uint64_t capPos = capturePosition_.load(std::memory_order_acquire);
        const uint64_t playPos = playbackPosition_.load(std::memory_order_relaxed);
        if (UnpackEpoch(capPos) != UnpackEpoch(playPos)) {
            return 0ULL;
        }
        const uint32_t producer = UnpackCursor(capPos);
        const uint32_t cursor = UnpackCursor(playPos);
        const int32_t diff = static_cast<int32_t>(producer - cursor);
        return (diff > 0) ? static_cast<uint64_t>(diff) : 0ULL;
    }

    [[nodiscard]] uint64_t CaptureCursor() const noexcept {
        return UnpackCursor(capturePosition_.load(std::memory_order_acquire));
    }
    [[nodiscard]] uint64_t PlaybackCursor() const noexcept {
        return UnpackCursor(playbackPosition_.load(std::memory_order_relaxed));
    }
    [[nodiscard]] uint32_t Epoch() const noexcept {
        return UnpackEpoch(capturePosition_.load(std::memory_order_acquire));
    }

private:
    std::atomic<uint32_t> slots_[kCapacity]{};
    std::atomic<uint32_t> slotSequences_[kCapacity]{};
    std::atomic<uint64_t> capturePosition_{Pack(1U, 0U)};
    std::atomic<uint64_t> playbackPosition_{Pack(1U, 0U)};
    std::atomic<uint64_t> epochTransitionSequence_{0U};
    std::atomic<bool> established_{false};
};

} // namespace ASFW::Encoding::Motu
