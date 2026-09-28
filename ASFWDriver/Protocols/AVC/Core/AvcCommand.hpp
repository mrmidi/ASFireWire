// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcCommand.hpp - The generic AV/C Command frame and operand codec concept.
//
// Contract:
// Every AV/C command has two separate concerns:
// 1. The frame: ctype, address, opcode, quadlet padding, response-code check.
//    Identical for all commands; written once in Command<Op>.
// 2. The operands: each command's own layout. The only per-command code.

#pragma once

#include "AvcError.hpp"
#include "AvcFrame.hpp"
#include "AvcTypes.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ASFW::AVC {

/// Fixed-capacity writer for constructing AV/C operand payloads.
class OperandWriter {
public:
    constexpr OperandWriter() noexcept = default;

    [[nodiscard]] constexpr Expected<void> Append(uint8_t byte) noexcept {
        if (size_ >= kMaxOperandBytes) {
            return Fail(AvcErrorKind::kFrameTooLong);
        }
        buffer_[size_++] = byte;
        return {};
    }

    [[nodiscard]] constexpr Expected<void> Append(std::span<const uint8_t> bytes) noexcept {
        if (size_ + bytes.size() > kMaxOperandBytes) {
            return Fail(AvcErrorKind::kFrameTooLong);
        }
        for (const uint8_t b : bytes) {
            buffer_[size_++] = b;
        }
        return {};
    }

    template <size_t N>
    [[nodiscard]] constexpr Expected<void> Append(const std::array<uint8_t, N>& bytes) noexcept {
        return Append(std::span<const uint8_t>{bytes.data(), N});
    }

    [[nodiscard]] constexpr std::span<const uint8_t> Bytes() const noexcept {
        return {buffer_.data(), size_};
    }

    [[nodiscard]] constexpr size_t Size() const noexcept {
        return size_;
    }

    constexpr void Reset() noexcept {
        size_ = 0;
    }

private:
    std::array<uint8_t, kMaxOperandBytes> buffer_{};
    size_t size_{0};
};

/// Concept defining an AV/C operand codec.
template <class Op>
concept AvcOperands = requires(const Op& op, OperandWriter& w, CommandType t, std::span<const uint8_t> in) {
    { Op::kOpcode } -> std::convertible_to<Opcode>;
    typename Op::Reply;
    { op.Write(w, t) } -> std::same_as<Expected<void>>;
    { op.Read(in) }   -> std::same_as<Expected<typename Op::Reply>>;
};

/// Generic AV/C command template combining addressing and an AvcOperands codec.
template <AvcOperands Op>
struct Command {
    SubunitAddress address{SubunitAddress::Unit()};
    Op operands{};

    using Reply = typename Op::Reply;

    [[nodiscard]] constexpr Expected<CommandFrame> Encode(CommandType type) const noexcept {
        if constexpr (requires { operands.ValidateAddress(address); }) {
            auto addressResult = operands.ValidateAddress(address);
            if (!addressResult) {
                return std::unexpected(addressResult.error());
            }
        }
        if constexpr (requires { { Op::kRequiresUnitAddress } -> std::convertible_to<bool>; }) {
            if constexpr (Op::kRequiresUnitAddress) {
                if (!address.IsUnit()) {
                    return Fail(AvcErrorKind::kInvalidArgument);
                }
            }
        }
        OperandWriter w;
        auto writeRes = operands.Write(w, type);
        if (!writeRes) {
            return std::unexpected(writeRes.error());
        }

        Opcode opcode = [this]() {
            if constexpr (requires { { operands.GetOpcode() } -> std::convertible_to<Opcode>; }) {
                return operands.GetOpcode();
            } else {
                return static_cast<Opcode>(Op::kOpcode);
            }
        }();

        return CommandFrame::Make(type, address, opcode, w.Bytes());
    }

    [[nodiscard]] Expected<Reply> Decode(std::span<const uint8_t> in) const noexcept {
        return operands.Read(in);
    }
};

namespace Cmd {
using AVC::OperandWriter;
using AVC::AvcOperands;
using AVC::Command;
} // namespace Cmd

} // namespace ASFW::AVC
