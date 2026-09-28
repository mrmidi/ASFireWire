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
// :711-722 (port types), :991-1020 (subfunction). Fresh clean-room implementation.
//
// Frame: [01][subunit address][02][C0][plug address, 5][info type][info data...]
//   The 5-byte plug address is the stream-format PlugAddress (same bytes: Linux
//   bebob.h:172-195 vs ta1394 stream-format PlugAddr), so it is reused here.
//   Response info data starts at operand 7 (frame byte 10): Linux reads the plug
//   type at buf[10] (:139), the channel count at buf[10] (:175).
//   Cluster (section) info sends the 1-based section id at operand 7 and reads the
//   section type at operand 8 (:228, :246). Linux zero-fills the rest (kzalloc).
//
// Used by BeBoB discovery. Never send to
// M-Audio special firmware (AVC_DEVICE_HAZARDS.md); the per-unit allowlist
// enforces that in the transaction engine, not here.

#pragma once

#include "../Commands/StreamFormatCommand.hpp"
#include "../Core/AvcCommand.hpp"
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

/// Channel positions: [section count] then per section [channel count]
/// ([stream position, 1-based][location in section, 1-based])...
/// Linux bebob_stream.c map_data_channels :254-376. Positions are returned
/// ZERO-based, matching the existing channel-position parser.
struct ChannelPosition {
    uint8_t streamPosition{0};
    uint8_t sectionLocation{0};
    friend constexpr bool operator==(const ChannelPosition&, const ChannelPosition&) noexcept = default;
};

inline constexpr size_t kMaxSections = 16;
inline constexpr size_t kMaxPositionsPerSection = 32;

struct ChannelSection {
    std::array<ChannelPosition, kMaxPositionsPerSection> positions{};
    uint8_t positionCount{0};
    friend constexpr bool operator==(const ChannelSection&, const ChannelSection&) noexcept = default;
};

struct ChannelPositions {
    std::array<ChannelSection, kMaxSections> sections{};
    uint8_t sectionCount{0};
    friend constexpr bool operator==(const ChannelPositions&, const ChannelPositions&) noexcept = default;
};

struct ExtendedPlugInfoReply {
    InfoType type{InfoType::kPlugType};
    std::array<uint8_t, kMaxOperandBytes> bytes{};
    uint16_t length{0};

    [[nodiscard]] std::span<const uint8_t> Data() const noexcept {
        return {bytes.data(), length};
    }

    [[nodiscard]] Expected<PlugType> AsPlugType() const noexcept;
    [[nodiscard]] Expected<uint8_t> AsChannelCount() const noexcept;
    [[nodiscard]] Expected<ChannelPositions> AsChannelPositions() const noexcept;
    [[nodiscard]] Expected<PortType> AsClusterPortType(uint8_t requestedSectionId) const noexcept;
};

// ===========================================================================
// Extended Plug Info Operands (AvcOperands)
// ===========================================================================

struct ExtendedPlugInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kPlugInfo;

    PlugAddress plug{};
    InfoType type{InfoType::kPlugType};
    std::optional<uint8_t> extra{std::nullopt};

    using Reply = ExtendedPlugInfoReply;

    [[nodiscard]] Expected<void> Write(Cmd::OperandWriter& w, CommandType t) const noexcept;
    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in, InfoType expectedType) noexcept;
};

using ExtendedPlugInfoCommand = Cmd::Command<ExtendedPlugInfoOperands>;

} // namespace ASFW::AVC::BridgeCo
