// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcFrame.cpp - AV/C command frame assembly and response parsing.
//
// Layouts and response validation: Linux ta1394 general/src/lib.rs:470-501.
// Fresh clean-room implementation.

#include "AvcFrame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC {

Expected<CommandFrame> CommandFrame::Make(CommandType type, SubunitAddress address, Opcode opcode,
                                          std::span<const uint8_t> operands) noexcept {
    if (operands.size() > kMaxOperandBytes) {
        return Fail(AvcErrorKind::kFrameTooLong);
    }
    if (address.IsExtendedType()) {
        return Fail(AvcErrorKind::kUnsupported);
    }

    CommandFrame frame;
    frame.bytes_[0] = static_cast<uint8_t>(type) & kCodeMask;
    frame.bytes_[1] = address.Byte();
    frame.bytes_[2] = static_cast<uint8_t>(opcode);

    if (!operands.empty()) {
        std::copy(operands.begin(), operands.end(), frame.bytes_.begin() + kHeaderBytes);
    }

    frame.size_ = static_cast<uint16_t>(kHeaderBytes + operands.size());
    const size_t padded = (static_cast<size_t>(frame.size_) + 3u) & ~size_t{3};
    for (size_t i = frame.size_; i < padded; ++i) {
        frame.bytes_[i] = 0;
    }

    return frame;
}

Expected<Response> ParseResponse(std::span<const uint8_t> frame) noexcept {
    if (frame.size() < kHeaderBytes) {
        return Fail(AvcErrorKind::kFrameTooShort);
    }
    if (frame.size() > kMaxFrameBytes) {
        return Fail(AvcErrorKind::kFrameTooLong);
    }

    if ((frame[0] & kCtsMask) != 0) {
        return Fail(AvcErrorKind::kNotAResponse);
    }

    const uint8_t codeNibble = frame[0] & kCodeMask;
    if (codeNibble < kFirstResponseCode) {
        return Fail(AvcErrorKind::kNotAResponse);
    }

    switch (codeNibble) {
        case static_cast<uint8_t>(ResponseCode::kNotImplemented):
        case static_cast<uint8_t>(ResponseCode::kAccepted):
        case static_cast<uint8_t>(ResponseCode::kRejected):
        case static_cast<uint8_t>(ResponseCode::kInTransition):
        case static_cast<uint8_t>(ResponseCode::kImplementedStable):
        case static_cast<uint8_t>(ResponseCode::kChanged):
        case static_cast<uint8_t>(ResponseCode::kInterim):
            break;
        default:
            return Fail(AvcErrorKind::kNotAResponse);
    }

    return Response{
        .code = static_cast<ResponseCode>(codeNibble),
        .address = SubunitAddress::FromByte(frame[1]),
        .opcode = static_cast<Opcode>(frame[2]),
        .operands = frame.subspan(kHeaderBytes),
    };
}

Expected<Response> ParseResponseFor(const CommandFrame& command,
                                    std::span<const uint8_t> frame) noexcept {
    auto resp = ParseResponse(frame);
    if (!resp) {
        return resp;
    }
    if (resp->address != command.Address()) {
        return Fail(AvcErrorKind::kAddressMismatch);
    }
    if (resp->opcode != command.OpcodeValue()) {
        return Fail(AvcErrorKind::kOpcodeMismatch);
    }
    return resp;
}

} // namespace ASFW::AVC
