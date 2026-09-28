// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BridgeCoPlugInfo.cpp - BridgeCo's extended PLUG INFO: PLUG INFO (0x02) with
// subfunction 0xC0. Vendor extension.
//
// Layouts and logic citations:
// - Linux firewire/bebob/bebob_command.c:91-106 (address fill, info type),
//   :109-287 (plug type, channel count, channel positions, section type).
// - ALSA userspace protocols/bebob/src/bridgeco.rs:844-851, :465-470, :711-722.
// - Linux firewire/bebob/bebob_stream.c:254-376 (channel mapping).
// Fresh clean-room implementation.

#include "BridgeCoPlugInfo.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC::BridgeCo {

// ---------------------------------------------------------------------------
// ExtendedPlugInfoReply Helpers
// ---------------------------------------------------------------------------

Expected<PlugType> ExtendedPlugInfoReply::AsPlugType() const noexcept {
    if (type != InfoType::kPlugType) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }
    if (data.empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 7);
    }
    return static_cast<PlugType>(data[0]);
}

Expected<uint8_t> ExtendedPlugInfoReply::AsChannelCount() const noexcept {
    if (type != InfoType::kChannelCount) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }
    if (data.empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 7);
    }
    return data[0];
}

Expected<ChannelPositions> ExtendedPlugInfoReply::AsChannelPositions() const noexcept {
    if (type != InfoType::kChannelPositions) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }
    if (data.empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 7);
    }

    const uint8_t sectionCount = data[0];
    if (sectionCount > kMaxSections) {
        return Fail(AvcErrorKind::kUnsupported);
    }

    ChannelPositions positions{};
    positions.sectionCount = sectionCount;
    size_t cursor = 1;

    for (size_t s = 0; s < sectionCount; ++s) {
        if (cursor >= data.size()) {
            return FailAt(AvcErrorKind::kMalformedOperands, static_cast<uint16_t>(7 + cursor));
        }
        const uint8_t positionCount = data[cursor++];
        if (positionCount > kMaxPositionsPerSection) {
            return Fail(AvcErrorKind::kUnsupported);
        }
        if (cursor + 2u * positionCount > data.size()) {
            return FailAt(AvcErrorKind::kMalformedOperands, static_cast<uint16_t>(7 + cursor));
        }
        positions.sections[s].positionCount = positionCount;
        for (size_t c = 0; c < positionCount; ++c) {
            const uint8_t streamPos = data[cursor++];
            const uint8_t secLoc = data[cursor++];
            if (streamPos == 0 || secLoc == 0) {
                return FailAt(AvcErrorKind::kMalformedOperands, static_cast<uint16_t>(7 + cursor - 2));
            }
            positions.sections[s].positions[c] = ChannelPosition{
                .streamPosition = static_cast<uint8_t>(streamPos - 1u),
                .sectionLocation = static_cast<uint8_t>(secLoc - 1u),
            };
        }
    }

    return positions;
}

Expected<PortType> ExtendedPlugInfoReply::AsClusterPortType(uint8_t requestedSectionId) const noexcept {
    if (type != InfoType::kClusterInfo) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }
    if (data.size() < 2) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(7 + data.size()));
    }
    if (data[0] != requestedSectionId) {
        return FailAt(AvcErrorKind::kMalformedOperands, 7);
    }
    return static_cast<PortType>(data[1]);
}

// ---------------------------------------------------------------------------
// ExtendedPlugInfoOperands
// ---------------------------------------------------------------------------

Expected<void> ExtendedPlugInfoOperands::Write(Cmd::OperandWriter& w, CommandType t) const noexcept {
    if (t != CommandType::kStatus) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }
    if (type == InfoType::kClusterInfo && !extra.has_value()) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }

    auto r1 = w.Append(kExtendedPlugInfoSubfunction);
    if (!r1) return r1;
    auto plugBytes = plug.Encode();
    auto r2 = w.Append(plugBytes);
    if (!r2) return r2;
    auto r3 = w.Append(static_cast<uint8_t>(type));
    if (!r3) return r3;

    if (extra.has_value()) {
        return w.Append(*extra);
    }
    return {};
}

Expected<ExtendedPlugInfoReply> ExtendedPlugInfoOperands::Read(std::span<const uint8_t> in) noexcept {
    if (in.size() < 7) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
    }
    if (in[0] != kExtendedPlugInfoSubfunction) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    return ExtendedPlugInfoReply{
        .type = static_cast<InfoType>(in[6]),
        .data = in.subspan(7),
    };
}

Expected<ExtendedPlugInfoReply> ExtendedPlugInfoOperands::Read(std::span<const uint8_t> in,
                                                              InfoType expectedType) noexcept {
    auto reply = Read(in);
    if (!reply) return reply;
    if (reply->type != expectedType) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }
    return reply;
}

} // namespace ASFW::AVC::BridgeCo
