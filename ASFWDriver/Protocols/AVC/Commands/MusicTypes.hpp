// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicTypes.hpp - The AV/C Music subunit command vocabulary: music plug types, plug and
// subunit plug numbers, stream positions, sub-functions and result codes.
//
// Source: TA Document 2001007, AV/C Music Subunit 1.0 (8 Apr 2002), referred to below as
// "Music". Table, Figure and section numbers are that document's. No reference stack
// (Linux, FFADO, Apple IOFireWireAVC, Apple AVCVideoServices) implements any of these
// commands, so every layout here is read from the spec text alone and has not been seen on
// a wire. Fresh implementation; no reference code copied.
//
// Contract (same as CcmTypes.hpp):
// - Callers name what they mean (MusicPlugId::Of(2), StreamPosition::Sequence(3)). A raw
//   byte only exists inside these types and in the codecs that serialise them.
// - Reading is lenient: every field keeps its raw value, and a named view returns
//   std::nullopt for a reserved value. A device that sends a reserved value is not a parse
//   error.

#pragma once

#include "../Core/AvcTypes.hpp"
#include "../Descriptors/DescriptorTypeCodes.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::AVC::Cmd {

/// The "no value / not applicable" byte every Music command field uses (Music §7.1.1.1:
/// "shall be set to FF16").
inline constexpr uint8_t kMusicNotApplicable = 0xFF;

// ---------------------------------------------------------------------------
// music_plug_type (Table 7.3; Table 7.18 adds "all kinds" for MUSIC PLUG INFO)
// ---------------------------------------------------------------------------

/// The kind of data a music plug manages. The values are the same ones music plug info
/// blocks carry (810B), defined once in DescriptorTypeCodes.hpp.
enum class MusicPlugType : uint8_t {
    kAudio = Protocols::AVC::Descriptors::kMusicPlugTypeAudio,              ///< 00
    kMidi = Protocols::AVC::Descriptors::kMusicPlugTypeMidi,                ///< 01
    kSmpteTimeCode = Protocols::AVC::Descriptors::kMusicPlugTypeSmpte,      ///< 02
    kSampleCount = Protocols::AVC::Descriptors::kMusicPlugTypeSampleCount,  ///< 03
    kAudioSync = Protocols::AVC::Descriptors::kMusicPlugTypeSync,           ///< 80
};

/// MUSIC PLUG INFO's request value for "all kind of plugs" (Table 7.18). Elsewhere FF is reserved.
inline constexpr uint8_t kAllMusicPlugTypes = 0xFF;

/// A music_plug_type as read from a reply, kept raw. Named view: nullopt for a reserved value.
struct MusicPlugTypeField {
    uint8_t raw{kMusicNotApplicable};

    [[nodiscard]] static constexpr MusicPlugTypeField Of(MusicPlugType type) noexcept {
        return MusicPlugTypeField{static_cast<uint8_t>(type)};
    }
    [[nodiscard]] static constexpr MusicPlugTypeField FromRaw(uint8_t raw) noexcept { return MusicPlugTypeField{raw}; }
    [[nodiscard]] static constexpr MusicPlugTypeField NotApplicable() noexcept { return MusicPlugTypeField{}; }

    [[nodiscard]] constexpr std::optional<MusicPlugType> Type() const noexcept {
        switch (raw) {
            case static_cast<uint8_t>(MusicPlugType::kAudio):
            case static_cast<uint8_t>(MusicPlugType::kMidi):
            case static_cast<uint8_t>(MusicPlugType::kSmpteTimeCode):
            case static_cast<uint8_t>(MusicPlugType::kSampleCount):
            case static_cast<uint8_t>(MusicPlugType::kAudioSync):
                return static_cast<MusicPlugType>(raw);
            default:
                return std::nullopt;
        }
    }

    friend constexpr bool operator==(MusicPlugTypeField, MusicPlugTypeField) noexcept = default;
};

// ---------------------------------------------------------------------------
// music_plug_ID (Table 7.4)
// ---------------------------------------------------------------------------

/// A music plug number: 0000..FFFE, big-endian on the wire. FFFF means "reserved for each
/// field": a query sends it where it asks for the plug rather than naming one (Table 7.10).
class MusicPlugId {
public:
    static constexpr uint16_t kUnspecified = 0xFFFF;

    [[nodiscard]] static constexpr MusicPlugId Of(uint16_t number) noexcept { return MusicPlugId{number}; }
    /// "Not named": what a query sends when it asks which plug (Table 7.10), and what
    /// DISCONNECT_ALL and DEFAULT_CONFIGURE send (§7.1.1.1).
    [[nodiscard]] static constexpr MusicPlugId Unspecified() noexcept { return MusicPlugId{kUnspecified}; }
    [[nodiscard]] static constexpr MusicPlugId FromRaw(uint16_t raw) noexcept { return MusicPlugId{raw}; }

    [[nodiscard]] constexpr uint16_t Raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr uint8_t High() const noexcept { return static_cast<uint8_t>(raw_ >> 8); }
    [[nodiscard]] constexpr uint8_t Low() const noexcept { return static_cast<uint8_t>(raw_ & 0xFF); }
    /// The plug number, or nullopt for the unspecified value.
    [[nodiscard]] constexpr std::optional<uint16_t> Number() const noexcept {
        if (raw_ == kUnspecified) return std::nullopt;
        return raw_;
    }

    friend constexpr bool operator==(MusicPlugId, MusicPlugId) noexcept = default;

private:
    explicit constexpr MusicPlugId(uint16_t raw) noexcept : raw_(raw) {}
    uint16_t raw_;
};

static_assert(MusicPlugId::Of(2).Number() == 2 && !MusicPlugId::Unspecified().Number().has_value());

// ---------------------------------------------------------------------------
// subunit_plug_ID (Table 7.5)
// ---------------------------------------------------------------------------

/// A subunit plug number as the Music commands carry it: 00..1E, or FF for "no plug".
class SubunitPlugId {
public:
    static constexpr uint8_t kMaxPlugNumber = 0x1E;  ///< destination plug [0..30] (Table 7.5)

    [[nodiscard]] static constexpr SubunitPlugId Of(uint8_t number) noexcept { return SubunitPlugId{number}; }
    /// "No plug": what a query sends where it asks which plug, and what DISCONNECT,
    /// DISCONNECT_ALL and DEFAULT_CONFIGURE send (§7.1.1.1).
    [[nodiscard]] static constexpr SubunitPlugId NoPlug() noexcept { return SubunitPlugId{kMusicNotApplicable}; }
    [[nodiscard]] static constexpr SubunitPlugId FromRaw(uint8_t raw) noexcept { return SubunitPlugId{raw}; }

    [[nodiscard]] constexpr uint8_t Raw() const noexcept { return raw_; }
    /// The plug number; nullopt for "no plug" or a reserved value (1F..FE).
    [[nodiscard]] constexpr std::optional<uint8_t> Number() const noexcept {
        if (raw_ <= kMaxPlugNumber) return raw_;
        return std::nullopt;
    }

    friend constexpr bool operator==(SubunitPlugId, SubunitPlugId) noexcept = default;

private:
    explicit constexpr SubunitPlugId(uint8_t raw) noexcept : raw_(raw) {}
    uint8_t raw_;
};

static_assert(SubunitPlugId::Of(30).Number() == 30);
static_assert(!SubunitPlugId::NoPlug().Number().has_value());

// ---------------------------------------------------------------------------
// stream_position (Figures 7.3, 7.4, 7.5)
// ---------------------------------------------------------------------------

/// Where a music plug's data sits in the AM824 packet: two bytes whose meaning depends on the
/// music_plug_type. The wire bytes are kept; the views read them in one of the three formats.
struct StreamPosition {
    static constexpr uint8_t kMaxMultiplexIndex = 7;  ///< multiplex_index is 0..7 (Figure 7.4)

    std::array<uint8_t, 2> bytes{kMusicNotApplicable, kMusicNotApplicable};

    /// Format 0 (audio, SMPTE time code, sample count): the sequence number, then FF (Figure 7.3).
    [[nodiscard]] static constexpr StreamPosition Sequence(uint8_t streamNumber) noexcept {
        return StreamPosition{{streamNumber, kMusicNotApplicable}};
    }
    /// Format 1 (MIDI): the sequence number and the multiplex index within it (Figure 7.4).
    [[nodiscard]] static constexpr StreamPosition Multiplexed(uint8_t streamNumber, uint8_t multiplexIndex) noexcept {
        return StreamPosition{{streamNumber, multiplexIndex}};
    }
    /// Format 2 (audio SYNC), and what DISCONNECT, DISCONNECT_ALL, DEFAULT_CONFIGURE and a
    /// query send: FF FF (Figure 7.5, §7.1.1.1).
    [[nodiscard]] static constexpr StreamPosition NotApplicable() noexcept { return StreamPosition{}; }
    [[nodiscard]] static constexpr StreamPosition FromBytes(uint8_t first, uint8_t second) noexcept {
        return StreamPosition{{first, second}};
    }

    /// Format 0 reading: the stream number if the second byte is FF.
    [[nodiscard]] constexpr std::optional<uint8_t> AsSequence() const noexcept {
        if (bytes[1] != kMusicNotApplicable) return std::nullopt;
        return bytes[0];
    }
    struct MultiplexedPosition {
        uint8_t streamNumber;
        uint8_t multiplexIndex;
    };
    /// Format 1 reading: nullopt when the multiplex index is not 0..7.
    [[nodiscard]] constexpr std::optional<MultiplexedPosition> AsMultiplexed() const noexcept {
        if (bytes[1] > kMaxMultiplexIndex) return std::nullopt;
        return MultiplexedPosition{bytes[0], bytes[1]};
    }
    [[nodiscard]] constexpr bool IsNotApplicable() const noexcept {
        return bytes[0] == kMusicNotApplicable && bytes[1] == kMusicNotApplicable;
    }

    friend constexpr bool operator==(const StreamPosition&, const StreamPosition&) noexcept = default;
};

static_assert(StreamPosition::Sequence(3).AsSequence() == 3);
static_assert(StreamPosition::Multiplexed(8, 5).AsMultiplexed()->multiplexIndex == 5);
static_assert(!StreamPosition::Multiplexed(8, 5).AsSequence().has_value());
static_assert(StreamPosition::NotApplicable().IsNotApplicable());

// ---------------------------------------------------------------------------
// DESTINATION PLUG CONFIGURE sub-functions and results (Tables 7.2, 7.6, 7.8)
// ---------------------------------------------------------------------------

/// The subfunction of one DESTINATION PLUG CONFIGURE subcommand (Table 7.2).
enum class PlugConfigureSubfunction : uint8_t {
    kConnect = 0x00,
    kChangeConnection = 0x01,
    kDisconnect = 0x02,
    kDisconnectAll = 0x03,
    kDefaultConfigure = 0x04,
};

/// result_status of a DESTINATION PLUG CONFIGURE control reply (Table 7.6).
enum class PlugConfigureControlResult : uint8_t {
    kOk = 0x00,
    kUnknownSubfunction = 0x01,
    kUnknownMusicPlugType = 0x02,
    kMusicPlugDoesNotExist = 0x03,
    kSubunitPlugDoesNotExist = 0x04,
    kMusicPlugAlreadyConnected = 0x05,
};

/// result_status of one subcommand in a DESTINATION / SOURCE PLUG CONFIGURE status reply (Table 7.8).
enum class PlugConfigureStatusResult : uint8_t {
    kOk = 0x00,
    kNoConnection = 0x01,
    kUnknownMusicPlugType = 0x02,
    kMusicPlugDoesNotExist = 0x03,
};

/// "FF" in the result_status of a command frame and of a REJECTED reply (Figures 7.1, 7.7; §7.1.1.2).
inline constexpr uint8_t kPlugConfigureResultUnset = 0xFF;

/// At most 72 subcommands fit one frame: 72 x 7 bytes plus the three header operands (§7.1.1.1).
inline constexpr size_t kMaxPlugConfigureSubcommands = 72;
inline constexpr size_t kPlugConfigureSubcommandBytes = 7;  ///< Figures 7.2, 7.8

// ---------------------------------------------------------------------------
// CURRENT CAPABILITY fields (Tables 7.19, 7.20)
// ---------------------------------------------------------------------------

/// music_plug_direction (Table 7.19).
enum class MusicPlugDirection : uint8_t {
    kInput = 0x00,   ///< music_input_plug
    kOutput = 0x01,  ///< music_output_plug
};

/// music_plug_attribute (Table 7.20).
enum class MusicPlugAttribute : uint8_t {
    kSimple = 0x00,    ///< The specified music plugs all have the same music_plug_type.
    kCompound = 0x01,  ///< The specified music plugs each have their own music_plug_type.
};

} // namespace ASFW::AVC::Cmd
