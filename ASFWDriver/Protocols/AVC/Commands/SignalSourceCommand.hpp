// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SignalSourceCommand.hpp - SIGNAL SOURCE (0x1A) operand codec.
//
// Layout: ta1394 ccm/src/lib.rs (MIT): SignalUnitAddr :10-60, SignalSource :161-212, OPCODE :213.

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::AVC::Cmd {

/// A 2-byte signal address: [subunit address byte][plug byte].
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
    uint8_t firstByte{0xFF};
    SignalAddress source{};
    SignalAddress destination{};
};

struct SignalSourceOperands {
    static constexpr Opcode kOpcode = Opcode::kSignalSource;
    static constexpr bool kRequiresUnitAddress = true;

    SignalAddress destination{};
    std::optional<SignalAddress> source{std::nullopt};
    uint8_t firstByte{kSignalSourceFirstByteDefault};

    using Reply = SignalSource;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t == CommandType::kStatus) {
            const auto wildcard = SignalAddress::StatusWildcard();
            const std::array<uint8_t, 5> ops = {
                firstByte,
                wildcard.bytes[0], wildcard.bytes[1],
                destination.bytes[0], destination.bytes[1]
            };
            return w.Append(ops);
        }
        // FFADO avc_signal_source.cpp:137-141 serializes CONTROL and
        // SPECIFIC INQUIRY alike; phase88.json inquiry_signal_source_0xFF
        // captures firstByte=FF and the explicit candidate source.
        if (t == CommandType::kControl || t == CommandType::kSpecificInquiry) {
            if (!source.has_value()) {
                return Fail(AvcErrorKind::kInvalidArgument);
            }
            const std::array<uint8_t, 5> ops = {
                firstByte,
                source->bytes[0], source->bytes[1],
                destination.bytes[0], destination.bytes[1]
            };
            return w.Append(ops);
        }
        return Fail(AvcErrorKind::kInvalidArgument);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 5) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return SignalSource{
            .firstByte = in[0],
            .source = SignalAddress{{in[1], in[2]}},
            .destination = SignalAddress{{in[3], in[4]}},
        };
    }
};

using SignalSourceCommand = Command<SignalSourceOperands>;

} // namespace ASFW::AVC::Cmd
