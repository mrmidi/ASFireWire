//
// MidiTraceLog.hpp
// ASFWDriver
//
// One place that formats a decoded MIDI message into a log record, so both
// directions print identically and a capture can be read as a single stream.
//
// Separated from MidiMessageTrace.hpp because that header is on the pure side
// of the content boundary and must stay free of logging; this one is the
// driver-side emitter.
//

#pragma once

#include <cstdint>

#include "MidiMessageTrace.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Midi::Trace {

/// Which way the message was going, from the host's point of view.
enum class MidiTraceDirection : uint8_t {
    /// Device -> host: lifted out of a received packet's MPX-MIDI slot.
    kIn,
    /// Host -> device: selected for insertion into a transmit packet.
    kOut,
};

/// Emit one decoded message.
///
/// `wireLabel`/`wireValue` carry the position it occupied, which differs by
/// direction and is the whole reason to log at this layer rather than at the
/// CoreMIDI seam: receive knows the packet's DBC, transmit knows the packet
/// index it is filling.
///
/// Debug level on purpose. Every byte of MIDI produces a record, so at notice
/// this would bury the audio bring-up records next to it; the level menu in the
/// log window is the lever, and a deliberate MIDI session turns it on. Real
/// Time bytes (Clock, Active Sensing) are the reason that matters -- a running
/// clock source emits 24 of them per quarter note.
inline void LogDecodedMidi(MidiTraceDirection direction, uint8_t port,
                           const DecodedMidiMessage& message,
                           const char* wireLabel, uint64_t wireValue,
                           uint8_t block, uint64_t hostTicks,
                           uint32_t leadPackets = 0) noexcept {
    const MidiTraceFields fields = FieldsFor(message);
    const char* const dir = (direction == MidiTraceDirection::kIn) ? "In " : "Out";
    // One FireWire isochronous cycle is 125 us, so the packet lead converts
    // straight to the delay this byte still owes before it reaches the wire.
    const uint32_t leadMicros = leadPackets * 125u;

    // Channel messages carry a channel; system messages do not, and printing
    // "ch=0" for a Clock would be a lie about the protocol. The two data fields
    // are named per message kind, so one shared tail keeps every variant's
    // wire position, lead and status in the same columns.
    if (message.channel != 0) {
        switch (fields.count) {
            case 0:
                ASFW_LOG_LEVELED(
                    Midi, ::ASFW::Logging::LogLevel::Debug,
                    "[Midi%{public}s] port=%u %{public}s ch=%u "
                    "%{public}s=%llu blk=%u lead=%up/%uus status=0x%02x rs=%u "
                    "t=%llu",
                    dir, port, MidiMessageKindName(message.kind),
                    message.channel,
                    wireLabel, wireValue, block, leadPackets, leadMicros,
                    message.status, message.runningStatus ? 1u : 0u, hostTicks);
                return;
            case 1:
                ASFW_LOG_LEVELED(
                    Midi, ::ASFW::Logging::LogLevel::Debug,
                    "[Midi%{public}s] port=%u %{public}s ch=%u %{public}s=%u "
                    "%{public}s=%llu blk=%u lead=%up/%uus status=0x%02x rs=%u "
                    "t=%llu",
                    dir, port, MidiMessageKindName(message.kind),
                    message.channel, fields.name1, fields.value1,
                    wireLabel, wireValue, block, leadPackets, leadMicros,
                    message.status, message.runningStatus ? 1u : 0u, hostTicks);
                return;
            default:
                ASFW_LOG_LEVELED(
                    Midi, ::ASFW::Logging::LogLevel::Debug,
                    "[Midi%{public}s] port=%u %{public}s ch=%u %{public}s=%u "
                    "%{public}s=%u %{public}s=%llu blk=%u lead=%up/%uus "
                    "status=0x%02x rs=%u t=%llu",
                    dir, port, MidiMessageKindName(message.kind),
                    message.channel, fields.name1, fields.value1,
                    fields.name2, fields.value2,
                    wireLabel, wireValue, block, leadPackets, leadMicros,
                    message.status, message.runningStatus ? 1u : 0u, hostTicks);
                return;
        }
    }

    if (fields.count == 0) {
        ASFW_LOG_LEVELED(
            Midi, ::ASFW::Logging::LogLevel::Debug,
            "[Midi%{public}s] port=%u %{public}s %{public}s=%llu blk=%u "
            "lead=%up/%uus status=0x%02x t=%llu",
            dir, port, MidiMessageKindName(message.kind),
            wireLabel, wireValue, block, leadPackets, leadMicros,
            message.status, hostTicks);
        return;
    }
    ASFW_LOG_LEVELED(
        Midi, ::ASFW::Logging::LogLevel::Debug,
        "[Midi%{public}s] port=%u %{public}s %{public}s=%u %{public}s=%llu "
        "blk=%u lead=%up/%uus status=0x%02x t=%llu",
        dir, port, MidiMessageKindName(message.kind),
        fields.name1, fields.value1, wireLabel, wireValue, block,
        leadPackets, leadMicros, message.status, hostTicks);
}

} // namespace ASFW::Midi::Trace
