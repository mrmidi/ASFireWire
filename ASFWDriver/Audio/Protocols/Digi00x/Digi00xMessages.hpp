// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xMessages.hpp - where a Digi 002 / 003 sends its asynchronous messages
// (issue #249).
//
// The host names one quadlet of its own address space in the device's message
// address register (Register::kMessageAddress); the device then writes
// one-quadlet messages there. What the messages mean is not documented, so they
// are answered and logged, not decoded.
//
// Behavior cross-validated with Linux sound/firewire/digi00x as a reference
// (digi00x-transaction.c: a 4-byte handler inside 0xffffe0000000-0xffffe000ffff,
// every request to it answered COMPLETE, the value handed to userspace
// unparsed; the address re-sent after each bus reset). No code is copied from
// it.

#pragma once

#include "Digi00xRegisters.hpp"

#include "../../../Async/Rx/LocalRequestDispatch.hpp"
#include "../../../Common/FWTypes.hpp"
#include "../../../Hardware/IEEE1394.hpp"
#include "../../../Logging/Logging.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::Audio::Digi00x::Messages {

/// One host address shared by every Digi 002 / 003 on the bus; a message is
/// attributed to its device by source node. It lies in the region Linux
/// registers from, clear of the MOTU mailbox slots at 0xffffe0000000-0xff
/// (MotuNotificationMailbox.hpp).
inline constexpr uint64_t kHostAddress = 0xffffe0001000ULL;
static_assert(kHostAddress >= 0xffffe0000100ULL && kHostAddress + 4 <= 0xffffe0010000ULL);
static_assert((kHostAddress & 3U) == 0);

/// The local node's full node_ID on the local bus (bus 0x3ff).
[[nodiscard]] constexpr std::optional<uint16_t> HostNodeId(FW::NodeId local) noexcept {
    if (!local.IsValid()) return std::nullopt;
    return static_cast<uint16_t>(0xffc0U | (local.value & FW::kNodeNumberMask));
}

/// The 8-byte block for Register::kMessageAddress, big-endian on the wire.
[[nodiscard]] constexpr std::optional<std::array<uint8_t, 8>> AddressBlock(FW::NodeId local) noexcept {
    const auto node = HostNodeId(local);
    if (!node) return std::nullopt;
    const auto words = MessageAddressWords(*node, kHostAddress);
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < words.size(); ++i) {
        bytes[i * 4] = static_cast<uint8_t>(words[i] >> 24U);
        bytes[i * 4 + 1] = static_cast<uint8_t>(words[i] >> 16U);
        bytes[i * 4 + 2] = static_cast<uint8_t>(words[i] >> 8U);
        bytes[i * 4 + 3] = static_cast<uint8_t>(words[i]);
    }
    return bytes;
}

/// The local-request handler for kHostAddress. A write of one quadlet, as a
/// quadlet or a 4-byte block, is logged and answered COMPLETE. Any other
/// request to the address is answered TYPE_ERROR; other addresses are not
/// ours.
[[nodiscard]] inline Async::LocalRequestResult Handle(const Async::LocalRequestContext& ctx) {
    using AReq = Async::HW::AsyncRequestHeader;
    if (ctx.destOffset != kHostAddress) return Async::LocalRequestResult::NotMine();
    if ((ctx.tCode != AReq::kTcodeWriteQuad && ctx.tCode != AReq::kTcodeWriteBlock) ||
        ctx.writePayload.size() != 4)
        return Async::LocalRequestResult::Write(Async::ResponseCode::TypeError);
    const auto& b = ctx.writePayload;
    const uint32_t message = (uint32_t{b[0]} << 24U) | (uint32_t{b[1]} << 16U) |
                             (uint32_t{b[2]} << 8U) | uint32_t{b[3]};
    // Undocumented and expected to be rare: every one is logged so hardware
    // runs can tell what they mean.
    ASFW_LOG(Audio, "[Digi00x] message from node=0x%04x gen=%u: 0x%08x", ctx.sourceID,
             ctx.generation, message);
    return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
}

} // namespace ASFW::Audio::Digi00x::Messages
