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

Expected<CommandFrame> BuildExtendedPlugInfoStatus(SubunitAddress subunit, const PlugAddress& plug,
                                                   InfoType type,
                                                   std::optional<uint8_t> extra) noexcept {
    if (type == InfoType::kClusterInfo && !extra.has_value()) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }

    const auto plugBytes = plug.Encode();

    if (extra.has_value()) {
        const std::array<uint8_t, 8> operands = {
            kExtendedPlugInfoSubfunction,
            plugBytes[0], plugBytes[1], plugBytes[2], plugBytes[3], plugBytes[4],
            static_cast<uint8_t>(type),
            *extra,
        };
        return CommandFrame::Make(CommandType::kStatus, subunit, Opcode::kPlugInfo, operands);
    }

    const std::array<uint8_t, 7> operands = {
        kExtendedPlugInfoSubfunction,
        plugBytes[0], plugBytes[1], plugBytes[2], plugBytes[3], plugBytes[4],
        static_cast<uint8_t>(type),
    };
    return CommandFrame::Make(CommandType::kStatus, subunit, Opcode::kPlugInfo, operands);
}

Expected<std::span<const uint8_t>> ExtendedPlugInfoData(std::span<const uint8_t> operands,
                                                        InfoType type) noexcept {
    if (operands.size() < 7) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != kExtendedPlugInfoSubfunction) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }
    if (operands[6] != static_cast<uint8_t>(type)) {
        return FailAt(AvcErrorKind::kMalformedOperands, 6);
    }

    return operands.subspan(7);
}

Expected<std::span<const uint8_t>> ExtendedPlugInfoData(const Response& response,
                                                        InfoType type) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    return ExtendedPlugInfoData(*operandsRes, type);
}

Expected<PlugType> ParsePlugType(std::span<const uint8_t> operands) noexcept {
    auto dataRes = ExtendedPlugInfoData(operands, InfoType::kPlugType);
    if (!dataRes) {
        return std::unexpected(dataRes.error());
    }
    if (dataRes->empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 7);
    }
    return static_cast<PlugType>((*dataRes)[0]);
}

Expected<PlugType> ParsePlugType(const Response& response) noexcept {
    return ParsePlugType(response.operands);
}

Expected<uint8_t> ParseChannelCount(std::span<const uint8_t> operands) noexcept {
    auto dataRes = ExtendedPlugInfoData(operands, InfoType::kChannelCount);
    if (!dataRes) {
        return std::unexpected(dataRes.error());
    }
    if (dataRes->empty()) {
        return FailAt(AvcErrorKind::kOperandsTooShort, 7);
    }
    return (*dataRes)[0];
}

Expected<uint8_t> ParseChannelCount(const Response& response) noexcept {
    return ParseChannelCount(response.operands);
}

Expected<ChannelPositions> ParseChannelPositions(std::span<const uint8_t> operands) noexcept {
    auto dataRes = ExtendedPlugInfoData(operands, InfoType::kChannelPositions);
    if (!dataRes) {
        return std::unexpected(dataRes.error());
    }
    const auto data = *dataRes;
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

Expected<ChannelPositions> ParseChannelPositions(const Response& response) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    return ParseChannelPositions(*operandsRes);
}

Expected<PortType> ParseClusterPortType(std::span<const uint8_t> operands,
                                        uint8_t requestedSectionId) noexcept {
    auto dataRes = ExtendedPlugInfoData(operands, InfoType::kClusterInfo);
    if (!dataRes) {
        return std::unexpected(dataRes.error());
    }
    if (dataRes->size() < 2) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(7 + dataRes->size()));
    }
    if ((*dataRes)[0] != requestedSectionId) {
        return FailAt(AvcErrorKind::kMalformedOperands, 7);
    }
    return static_cast<PortType>((*dataRes)[1]);
}

Expected<PortType> ParseClusterPortType(const Response& response,
                                        uint8_t requestedSectionId) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    return ParseClusterPortType(*operandsRes, requestedSectionId);
}

} // namespace ASFW::AVC::BridgeCo
