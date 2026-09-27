// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SignalSourceCommand.hpp - SIGNAL SOURCE (0x1A), Connection and Compatibility
// Management: which plug feeds a destination plug. Used to read and select a
// device's sync (clock) source.
//
// Layout: ta1394 ccm/src/lib.rs (MIT): SignalUnitAddr :10-60, SignalSource :161-212,
// OPCODE :213. Fresh implementation; no reference code copied.
//
// Operands: [first byte][source address, 2][destination address, 2]
//   STATUS:  source = FF FE (wildcard; the response fills it). ta1394 :183.
//   CONTROL: source and destination given.
//   first byte: ta1394 sends 0xFF for both. The Orpheus fork (Prism control panel
//   trace) sends 0x0F. In a response it carries status bits this layer returns raw
//   (no CCM spec on hand to decode them). The phase-1 hardware matrix records
//   what Duet and Phase 88 answer to each.
//
// Implementation: SignalSourceCommand.cpp (phase 1).

#pragma once

#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstdint>

namespace ASFW::AVC::Cmd {

/// A 2-byte signal address: [subunit address byte][plug byte].
/// Unit plugs: byte 0 = 0xFF; plug byte bit 7 = external plug, bits 6..0 = plug id
/// (ta1394 ccm lib.rs:30-31). Subunit plugs: [subunit address][plug id].
struct SignalAddress {
    std::array<uint8_t, 2> bytes{0xFF, 0xFE};

    [[nodiscard]] static constexpr SignalAddress UnitIsochronousPlug(uint8_t plugId) noexcept {
        return SignalAddress{{0xFF, static_cast<uint8_t>(plugId & 0x7F)}};
    }
    [[nodiscard]] static constexpr SignalAddress UnitExternalPlug(uint8_t plugId) noexcept {
        return SignalAddress{{0xFF, static_cast<uint8_t>(0x80 | (plugId & 0x7F))}};
    }
    [[nodiscard]] static constexpr SignalAddress SubunitPlug(SubunitAddress subunit, uint8_t plugId) noexcept {
        return SignalAddress{{subunit.Byte(), plugId}};
    }
    /// STATUS source wildcard FF FE (ta1394 ccm lib.rs:183).
    [[nodiscard]] static constexpr SignalAddress StatusWildcard() noexcept { return SignalAddress{{0xFF, 0xFE}}; }

    [[nodiscard]] constexpr bool IsUnit() const noexcept { return bytes[0] == 0xFF; }
    [[nodiscard]] constexpr bool IsExternalUnitPlug() const noexcept { return IsUnit() && (bytes[1] & 0x80) != 0; }
    [[nodiscard]] constexpr uint8_t PlugId() const noexcept {
        return IsUnit() ? static_cast<uint8_t>(bytes[1] & 0x7F) : bytes[1];
    }
    [[nodiscard]] constexpr SubunitAddress Subunit() const noexcept { return SubunitAddress::FromByte(bytes[0]); }

    friend constexpr bool operator==(const SignalAddress&, const SignalAddress&) noexcept = default;
};

inline constexpr uint8_t kSignalSourceFirstByteDefault = 0xFF;  ///< ta1394
inline constexpr uint8_t kSignalSourceFirstBytePrism = 0x0F;    ///< Orpheus fork / Prism panel

struct SignalSource {
    uint8_t firstByte{0xFF};  ///< raw; see file comment
    SignalAddress source{};
    SignalAddress destination{};
};

/// Frame address: the unit (0xFF). ta1394 addresses SIGNAL SOURCE at the unit.
[[nodiscard]] Expected<CommandFrame> BuildSignalSourceStatus(
    SignalAddress destination, uint8_t firstByte = kSignalSourceFirstByteDefault) noexcept;

[[nodiscard]] Expected<CommandFrame> BuildSignalSourceControl(
    SignalAddress source, SignalAddress destination,
    uint8_t firstByte = kSignalSourceFirstByteDefault) noexcept;

/// `expected`: kImplementedStable for STATUS, kAccepted for CONTROL. Needs at least
/// 5 operands (ta1394 LENGTH_MIN).
[[nodiscard]] Expected<SignalSource> ParseSignalSource(const Response& response, ResponseCode expected) noexcept;

} // namespace ASFW::AVC::Cmd
