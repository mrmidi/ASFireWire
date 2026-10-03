// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicNames.hpp - Spec names for the Music subunit command fields and replies (TA 2001007 §7),
// for logs and diagnostics.
//
// Same contract as Core/AvcNames.hpp: `name(0xNN)` for a value the table knows,
// `UNKNOWN(<table>:0xNN)` for one it does not. Each table cites the spec table it names.
// music_plug_type is named once, in Descriptors/DescriptorNames.hpp, because music plug info
// blocks carry the same codes.

#pragma once

#include "MusicPlugCommands.hpp"

#include "../Core/AvcNames.hpp"
#include "../Descriptors/DescriptorNames.hpp"

#include <string>

namespace ASFW::AVC::Cmd {

/// Which reading `first` of a PlugConfigureEntry gets: the subfunction of a CONTROL subcommand
/// (request and reply, Figure 7.2) or the result_status of a STATUS subcommand (Figure 7.8).
enum class PlugConfigureKind : uint8_t {
    kControl,
    kStatus,
};

namespace names {

// TA 2001007 Table 7.2.
inline constexpr std::array kPlugConfigureSubfunctions{
    NameEntry{static_cast<uint32_t>(PlugConfigureSubfunction::kConnect), "CONNECT"},
    NameEntry{static_cast<uint32_t>(PlugConfigureSubfunction::kChangeConnection), "CHANGE_CONNECTION"},
    NameEntry{static_cast<uint32_t>(PlugConfigureSubfunction::kDisconnect), "DISCONNECT"},
    NameEntry{static_cast<uint32_t>(PlugConfigureSubfunction::kDisconnectAll), "DISCONNECT_ALL"},
    NameEntry{static_cast<uint32_t>(PlugConfigureSubfunction::kDefaultConfigure), "DEFAULT_CONFIGURE"},
};

// TA 2001007 Table 7.6.
inline constexpr std::array kPlugConfigureControlResults{
    NameEntry{static_cast<uint32_t>(PlugConfigureControlResult::kOk), "OK"},
    NameEntry{static_cast<uint32_t>(PlugConfigureControlResult::kUnknownSubfunction), "unknown subfunction"},
    NameEntry{static_cast<uint32_t>(PlugConfigureControlResult::kUnknownMusicPlugType), "unknown music_plug_type"},
    NameEntry{static_cast<uint32_t>(PlugConfigureControlResult::kMusicPlugDoesNotExist), "music_plug does not exist"},
    NameEntry{static_cast<uint32_t>(PlugConfigureControlResult::kSubunitPlugDoesNotExist),
              "subunit_plug does not exist"},
    NameEntry{static_cast<uint32_t>(PlugConfigureControlResult::kMusicPlugAlreadyConnected),
              "music_plug already connected"},
};

// TA 2001007 Table 7.8.
inline constexpr std::array kPlugConfigureStatusResults{
    NameEntry{static_cast<uint32_t>(PlugConfigureStatusResult::kOk), "OK"},
    NameEntry{static_cast<uint32_t>(PlugConfigureStatusResult::kNoConnection), "no connection"},
    NameEntry{static_cast<uint32_t>(PlugConfigureStatusResult::kUnknownMusicPlugType), "unknown music_plug_type"},
    NameEntry{static_cast<uint32_t>(PlugConfigureStatusResult::kMusicPlugDoesNotExist), "music_plug does not exist"},
};

// TA 2001007 Table 7.19.
inline constexpr std::array kMusicPlugDirections{
    NameEntry{static_cast<uint32_t>(MusicPlugDirection::kInput), "music_input_plug"},
    NameEntry{static_cast<uint32_t>(MusicPlugDirection::kOutput), "music_output_plug"},
};

// TA 2001007 Table 7.20.
inline constexpr std::array kMusicPlugAttributes{
    NameEntry{static_cast<uint32_t>(MusicPlugAttribute::kSimple), "simple"},
    NameEntry{static_cast<uint32_t>(MusicPlugAttribute::kCompound), "compound"},
};

} // namespace names

[[nodiscard]] inline std::string Describe(PlugConfigureSubfunction value) {
    return DescribeValue(names::kPlugConfigureSubfunctions, "plug_configure_subfunction",
                         static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(PlugConfigureControlResult value) {
    return DescribeValue(names::kPlugConfigureControlResults, "plug_configure_result", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(PlugConfigureStatusResult value) {
    return DescribeValue(names::kPlugConfigureStatusResults, "plug_configure_status", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(MusicPlugDirection value) {
    return DescribeValue(names::kMusicPlugDirections, "music_plug_direction", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(MusicPlugAttribute value) {
    return DescribeValue(names::kMusicPlugAttributes, "music_plug_attribute", static_cast<uint32_t>(value), 2);
}

/// A music_plug_type read from a reply. FF is the "not applicable" a DISCONNECT_ALL, a
/// DEFAULT_CONFIGURE and an unconnected plug carry; it prints as "none(0xff)".
[[nodiscard]] inline std::string Describe(MusicPlugTypeField value) {
    if (value.raw == kMusicNotApplicable) return "none(" + Hex(value.raw) + ")";
    return Protocols::AVC::Descriptors::DescribeMusicPlugType(value.raw);
}

/// The music_plug_type a MUSIC PLUG INFO request carries: a plug type, or FF for "all kind of
/// plugs" (Table 7.18).
[[nodiscard]] inline std::string DescribeMusicPlugInfoRequestType(std::optional<MusicPlugType> type) {
    if (!type) return "all kinds(" + Hex(kAllMusicPlugTypes) + ")";
    return Protocols::AVC::Descriptors::DescribeMusicPlugType(static_cast<uint8_t>(*type));
}

/// "0x0002", or "unspecified(0xffff)" for the value a query sends where it asks which plug.
[[nodiscard]] inline std::string Describe(MusicPlugId value) {
    if (value.Raw() == MusicPlugId::kUnspecified) return "unspecified(" + Hex(value.Raw(), 4) + ")";
    return Hex(value.Raw(), 4);
}

/// "0x01", "no plug(0xff)", or "UNKNOWN(subunit_plug_id:0x20)" for a reserved value (Table 7.5).
[[nodiscard]] inline std::string Describe(SubunitPlugId value) {
    if (value.Number()) return Hex(value.Raw());
    if (value.Raw() == kMusicNotApplicable) return "no plug(" + Hex(value.Raw()) + ")";
    return "UNKNOWN(subunit_plug_id:" + Hex(value.Raw()) + ")";
}

/// A stream_position read in the layout its plug type uses: "stream 3" (audio, SMPTE, sample
/// count; Figure 7.3), "stream 8 multiplex 5" (MIDI; Figure 7.4), "none" for FF FF (audio SYNC;
/// Figure 7.5), or UNKNOWN(stream_position:0x03 0x01) when the bytes do not fit the layout.
[[nodiscard]] inline std::string Describe(const StreamPosition& position, MusicPlugTypeField plugType) {
    if (position.IsNotApplicable()) return "none(0xff 0xff)";
    const auto type = plugType.Type();
    if (type == MusicPlugType::kMidi) {
        if (const auto midi = position.AsMultiplexed()) {
            return "stream " + std::to_string(midi->streamNumber) + " multiplex " + std::to_string(midi->multiplexIndex);
        }
    } else if (type == MusicPlugType::kAudio || type == MusicPlugType::kSmpteTimeCode ||
               type == MusicPlugType::kSampleCount) {
        if (const auto stream = position.AsSequence()) return "stream " + std::to_string(*stream);
    }
    return "UNKNOWN(stream_position:" + HexBytes(position.bytes) + ")";
}

/// One DESTINATION / SOURCE PLUG CONFIGURE subcommand, `first` read as the kind says:
/// "subfunction=CONNECT(0x00) type=audio(0x00) plug=0x0001 subunit_plug=0x00 position=stream 1".
[[nodiscard]] inline std::string Describe(const PlugConfigureEntry& entry, PlugConfigureKind kind) {
    std::string text = kind == PlugConfigureKind::kControl
                           ? "subfunction=" + DescribeValue(names::kPlugConfigureSubfunctions,
                                                            "plug_configure_subfunction", entry.first, 2)
                           : "result=" + DescribeValue(names::kPlugConfigureStatusResults, "plug_configure_status",
                                                       entry.first, 2);
    return text + " type=" + Describe(entry.plugType) + " plug=" + Describe(entry.plugId) +
           " subunit_plug=" + Describe(entry.subunitPlug) + " position=" + Describe(entry.position, entry.plugType);
}

/// One music_plug_info of a DESTINATION / SOURCE CONFIGURATIONS reply (Figure 7.13).
[[nodiscard]] inline std::string Describe(const MusicPlugInfoEntry& entry) {
    return "type=" + Describe(entry.plugType) + " plug=" + Describe(entry.plugId) +
           " position=" + Describe(entry.position, entry.plugType);
}

/// One music_plug_type_info of a MUSIC PLUG INFO reply (Figure 7.20).
[[nodiscard]] inline std::string Describe(const MusicPlugCounts& counts) {
    return "type=" + Describe(counts.plugType) + " input_plugs=" + std::to_string(counts.inputPlugs) +
           " output_plugs=" + std::to_string(counts.outputPlugs);
}

/// A format_info in the layout its plug type uses (Figures 7.24-7.27, Figure 5.9):
/// "fdf=0x00 am824_label=0x40", "MIDI 1.0 adaptation_layer=0x00", "rx=yes tx=no", "bus=yes external=no",
/// or UNKNOWN(music_format_info:..) with the two bytes for a plug type the spec gives no layout.
[[nodiscard]] inline std::string Describe(const MusicFormatInfo& format, MusicPlugTypeField plugType) {
    const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
    const auto type = plugType.Type();
    if (!type) return "UNKNOWN(music_format_info:" + HexBytes(format.bytes) + ")";
    switch (*type) {
        case MusicPlugType::kAudio:
            return "fdf=" + Hex(format.AsAudio().fdf) + " am824_label=" + Hex(format.AsAudio().am824Label);
        case MusicPlugType::kMidi: {
            const auto midi = format.AsMidi();
            return "MIDI " + std::to_string(midi.version) + "." + std::to_string(midi.revision) +
                   " adaptation_layer=" + Hex(midi.adaptationLayerVersion);
        }
        case MusicPlugType::kSmpteTimeCode:
        case MusicPlugType::kSampleCount: {
            const auto flags = format.AsTransfer();
            return std::string("rx=") + yesNo(flags.receive) + " tx=" + yesNo(flags.transmit);
        }
        case MusicPlugType::kAudioSync: {
            const auto flags = format.AsSync();
            return std::string("bus=") + yesNo(flags.bus) + " external=" + yesNo(flags.external);
        }
    }
    return "UNKNOWN(music_format_info:" + HexBytes(format.bytes) + ")";
}

/// One music_plug_format_info of a CURRENT CAPABILITY reply, read as `plugType` (Figure 7.23).
[[nodiscard]] inline std::string Describe(const MusicPlugFormat& entry, MusicPlugTypeField plugType) {
    return "plug=" + Describe(entry.plugId) + " " + Describe(entry.format, plugType);
}

} // namespace ASFW::AVC::Cmd
