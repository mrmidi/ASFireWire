//
// MidiTransportBlock.hpp
// ASFWDriver
//
// The byte seam between the MIDI service and the FireWire transport: eight
// single-producer/single-consumer byte rings per direction, laid out in one
// IOBufferMemoryDescriptor the core driver owns.
//
// Why a shared block rather than calls across the seam: the two ends run in
// different IOServices on different queues, and one of them is MIDIDriverKit's
// real-time thread, which may not block. Raw pointers between services on
// different queues is the FW-60 shape, so the only thing that crosses is this
// descriptor, retained by the owner and mapped by each user until that user's
// queue is quiescent.
//
// SPSC is a claim about callers, not a property of this type. It holds only
// while each ring has exactly one producer and one consumer:
//
//   deviceToHost[p]: produced by the core driver's receive queue,
//                    consumed by the MIDI service.
//   hostToDevice[p]: produced by the MIDI service (MIDIIOBlock, RT thread),
//                    consumed by whichever queue owns TX content composition.
//
// Pure layout + logic: no DriverKit dependency, no allocation, no logging, no
// locks. Host-testable, and safe on a real-time thread.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <span>

namespace ASFW::Midi {

/// Ports multiplexed into one MPX-MIDI AM824 slot.
inline constexpr uint32_t kMidiPortsPerDirection = 8;

/// Bytes buffered per port, per direction. Power of two so the index masks.
///
/// MIDI 1.0 runs at 3125 bytes/s, so 1024 bytes is about 328 ms of backlog --
/// far beyond any dispatch stall this driver has recorded (the worst noted is
/// 5.25 ms) and enough for a long SysEx to sit through one. It is a bound, not
/// a target: anything approaching it means the consumer has stopped.
inline constexpr uint32_t kMidiRingCapacityBytes = 1024;
static_assert((kMidiRingCapacityBytes & (kMidiRingCapacityBytes - 1)) == 0,
              "capacity must be a power of two for the index mask");

/// One port's byte ring.
///
/// Indices are free-running and never wrapped by hand; only the byte access is
/// masked. Wrapping the indices themselves would make "full" and "empty"
/// indistinguishable, which is the classic way this structure goes wrong.
/// uint32 free-running at 3125 B/s wraps after about 15 days of continuous
/// traffic, and a wrap is still consistent because only the difference is ever
/// used.
struct MidiByteRing final {
    /// Producer-owned. Published with release so the bytes written before it
    /// are visible to a consumer that acquires it.
    std::atomic<uint32_t> writeIndex{0};
    /// Consumer-owned, same discipline in the other direction.
    std::atomic<uint32_t> readIndex{0};

    /// Bytes the producer could not enqueue because the ring was full.
    /// Producer-owned; a nonzero value means the consumer stalled.
    std::atomic<uint64_t> droppedBytes{0};
    /// Points at which the stream is known to be broken -- an overflow, a
    /// reset, an epoch change. The consumer resets its parser here so bytes
    /// from either side of the gap cannot join into a message nobody sent.
    std::atomic<uint64_t> discontinuities{0};

    uint8_t bytes[kMidiRingCapacityBytes]{};

    [[nodiscard]] uint32_t Available() const noexcept {
        const uint32_t w = writeIndex.load(std::memory_order_acquire);
        const uint32_t r = readIndex.load(std::memory_order_relaxed);
        return w - r;
    }

    [[nodiscard]] uint32_t FreeSpace() const noexcept {
        return kMidiRingCapacityBytes - Available();
    }

    [[nodiscard]] bool Empty() const noexcept { return Available() == 0; }

    /// Producer: enqueue a whole run, or nothing.
    ///
    /// All-or-nothing is the point. A partial write would put half a MIDI
    /// message on the wire, and a device that receives a status byte with only
    /// one of its two data bytes stays desynchronised until the next status
    /// byte -- so a rejected message is strictly better than a truncated one.
    [[nodiscard]] bool TryWrite(std::span<const uint8_t> run) noexcept {
        if (run.empty()) return true;
        if (run.size() > kMidiRingCapacityBytes) {
            droppedBytes.fetch_add(run.size(), std::memory_order_relaxed);
            return false;
        }
        const uint32_t w = writeIndex.load(std::memory_order_relaxed);
        const uint32_t r = readIndex.load(std::memory_order_acquire);
        const uint32_t free = kMidiRingCapacityBytes - (w - r);
        if (run.size() > free) {
            droppedBytes.fetch_add(run.size(), std::memory_order_relaxed);
            MarkDiscontinuity();
            return false;
        }
        for (std::size_t i = 0; i < run.size(); ++i) {
            bytes[(w + static_cast<uint32_t>(i)) & (kMidiRingCapacityBytes - 1)] =
                run[i];
        }
        // Release: the byte stores above must be visible before the consumer
        // can see the index that exposes them.
        writeIndex.store(w + static_cast<uint32_t>(run.size()),
                         std::memory_order_release);
        return true;
    }

    /// Consumer: copy up to `out.size()` bytes without consuming them.
    ///
    /// Peek and Consume are separate so a consumer that may have to abandon
    /// the attempt -- the TX packet path, which can lose its publication race
    /// -- can put the bytes back by simply not consuming.
    [[nodiscard]] uint32_t Peek(std::span<uint8_t> out) const noexcept {
        const uint32_t w = writeIndex.load(std::memory_order_acquire);
        const uint32_t r = readIndex.load(std::memory_order_relaxed);
        uint32_t count = w - r;
        if (count > out.size()) count = static_cast<uint32_t>(out.size());
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = bytes[(r + i) & (kMidiRingCapacityBytes - 1)];
        }
        return count;
    }

    /// Consumer: retire `count` bytes previously peeked.
    ///
    /// Retiring more than is available would move the read index past the
    /// write index and make Available() enormous, so it is clamped rather than
    /// trusted.
    void Consume(uint32_t count) noexcept {
        const uint32_t w = writeIndex.load(std::memory_order_acquire);
        const uint32_t r = readIndex.load(std::memory_order_relaxed);
        const uint32_t available = w - r;
        if (count > available) count = available;
        readIndex.store(r + count, std::memory_order_release);
    }

    /// Record that the byte stream is broken at the current position.
    void MarkDiscontinuity() noexcept {
        discontinuities.fetch_add(1, std::memory_order_relaxed);
    }

    /// Owner-only: return the ring to its initial state.
    ///
    /// Not assignment: the atomics make MidiByteRing non-copyable, and that is
    /// the right default -- a ring in shared memory must never be clobbered
    /// wholesale by a caller that merely has a reference to it. Only the owner
    /// calls this, between streams, with no producer or consumer running.
    void Clear() noexcept {
        writeIndex.store(0, std::memory_order_relaxed);
        readIndex.store(0, std::memory_order_relaxed);
        droppedBytes.store(0, std::memory_order_relaxed);
        discontinuities.store(0, std::memory_order_relaxed);
    }

    /// Drop everything buffered. Called on epoch change and teardown by the
    /// side that owns the cursor being moved.
    void ResetConsumer() noexcept {
        readIndex.store(writeIndex.load(std::memory_order_acquire),
                        std::memory_order_release);
        MarkDiscontinuity();
    }
};

/// The whole seam: both directions, eight ports each.
///
/// Standard layout with no virtuals and no pointers, because it lives in memory
/// mapped into more than one process image -- a pointer stored here would be
/// meaningless on the other side.
struct MidiTransportBlock final {
    /// Bumped when the layout changes, so a stale mapping is detected rather
    /// than silently misread.
    static constexpr uint32_t kLayoutVersion = 1;

    std::atomic<uint32_t> layoutVersion{kLayoutVersion};
    /// Stream epoch this content belongs to. A consumer that sees a different
    /// epoch than it expects must discard, not interpret.
    std::atomic<uint64_t> streamEpoch{0};
    /// Set by the owner when the rings must not be used at all, so a mapping
    /// that outlives its stream cannot be mistaken for a live one.
    std::atomic<uint32_t> active{0};

    MidiByteRing deviceToHost[kMidiPortsPerDirection]{};
    MidiByteRing hostToDevice[kMidiPortsPerDirection]{};

    [[nodiscard]] bool Usable(uint64_t expectedEpoch) const noexcept {
        return active.load(std::memory_order_acquire) != 0 &&
               layoutVersion.load(std::memory_order_relaxed) == kLayoutVersion &&
               streamEpoch.load(std::memory_order_acquire) == expectedEpoch;
    }

    /// Owner: arm for a new stream. Clears every ring and publishes the epoch
    /// before marking the block active, so no consumer can observe live rings
    /// carrying the previous stream's bytes.
    void Arm(uint64_t epoch) noexcept {
        active.store(0, std::memory_order_release);
        for (auto& ring : deviceToHost) ring.Clear();
        for (auto& ring : hostToDevice) ring.Clear();
        layoutVersion.store(kLayoutVersion, std::memory_order_relaxed);
        streamEpoch.store(epoch, std::memory_order_release);
        active.store(1, std::memory_order_release);
    }

    /// Owner: stop the rings being used. Does not free anything -- the mapping
    /// stays valid until every user's queue is quiescent.
    void Quiesce() noexcept {
        active.store(0, std::memory_order_release);
    }
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Midi
