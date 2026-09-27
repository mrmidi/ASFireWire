// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GeneralCommands.cpp - AV/C General commands: UNIT INFO, SUBUNIT INFO,
// PLUG INFO, INPUT/OUTPUT PLUG SIGNAL FORMAT, VENDOR-DEPENDENT.
//
// Layouts and logic citations:
// - UNIT INFO: Linux ta1394 general/src/general.rs:37-77 (UnitInfo).
// - SUBUNIT INFO: ta1394 general.rs:130-192 (SubunitInfo).
// - PLUG INFO: ta1394 general.rs:378-434 (PlugInfo).
// - PLUG SIGNAL FORMAT: TA 2004006 General 4.2 §10.10, §10.11; ta1394 general.rs:521-580; Linux fcp.c:58-106.
// - VENDOR-DEPENDENT: ta1394 general.rs:226-258 (VendorDependent).
// Fresh clean-room implementation.

#include "GeneralCommands.hpp"

#include "../Core/OperandPack.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// UNIT INFO (0x30)
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildUnitInfoStatus() noexcept {
    constexpr std::array<uint8_t, 5> kOperands = {0x07, 0xFF, 0xFF, 0xFF, 0xFF};
    return CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kUnitInfo, kOperands);
}

Expected<UnitInfo> ParseUnitInfo(const Response& response) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != 0x07) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }

    return UnitInfo{
        .unitType = static_cast<SubunitType>((operands[1] >> 3) & 0x1F),
        .unitId = static_cast<uint8_t>(operands[1] & 0x07),
        .companyId = CompanyId{operands[2], operands[3], operands[4]},
    };
}

// ---------------------------------------------------------------------------
// SUBUNIT INFO (0x31)
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildSubunitInfoStatus(uint8_t page, uint8_t extensionCode) noexcept {
    const std::array<uint8_t, 5> operands = {
        static_cast<uint8_t>(((page & 0x07) << 4) | (extensionCode & 0x07)),
        0xFF, 0xFF, 0xFF, 0xFF
    };
    return CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kSubunitInfo, operands);
}

Expected<SubunitInfo> ParseSubunitInfo(const Response& response) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }

    SubunitInfo info{
        .page = static_cast<uint8_t>((operands[0] >> 4) & 0x07),
        .extensionCode = static_cast<uint8_t>(operands[0] & 0x07),
        .entries = {},
        .entryCount = 0,
    };

    for (size_t i = 0; i < 4; ++i) {
        const uint8_t entryByte = operands[1 + i];
        if (entryByte == 0xFF) {
            break;
        }
        info.entries[info.entryCount++] = SubunitInfoEntry{
            .type = static_cast<SubunitType>((entryByte >> 3) & 0x1F),
            .maximumId = static_cast<uint8_t>(entryByte & 0x07),
        };
    }

    return info;
}

// ---------------------------------------------------------------------------
// PLUG INFO (0x02)
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildUnitPlugInfoStatus(UnitPlugInfoKind kind) noexcept {
    const std::array<uint8_t, 5> operands = {
        static_cast<uint8_t>(kind), 0xFF, 0xFF, 0xFF, 0xFF
    };
    return CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kPlugInfo, operands);
}

Expected<CommandFrame> BuildSubunitPlugInfoStatus(SubunitAddress subunit) noexcept {
    if (subunit.IsUnit()) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }
    constexpr std::array<uint8_t, 5> kOperands = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    return CommandFrame::Make(CommandType::kStatus, subunit, Opcode::kPlugInfo, kOperands);
}

Expected<UnitIsochronousExternalPlugs> ParseUnitIsochronousExternalPlugs(const Response& response) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != 0x00) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }

    return UnitIsochronousExternalPlugs{
        .isochronousInputs = operands[1],
        .isochronousOutputs = operands[2],
        .externalInputs = operands[3],
        .externalOutputs = operands[4],
    };
}

Expected<UnitAsynchronousPlugs> ParseUnitAsynchronousPlugs(const Response& response) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 3) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != 0x01) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }

    return UnitAsynchronousPlugs{
        .asynchronousInputs = operands[1],
        .asynchronousOutputs = operands[2],
    };
}

Expected<SubunitPlugs> ParseSubunitPlugs(const Response& response) noexcept {
    auto operandsRes = OperandsIf(response, ResponseCode::kImplementedStable);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 3) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }
    if (operands[0] != 0x00) {
        return FailAt(AvcErrorKind::kMalformedOperands, 0);
    }

    return SubunitPlugs{
        .destinationPlugs = operands[1],
        .sourcePlugs = operands[2],
    };
}

// ---------------------------------------------------------------------------
// INPUT / OUTPUT PLUG SIGNAL FORMAT (0x19 / 0x18)
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildPlugSignalFormatStatus(PlugSignalDirection direction, uint8_t plugId,
                                                   SignalFormatQuery query) noexcept {
    const Opcode opcode = (direction == PlugSignalDirection::kInput)
                              ? Opcode::kInputPlugSignalFormat
                              : Opcode::kOutputPlugSignalFormat;
    const std::array<uint8_t, 5> operands = {
        plugId,
        (query == SignalFormatQuery::kAm824Wildcard) ? kFmtAm824 : uint8_t{0xFF},
        0xFF, 0xFF, 0xFF
    };
    return CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), opcode, operands);
}

Expected<CommandFrame> BuildPlugSignalFormatControl(PlugSignalDirection direction,
                                                    const PlugSignalFormat& format) noexcept {
    const Opcode opcode = (direction == PlugSignalDirection::kInput)
                              ? Opcode::kInputPlugSignalFormat
                              : Opcode::kOutputPlugSignalFormat;
    const std::array<uint8_t, 5> operands = {
        format.plugId,
        format.fmt,
        format.fdf[0],
        format.fdf[1],
        format.fdf[2]
    };
    return CommandFrame::Make(CommandType::kControl, SubunitAddress::Unit(), opcode, operands);
}

Expected<PlugSignalFormat> ParsePlugSignalFormat(const Response& response, ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }

    return PlugSignalFormat{
        .plugId = operands[0],
        .fmt = operands[1],
        .fdf = {operands[2], operands[3], operands[4]},
    };
}

// ---------------------------------------------------------------------------
// VENDOR-DEPENDENT (0x00)
// ---------------------------------------------------------------------------

Expected<CommandFrame> BuildVendorDependent(CommandType type, SubunitAddress address,
                                            const CompanyId& companyId,
                                            std::span<const uint8_t> payload) noexcept {
    if (payload.empty()) {
        return Fail(AvcErrorKind::kInvalidArgument);
    }
    if (3u + payload.size() > kMaxOperandBytes) {
        return Fail(AvcErrorKind::kFrameTooLong);
    }

    std::array<uint8_t, kMaxOperandBytes> operands{};
    operands[0] = companyId[0];
    operands[1] = companyId[1];
    operands[2] = companyId[2];
    std::copy(payload.begin(), payload.end(), operands.begin() + 3);

    return CommandFrame::Make(type, address, Opcode::kVendorDependent,
                              std::span<const uint8_t>{operands.data(), 3u + payload.size()});
}

Expected<VendorDependentReply> ParseVendorDependent(const Response& response, ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() <= 3) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }

    return VendorDependentReply{
        .companyId = CompanyId{operands[0], operands[1], operands[2]},
        .payload = operands.subspan(3),
    };
}

} // namespace ASFW::AVC::Cmd
