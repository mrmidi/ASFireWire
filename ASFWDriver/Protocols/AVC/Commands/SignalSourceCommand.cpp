// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SignalSourceCommand.cpp - SIGNAL SOURCE (0x1A), Connection and Compatibility
// Management: which plug feeds a destination plug.
//
// Layouts and logic citations:
// - ta1394 ccm/src/lib.rs (MIT): SignalUnitAddr :10-60, SignalSource :161-212, OPCODE :213.
// Fresh clean-room implementation.

#include "SignalSourceCommand.hpp"

#include <array>
#include <cstdint>

namespace ASFW::AVC::Cmd {

Expected<CommandFrame> BuildSignalSourceStatus(SignalAddress destination, uint8_t firstByte) noexcept {
    const auto wildcard = SignalAddress::StatusWildcard();
    const std::array<uint8_t, 5> operands = {
        firstByte,
        wildcard.bytes[0], wildcard.bytes[1],
        destination.bytes[0], destination.bytes[1],
    };

    return CommandFrame::Make(CommandType::kStatus, SubunitAddress::Unit(), Opcode::kSignalSource, operands);
}

Expected<CommandFrame> BuildSignalSourceControl(SignalAddress source, SignalAddress destination,
                                                uint8_t firstByte) noexcept {
    const std::array<uint8_t, 5> operands = {
        firstByte,
        source.bytes[0], source.bytes[1],
        destination.bytes[0], destination.bytes[1],
    };

    return CommandFrame::Make(CommandType::kControl, SubunitAddress::Unit(), Opcode::kSignalSource, operands);
}

Expected<SignalSource> ParseSignalSource(const Response& response, ResponseCode expected) noexcept {
    auto operandsRes = OperandsIf(response, expected);
    if (!operandsRes) {
        return std::unexpected(operandsRes.error());
    }
    const auto& operands = *operandsRes;
    if (operands.size() < 5) {
        return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(operands.size()));
    }

    return SignalSource{
        .firstByte = operands[0],
        .source = SignalAddress{{operands[1], operands[2]}},
        .destination = SignalAddress{{operands[3], operands[4]}},
    };
}

} // namespace ASFW::AVC::Cmd
