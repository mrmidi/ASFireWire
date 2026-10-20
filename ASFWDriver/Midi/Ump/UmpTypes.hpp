//
// UmpTypes.hpp
// ASFWDriver
//
// Universal MIDI Packet word layouts, shared by the byte-stream converters.
//
// Layouts transcribed from the inline constexpr builders in CoreMIDI's
// MIDIMessages.h. A DriverKit extension cannot link CoreMIDI, so the packing is
// reimplemented here; the field positions are the contract, not the helpers.
//
// Pure header: no DriverKit dependency, no allocation, no logging.
//

#pragma once

#include <cstdint>

namespace ASFW::Midi::Ump {

/// One 32-bit Universal MIDI Packet word. Matches IOUserMIDIUMPWord.
using UmpWord = uint32_t;

/// UMP message type, the top nibble of word 0.
enum class MessageType : uint8_t {
    kUtility          = 0x0,  ///< NOOP, JR clock/timestamp, delta clockstamp.
    kSystem           = 0x1,  ///< System Common and System Real Time.
    kMidi1ChannelVoice = 0x2, ///< MIDI 1.0 Channel Voice.
    kData64           = 0x3,  ///< SysEx7.
    kMidi2ChannelVoice = 0x4, ///< MIDI 2.0 Channel Voice.
    kData128          = 0x5,  ///< SysEx8, Mixed Data Set.
    kFlexData         = 0xD,
    kUmpStream        = 0xF,
};

/// SysEx7 packet status, word 0 bits 23..20. MIDIMessages.h MIDISysExStatus.
enum class SysExStatus : uint8_t {
    kComplete = 0x0,  ///< Whole message in this one packet.
    kStart    = 0x1,
    kContinue = 0x2,
    kEnd      = 0x3,
};

/// Utility status, word 0 bits 23..20. MIDIMessages.h MIDIUtilityStatus.
enum class UtilityStatus : uint8_t {
    kNoOp                       = 0x0,
    kJitterReductionClock       = 0x1,
    kJitterReductionTimestamp   = 0x2,
    kDeltaClockstampTicksPerQN  = 0x3,
    kTicksSinceLastEvent        = 0x4,
};

/// Words occupied by a UMP packet whose message-type nibble is `nibble`.
///
/// Every message type has a fixed packet length, including the types that are
/// currently undefined, so an unrecognised packet can be skipped exactly rather
/// than resynchronised on. Table transcribed from CoreMIDI's MIDIMessages.h
/// MIDIMessageType enum (MacOSX26.5.sdk), which documents the sizes of the
/// undefined types 6-C and E alongside the defined ones.
[[nodiscard]] constexpr uint8_t UmpPacketWords(uint8_t nibble) noexcept {
    switch (nibble & 0x0F) {
        case 0x0: return 1;  // Utility
        case 0x1: return 1;  // System
        case 0x2: return 1;  // MIDI 1.0 Channel Voice
        case 0x3: return 2;  // SysEx7 / Data
        case 0x4: return 2;  // MIDI 2.0 Channel Voice
        case 0x5: return 4;  // Data128
        case 0x6: return 1;  // undefined
        case 0x7: return 1;  // undefined
        case 0x8: return 2;  // undefined
        case 0x9: return 2;  // undefined
        case 0xA: return 2;  // undefined
        case 0xB: return 3;  // undefined
        case 0xC: return 3;  // undefined
        case 0xD: return 4;  // Flex Data
        case 0xE: return 4;  // undefined
        default:  return 4;  // 0xF, UMP Stream
    }
}

/// Words occupied by a message of this type.
///
/// Validate against the words actually available before reading any field
/// beyond word 0: a truncated multi-word message must never be reinterpreted
/// as a sequence of shorter ones.
[[nodiscard]] constexpr uint8_t WordsForMessageType(MessageType type) noexcept {
    return UmpPacketWords(static_cast<uint8_t>(type));
}

/// Message type of a word-0 value.
[[nodiscard]] constexpr uint8_t MessageTypeNibble(UmpWord word0) noexcept {
    return static_cast<uint8_t>((word0 >> 28) & 0x0F);
}

/// Group of a word-0 value, bits 27..24.
[[nodiscard]] constexpr uint8_t GroupOf(UmpWord word0) noexcept {
    return static_cast<uint8_t>((word0 >> 24) & 0x0F);
}

/// Whether a message type carries a group in bits 27..24.
///
/// Utility (MT 0x0) and UMP Stream (MT 0xF) are groupless: those bits are
/// reserved or hold other fields, so filtering them by group would silently
/// misclassify every such packet on a nonzero group.
[[nodiscard]] constexpr bool IsGroupScoped(uint8_t nibble) noexcept {
    const uint8_t mt = static_cast<uint8_t>(nibble & 0x0F);
    return mt != static_cast<uint8_t>(MessageType::kUtility) &&
           mt != static_cast<uint8_t>(MessageType::kUmpStream);
}

/// Maximum SysEx7 payload bytes in one UMP packet.
/// MIDIMessages.h kMIDI1UPMaxSysexSize.
inline constexpr uint8_t kMaxSysEx7PayloadBytes = 6;

/// Highest valid UMP group index.
inline constexpr uint8_t kMaxGroup = 0x0F;

//==============================================================================
// Builders
//==============================================================================

/// MT 0x2 - MIDI 1.0 Channel Voice. MIDIMessages.h:276.
///
///  31..28  27..24  23..20   19..16    15..8   7..0
///   0x2    group   status   channel   data1   data2
///
/// `status` is the high nibble of the MIDI status byte (0x8..0xE); `data1` and
/// `data2` must already be 7-bit.
[[nodiscard]] constexpr UmpWord MakeMidi1ChannelVoice(
    uint8_t group, uint8_t status, uint8_t channel,
    uint8_t data1, uint8_t data2) noexcept {
    return (static_cast<UmpWord>(MessageType::kMidi1ChannelVoice) << 28)
         | (static_cast<UmpWord>(group   & 0x0F) << 24)
         | (static_cast<UmpWord>(status  & 0x0F) << 20)
         | (static_cast<UmpWord>(channel & 0x0F) << 16)
         | (static_cast<UmpWord>(data1   & 0x7F) << 8)
         |  static_cast<UmpWord>(data2   & 0x7F);
}

/// MT 0x1 - System Common and System Real Time. MIDIMessages.h:311.
///
///  31..28  27..24  23..16          15..8   7..0
///   0x1    group   status (full)   byte1   byte2
///
/// Note that `status` is a whole byte here, not a nibble. Unused data byte
/// positions are zero.
[[nodiscard]] constexpr UmpWord MakeSystem(
    uint8_t group, uint8_t status, uint8_t byte1, uint8_t byte2) noexcept {
    return (static_cast<UmpWord>(MessageType::kSystem) << 28)
         | (static_cast<UmpWord>(group & 0x0F) << 24)
         | (static_cast<UmpWord>(status) << 16)
         | (static_cast<UmpWord>(byte1 & 0x7F) << 8)
         |  static_cast<UmpWord>(byte2 & 0x7F);
}

/// MT 0x3 word 0 - SysEx7 header. MIDIMessages.h:315.
///
///  31..28  27..24  23..20   19..16     15..8   7..0
///   0x3    group   status   numBytes   byte1   byte2
[[nodiscard]] constexpr UmpWord MakeSysEx7Word0(
    uint8_t group, SysExStatus status, uint8_t numBytes,
    uint8_t byte1, uint8_t byte2) noexcept {
    return (static_cast<UmpWord>(MessageType::kData64) << 28)
         | (static_cast<UmpWord>(group & 0x0F) << 24)
         | (static_cast<UmpWord>(static_cast<uint8_t>(status) & 0x0F) << 20)
         | (static_cast<UmpWord>(numBytes & 0x0F) << 16)
         | (static_cast<UmpWord>(byte1 & 0x7F) << 8)
         |  static_cast<UmpWord>(byte2 & 0x7F);
}

/// MT 0x3 word 1 - SysEx7 payload bytes 3..6.
[[nodiscard]] constexpr UmpWord MakeSysEx7Word1(
    uint8_t byte3, uint8_t byte4, uint8_t byte5, uint8_t byte6) noexcept {
    return (static_cast<UmpWord>(byte3 & 0x7F) << 24)
         | (static_cast<UmpWord>(byte4 & 0x7F) << 16)
         | (static_cast<UmpWord>(byte5 & 0x7F) << 8)
         |  static_cast<UmpWord>(byte6 & 0x7F);
}

//==============================================================================
// MIDI 1.0 byte-stream classification
//==============================================================================

/// A MIDI 1.0 status byte has bit 7 set; data bytes are 7-bit.
[[nodiscard]] constexpr bool IsStatusByte(uint8_t byte) noexcept {
    return (byte & 0x80) != 0;
}

/// System Real Time: 0xF8-0xFF. May appear between any two bytes of another
/// message, including inside a System Exclusive, and never disturbs it.
[[nodiscard]] constexpr bool IsSystemRealTime(uint8_t byte) noexcept {
    return byte >= 0xF8;
}

/// System Common: 0xF0-0xF7. Clears running status.
[[nodiscard]] constexpr bool IsSystemCommon(uint8_t byte) noexcept {
    return byte >= 0xF0 && byte <= 0xF7;
}

/// Channel Voice or Channel Mode: 0x80-0xEF. Sets running status.
[[nodiscard]] constexpr bool IsChannelStatus(uint8_t byte) noexcept {
    return byte >= 0x80 && byte <= 0xEF;
}

inline constexpr uint8_t kSysExStart = 0xF0;
inline constexpr uint8_t kSysExEnd   = 0xF7;

/// Data bytes carried by a Channel Voice status byte.
///
/// Program Change (0xC0) and Channel Pressure (0xD0) take one; every other
/// channel status takes two.
[[nodiscard]] constexpr uint8_t ChannelMessageDataBytes(uint8_t status) noexcept {
    const uint8_t high = static_cast<uint8_t>(status & 0xF0);
    return (high == 0xC0 || high == 0xD0) ? 1 : 2;
}

/// Data bytes carried by a System Common status byte, or 0xFF if the status is
/// not a System Common message this converter accepts.
///
/// 0xF0 and 0xF7 are SysEx framing and are handled by the SysEx path, never as
/// standalone System Common. 0xF4 and 0xF5 are undefined in MIDI 1.0 and are
/// rejected rather than forwarded to an arbitrary device.
[[nodiscard]] constexpr uint8_t SystemCommonDataBytes(uint8_t status) noexcept {
    switch (status) {
        case 0xF1: return 1;   // MIDI Time Code Quarter Frame
        case 0xF2: return 2;   // Song Position Pointer
        case 0xF3: return 1;   // Song Select
        case 0xF6: return 0;   // Tune Request
        default:   return 0xFF;
    }
}

} // namespace ASFW::Midi::Ump
