//
// MidiMessageTrace.hpp
// ASFWDriver
//
// MIDI 1.0 byte stream -> decoded message, for tracing only.
//
// One instance per port per direction. It exists so a human can read what
// actually crossed the wire: the byte stream carries no message boundaries, so
// a log of raw bytes is unreadable and a log of ring writes says nothing about
// what was played.
//
// This is deliberately NOT MidiByteStreamToUmp. That converter is on the data
// path and its output is UMP words for CoreMIDI; re-deriving a note number from
// a UMP word to print it would make the log a statement about the converter
// rather than about the wire. This reads the same bytes independently.
//
// Pure logic: no DriverKit dependency, no allocation, no locking, no logging.
//

#pragma once

#include <cstdint>

namespace ASFW::Midi::Trace {

enum class MidiMessageKind : uint8_t {
    kNone,
    kNoteOff,
    kNoteOn,
    kPolyAftertouch,
    kControlChange,
    kProgramChange,
    kChannelAftertouch,
    kPitchBend,
    kSysExStart,
    kSysExEnd,
    kTimeCodeQuarterFrame,
    kSongPosition,
    kSongSelect,
    kTuneRequest,
    kClock,
    kStart,
    kContinue,
    kStop,
    kActiveSensing,
    kSystemReset,
    kUnknown,
};

[[nodiscard]] constexpr const char* MidiMessageKindName(MidiMessageKind kind) noexcept {
    switch (kind) {
        case MidiMessageKind::kNoteOff:            return "NoteOff";
        case MidiMessageKind::kNoteOn:             return "NoteOn";
        case MidiMessageKind::kPolyAftertouch:     return "PolyAT";
        case MidiMessageKind::kControlChange:      return "CC";
        case MidiMessageKind::kProgramChange:      return "Program";
        case MidiMessageKind::kChannelAftertouch:  return "ChanAT";
        case MidiMessageKind::kPitchBend:          return "PitchBend";
        case MidiMessageKind::kSysExStart:         return "SysExStart";
        case MidiMessageKind::kSysExEnd:           return "SysExEnd";
        case MidiMessageKind::kTimeCodeQuarterFrame: return "MTCQuarter";
        case MidiMessageKind::kSongPosition:       return "SongPosition";
        case MidiMessageKind::kSongSelect:         return "SongSelect";
        case MidiMessageKind::kTuneRequest:        return "TuneRequest";
        case MidiMessageKind::kClock:              return "Clock";
        case MidiMessageKind::kStart:              return "Start";
        case MidiMessageKind::kContinue:           return "Continue";
        case MidiMessageKind::kStop:               return "Stop";
        case MidiMessageKind::kActiveSensing:      return "ActiveSensing";
        case MidiMessageKind::kSystemReset:        return "SystemReset";
        case MidiMessageKind::kUnknown:            return "Unknown";
        case MidiMessageKind::kNone:               break;
    }
    return "None";
}

/// One completed message, already decoded into the fields worth printing.
struct DecodedMidiMessage final {
    MidiMessageKind kind{MidiMessageKind::kNone};
    uint8_t status{0};
    /// 1..16 for channel messages, 0 for system messages.
    uint8_t channel{0};
    uint8_t data1{0};
    uint8_t data2{0};
    /// Pitch bend as a 14-bit value; 8192 is centre. Zero for everything else.
    uint16_t value14{0};
    /// Data bytes accumulated in a System Exclusive, reported at its end.
    uint32_t sysExBytes{0};
    /// True when this arrived as a data byte under an inherited running status.
    bool runningStatus{false};
};

/// Assembles one port's MIDI 1.0 byte stream into complete messages.
///
/// System Real Time bytes (0xF8..0xFF) are emitted immediately and, per the
/// spec, do not disturb a message in progress or running status -- including
/// inside a System Exclusive, where a synthesiser is required to tolerate them.
class MidiMessageTrace final {
public:
    /// Feed one byte. Returns true when `out` holds a completed message.
    [[nodiscard]] bool Feed(uint8_t byte, DecodedMidiMessage& out) noexcept {
        if (byte >= 0xF8u) {
            out = DecodedMidiMessage{};
            out.status = byte;
            out.kind = RealTimeKind(byte);
            return true;
        }

        if (byte >= 0x80u) {
            // A status byte ends any System Exclusive, whether or not it is the
            // 0xF7 that should have.
            if (sysExActive_) {
                sysExActive_ = false;
                const uint32_t bytes = sysExBytes_;
                sysExBytes_ = 0;
                pending_ = 0;
                expected_ = 0;
                received_ = 0;
                if (byte == 0xF7u) {
                    out = DecodedMidiMessage{};
                    out.status = 0xF7u;
                    out.kind = MidiMessageKind::kSysExEnd;
                    out.sysExBytes = bytes;
                    return true;
                }
                // Fall through: the new status still has to be processed, but
                // the truncated SysEx is reported first on the next Feed. Report
                // the SysEx now and stash the status byte.
                deferredStatus_ = byte;
                hasDeferredStatus_ = true;
                out = DecodedMidiMessage{};
                out.status = 0xF7u;
                out.kind = MidiMessageKind::kSysExEnd;
                out.sysExBytes = bytes;
                return true;
            }
            return BeginStatus(byte, out);
        }

        // Data byte.
        if (sysExActive_) {
            ++sysExBytes_;
            return false;
        }
        if (expected_ == 0) {
            if (runningStatus_ == 0) {
                ++orphanDataBytes_;
                return false;
            }
            // Inherit running status and start a fresh message with this byte.
            pending_ = runningStatus_;
            expected_ = ExpectedDataBytes(runningStatus_);
            received_ = 0;
            inherited_ = true;
        }
        data_[received_ < 2 ? received_ : 1] = byte;
        ++received_;
        if (received_ < expected_) {
            return false;
        }
        Complete(out);
        return true;
    }

    /// Drain a status byte that arrived while a System Exclusive was open.
    ///
    /// Feed() reports the truncated SysEx first and keeps the status byte; call
    /// this immediately after a kSysExEnd to process it, exactly as Feed would
    /// have. Returns true when it in turn completed a message.
    [[nodiscard]] bool TakeDeferred(DecodedMidiMessage& out) noexcept {
        if (!hasDeferredStatus_) return false;
        hasDeferredStatus_ = false;
        const uint8_t status = deferredStatus_;
        deferredStatus_ = 0;
        return BeginStatus(status, out);
    }

    [[nodiscard]] bool HasDeferred() const noexcept { return hasDeferredStatus_; }

    /// Drop every partial message and clear running status.
    ///
    /// Call on stream loss, ring overflow, epoch change and teardown, so bytes
    /// from either side of a gap cannot join into a message nobody sent.
    void Reset() noexcept {
        pending_ = 0;
        expected_ = 0;
        received_ = 0;
        runningStatus_ = 0;
        sysExActive_ = false;
        sysExBytes_ = 0;
        hasDeferredStatus_ = false;
        deferredStatus_ = 0;
        inherited_ = false;
    }

    [[nodiscard]] uint64_t OrphanDataBytes() const noexcept { return orphanDataBytes_; }
    [[nodiscard]] bool InSysEx() const noexcept { return sysExActive_; }

private:
    [[nodiscard]] bool BeginStatus(uint8_t status, DecodedMidiMessage& out) noexcept {
        if (status == 0xF0u) {
            sysExActive_ = true;
            sysExBytes_ = 0;
            runningStatus_ = 0;
            expected_ = 0;
            received_ = 0;
            out = DecodedMidiMessage{};
            out.status = status;
            out.kind = MidiMessageKind::kSysExStart;
            return true;
        }
        if (status == 0xF7u) {
            // End with no System Exclusive open: nothing to report.
            runningStatus_ = 0;
            expected_ = 0;
            received_ = 0;
            return false;
        }

        pending_ = status;
        expected_ = ExpectedDataBytes(status);
        received_ = 0;
        inherited_ = false;
        // Only Channel Voice messages establish running status; System Common
        // clears it (MIDI 1.0 spec).
        runningStatus_ = (status < 0xF0u) ? status : 0;

        if (expected_ == 0) {
            Complete(out);
            return true;
        }
        return false;
    }

    void Complete(DecodedMidiMessage& out) noexcept {
        out = DecodedMidiMessage{};
        out.status = pending_;
        out.runningStatus = inherited_;
        const uint8_t high = static_cast<uint8_t>(pending_ & 0xF0u);
        if (pending_ < 0xF0u) {
            out.channel = static_cast<uint8_t>((pending_ & 0x0Fu) + 1u);
            out.data1 = data_[0];
            out.data2 = data_[1];
            switch (high) {
                case 0x80u: out.kind = MidiMessageKind::kNoteOff; break;
                case 0x90u:
                    // Velocity zero is a Note Off by every convention that
                    // matters; printing it as a Note On reads as a stuck note.
                    out.kind = (data_[1] == 0) ? MidiMessageKind::kNoteOff
                                               : MidiMessageKind::kNoteOn;
                    break;
                case 0xA0u: out.kind = MidiMessageKind::kPolyAftertouch; break;
                case 0xB0u: out.kind = MidiMessageKind::kControlChange; break;
                case 0xC0u: out.kind = MidiMessageKind::kProgramChange; break;
                case 0xD0u: out.kind = MidiMessageKind::kChannelAftertouch; break;
                case 0xE0u:
                    out.kind = MidiMessageKind::kPitchBend;
                    out.value14 = static_cast<uint16_t>(
                        (static_cast<uint16_t>(data_[1] & 0x7Fu) << 7) |
                        (data_[0] & 0x7Fu));
                    break;
                default: out.kind = MidiMessageKind::kUnknown; break;
            }
        } else {
            out.data1 = data_[0];
            out.data2 = data_[1];
            switch (pending_) {
                case 0xF1u: out.kind = MidiMessageKind::kTimeCodeQuarterFrame; break;
                case 0xF2u:
                    out.kind = MidiMessageKind::kSongPosition;
                    out.value14 = static_cast<uint16_t>(
                        (static_cast<uint16_t>(data_[1] & 0x7Fu) << 7) |
                        (data_[0] & 0x7Fu));
                    break;
                case 0xF3u: out.kind = MidiMessageKind::kSongSelect; break;
                case 0xF6u: out.kind = MidiMessageKind::kTuneRequest; break;
                default:    out.kind = MidiMessageKind::kUnknown; break;
            }
        }
        expected_ = 0;
        received_ = 0;
        inherited_ = false;
    }

    [[nodiscard]] static constexpr uint8_t ExpectedDataBytes(uint8_t status) noexcept {
        if (status < 0x80u) return 0;
        if (status < 0xF0u) {
            const uint8_t high = static_cast<uint8_t>(status & 0xF0u);
            // Program Change and Channel Aftertouch take one; the rest take two.
            return (high == 0xC0u || high == 0xD0u) ? 1u : 2u;
        }
        switch (status) {
            case 0xF1u: return 1u;  // MTC quarter frame
            case 0xF2u: return 2u;  // Song Position Pointer
            case 0xF3u: return 1u;  // Song Select
            default:    return 0u;  // F4, F5 undefined; F6 Tune Request
        }
    }

    [[nodiscard]] static constexpr MidiMessageKind RealTimeKind(uint8_t status) noexcept {
        switch (status) {
            case 0xF8u: return MidiMessageKind::kClock;
            case 0xFAu: return MidiMessageKind::kStart;
            case 0xFBu: return MidiMessageKind::kContinue;
            case 0xFCu: return MidiMessageKind::kStop;
            case 0xFEu: return MidiMessageKind::kActiveSensing;
            case 0xFFu: return MidiMessageKind::kSystemReset;
            default:    return MidiMessageKind::kUnknown;
        }
    }

    uint8_t pending_{0};
    uint8_t runningStatus_{0};
    uint8_t expected_{0};
    uint8_t received_{0};
    uint8_t data_[2]{};
    uint8_t deferredStatus_{0};
    bool hasDeferredStatus_{false};
    bool sysExActive_{false};
    bool inherited_{false};
    uint32_t sysExBytes_{0};
    uint64_t orphanDataBytes_{0};
};

/// Field names and values worth printing for one message.
///
/// Kind-specific names, because "d1=60 d2=100" is not readable and "note=60
/// vel=100" is -- which is the entire point of tracing at message level rather
/// than logging bytes.
struct MidiTraceFields final {
    const char* name1{"d1"};
    uint32_t value1{0};
    const char* name2{"d2"};
    uint32_t value2{0};
    /// How many of the two fields actually apply.
    uint8_t count{2};
};

[[nodiscard]] inline MidiTraceFields FieldsFor(const DecodedMidiMessage& m) noexcept {
    MidiTraceFields f{};
    f.value1 = m.data1;
    f.value2 = m.data2;
    switch (m.kind) {
        case MidiMessageKind::kNoteOn:
        case MidiMessageKind::kNoteOff:
            f.name1 = "note"; f.name2 = "vel"; break;
        case MidiMessageKind::kPolyAftertouch:
            f.name1 = "note"; f.name2 = "press"; break;
        case MidiMessageKind::kControlChange:
            f.name1 = "cc"; f.name2 = "val"; break;
        case MidiMessageKind::kProgramChange:
            f.name1 = "pgm"; f.count = 1; break;
        case MidiMessageKind::kChannelAftertouch:
            f.name1 = "press"; f.count = 1; break;
        case MidiMessageKind::kPitchBend:
            // 14-bit, centre 8192. Printing the raw pair would make the reader
            // do the arithmetic every time.
            f.name1 = "bend"; f.value1 = m.value14; f.count = 1; break;
        case MidiMessageKind::kSongPosition:
            f.name1 = "beats"; f.value1 = m.value14; f.count = 1; break;
        case MidiMessageKind::kSongSelect:
            f.name1 = "song"; f.count = 1; break;
        case MidiMessageKind::kTimeCodeQuarterFrame:
            f.name1 = "nibble"; f.count = 1; break;
        case MidiMessageKind::kSysExEnd:
            f.name1 = "bytes"; f.value1 = m.sysExBytes; f.count = 1; break;
        case MidiMessageKind::kSysExStart:
        case MidiMessageKind::kTuneRequest:
        case MidiMessageKind::kClock:
        case MidiMessageKind::kStart:
        case MidiMessageKind::kContinue:
        case MidiMessageKind::kStop:
        case MidiMessageKind::kActiveSensing:
        case MidiMessageKind::kSystemReset:
            f.count = 0; break;
        default:
            break;
    }
    return f;
}

} // namespace ASFW::Midi::Trace
