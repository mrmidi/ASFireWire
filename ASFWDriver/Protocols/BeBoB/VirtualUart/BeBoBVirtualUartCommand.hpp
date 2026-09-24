// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBVirtualUartCommand.hpp — Wire facts the driver's bootloader-window guard
// needs to recognise a safe 1394 Virtual UART command.
//
// The mailbox conversation itself lives app-side (ASFW/DriverConnector+BeBoB.swift):
// the UI and MCP drive it through raw block transactions, so a second encoder in
// the driver would be an unused copy that drifts. What must stay here is only what
// `TransactionHandler` uses to tell a shell command from one that reprograms flash.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::Protocols::BeBoB::VirtualUart {

// Register offsets in IEEE 1394 48-bit address space
inline constexpr uint16_t kVirtualUartAddressHi = 0xFFFF;
inline constexpr uint32_t kBootloaderWindowMask = 0xFFFF'0000;
inline constexpr uint32_t kBootloaderWindowBase = 0xC802'0000;
inline constexpr uint32_t kRequestAddressLo       = 0xC802'1000;
inline constexpr uint32_t kRequestBufferAddressLo = 0xC802'1040;
inline constexpr uint32_t kResponseAddressLo      = 0xC802'9000;
inline constexpr uint32_t kResponseBufferAddressLo = 0xC802'9040;

inline constexpr size_t kCommandEnvelopeBytes = 12;

/// Request envelope layout (FFADO `bebob_dl_codes.cpp:52-64`), little-endian:
///
///     q0        protocol version
///     q1  [b0]  commandId low   [b1] commandId high
///         [b2]  commandCode     [b3] operandSize, counted in QUADLETS
///     q2+       operands
///
/// The guard reads `commandCode` out of q1, so that byte position is load-bearing.
inline constexpr size_t kOpcodeQuadletOffset = 4;
inline constexpr unsigned kOpcodeBitShift = 16U;

enum class VirtualUartOpcode : uint8_t {
    kSwitchTo1394Shell = 0x07,
    kReadShellChars    = 0x08,
    kWriteShellChars   = 0x09,
};

/// Only the shell opcodes are safe to let a user client issue. The rest of this
/// window permanently reprograms device identity in flash — 0x0a rewrites the
/// EUI-64 GUID, 0x10 the hardware ID, 0x04-0x06 the firmware image — and the
/// bootloader command processor is a *permanent* service thread, so it answers in
/// application mode too. The filter therefore matters in every device state.
[[nodiscard]] constexpr bool IsPermittedVirtualUartOpcode(uint8_t opcode) noexcept {
    return opcode == static_cast<uint8_t>(VirtualUartOpcode::kSwitchTo1394Shell) ||
           opcode == static_cast<uint8_t>(VirtualUartOpcode::kReadShellChars) ||
           opcode == static_cast<uint8_t>(VirtualUartOpcode::kWriteShellChars);
}

/// Helper to load little-endian quadlet from byte array.
[[nodiscard]] constexpr uint32_t LoadLittleEndianQuadlet(std::span<const uint8_t> src, size_t offset) noexcept {
    return static_cast<uint32_t>(src[offset]) |
           (static_cast<uint32_t>(src[offset + 1]) << 8U) |
           (static_cast<uint32_t>(src[offset + 2]) << 16U) |
           (static_cast<uint32_t>(src[offset + 3]) << 24U);
}

/// Extracts the command code from a 12-byte request envelope.
[[nodiscard]] constexpr uint8_t VirtualUartOpcodeOf(std::span<const uint8_t> envelope) noexcept {
    return static_cast<uint8_t>(
        (LoadLittleEndianQuadlet(envelope, kOpcodeQuadletOffset) >> kOpcodeBitShift) & 0xFFU);
}

/// User-client writes into the BeBoB bootloader window are restricted to the
/// request character buffer and exact safe shell command envelopes. The rest of
/// the window accepts persistent firmware/identity operations and stays private
/// to the frozen driver-owned boot cue path.
[[nodiscard]] constexpr bool IsPermittedUserClientWrite(
    uint16_t addressHi, uint32_t addressLo, std::span<const uint8_t> payload) noexcept {
    const bool inWindow = addressHi == kVirtualUartAddressHi &&
        (addressLo & kBootloaderWindowMask) == kBootloaderWindowBase;
    if (!inWindow) return true;
    if (addressLo == kRequestBufferAddressLo) {
        return !payload.empty() && payload.size() <= 1024 && payload.size() % 4 == 0;
    }
    return addressLo == kRequestAddressLo && payload.size() == kCommandEnvelopeBytes &&
           IsPermittedVirtualUartOpcode(VirtualUartOpcodeOf(payload));
}

} // namespace ASFW::Protocols::BeBoB::VirtualUart
