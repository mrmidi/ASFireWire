//
// MidiRxDrain.hpp
// ASFWDriver
//
// The consumer half of the device -> host rings: bytes off the wire become UMP
// words for CoreMIDI, without ever joining two messages across a gap.
//
// Runs on the MIDI service's own queue, never on the core driver's receive
// queue. Pure logic over the shared block and the byte-stream converter, so the
// interleavings that matter (a gap marked mid-drain, a stream restart under a
// live backlog) are host-tested rather than reasoned about.
//
// The protocol, and why each step is there:
//
//  * Epoch. The block's `streamEpoch` names the stream the bytes belong to. On a
//    change this consumer discards its own backlog (MidiByteRing::DiscardBacklog,
//    which writes only the consumer cursor) and resets its parser. The owner
//    never clears a ring a producer may be writing.
//
//  * Gap. A producer that drops bytes, or a receive path that lost a packet,
//    bumps the ring's discontinuity count and records the write position. The
//    consumer acts on a change of that count BEFORE each peek by discarding
//    everything up to the recorded position and resetting the parser, and acts
//    on it AGAIN after converting and before publishing. The second look is the
//    point: the count used to be sampled once per drain, so a gap marked after
//    the sample let pre-gap bytes and post-gap bytes join into a message nobody
//    sent (reviews/midi-2026-09-14, finding 1). A span that converted across a
//    gap that appeared meanwhile is dropped unsent and re-handled from the top.
//    The cost is a few valid bytes just before a gap; the alternative is a
//    fabricated Note On.
//

#pragma once

#include <cstdint>
#include <span>

#include "../Ump/MidiByteStreamToUmp.hpp"
#include "MidiTransportBlock.hpp"

namespace ASFW::Midi {

struct MidiRxDrainCounters final {
    uint64_t bytesDrained{0};
    uint64_t wordsEmitted{0};
    /// Gaps acted on: the backlog up to the gap was discarded.
    uint64_t gapsHandled{0};
    /// Converted spans dropped unsent because a gap appeared while converting.
    uint64_t spansDroppedForGap{0};
    /// Epoch changes observed (a new stream began under this consumer).
    uint64_t epochChanges{0};
    /// Drains that stopped at the pass cap with bytes still queued.
    uint64_t passCapHits{0};
};

class MidiRxDrain final {
public:
    /// Passes per DrainPort call. A bound, not a target: each pass either moves
    /// the read cursor or handles a gap, so reaching it means a producer that
    /// keeps marking gaps faster than this consumer can act on them.
    static constexpr uint32_t kMaxPassesPerDrain = 32;

    /// Worst case for one pass: every byte of a full ring, at the converter's
    /// per-byte bound.
    static constexpr uint32_t kWordCapacity =
        kMidiRingCapacityBytes * Ump::MidiByteStreamToUmp::kMaxWordsPerByte;

    /// Bring the consumer in line with the block's stream epoch.
    ///
    /// Returns true when the epoch changed. Call before draining; cheap enough
    /// to call on every wake.
    bool ObserveEpoch(MidiTransportBlock& block) noexcept {
        const uint64_t epoch = block.streamEpoch.load(std::memory_order_acquire);
        if (servedEpoch_ == epoch && epochKnown_) return false;
        for (uint32_t port = 0; port < kMidiPortsPerDirection; ++port) {
            auto& ring = block.deviceToHost[port];
            ring.DiscardBacklog();
            fromWire_[port].Reset();
            seenGaps_[port] = ring.discontinuities.load(std::memory_order_acquire);
        }
        servedEpoch_ = epoch;
        epochKnown_ = true;
        ++counters_.epochChanges;
        return true;
    }

    /// Convert everything queued on `port` and hand each run of complete UMP
    /// words to `emit(std::span<const UmpWord>)`.
    template <typename Emit>
    void DrainPort(MidiTransportBlock& block, uint32_t port, Emit&& emit) noexcept {
        DrainPort(block, port, static_cast<Emit&&>(emit), [] {});
    }

    /// As above, with `afterConvert()` run between converting a span and the
    /// check that guards publishing it. Production passes nothing; the host
    /// suite uses it to land a producer's gap in exactly that window.
    template <typename Emit, typename AfterConvert>
    void DrainPort(MidiTransportBlock& block, uint32_t port, Emit&& emit,
                   AfterConvert&& afterConvert) noexcept {
        if (port >= kMidiPortsPerDirection) return;
        if (!block.Usable(servedEpoch_)) return;
        auto& ring = block.deviceToHost[port];

        for (uint32_t pass = 0; pass < kMaxPassesPerDrain; ++pass) {
            const uint64_t gaps = ring.discontinuities.load(std::memory_order_acquire);
            if (gaps != seenGaps_[port]) {
                HandleGap(ring, port, gaps);
            }

            const uint32_t count = ring.Peek(rxBytes_);
            if (count == 0) return;
            const auto pushed = fromWire_[port].Push(
                std::span<const uint8_t>(rxBytes_, count), rxWords_);
            afterConvert();

            // A gap marked after the sample above: what was just converted may
            // span it. Publish nothing from this span; the next pass discards
            // through the gap and resets the parser.
            if (ring.discontinuities.load(std::memory_order_acquire) !=
                seenGaps_[port]) {
                ++counters_.spansDroppedForGap;
                continue;
            }

            ring.Consume(pushed.bytesConsumed);
            counters_.bytesDrained += pushed.bytesConsumed;
            if (pushed.wordsWritten > 0) {
                counters_.wordsEmitted += pushed.wordsWritten;
                emit(std::span<const Ump::UmpWord>(rxWords_, pushed.wordsWritten));
            }
            if (pushed.bytesConsumed == 0) return;
        }
        ++counters_.passCapHits;
    }

    /// Forget all parser state; the next ObserveEpoch re-syncs the cursors.
    void Reset() noexcept {
        for (auto& parser : fromWire_) parser.Reset();
        epochKnown_ = false;
    }

    [[nodiscard]] uint64_t ServedEpoch() const noexcept { return servedEpoch_; }
    [[nodiscard]] const MidiRxDrainCounters& Counters() const noexcept { return counters_; }
    [[nodiscard]] const Ump::MidiByteStreamToUmp& Parser(uint32_t port) const noexcept {
        return fromWire_[port < kMidiPortsPerDirection ? port : 0];
    }

private:
    void HandleGap(MidiByteRing& ring, uint32_t port, uint64_t gaps) noexcept {
        seenGaps_[port] = gaps;
        const uint32_t boundary = ring.discontinuityIndex.load(std::memory_order_acquire);
        const uint32_t read = ring.readIndex.load(std::memory_order_relaxed);
        const uint32_t toGap = boundary - read;
        if (toGap > 0 && toGap <= ring.Available()) {
            ring.Consume(toGap);
        }
        fromWire_[port].Reset();
        ++counters_.gapsHandled;
    }

    Ump::MidiByteStreamToUmp fromWire_[kMidiPortsPerDirection]{};
    uint64_t seenGaps_[kMidiPortsPerDirection]{};
    uint64_t servedEpoch_{0};
    bool epochKnown_{false};
    MidiRxDrainCounters counters_{};

    // Bounded scratch, reused on the MIDI service's queue only.
    uint8_t rxBytes_[kMidiRingCapacityBytes]{};
    Ump::UmpWord rxWords_[kWordCapacity]{};
};

} // namespace ASFW::Midi
