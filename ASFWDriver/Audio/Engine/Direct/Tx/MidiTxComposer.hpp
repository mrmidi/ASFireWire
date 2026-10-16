//
// MidiTxComposer.hpp
// ASFWDriver
//
// Puts host -> device MIDI bytes into the MPX-MIDI slot of packets that are
// already armed, and nowhere else.
//
// Where it fits in main's transmit model. The producer arms packets up to the
// coverage lead (about 126 ms) ahead, each one with every non-PCM slot holding
// the empty MIDI quadlet 0x80000000 -- which is already a correct "no byte this
// block" on the wire. PCM reaches those packets later, once, from the HAL ring,
// and only for packets beyond the finality frontier (the projected hardware
// position plus a guard). MIDI follows the same ownership rule:
//
//   * A packet below `firstWritable` is treated as taken by the hardware and is
//     never touched. This is the only seal there is on this path: the transport
//     reads payload bytes straight from the slab, so there is no second image
//     and no "bind" step that makes a write fatal, but a byte stored after the
//     hardware fetched the packet is simply lost. That is the same exposure PCM
//     has, with the same guard.
//
//   * A packet at or beyond the armed end does not exist yet and is not touched.
//
//   * The composition window is a few completion groups beyond the frontier,
//     NOT the whole armed span. Composing at arm time would put a byte 126 ms
//     behind its enqueue; composing at the frontier puts it a few packets
//     behind. Bytes go into the nearest packets, so a later byte is not queued
//     behind an earlier one for longer than the UART takes to send it anyway.
//
// A byte leaves its ring only when it has been written into a packet at or
// beyond the frontier (reserve, write, commit). A packet that cannot be written
// -- unarmed, wrong geometry, below the frontier -- cancels nothing because it
// reserved nothing: the byte stays queued for a later packet.
//
// Threading: Compose runs on the TX preparation queue, the same queue that arms
// and recycles the timeline slots, so reading a live slot is safe (the timeline
// documents SlotByIndex as pump-side only). The PCM fill runs on the CoreAudio
// IO thread and writes only the PCM slots of its packets; the MIDI slot is a
// different quadlet of the same block, so the two never write the same bytes.
//
// Wire behaviour: Linux write_midi_messages (amdtp-am824.c:295-320) and the
// first-eight-blocks cap (:28,306); see Audio/Wire/AM824/MpxMidiMux.hpp.
//
// Pure logic over the timeline and the byte seam: host-testable with real
// packets built by the production packetizer.
//

#pragma once

#include <atomic>
#include <cstdint>

#include "../../../Wire/AM824/MpxMidiMux.hpp"
#include "../../../Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "../../../../Midi/Transport/MidiTransportBlock.hpp"
#include "../../../../Midi/Transport/MidiTxReservation.hpp"
#include "../../../../Shared/Isoch/AudioTimingGeometry.hpp"

namespace ASFW::AudioEngine::Direct::Tx {

struct MidiTxComposerConfig final {
    /// Where the MPX slot sits in each data block of the host -> device stream.
    ASFW::Encoding::MpxMidiGeometry geometry{};
    /// For the UART model: the stream's rate and its SYT interval.
    uint32_t sampleRateHz{0};
    uint32_t sytIntervalFrames{0};
    /// Packets past the finality frontier that one pass may compose. Two
    /// completion groups: the producer is woken once per group, so a window of
    /// one group would leave the packets between wakes empty, and anything much
    /// larger only adds latency for bytes that arrive after it was written.
    uint32_t windowPackets{2 * ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup};
};

struct MidiTxComposerCounters final {
    uint64_t passes{0};
    uint64_t packetsComposed{0};
    uint64_t bytesComposed{0};
    /// Packets the frontier overtook before any pass reached them. Their MIDI
    /// opportunity is gone; no byte is lost, it waits for a later packet.
    uint64_t packetsPastFrontier{0};
    /// A DATA packet whose slot geometry does not match the MIDI geometry.
    uint64_t geometryMismatches{0};
    /// A pass that found the block not armed for any stream.
    uint64_t inactivePasses{0};
    uint64_t epochChanges{0};
};

class MidiTxComposer final {
public:
    /// CIP header on a CIP-framed packet: two quadlets before the data blocks.
    static constexpr uint32_t kCipHeaderBytes = 8;

    struct PassResult final {
        uint32_t packetsExamined{0};
        uint32_t packetsComposed{0};
        uint32_t bytesComposed{0};
    };

    [[nodiscard]] bool Configure(const MidiTxComposerConfig& config) noexcept {
        if (!config.geometry.Valid() || config.sampleRateHz == 0 ||
            config.sytIntervalFrames == 0 || config.windowPackets == 0) {
            configured_ = false;
            return false;
        }
        config_ = config;
        scope_.ConfigureLimiter(config.sampleRateHz, config.sytIntervalFrames);
        configured_ = true;
        ResetForRun();
        return true;
    }

    [[nodiscard]] bool Configured() const noexcept { return configured_; }

    /// A new TX run begins: packet indices restart from zero and every byte
    /// still reserved or modelled belongs to the run that ended.
    void ResetForRun() noexcept {
        cursor_ = 0;
        servedEpoch_ = 0;
        epochKnown_ = false;
        scope_.Reset();
    }

    /// Compose one pass over [max(cursor, firstWritable), min(armedEnd,
    /// firstWritable + window)).
    PassResult Compose(ASFW::Midi::MidiTransportBlock* block,
                       ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline& timeline,
                       uint64_t firstWritable, uint64_t armedEnd) noexcept {
        PassResult result{};
        if (!configured_ || block == nullptr) return result;
        ++counters_.passes;

        const uint64_t epoch = block->streamEpoch.load(std::memory_order_acquire);
        if (!block->Usable(epoch)) {
            ++counters_.inactivePasses;
            epochKnown_ = false;
            return result;
        }
        if (!epochKnown_ || epoch != servedEpoch_) {
            // A new stream. Whatever was queued while no stream ran, or by the
            // previous stream, is not for this wire; each ring's cursor is moved
            // by its consumer (here), never by the owner.
            for (auto& ring : block->hostToDevice) ring.DiscardBacklog();
            scope_.Reset();
            servedEpoch_ = epoch;
            epochKnown_ = true;
            ++counters_.epochChanges;
        }

        uint64_t from = cursor_;
        if (from < firstWritable) {
            // Count only what a pass could have reached; the cursor starts at
            // zero on every run, so the first pass overtakes the whole prefix.
            if (cursor_ != 0) counters_.packetsPastFrontier += firstWritable - cursor_;
            from = firstWritable;
        }
        const uint64_t windowEnd = firstWritable + config_.windowPackets;
        const uint64_t limit = armedEnd < windowEnd ? armedEnd : windowEnd;

        uint64_t packet = from;
        for (; packet < limit; ++packet) {
            const auto* slot = timeline.SlotByIndex(packet);
            if (slot == nullptr) break;  // not armed (or recycled): stop, retry later

            ++result.packetsExamined;
            if (!slot->isData || slot->packetBytes == nullptr ||
                slot->framesInPacket == 0) {
                continue;  // NO-DATA carries no data blocks, so no MIDI slot
            }
            const uint32_t blocks = slot->framesInPacket;
            const uint32_t dataBytes = blocks * slot->dbs * 4u;
            if (slot->dbs != config_.geometry.dbs ||
                slot->packetSizeBytes < kCipHeaderBytes + dataBytes) {
                ++counters_.geometryMismatches;
                continue;
            }

            ASFW::Midi::MidiPacketReservation reservation{};
            if (!scope_.Begin(*block, epoch, static_cast<uint32_t>(packet), reservation)) {
                break;
            }
            if (reservation.Any()) {
                ASFW::Encoding::MpxMidiPacketBytes bytes{};
                for (uint32_t port = 0; port < ASFW::Encoding::kMpxMidiPorts; ++port) {
                    bytes.hasByteForPort[port] = reservation.hasByteForPort[port];
                    bytes.byteForPort[port] = reservation.byteForPort[port];
                }
                // CIP quadlet 0 carries the DBC in its low byte (big-endian), the
                // value the port rotation counts from.
                const uint8_t dbc = slot->packetBytes[3];
                ASFW::Encoding::WriteMpxMidiSlot(
                    slot->packetBytes + kCipHeaderBytes, blocks, dbc,
                    config_.geometry, bytes);
                for (uint32_t port = 0; port < ASFW::Encoding::kMpxMidiPorts; ++port) {
                    if (bytes.hasByteForPort[port]) ++result.bytesComposed;
                }
                ++result.packetsComposed;
            }
            // The bytes are in the packet image. Retire them from the rings.
            (void)scope_.Commit(*block, reservation);
        }
        cursor_ = packet;

        if (result.packetsComposed != 0) {
            // The slab is read by DMA: publish the stores before the hardware
            // can reach these packets.
            std::atomic_thread_fence(std::memory_order_release);
            counters_.packetsComposed += result.packetsComposed;
            counters_.bytesComposed += result.bytesComposed;
        }
        return result;
    }

    [[nodiscard]] uint64_t Cursor() const noexcept { return cursor_; }
    [[nodiscard]] const MidiTxComposerCounters& Counters() const noexcept { return counters_; }
    [[nodiscard]] const ASFW::Midi::MidiReservationCounters& ReservationCounters() const noexcept {
        return scope_.Counters();
    }

private:
    MidiTxComposerConfig config_{};
    bool configured_{false};
    uint64_t cursor_{0};
    uint64_t servedEpoch_{0};
    bool epochKnown_{false};
    ASFW::Midi::MidiTxReservationScope scope_{};
    MidiTxComposerCounters counters_{};
};

} // namespace ASFW::AudioEngine::Direct::Tx
