//
// MotuMidiTxComposer.hpp
// ASFWDriver
//
// Host -> device MIDI for MOTU streams: the counterpart of MidiTxComposer for a
// stream with no MPX slot. A MOTU data block carries at most one MIDI byte in
// its message bytes (MotuMidi.hpp), and the unit's MIDI output takes one byte
// per MidiDataBlockInterval(rate) + 1 blocks at most.
//
// Ownership is MidiTxComposer's, unchanged: only packets in
// [max(cursor, firstWritable), min(armedEnd, firstWritable + window)) are
// written, a byte leaves its ring only once it is in such a packet (reserve,
// write, commit through MidiTxReservationScope), and a packet that cannot be
// written reserves nothing. The MOTU payload writer writes PCM chunks only,
// from the PCM byte offset on, so the message bytes are never written by both.
//
// Pacing is per data block, as Linux paces it (amdtp-motu.c:211-235). Packets
// the frontier overtook are not counted, so the real spacing on the wire is
// only ever wider than the pacer's.
//
// Pure logic over the timeline and the byte seam: host-testable.
//

#pragma once

#include <atomic>
#include <cstdint>

#include "../../../Wire/MOTU/MotuMidi.hpp"
#include "../../../Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "../../../../Midi/Transport/MidiTransportBlock.hpp"
#include "../../../../Midi/Transport/MidiTxReservation.hpp"
#include "../../../../Shared/Isoch/AudioTimingGeometry.hpp"

namespace ASFW::AudioEngine::Direct::Tx {

struct MotuMidiTxComposerConfig final {
    ASFW::Encoding::Motu::MidiSlotOffsets slot{};
    /// Data block size of the host -> device stream, in quadlets.
    uint32_t dbs{0};
    uint32_t sampleRateHz{0};
    uint32_t framesPerDataPacket{0};
    uint32_t windowPackets{2 * ASFW::IsochTransport::AudioTimingGeometry::kTxPacketsPerGroup};
};

struct MotuMidiTxComposerCounters final {
    uint64_t passes{0};
    uint64_t packetsComposed{0};
    uint64_t bytesComposed{0};
    uint64_t packetsPastFrontier{0};
    uint64_t geometryMismatches{0};
    uint64_t inactivePasses{0};
    uint64_t epochChanges{0};
};

class MotuMidiTxComposer final {
public:
    static constexpr uint32_t kCipHeaderBytes = 8;

    struct PassResult final {
        uint32_t packetsExamined{0};
        uint32_t packetsComposed{0};
        uint32_t bytesComposed{0};
    };

    [[nodiscard]] bool Configure(const MotuMidiTxComposerConfig& config) noexcept {
        const size_t stride = static_cast<size_t>(config.dbs) * 4u;
        if (config.dbs == 0 || stride <= config.slot.flag || stride <= config.slot.byte ||
            config.sampleRateHz == 0 || config.framesPerDataPacket == 0 || config.windowPackets == 0) {
            configured_ = false;
            return false;
        }
        config_ = config;
        scope_.ConfigureLimiter(config.sampleRateHz, config.framesPerDataPacket);
        configured_ = true;
        ResetForRun();
        return true;
    }

    [[nodiscard]] bool Configured() const noexcept { return configured_; }

    void ResetForRun() noexcept {
        cursor_ = 0;
        servedEpoch_ = 0;
        epochKnown_ = false;
        scope_.Reset();
        pacer_.Configure(config_.sampleRateHz);
    }

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
            // A new stream: bytes queued for the old one are not for this wire.
            for (auto& ring : block->hostToDevice) ring.DiscardBacklog();
            scope_.Reset();
            pacer_.Configure(config_.sampleRateHz);
            servedEpoch_ = epoch;
            epochKnown_ = true;
            ++counters_.epochChanges;
        }

        uint64_t from = cursor_;
        if (from < firstWritable) {
            if (cursor_ != 0) counters_.packetsPastFrontier += firstWritable - cursor_;
            from = firstWritable;
        }
        const uint64_t windowEnd = firstWritable + config_.windowPackets;
        const uint64_t limit = armedEnd < windowEnd ? armedEnd : windowEnd;
        const size_t stride = static_cast<size_t>(config_.dbs) * 4u;

        uint64_t packet = from;
        for (; packet < limit; ++packet) {
            const auto* slot = timeline.SlotByIndex(packet);
            if (slot == nullptr) break;  // not armed (or recycled): stop, retry later

            ++result.packetsExamined;
            uint8_t* bytes = slot->packetBytes.load(std::memory_order_relaxed);
            const uint32_t blocks = slot->framesInPacket.load(std::memory_order_relaxed);
            if (!slot->isData.load(std::memory_order_relaxed) || bytes == nullptr || blocks == 0) {
                continue;  // NO-DATA carries no data blocks
            }
            if (slot->dbs.load(std::memory_order_relaxed) != config_.dbs ||
                slot->packetSizeBytes.load(std::memory_order_relaxed) < kCipHeaderBytes + blocks * stride) {
                ++counters_.geometryMismatches;
                continue;
            }

            // Ask for port 0 only when a block of this packet may carry a byte;
            // Begin still ages the cable model for this packet either way.
            ASFW::Midi::MidiPacketReservation reservation{};
            const uint32_t mask = pacer_.HasSlotWithin(blocks) ? 1u : 0u;
            if (!scope_.Begin(*block, epoch, static_cast<uint32_t>(packet), reservation, mask)) break;
            bool taken = false;
            const uint32_t written = pacer_.Compose(
                bytes + kCipHeaderBytes, blocks, stride, config_.slot, [&](uint8_t& out) {
                    if (taken || !reservation.hasByteForPort[0]) return false;
                    out = reservation.byteForPort[0];
                    taken = true;
                    return true;
                });
            if (written != 0) {
                result.bytesComposed += written;
                ++result.packetsComposed;
            }
            (void)scope_.Commit(*block, reservation);
        }
        cursor_ = packet;

        if (result.packetsComposed != 0) {
            // The slab is read by DMA: publish the stores first.
            std::atomic_thread_fence(std::memory_order_release);
            counters_.packetsComposed += result.packetsComposed;
            counters_.bytesComposed += result.bytesComposed;
        }
        return result;
    }

    [[nodiscard]] uint64_t Cursor() const noexcept { return cursor_; }
    [[nodiscard]] const MotuMidiTxComposerCounters& Counters() const noexcept { return counters_; }
    [[nodiscard]] const ASFW::Midi::MidiReservationCounters& ReservationCounters() const noexcept {
        return scope_.Counters();
    }

private:
    MotuMidiTxComposerConfig config_{};
    bool configured_{false};
    uint64_t cursor_{0};
    uint64_t servedEpoch_{0};
    bool epochKnown_{false};
    ASFW::Encoding::Motu::MotuMidiTxPacer pacer_{};
    ASFW::Midi::MidiTxReservationScope scope_{};
    MotuMidiTxComposerCounters counters_{};
};

} // namespace ASFW::AudioEngine::Direct::Tx
