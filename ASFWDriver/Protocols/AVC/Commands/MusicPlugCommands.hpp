// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicPlugCommands.hpp - The AV/C Music subunit commands (TA 2001007 §7, Table 7.1):
//   DESTINATION PLUG CONFIGURE (0x40)  §7.1   CONTROL, STATUS
//   SOURCE PLUG CONFIGURE (0x41)       §7.2   STATUS
//   DESTINATION CONFIGURATIONS (0x42)  §7.3   STATUS
//   SOURCE CONFIGURATIONS (0x43)       §7.4   STATUS
//   MUSIC PLUG INFO (0xC0)             §7.5   STATUS
//   CURRENT CAPABILITY (0xC1)          §7.6   STATUS
//
// NOT SEEN ON A WIRE. Linux, FFADO, Apple IOFireWireAVC and Apple AVCVideoServices implement
// none of these, and no capture holds one. Every layout below is the spec text and its Annex A
// examples, nothing more. Nothing in the driver sends them: discovery does not probe them, and a
// device that carries a command allowlist refuses them. Do not add one to an attach path
// before a device has answered it (see avc-attach-sends-only-captured-frames).
//
// Named requests (build these, do not assemble bytes). Each takes the Music subunit it is
// addressed to; every one is a STATUS unless marked CONTROL.
//   QueryMusicPlugInfo(music, type)                       MUSIC PLUG INFO
//   QueryAllMusicPlugInfo(music)                          MUSIC PLUG INFO, "all kind of plugs"
//   QueryCurrentCapability(music, direction, type, ids)   CURRENT CAPABILITY
//   QueryDestinationConfigurations(music, plug)           DESTINATION CONFIGURATIONS
//   QuerySourceConfigurations(music, plug)                SOURCE CONFIGURATIONS
//   QueryDestinationPlugConfigure(music, entries)         DESTINATION PLUG CONFIGURE
//   QuerySourcePlugConfigure(music, entries)              SOURCE PLUG CONFIGURE
//   ConfigureDestinationPlugs(music, entries)             DESTINATION PLUG CONFIGURE, CONTROL
// Entries are built with PlugConfigureEntry::Connect(...), ::QueryConnection(...) and the like.
// Replies keep every wire value; a named view returns nullopt for a reserved one.

#pragma once

#include "MusicTypes.hpp"

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::AVC::Cmd {

namespace music_detail {

/// Every Music subunit command is addressed to a Music subunit (TA 2001007 §7: "subunit
/// commands developed for Music subunit").
[[nodiscard]] constexpr Expected<void> RequireMusicSubunit(SubunitAddress address) noexcept {
    if (address.IsUnit() || address.Type() != SubunitType::kMusic) return Fail(AvcErrorKind::kInvalidArgument);
    return {};
}

[[nodiscard]] constexpr uint16_t BigEndian16(uint8_t high, uint8_t low) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(high) << 8) | low);
}

} // namespace music_detail

// ---------------------------------------------------------------------------
// DESTINATION PLUG CONFIGURE / SOURCE PLUG CONFIGURE (§7.1, §7.2)
// Operands: number_of_subcommands, result_status, number_of_completed_subcommands, then
// that many 7-byte subcommands (Figures 7.1, 7.6, 7.7, 7.9, 7.10).
// ---------------------------------------------------------------------------

/// One subcommand (Figures 7.2, 7.8). `first` is the subfunction in a CONTROL request and the
/// result_status everywhere else, so read it through the view that fits what was sent.
struct PlugConfigureEntry {
    uint8_t first{kPlugConfigureResultUnset};
    MusicPlugTypeField plugType{};
    MusicPlugId plugId{MusicPlugId::Unspecified()};
    SubunitPlugId subunitPlug{SubunitPlugId::NoPlug()};
    StreamPosition position{};

    // --- CONTROL subcommands (Table 7.2) -------------------------------------------------

    /// CONNECT: connect the music input plug to a subunit destination plug at `position` (§7.1.1.1).
    [[nodiscard]] static constexpr PlugConfigureEntry Connect(MusicPlugType type, MusicPlugId plug,
                                                              SubunitPlugId subunitPlug,
                                                              StreamPosition position) noexcept {
        return Control(PlugConfigureSubfunction::kConnect, type, plug, subunitPlug, position);
    }
    /// CHANGE_CONNECTION: CONNECT and DISCONNECT in one, for a plug this controller connected.
    [[nodiscard]] static constexpr PlugConfigureEntry ChangeConnection(MusicPlugType type, MusicPlugId plug,
                                                                       SubunitPlugId subunitPlug,
                                                                       StreamPosition position) noexcept {
        return Control(PlugConfigureSubfunction::kChangeConnection, type, plug, subunitPlug, position);
    }
    /// DISCONNECT: subunit_plug_ID and stream_position are FF (§7.1.1.1).
    [[nodiscard]] static constexpr PlugConfigureEntry Disconnect(MusicPlugType type, MusicPlugId plug) noexcept {
        return Control(PlugConfigureSubfunction::kDisconnect, type, plug, SubunitPlugId::NoPlug(),
                       StreamPosition::NotApplicable());
    }
    /// DISCONNECT_ALL: every other field is FF (§7.1.1.1).
    [[nodiscard]] static constexpr PlugConfigureEntry DisconnectAll() noexcept {
        return PlugConfigureEntry{.first = static_cast<uint8_t>(PlugConfigureSubfunction::kDisconnectAll)};
    }
    /// DEFAULT_CONFIGURE: reset every music plug to its default configuration; every other field is FF.
    [[nodiscard]] static constexpr PlugConfigureEntry DefaultConfigure() noexcept {
        return PlugConfigureEntry{.first = static_cast<uint8_t>(PlugConfigureSubfunction::kDefaultConfigure)};
    }

    // --- STATUS subcommands (Tables 7.10, 7.11, 7.13, 7.14) ----------------------------

    /// Which subunit plug and stream position is music plug `plug` connected to? (Table 7.11)
    [[nodiscard]] static constexpr PlugConfigureEntry QueryConnection(MusicPlugType type, MusicPlugId plug) noexcept {
        return PlugConfigureEntry{.plugType = MusicPlugTypeField::Of(type), .plugId = plug};
    }
    /// Which music plug of `type` is connected to `subunitPlug` at `position`? (Table 7.10)
    [[nodiscard]] static constexpr PlugConfigureEntry QueryPlugAt(MusicPlugType type, SubunitPlugId subunitPlug,
                                                                  StreamPosition position) noexcept {
        return PlugConfigureEntry{.plugType = MusicPlugTypeField::Of(type),
                                  .plugId = MusicPlugId::Unspecified(),
                                  .subunitPlug = subunitPlug,
                                  .position = position};
    }

    // --- Readings ---------------------------------------------------------------------------

    /// `first` of a CONTROL request or its reply, as a subfunction (Table 7.2).
    [[nodiscard]] constexpr std::optional<PlugConfigureSubfunction> Subfunction() const noexcept {
        if (first > static_cast<uint8_t>(PlugConfigureSubfunction::kDefaultConfigure)) return std::nullopt;
        return static_cast<PlugConfigureSubfunction>(first);
    }
    /// `first` of a STATUS reply, as a result (Table 7.8).
    [[nodiscard]] constexpr std::optional<PlugConfigureStatusResult> StatusResult() const noexcept {
        if (first > static_cast<uint8_t>(PlugConfigureStatusResult::kMusicPlugDoesNotExist)) return std::nullopt;
        return static_cast<PlugConfigureStatusResult>(first);
    }

    friend constexpr bool operator==(const PlugConfigureEntry&, const PlugConfigureEntry&) noexcept = default;

private:
    [[nodiscard]] static constexpr PlugConfigureEntry Control(PlugConfigureSubfunction subfunction, MusicPlugType type,
                                                              MusicPlugId plug, SubunitPlugId subunitPlug,
                                                              StreamPosition position) noexcept {
        return PlugConfigureEntry{.first = static_cast<uint8_t>(subfunction),
                                  .plugType = MusicPlugTypeField::Of(type),
                                  .plugId = plug,
                                  .subunitPlug = subunitPlug,
                                  .position = position};
    }
};

/// A reply to DESTINATION or SOURCE PLUG CONFIGURE.
struct PlugConfigureReply {
    uint8_t numberOfSubcommands{0};
    uint8_t resultStatus{kPlugConfigureResultUnset};      ///< operand[1]: Table 7.6 in an ACCEPTED CONTROL reply, FF otherwise
    uint8_t completedSubcommands{kPlugConfigureResultUnset};  ///< operand[2]: successes (CONTROL) or plugs acquired (STATUS)
    std::vector<PlugConfigureEntry> entries;

    /// The result_status of a CONTROL reply (Table 7.6); nullopt for FF or a reserved value.
    [[nodiscard]] std::optional<PlugConfigureControlResult> ControlResult() const noexcept {
        if (resultStatus > static_cast<uint8_t>(PlugConfigureControlResult::kMusicPlugAlreadyConnected)) {
            return std::nullopt;
        }
        return static_cast<PlugConfigureControlResult>(resultStatus);
    }
};

template <Opcode kOperandOpcode, bool kAcceptsControl>
struct PlugConfigureOperands {
    static constexpr Opcode kOpcode = kOperandOpcode;

    std::vector<PlugConfigureEntry> entries;

    using Reply = PlugConfigureReply;

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        return music_detail::RequireMusicSubunit(address);
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        const bool control = t == CommandType::kControl;
        if (!(t == CommandType::kStatus || (control && kAcceptsControl))) return Fail(AvcErrorKind::kInvalidArgument);
        if (entries.empty() || entries.size() > kMaxPlugConfigureSubcommands) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        // A CONTROL entry names a subfunction; a STATUS entry carries FF where the result will come back.
        for (const auto& entry : entries) {
            const bool valid = control ? entry.Subfunction().has_value() : entry.first == kPlugConfigureResultUnset;
            if (!valid) return Fail(AvcErrorKind::kInvalidArgument);
        }
        const std::array<uint8_t, 3> header = {static_cast<uint8_t>(entries.size()), kPlugConfigureResultUnset,
                                               kPlugConfigureResultUnset};
        if (auto written = w.Append(header); !written) return written;
        for (const auto& entry : entries) {
            const std::array<uint8_t, kPlugConfigureSubcommandBytes> bytes = {
                entry.first, entry.plugType.raw, entry.plugId.High(), entry.plugId.Low(),
                entry.subunitPlug.Raw(), entry.position.bytes[0], entry.position.bytes[1]};
            if (auto written = w.Append(bytes); !written) return written;
        }
        return {};
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        constexpr size_t kHeaderBytes = 3;
        if (in.size() < kHeaderBytes) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        const size_t count = in[0];
        if (in.size() < kHeaderBytes + count * kPlugConfigureSubcommandBytes) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        Reply reply{.numberOfSubcommands = in[0], .resultStatus = in[1], .completedSubcommands = in[2], .entries = {}};
        reply.entries.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto bytes = in.subspan(kHeaderBytes + i * kPlugConfigureSubcommandBytes);
            reply.entries.push_back(PlugConfigureEntry{
                .first = bytes[0],
                .plugType = MusicPlugTypeField::FromRaw(bytes[1]),
                .plugId = MusicPlugId::FromRaw(music_detail::BigEndian16(bytes[2], bytes[3])),
                .subunitPlug = SubunitPlugId::FromRaw(bytes[4]),
                .position = StreamPosition::FromBytes(bytes[5], bytes[6]),
            });
        }
        return reply;
    }
};

using DestinationPlugConfigureCommand = Command<PlugConfigureOperands<Opcode::kDestinationPlugConfigure, true>>;
using SourcePlugConfigureCommand = Command<PlugConfigureOperands<Opcode::kSourcePlugConfigure, false>>;

/// STATUS: how each listed music input plug is connected (§7.1.2). Build the entries with
/// PlugConfigureEntry::QueryConnection / QueryPlugAt. Issue it to a listener.
[[nodiscard]] inline DestinationPlugConfigureCommand QueryDestinationPlugConfigure(
    SubunitAddress music, std::vector<PlugConfigureEntry> entries) {
    return DestinationPlugConfigureCommand{.address = music, .operands = {.entries = std::move(entries)}};
}

/// CONTROL: connect, change or disconnect music input plugs, in order (§7.1.1). Changes device
/// state. Send only to a device whose frames are proven.
[[nodiscard]] inline DestinationPlugConfigureCommand ConfigureDestinationPlugs(
    SubunitAddress music, std::vector<PlugConfigureEntry> entries) {
    return DestinationPlugConfigureCommand{.address = music, .operands = {.entries = std::move(entries)}};
}

/// STATUS: how each listed music output plug is connected (§7.2). Issue it to a talker.
[[nodiscard]] inline SourcePlugConfigureCommand QuerySourcePlugConfigure(SubunitAddress music,
                                                                         std::vector<PlugConfigureEntry> entries) {
    return SourcePlugConfigureCommand{.address = music, .operands = {.entries = std::move(entries)}};
}

// ---------------------------------------------------------------------------
// DESTINATION CONFIGURATIONS / SOURCE CONFIGURATIONS (§7.3, §7.4)
// Request: subunit_plug_ID. Reply: subunit_plug_ID, start_of_music_plug_ID (2),
// end_of_music_plug_ID (2), then 5-byte music_plug_info entries (Figures 7.11-7.13, 7.15, 7.16).
// ---------------------------------------------------------------------------

/// One music_plug_info: a sequence of the plug's isochronous stream (Figure 7.13). The order
/// of the entries is the order of the sequences in the stream (§7.3.1.3).
struct MusicPlugInfoEntry {
    MusicPlugTypeField plugType{};
    MusicPlugId plugId{MusicPlugId::Unspecified()};
    StreamPosition position{};

    friend constexpr bool operator==(const MusicPlugInfoEntry&, const MusicPlugInfoEntry&) noexcept = default;
};

/// A reply to DESTINATION or SOURCE CONFIGURATIONS.
struct PlugConfigurations {
    SubunitPlugId subunitPlug{SubunitPlugId::NoPlug()};
    MusicPlugId startOfMusicPlugId{MusicPlugId::Unspecified()};
    MusicPlugId endOfMusicPlugId{MusicPlugId::Unspecified()};
    std::vector<MusicPlugInfoEntry> plugs;
};

template <Opcode kOperandOpcode>
struct PlugConfigurationsOperands {
    static constexpr Opcode kOpcode = kOperandOpcode;

    /// The subunit destination plug (DESTINATION CONFIGURATIONS) or source plug (SOURCE CONFIGURATIONS) asked about.
    SubunitPlugId subunitPlug{SubunitPlugId::NoPlug()};

    using Reply = PlugConfigurations;

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        return music_detail::RequireMusicSubunit(address);
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) return Fail(AvcErrorKind::kInvalidArgument);
        return w.Append(subunitPlug.Raw());
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        constexpr size_t kHeaderBytes = 5;
        constexpr size_t kInfoBytes = 5;
        if (in.size() < kHeaderBytes) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        Reply reply{.subunitPlug = SubunitPlugId::FromRaw(in[0]),
                    .startOfMusicPlugId = MusicPlugId::FromRaw(music_detail::BigEndian16(in[1], in[2])),
                    .endOfMusicPlugId = MusicPlugId::FromRaw(music_detail::BigEndian16(in[3], in[4])),
                    .plugs = {}};
        // The spec gives no entry count; whole entries are counted, and up to three trailing bytes
        // are the response's quadlet padding (AvcFrame.hpp).
        const size_t count = (in.size() - kHeaderBytes) / kInfoBytes;
        reply.plugs.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto bytes = in.subspan(kHeaderBytes + i * kInfoBytes);
            reply.plugs.push_back(MusicPlugInfoEntry{
                .plugType = MusicPlugTypeField::FromRaw(bytes[0]),
                .plugId = MusicPlugId::FromRaw(music_detail::BigEndian16(bytes[1], bytes[2])),
                .position = StreamPosition::FromBytes(bytes[3], bytes[4]),
            });
        }
        return reply;
    }
};

using DestinationConfigurationsCommand = Command<PlugConfigurationsOperands<Opcode::kDestinationConfigurations>>;
using SourceConfigurationsCommand = Command<PlugConfigurationsOperands<Opcode::kSourceConfigurations>>;

/// STATUS: the sequences of the stream arriving on subunit destination plug `plug` (§7.3).
[[nodiscard]] constexpr DestinationConfigurationsCommand QueryDestinationConfigurations(
    SubunitAddress music, SubunitPlugId plug) noexcept {
    return DestinationConfigurationsCommand{.address = music, .operands = {.subunitPlug = plug}};
}

/// STATUS: the sequences of the stream leaving subunit source plug `plug` (§7.4).
[[nodiscard]] constexpr SourceConfigurationsCommand QuerySourceConfigurations(SubunitAddress music,
                                                                              SubunitPlugId plug) noexcept {
    return SourceConfigurationsCommand{.address = music, .operands = {.subunitPlug = plug}};
}

// ---------------------------------------------------------------------------
// MUSIC PLUG INFO (§7.5)
// Request: music_plug_type. Reply: FF, number_of_music_plug_type_info, then 5-byte entries
// {music_plug_type, number_of_music_input_plug (2), number_of_music_output_plug (2)}
// (Figures 7.18-7.20).
// ---------------------------------------------------------------------------

/// How many music input and output plugs the subunit has of one plug type (Figure 7.20).
struct MusicPlugCounts {
    MusicPlugTypeField plugType{};
    uint16_t inputPlugs{0};
    uint16_t outputPlugs{0};

    friend constexpr bool operator==(const MusicPlugCounts&, const MusicPlugCounts&) noexcept = default;
};

struct MusicPlugInfo {
    std::vector<MusicPlugCounts> types;

    /// The counts of one plug type, if the reply lists it.
    [[nodiscard]] const MusicPlugCounts* Find(MusicPlugType type) const noexcept {
        for (const auto& counts : types) {
            if (counts.plugType == MusicPlugTypeField::Of(type)) return &counts;
        }
        return nullptr;
    }
};

struct MusicPlugInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kMusicPlugInfo;

    /// The plug type asked about; nullopt asks for "all kind of plugs" (Table 7.18).
    std::optional<MusicPlugType> plugType{std::nullopt};

    using Reply = MusicPlugInfo;

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        return music_detail::RequireMusicSubunit(address);
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) return Fail(AvcErrorKind::kInvalidArgument);
        return w.Append(plugType ? static_cast<uint8_t>(*plugType) : kAllMusicPlugTypes);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        constexpr size_t kHeaderBytes = 2;
        constexpr size_t kTypeInfoBytes = 5;
        if (in.size() < kHeaderBytes) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        const size_t count = in[1];
        if (in.size() < kHeaderBytes + count * kTypeInfoBytes) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        Reply reply;
        reply.types.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto bytes = in.subspan(kHeaderBytes + i * kTypeInfoBytes);
            reply.types.push_back(MusicPlugCounts{
                .plugType = MusicPlugTypeField::FromRaw(bytes[0]),
                .inputPlugs = music_detail::BigEndian16(bytes[1], bytes[2]),
                .outputPlugs = music_detail::BigEndian16(bytes[3], bytes[4]),
            });
        }
        return reply;
    }
};

using MusicPlugInfoCommand = Command<MusicPlugInfoOperands>;

/// STATUS: how many music plugs of `type` the subunit has (§7.5.1).
[[nodiscard]] constexpr MusicPlugInfoCommand QueryMusicPlugInfo(SubunitAddress music, MusicPlugType type) noexcept {
    return MusicPlugInfoCommand{.address = music, .operands = {.plugType = type}};
}

/// STATUS: how many music plugs of every type the subunit has (Table 7.18, FF = all kinds).
[[nodiscard]] constexpr MusicPlugInfoCommand QueryAllMusicPlugInfo(SubunitAddress music) noexcept {
    return MusicPlugInfoCommand{.address = music, .operands = {.plugType = std::nullopt}};
}

// ---------------------------------------------------------------------------
// CURRENT CAPABILITY (§7.6)
// Request: music_plug_direction, music_plug_type, FF, start_of_music_plug_ID (2),
// end_of_music_plug_ID (2). Reply: the same seven bytes with music_plug_attribute in the
// third, then 4-byte music_plug_format_info {music_plug_ID (2), format_info (2)}
// (Figures 7.21-7.23).
//
// The spec is inconsistent about the entry size: Figure 7.22 says 3 bytes, Figure 7.23 lays out
// 2 + 2. The fields add up to 4, so 4 is what is read.
// ---------------------------------------------------------------------------

/// format_info (Figures 7.24-7.27; the audio form is Figure 5.9). Two bytes whose meaning
/// depends on the plug's type; the views read them as each type's layout.
struct MusicFormatInfo {
    static constexpr uint8_t kRxMask = 0x01;   ///< Rx (Tables 7.21, 7.22) and Bus (Table 7.23): bit 0
    static constexpr uint8_t kTxMask = 0x02;   ///< Tx (Tables 7.21, 7.22) and Ex (Table 7.23): bit 1
    static constexpr uint8_t kMidiVersionShift = 4;  ///< MIDI_version is the high nibble (Figure 7.24)
    static constexpr uint8_t kMidiRevisionMask = 0x0F;

    std::array<uint8_t, 2> bytes{};

    /// Audio: the FDF and the AM824 label (Figure 5.9, Table 5.7).
    struct AudioFormat {
        uint8_t fdf;
        uint8_t am824Label;
    };
    [[nodiscard]] constexpr AudioFormat AsAudio() const noexcept { return {bytes[0], bytes[1]}; }

    /// MIDI: version, revision and adaptation layer version (Figure 7.24, Table 5.9).
    struct MidiFormat {
        uint8_t version;
        uint8_t revision;
        uint8_t adaptationLayerVersion;
    };
    [[nodiscard]] constexpr MidiFormat AsMidi() const noexcept {
        return {static_cast<uint8_t>(bytes[0] >> kMidiVersionShift),
                static_cast<uint8_t>(bytes[0] & kMidiRevisionMask), bytes[1]};
    }

    /// SMPTE time code and sample count: can the subunit receive and transmit it (Tables 7.21, 7.22).
    struct TransferFlags {
        bool receive;
        bool transmit;
    };
    [[nodiscard]] constexpr TransferFlags AsTransfer() const noexcept {
        return {(bytes[0] & kRxMask) != 0, (bytes[0] & kTxMask) != 0};
    }

    /// Audio SYNC: can the subunit take sync from the 1394 bus, and from an external source (Table 7.23).
    struct SyncFlags {
        bool bus;
        bool external;
    };
    [[nodiscard]] constexpr SyncFlags AsSync() const noexcept {
        return {(bytes[0] & kRxMask) != 0, (bytes[0] & kTxMask) != 0};
    }

    friend constexpr bool operator==(const MusicFormatInfo&, const MusicFormatInfo&) noexcept = default;
};

/// music_plug_format_info (Figure 7.23).
struct MusicPlugFormat {
    MusicPlugId plugId{MusicPlugId::Unspecified()};
    MusicFormatInfo format{};

    friend constexpr bool operator==(const MusicPlugFormat&, const MusicPlugFormat&) noexcept = default;
};

struct CurrentCapability {
    uint8_t direction{0};  ///< music_plug_direction, raw (Table 7.19)
    MusicPlugTypeField plugType{};
    uint8_t attribute{0};  ///< music_plug_attribute, raw (Table 7.20)
    MusicPlugId startOfMusicPlugId{MusicPlugId::Unspecified()};
    MusicPlugId endOfMusicPlugId{MusicPlugId::Unspecified()};
    std::vector<MusicPlugFormat> formats;

    [[nodiscard]] std::optional<MusicPlugDirection> Direction() const noexcept {
        if (direction > static_cast<uint8_t>(MusicPlugDirection::kOutput)) return std::nullopt;
        return static_cast<MusicPlugDirection>(direction);
    }
    [[nodiscard]] std::optional<MusicPlugAttribute> Attribute() const noexcept {
        if (attribute > static_cast<uint8_t>(MusicPlugAttribute::kCompound)) return std::nullopt;
        return static_cast<MusicPlugAttribute>(attribute);
    }
};

struct CurrentCapabilityOperands {
    static constexpr Opcode kOpcode = Opcode::kCurrentCapability;

    MusicPlugDirection direction{MusicPlugDirection::kInput};
    MusicPlugType plugType{MusicPlugType::kAudio};
    MusicPlugId startOfMusicPlugId{MusicPlugId::Of(0)};
    MusicPlugId endOfMusicPlugId{MusicPlugId::Of(0)};

    using Reply = CurrentCapability;

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        return music_detail::RequireMusicSubunit(address);
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) return Fail(AvcErrorKind::kInvalidArgument);
        const std::array<uint8_t, kRequestBytes> bytes = {
            static_cast<uint8_t>(direction), static_cast<uint8_t>(plugType), kMusicNotApplicable,
            startOfMusicPlugId.High(), startOfMusicPlugId.Low(), endOfMusicPlugId.High(), endOfMusicPlugId.Low()};
        return w.Append(bytes);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        constexpr size_t kFormatBytes = 4;
        if (in.size() < kRequestBytes) return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        Reply reply{.direction = in[0],
                    .plugType = MusicPlugTypeField::FromRaw(in[1]),
                    .attribute = in[2],
                    .startOfMusicPlugId = MusicPlugId::FromRaw(music_detail::BigEndian16(in[3], in[4])),
                    .endOfMusicPlugId = MusicPlugId::FromRaw(music_detail::BigEndian16(in[5], in[6])),
                    .formats = {}};
        // Whole entries only: up to three trailing bytes are the response's quadlet padding.
        const size_t count = (in.size() - kRequestBytes) / kFormatBytes;
        reply.formats.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto bytes = in.subspan(kRequestBytes + i * kFormatBytes);
            reply.formats.push_back(MusicPlugFormat{
                .plugId = MusicPlugId::FromRaw(music_detail::BigEndian16(bytes[0], bytes[1])),
                .format = MusicFormatInfo{{bytes[2], bytes[3]}},
            });
        }
        return reply;
    }

private:
    static constexpr size_t kRequestBytes = 7;  // Figure 7.21: direction, type, FF, start (2), end (2)
};

using CurrentCapabilityCommand = Command<CurrentCapabilityOperands>;

/// STATUS: the format currently used by music plugs `first`..`last` of `direction` and `type` (§7.6.1).
[[nodiscard]] constexpr CurrentCapabilityCommand QueryCurrentCapability(SubunitAddress music,
                                                                       MusicPlugDirection direction,
                                                                       MusicPlugType type, MusicPlugId first,
                                                                       MusicPlugId last) noexcept {
    return CurrentCapabilityCommand{.address = music,
                                    .operands = {.direction = direction,
                                                 .plugType = type,
                                                 .startOfMusicPlugId = first,
                                                 .endOfMusicPlugId = last}};
}

} // namespace ASFW::AVC::Cmd
