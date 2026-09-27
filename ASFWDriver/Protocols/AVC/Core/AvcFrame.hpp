// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcFrame.hpp - Build AV/C command frames and parse response frames.
//
// Frame layout (both directions): [ctype|response][subunit address][opcode][operands...]
// ta1394 general/src/lib.rs:470-501 (compose + response detection). Maximum frame
// size 512 bytes: ta1394 lib.rs:454 (FRAME_SIZE = 0x200).
//
// Pure codec: no transport, no allocation, no logging. Every function is
// noexcept and reports failure through Expected<T>.
//
// Implementation: AvcFrame.cpp (phase 1, docs/avc-rebuild/phase-1.md).

#pragma once

#include "AvcError.hpp"
#include "AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC {

inline constexpr size_t kHeaderBytes = 3;
inline constexpr size_t kMaxFrameBytes = 512;
inline constexpr size_t kMaxOperandBytes = kMaxFrameBytes - kHeaderBytes;

/// An encoded command frame, held by value (fixed 512-byte storage).
class CommandFrame {
public:
    /// Build a frame. Fails with kFrameTooLong when operands exceed
    /// kMaxOperandBytes, and with kUnsupported for an extended subunit type.
    [[nodiscard]] static Expected<CommandFrame> Make(CommandType type, SubunitAddress address, Opcode opcode,
                                                     std::span<const uint8_t> operands) noexcept;

    [[nodiscard]] CommandType Type() const noexcept { return static_cast<CommandType>(bytes_[0] & kCodeMask); }
    [[nodiscard]] SubunitAddress Address() const noexcept { return SubunitAddress::FromByte(bytes_[1]); }
    [[nodiscard]] Opcode OpcodeValue() const noexcept { return static_cast<Opcode>(bytes_[2]); }

    /// Exactly header + operands; what a trace or a log shows.
    [[nodiscard]] std::span<const uint8_t> Bytes() const noexcept { return {bytes_.data(), size_}; }

    [[nodiscard]] std::span<const uint8_t> Operands() const noexcept {
        return {bytes_.data() + kHeaderBytes, size_ - kHeaderBytes};
    }

    /// Bytes() zero-padded to a quadlet boundary: what the transport writes to
    /// the FCP command register. Matches today's AVCCdb::Encode (hardware-proven
    /// on Phase 88 and Duet). Linux fcp.c:250-253 writes exactly what callers pass,
    /// and its callers size buffers in whole quadlets.
    [[nodiscard]] std::span<const uint8_t> WireBytes() const noexcept {
        return {bytes_.data(), (static_cast<size_t>(size_) + 3u) & ~size_t{3}};
    }

private:
    CommandFrame() noexcept = default;
    std::array<uint8_t, kMaxFrameBytes + 3> bytes_{};  // +3: padding never overruns
    uint16_t size_{0};
};

/// A parsed response. A VIEW: `operands` points into the buffer passed to
/// ParseResponse and is valid only while that buffer lives.
///
/// Trailing bytes: a response may carry quadlet padding after the real operands.
/// Command codecs must read only the operands their layout defines and must not
/// treat extra trailing bytes as an error.
struct Response {
    ResponseCode code{ResponseCode::kNotImplemented};
    SubunitAddress address{SubunitAddress::Unit()};
    Opcode opcode{Opcode::kVendorDependent};
    std::span<const uint8_t> operands{};
};

/// Split a response frame into its fields. Fails with kFrameTooShort (< 3 bytes),
/// kFrameTooLong (> kMaxFrameBytes) or kNotAResponse (low nibble < 0x8).
/// Nonzero CTS bits (high nibble) are kNotAResponse too: not an AV/C frame.
[[nodiscard]] Expected<Response> ParseResponse(std::span<const uint8_t> frame) noexcept;

/// ParseResponse, then check that address and opcode echo `command`
/// (ta1394 lib.rs:488-501). kAddressMismatch / kOpcodeMismatch otherwise.
[[nodiscard]] Expected<Response> ParseResponseFor(const CommandFrame& command,
                                                  std::span<const uint8_t> frame) noexcept;

/// The operands of `response` if its code is `expected`; otherwise
/// AvcError::Unexpected(response.code). The helper every command parser starts with:
/// STATUS expects kImplementedStable, CONTROL expects kAccepted.
[[nodiscard]] constexpr Expected<std::span<const uint8_t>> OperandsIf(const Response& response,
                                                                      ResponseCode expected) noexcept {
    if (response.code != expected) {
        return std::unexpected(AvcError::Unexpected(response.code));
    }
    return response.operands;
}

} // namespace ASFW::AVC
