// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BridgeCoPlugInfo.hpp - BridgeCo's extended PLUG INFO: PLUG INFO (0x02) with
// subfunction 0xC0. Vendor extension, so it lives outside Commands/.
//
// Layout: Linux bebob_command.c:91-106 (address fill, info type at frame byte 9),
// :109-287 (plug type, channel count, channel positions, section type, plug input);
// bebob.h:147-195 (address bytes, plug and mode enums). ALSA userspace
// protocols/bebob/src/bridgeco.rs:844-851 (info types), :465-470 (plug types),
// :711-722 (port types), :991-1020 (subfunction). Fresh implementation.
//
// Frame: [01][subunit address][02][C0][plug address, 5][info type][info data...]
//   The 5-byte plug address is the stream-format PlugAddress (same bytes: Linux
//   bebob.h:172-195 vs ta1394 stream-format PlugAddr), so it is reused here.
//   Response info data starts at operand 7 (frame byte 10): Linux reads the plug
//   type at buf[10] (:139), the channel count at buf[10] (:175).
//   Cluster (section) info sends the 1-based section id at operand 7 and reads the
//   section type at operand 8 (:228, :246). Linux zero-fills the rest (kzalloc).
//
// Used by BeBoB discovery (today: BeBoBPlug0StreamDiscovery). Never send to
// M-Audio special firmware (AVC_DEVICE_HAZARDS.md); the per-unit allowlist
// enforces that in the transaction engine, not here.
//
// Implementation: BridgeCoPlugInfo.cpp (phase 1).

#pragma once

#include "../Commands/StreamFormatCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::AVC::BridgeCo {

inline constexpr uint8_t kExtendedPlugInfoSubfunction = 0xC0;

/// bridgeco.rs:844-851.
enum class InfoType : uint8_t {
    kPlugType = 0x00,
    kPlugName = 0x01,
    kChannelCount = 0x02,
    kChannelPositions = 0x03,
    kChannelName = 0x04,
    kPlugInput = 0x05,
    kPlugOutputs = 0x06,
    kClusterInfo = 0x07,
};

/// bridgeco.rs:465-470; Linux bebob.h:162-170 (adds 0x06 "addition").
enum class PlugType : uint8_t {
    kIsochronousStream = 0x00,
    kAsynchronousStream = 0x01,
    kMidi = 0x02,
    kSync = 0x03,
    kAnalog = 0x04,
    kDigital = 0x05,
    kAddition = 0x06,
    kNone = 0xFF,
};

/// Cluster (section) port type. bridgeco.rs:711-722. Linux map_data_channels uses
/// 0x0A for MIDI (bebob_stream.c:334).
enum class PortType : uint8_t {
    kSpeaker = 0x00,
    kHeadphone = 0x01,
    kMicrophone = 0x02,
    kLine = 0x03,
    kSpdif = 0x04,
    kAdat = 0x05,
    kTdif = 0x06,
    kMadi = 0x07,
    kAnalog = 0x08,
    kDigital = 0x09,
    kMidi = 0x0A,
    kNoType = 0xFF,
};

using Cmd::PlugAddress;

/// STATUS [C0][plug][info type] plus, only when given, [extra] at operand 7.
/// kClusterInfo requires `extra` = the 1-based section id (kInvalidArgument
/// without it); other info types pass nothing. Nothing else is appended: 7 operands
/// (10-byte frame), or 8 with extra (11 bytes), as today's BeBoBPlug0StreamDiscovery
/// sends. WireBytes() zero-pads to 12, byte-identical to Linux, which sends a
/// 12-byte kzalloc'd buffer (bebob_command.c:116, :222), so trailing bytes are 0x00.
[[nodiscard]] Expected<CommandFrame> BuildExtendedPlugInfoStatus(SubunitAddress subunit, const PlugAddress& plug,
                                                                 InfoType type,
                                                                 std::optional<uint8_t> extra = std::nullopt) noexcept;

/// Common checks for every reply: subfunction C0, info type
/// echoed. Returns the info data (operands from 7). A VIEW into the response buffer.
[[nodiscard]] Expected<std::span<const uint8_t>> ExtendedPlugInfoData(
    std::span<const uint8_t> operands,
    InfoType type) noexcept;
[[nodiscard]] Expected<std::span<const uint8_t>> ExtendedPlugInfoData(
    const Response& response,
    InfoType type) noexcept;

[[nodiscard]] Expected<PlugType> ParsePlugType(std::span<const uint8_t> operands) noexcept;
[[nodiscard]] Expected<PlugType> ParsePlugType(const Response& response) noexcept;

[[nodiscard]] Expected<uint8_t> ParseChannelCount(std::span<const uint8_t> operands) noexcept;
[[nodiscard]] Expected<uint8_t> ParseChannelCount(const Response& response) noexcept;

/// Channel positions: [section count] then per section [channel count]
/// ([stream position, 1-based][location in section, 1-based])...
/// Linux bebob_stream.c map_data_channels :254-376. Positions are returned
/// ZERO-based, as today's ParseChannelPositionSections does (behaviour to keep).
struct ChannelPosition {
    uint8_t streamPosition{0};
    uint8_t sectionLocation{0};
};
inline constexpr size_t kMaxSections = 16;
inline constexpr size_t kMaxPositionsPerSection = 32;
struct ChannelSection {
    std::array<ChannelPosition, kMaxPositionsPerSection> positions{};
    uint8_t positionCount{0};
};
struct ChannelPositions {
    std::array<ChannelSection, kMaxSections> sections{};
    uint8_t sectionCount{0};
};
/// kUnsupported when a count exceeds the fixed capacity; kMalformedOperands when
/// the counts run past the data.
[[nodiscard]] Expected<ChannelPositions> ParseChannelPositions(std::span<const uint8_t> operands) noexcept;
[[nodiscard]] Expected<ChannelPositions> ParseChannelPositions(const Response& response) noexcept;

/// Section (cluster) type for the section requested with BuildExtendedPlugInfoStatus
/// (kClusterInfo, extra = 1-based section id). The reply echoes the id at operand 7
/// (kMalformedOperands at 7 on mismatch); the port type is at operand 8.
[[nodiscard]] Expected<PortType> ParseClusterPortType(
    std::span<const uint8_t> operands,
    uint8_t requestedSectionId) noexcept;
[[nodiscard]] Expected<PortType> ParseClusterPortType(
    const Response& response,
    uint8_t requestedSectionId) noexcept;

// ===========================================================================
// Typed command struct satisfying the AvcCommand concept
// ===========================================================================

struct ExtendedPlugInfoCommand {
    PlugAddress plug{};
    InfoType type{InfoType::kPlugType};
    std::optional<uint8_t> extra{std::nullopt};
    SubunitAddress subunit{kAudioSubunit0};

    using Reply = std::span<const uint8_t>;

    [[nodiscard]] Expected<CommandFrame> Encode(CommandType ctype = CommandType::kStatus) const noexcept {
        (void)ctype;
        return BuildExtendedPlugInfoStatus(subunit, plug, type, extra);
    }

    [[nodiscard]] Expected<Reply> Decode(std::span<const uint8_t> operands) const noexcept {
        return ExtendedPlugInfoData(operands, type);
    }
    [[nodiscard]] Expected<Reply> Decode(const Response& response) const noexcept {
        return ExtendedPlugInfoData(response, type);
    }
};

} // namespace ASFW::AVC::BridgeCo

