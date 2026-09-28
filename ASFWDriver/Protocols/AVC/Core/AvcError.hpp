// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcError.hpp - Errors of the AV/C codec layer.
//
// Codec functions return Expected<T>. The error says what went wrong at the
// codec level; it is NOT an IOReturn. Mapping to IOReturn happens once, at the
// transaction-engine boundary, so every caller sees the same mapping.

#pragma once

#include "AvcTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <expected>
#include <optional>

namespace ASFW::AVC {

enum class AvcErrorKind : uint8_t {
    kFrameTooShort,        ///< Fewer than 3 bytes (ctype, address, opcode).
    kFrameTooLong,         ///< More than kMaxFrameBytes, or operands that would make it so.
    kNotAResponse,         ///< Byte 0 low nibble is a command type, not a response code.
    kAddressMismatch,      ///< Response address differs from the command's.
    kOpcodeMismatch,       ///< Response opcode differs from the command's.
    kUnexpectedResponse,   ///< Well-formed, but not the response code this operation accepts.
                           ///< AvcError::response holds the code (NOT IMPLEMENTED, REJECTED, ...).
    kOperandsTooShort,     ///< operandOffset = number of operand bytes that were present.
    kMalformedOperands,    ///< operandOffset = first offending operand byte.
    kInvalidArgument,      ///< Build-time: an argument this command cannot carry
                           ///< (e.g. a subunit address for a unit-only command).
    kUnsupported,          ///< Valid per spec, not modelled here (extended addressing,
                           ///< more compound entries than the fixed capacity, ...).
    kTimeout,              ///< Command timed out waiting for FCP response.
    kBusReset,             ///< Bus reset occurred during command execution.
    kTransportError,       ///< Async 1394 transport failure.
    kRefused,              ///< Refused by command allowlist/policy.
};

struct AvcError {
    AvcErrorKind kind{AvcErrorKind::kMalformedOperands};
    std::optional<ResponseCode> response{};
    uint16_t operandOffset{0};

    [[nodiscard]] static constexpr AvcError Of(AvcErrorKind kind) noexcept { return AvcError{kind, {}, 0}; }

    [[nodiscard]] static constexpr AvcError Unexpected(ResponseCode code) noexcept {
        return AvcError{AvcErrorKind::kUnexpectedResponse, code, 0};
    }

    [[nodiscard]] static constexpr AvcError AtOperand(AvcErrorKind kind, uint16_t offset) noexcept {
        return AvcError{kind, {}, offset};
    }

    friend constexpr bool operator==(const AvcError&, const AvcError&) noexcept = default;
};

template <class T>
using Expected = std::expected<T, AvcError>;

[[nodiscard]] constexpr std::unexpected<AvcError> Fail(AvcErrorKind kind) noexcept {
    return std::unexpected(AvcError::Of(kind));
}

[[nodiscard]] constexpr std::unexpected<AvcError> FailAt(AvcErrorKind kind, uint16_t offset) noexcept {
    return std::unexpected(AvcError::AtOperand(kind, offset));
}

/// Single authority for mapping AV/C codec and transaction errors to IOReturn.
[[nodiscard]] constexpr IOReturn ToIOReturn(const AvcError& error) noexcept {
    switch (error.kind) {
        case AvcErrorKind::kTimeout: return kIOReturnTimeout;
        case AvcErrorKind::kBusReset: return kIOReturnNotResponding;
        case AvcErrorKind::kRefused: return kIOReturnNotPermitted;
        case AvcErrorKind::kInvalidArgument:
        case AvcErrorKind::kMalformedOperands:
        case AvcErrorKind::kOperandsTooShort:
        case AvcErrorKind::kFrameTooShort:
        case AvcErrorKind::kFrameTooLong:
            return kIOReturnBadArgument;
        case AvcErrorKind::kUnsupported:
            return kIOReturnUnsupported;
        case AvcErrorKind::kNotAResponse:
        case AvcErrorKind::kAddressMismatch:
        case AvcErrorKind::kOpcodeMismatch:
            return kIOReturnInvalid;
        case AvcErrorKind::kUnexpectedResponse:
            if (error.response.has_value()) {
                switch (*error.response) {
                    case ResponseCode::kNotImplemented:
                        return kIOReturnUnsupported;
                    case ResponseCode::kInTransition:
                    case ResponseCode::kInterim:
                        return kIOReturnBusy;
                    case ResponseCode::kRejected:
                        return kIOReturnError;
                    default:
                        return kIOReturnError;
                }
            }
            return kIOReturnError;
        case AvcErrorKind::kTransportError:
        default:
            return kIOReturnError;
    }
}

} // namespace ASFW::AVC
