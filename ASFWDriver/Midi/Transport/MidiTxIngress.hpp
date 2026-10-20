//
// MidiTxIngress.hpp
// ASFWDriver
//
// The producer half of the host -> device rings: UMP from CoreMIDI becomes MIDI
// 1.0 bytes in a port's ring, and either a whole message goes in or none does.
//
// This is what MIDIDriverKit's destination I/O block calls. That block runs on
// a real-time thread, so everything here is bounded, lock-free and
// allocation-free. Pure logic over the shared block and the converter, so the
// failure paths are host-tested.
//
// What it guarantees, and the review findings behind each guarantee
// (reviews/midi-2026-09-13, findings 5 and 6):
//
//  * The whole callback is processed. The converter stops when its output
//    scratch is full, and the old caller took that as "done" and returned
//    success for input it had not read (256 Note Ons -> 169 converted, 87
//    silently lost). Here every complete packet is consumed or the call fails.
//
//  * A rejected run never leaves the converter believing it was sent. When the
//    ring refuses a run the SysEx state machine is aborted, so a later Continue
//    or End cannot reach the wire as bare payload or a naked F7 under whatever
//    running status the device had.
//
//  * The inverse is also handled. If the F0 of a SysEx already reached the ring
//    and the run carrying its end was refused, the device is left mid-SysEx. The
//    port remembers that and sends an F7 ahead of the next accepted run, so one
//    dropped message cannot swallow the next one into its payload.
//
//  * A truncated multi-word packet is an error, not a silent success.
//
//  * Every port tracks the epoch it last served on its own, so a stream restart
//    resets that port's converter without one port's real-time thread touching
//    another's state.
//

#pragma once

#include <cstdint>
#include <span>

#include "../Ump/UmpToMidiByteStream.hpp"
#include "MidiTransportBlock.hpp"

namespace ASFW::Midi {

enum class MidiTxStatus : uint8_t {
    kOk = 0,
    /// The block is not armed for any stream (no audio stream is running).
    /// Nothing was queued: bytes written while the wire is down would otherwise
    /// sit in a ring and burst out when a stream finally starts.
    kNotActive,
    kBadPort,
    /// The ring could not take a run; the rest of the callback was abandoned.
    kRingFull,
    /// The input ended in the middle of a multi-word UMP packet.
    kTruncated,
};

struct MidiTxIngressCounters final {
    uint64_t callbacks{0};
    uint64_t bytesQueued{0};
    uint64_t droppedNotActive{0};
    uint64_t droppedRingFull{0};
    uint64_t truncatedInputs{0};
    uint64_t sysExTerminatorsSent{0};
    uint64_t epochResets{0};
};

class MidiTxIngress final {
public:
    /// Bytes converted before they are offered to the ring. One ring's worth: a
    /// run larger than the ring can never be accepted whole anyway.
    static constexpr uint32_t kChunkBytes = kMidiRingCapacityBytes;

    /// Convert `words` for `port` and enqueue the bytes.
    ///
    /// `hostTicks` dates every byte (enqueue time; the destination callback
    /// carries no requested time).
    [[nodiscard]] MidiTxStatus Submit(MidiTransportBlock& block, uint32_t port,
                                      std::span<const Ump::UmpWord> words,
                                      uint64_t hostTicks) noexcept {
        if (port >= kMidiPortsPerDirection) return MidiTxStatus::kBadPort;
        auto& state = ports_[port];
        ++state.counters.callbacks;

        const uint64_t epoch = block.streamEpoch.load(std::memory_order_acquire);
        if (!block.Usable(epoch)) {
            ++state.counters.droppedNotActive;
            // A stopped wire also ends any message that was half-sent.
            state.toWire.Reset();
            state.terminatorOwed = false;
            state.wireSysExOpen = false;
            state.epochKnown = false;
            return MidiTxStatus::kNotActive;
        }
        if (!state.epochKnown || state.servedEpoch != epoch) {
            // A new stream: nothing of the old one's SysEx exists on this wire.
            state.toWire.Reset();
            state.terminatorOwed = false;
            state.wireSysExOpen = false;
            state.servedEpoch = epoch;
            state.epochKnown = true;
            ++state.counters.epochResets;
        }

        auto& ring = block.hostToDevice[port];

        // A previous callback left the device mid-SysEx: close it first, so the
        // message this callback carries is not read as more of that payload.
        if (state.terminatorOwed) {
            const uint8_t eox = kEndOfSysEx;
            if (ring.TryWrite(std::span<const uint8_t>(&eox, 1), hostTicks)) {
                state.terminatorOwed = false;
                state.wireSysExOpen = false;
                ++state.counters.sysExTerminatorsSent;
                ++state.counters.bytesQueued;
            } else {
                ++state.counters.droppedRingFull;
                return MidiTxStatus::kRingFull;
            }
        }

        size_t offset = 0;
        while (offset < words.size()) {
            const auto pulled = state.toWire.Pull(
                words.subspan(offset), std::span<uint8_t>(state.scratch, kChunkBytes));
            if (pulled.wordsConsumed == 0) {
                // Either the input ends mid-packet or the scratch cannot hold
                // even one packet (it always can: kChunkBytes >> 8).
                if (pulled.needsMoreWords) {
                    ++state.counters.truncatedInputs;
                    AbortAndOwe(state);
                    return MidiTxStatus::kTruncated;
                }
                break;
            }
            offset += pulled.wordsConsumed;

            if (pulled.bytesWritten > 0) {
                const std::span<const uint8_t> run(state.scratch, pulled.bytesWritten);
                if (!ring.TryWrite(run, hostTicks)) {
                    ++state.counters.droppedRingFull;
                    AbortAndOwe(state);
                    return MidiTxStatus::kRingFull;
                }
                state.counters.bytesQueued += pulled.bytesWritten;
                TrackSysEx(state, run);
            }
        }
        return MidiTxStatus::kOk;
    }

    [[nodiscard]] const MidiTxIngressCounters& Counters(uint32_t port) const noexcept {
        return ports_[port < kMidiPortsPerDirection ? port : 0].counters;
    }
    [[nodiscard]] bool OwesTerminator(uint32_t port) const noexcept {
        return ports_[port < kMidiPortsPerDirection ? port : 0].terminatorOwed;
    }
    [[nodiscard]] const Ump::UmpToMidiByteStream& Converter(uint32_t port) const noexcept {
        return ports_[port < kMidiPortsPerDirection ? port : 0].toWire;
    }

private:
    static constexpr uint8_t kStartOfSysEx = 0xF0;
    static constexpr uint8_t kEndOfSysEx = 0xF7;

    struct PortState final {
        Ump::UmpToMidiByteStream toWire{};
        uint64_t servedEpoch{0};
        bool epochKnown{false};
        /// An F0 has reached the ring and its F7 has not.
        bool wireSysExOpen{false};
        /// A callback was abandoned while the device was mid-SysEx.
        bool terminatorOwed{false};
        MidiTxIngressCounters counters{};
        uint8_t scratch[kChunkBytes]{};
    };

    static void AbortAndOwe(PortState& state) noexcept {
        state.toWire.AbortSysEx();
        if (state.wireSysExOpen) state.terminatorOwed = true;
    }

    /// Follow what actually reached the ring, not what the converter intended.
    /// Any status byte other than a real-time byte closes a System Exclusive on
    /// the device (MIDI 1.0), and the converter emits F7 itself when a message
    /// interrupts one.
    static void TrackSysEx(PortState& state, std::span<const uint8_t> run) noexcept {
        for (const uint8_t byte : run) {
            if (byte == kStartOfSysEx) {
                state.wireSysExOpen = true;
            } else if (byte >= 0x80 && byte < 0xF8) {
                state.wireSysExOpen = false;
            }
        }
    }

    PortState ports_[kMidiPortsPerDirection]{};
};

} // namespace ASFW::Midi
