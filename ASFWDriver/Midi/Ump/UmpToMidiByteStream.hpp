//
// UmpToMidiByteStream.hpp
// ASFWDriver
//
// Universal MIDI Packet -> MIDI 1.0 byte stream, one instance per port.
//
// MIDIDriverKit's IOUserMIDIDestination delivers UMP words; the wire carries a
// MIDI 1.0 byte stream. This is the transmit half of that bridge.
//
// Pure logic: no DriverKit dependency, no allocation, no locking, no logging.
// MIDIIOBlock runs on the framework's real-time thread, so nothing here may
// block.
//

#pragma once

#include <cstdint>
#include <span>

#include "UmpTypes.hpp"

namespace ASFW::Midi::Ump {

/// Converts one port's UMP stream into MIDI 1.0 bytes.
///
/// Running status is never used: every Channel Voice message is emitted with
/// its full status byte. Compression saves at most one byte per message on a
/// link that is already an order of magnitude faster than MIDI 1.0, and costs
/// correctness against devices with imperfect running-status handling.
class UmpToMidiByteStream final {
public:
    /// Upper bound on bytes a single UMP packet can produce: 0xF0, six payload
    /// bytes and 0xF7 for a one-packet System Exclusive.
    static constexpr uint32_t kMaxBytesPerPacket = 8;

    struct Counters final {
        uint64_t wordsConsumed{0};
        uint64_t bytesEmitted{0};
        /// Packet whose message type this converter does not translate
        /// (MIDI 2.0 Channel Voice, Data128, Flex Data, UMP Stream, and the
        /// undefined types). Skipped by its exact packet length.
        uint64_t unsupportedMessageType{0};
        /// Utility packet (MT 0x0). Transport metadata with no DIN byte
        /// equivalent: counted and skipped, never translated.
        uint64_t utilityPacketsIgnored{0};
        /// Packet addressed to a different UMP group.
        uint64_t foreignGroupPackets{0};
        /// Packet rejected because a field was out of range: a data byte with
        /// bit 7 set, a SysEx7 length above six, or a status this converter
        /// does not accept. Emitting such a packet would inject a spurious
        /// status byte into the device's byte stream.
        uint64_t malformedPackets{0};
        /// SysEx7 Continue or End arriving with no System Exclusive open.
        uint64_t sysExOutOfSequence{0};
        /// SysEx7 Start or Complete arriving while one is already open.
        uint64_t sysExInterrupted{0};
    };

    struct PullResult final {
        /// UMP words actually consumed. Less than the input size when the
        /// output span ran out of room or the input ends mid-packet; call
        /// again with the remainder.
        uint32_t wordsConsumed{0};
        uint32_t bytesWritten{0};
        /// True when the input ended in the middle of a multi-word packet.
        /// The caller must preserve the unconsumed words and supply the rest.
        bool needsMoreWords{false};
    };

    explicit constexpr UmpToMidiByteStream(uint8_t group = 0) noexcept
        : group_(group & kMaxGroup) {}

    /// Translate whole UMP packets from `words`, appending bytes to `out`.
    ///
    /// A packet is consumed only if it is complete in `words` and its byte
    /// expansion fits entirely in `out`, so a message is never half-emitted.
    [[nodiscard]] PullResult Pull(std::span<const UmpWord> words,
                                  std::span<uint8_t> out) noexcept;

    /// Drop any open System Exclusive state.
    ///
    /// Call on stream loss, epoch change and teardown. It emits nothing: a
    /// trailing 0xF7 would claim a message completed that did not.
    void Reset() noexcept;

    [[nodiscard]] const Counters& GetCounters() const noexcept { return counters_; }
    [[nodiscard]] uint8_t Group() const noexcept { return group_; }

    /// True while a System Exclusive is open. Diagnostic only.
    [[nodiscard]] bool InSysEx() const noexcept { return sysExActive_; }

private:
    /// Translate one complete, space-checked packet, appending its bytes to
    /// `out` at `written` and updating SysEx state and counters.
    ///
    /// Pull() reserves kMaxBytesPerPacket before calling, so this never has to
    /// measure first and can never half-write a message.
    void TranslatePacket(uint8_t nibble, UmpWord word0, UmpWord word1,
                         std::span<uint8_t> out, uint32_t& written) noexcept;

    void TranslateSysEx7(UmpWord word0, UmpWord word1,
                         std::span<uint8_t> out, uint32_t& written) noexcept;

    uint8_t group_{0};
    bool sysExActive_{false};
    Counters counters_{};
};

} // namespace ASFW::Midi::Ump
