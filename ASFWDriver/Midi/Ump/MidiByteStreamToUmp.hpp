//
// MidiByteStreamToUmp.hpp
// ASFWDriver
//
// MIDI 1.0 byte stream -> Universal MIDI Packet, one instance per port.
//
// The wire carries a MIDI 1.0 byte stream; MIDIDriverKit's IOUserMIDISource
// accepts UMP words only. This is the receive half of that bridge.
//
// Pure logic: no DriverKit dependency, no allocation, no locking, no logging.
// Safe to call from a receive queue or an RT callback.
//

#pragma once

#include <cstdint>
#include <span>

#include "UmpTypes.hpp"

namespace ASFW::Midi::Ump {

/// Converts one port's MIDI 1.0 byte stream into UMP words.
///
/// Stateful across calls: running status, a partially received Channel Voice or
/// System Common message, and an in-progress System Exclusive all persist until
/// completed or until Reset() is called.
///
/// A single AM824 quadlet can deliver up to three bytes, so Push() takes a run
/// rather than assuming one byte per call.
class MidiByteStreamToUmp final {
public:
    /// Upper bound on UMP words a single input byte can produce.
    ///
    /// Only a SysEx packet flush emits two words; every other completion emits
    /// one. A caller sizing its output span at kMaxWordsPerByte * bytes can
    /// never be asked to call again.
    static constexpr uint32_t kMaxWordsPerByte = 2;

    struct Counters final {
        uint64_t bytesConsumed{0};
        uint64_t wordsEmitted{0};
        /// Data byte arriving with no status and no running status to inherit.
        uint64_t orphanDataBytes{0};
        /// Status byte that this converter does not forward (0xF4, 0xF5, and a
        /// standalone 0xF7 outside a System Exclusive).
        uint64_t unsupportedStatusBytes{0};
        /// A System Exclusive closed by something other than 0xF7. Emitted as
        /// End so the client's SysEx state cannot hang open; see the note on
        /// TerminateSysEx().
        uint64_t sysExTruncated{0};
        /// Reset() calls that discarded partial state.
        uint64_t partialMessagesDiscarded{0};
    };

    struct PushResult final {
        /// Input bytes actually consumed. Less than the input size only when
        /// the output span ran out of room; call again with the remainder.
        uint32_t bytesConsumed{0};
        uint32_t wordsWritten{0};
    };

    constexpr MidiByteStreamToUmp() noexcept = default;
    explicit constexpr MidiByteStreamToUmp(uint8_t group) noexcept
        : group_(group & kMaxGroup) {}

    /// Feed a run of MIDI 1.0 bytes, appending complete UMP messages to `out`.
    ///
    /// Consumes bytes only while at least kMaxWordsPerByte words remain free,
    /// so a message is never split across calls.
    [[nodiscard]] PushResult Push(std::span<const uint8_t> bytes,
                                  std::span<UmpWord> out) noexcept;

    /// Discard every partially received message and clear running status.
    ///
    /// Call this on stream loss, ring overflow, epoch change and teardown. It
    /// is what stops bytes from either side of a gap joining into a message
    /// that was never sent. It emits nothing: a partial message is dropped, not
    /// completed.
    void Reset() noexcept;

    [[nodiscard]] const Counters& GetCounters() const noexcept { return counters_; }
    [[nodiscard]] uint8_t Group() const noexcept { return group_; }

    /// True while a System Exclusive is open. Diagnostic only.
    [[nodiscard]] bool InSysEx() const noexcept { return sysExActive_; }

private:
    /// Emit a SysEx7 packet holding the currently buffered payload.
    void FlushSysEx(SysExStatus status, std::span<UmpWord> out,
                    uint32_t& written) noexcept;

    /// Close an open System Exclusive that did not end with 0xF7.
    ///
    /// UMP's SysEx7 status set has no abort code, so the choice is between
    /// emitting End -- which tells the client a message completed that did not
    /// -- and dropping the payload, which leaves the client's SysEx open
    /// forever. We emit End and count it: a stuck client is the worse failure,
    /// and sysExTruncated makes the event observable rather than silent.
    void TerminateSysEx(std::span<UmpWord> out, uint32_t& written) noexcept;

    void HandleStatusByte(uint8_t byte, std::span<UmpWord> out,
                          uint32_t& written) noexcept;
    void HandleDataByte(uint8_t byte, std::span<UmpWord> out,
                        uint32_t& written) noexcept;

    uint8_t group_{0};

    /// Status byte of the message currently being assembled, or 0 for none.
    uint8_t pendingStatus_{0};
    /// Data bytes still needed to complete pendingStatus_.
    uint8_t pendingRemaining_{0};
    /// Data bytes collected so far for pendingStatus_.
    uint8_t pendingData_[2]{0, 0};
    uint8_t pendingCount_{0};

    /// Running status: the last Channel Voice status byte seen. Cleared by any
    /// System Common byte, never by System Real Time.
    uint8_t runningStatus_{0};

    bool sysExActive_{false};
    /// True once a Start or Continue has been emitted for the open message, so
    /// its final packet must be End rather than Complete.
    bool sysExAnyEmitted_{false};
    uint8_t sysExBuf_[kMaxSysEx7PayloadBytes]{};
    uint8_t sysExCount_{0};

    Counters counters_{};
};

} // namespace ASFW::Midi::Ump
