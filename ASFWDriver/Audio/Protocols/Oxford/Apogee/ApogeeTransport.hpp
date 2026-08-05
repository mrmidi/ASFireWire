// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeTransport.hpp - How Duet data actually reaches the wire (FW-129).
//
// The Duet uses *two unrelated transports*, and which one applies used to be
// implied by whichever private helper a method happened to call:
//
//   VendorFcp       parameters travel as AV/C VENDOR-DEPENDENT commands
//                   through FCP, with command/response framing
//   MeterRegisters  meters are plain async block reads to a device address
//                   offset - no AV/C, no FCP, no response matching
//
// Splitting them into two namespaces makes that distinction structural rather
// than incidental. The Oxford ID registers are the same *mechanism* as the
// meters but are chip-common, so they live in Oxford/OxfordCsr (FW-137); the
// offsets below are Duet-specific (apogee.rs:128-130).

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "ApogeeTypes.hpp"
#include "ApogeeVendorCodec.hpp"
#include "../OxfordCsr.hpp"
#include "../../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../../Discovery/DeviceRegistry.hpp"

namespace ASFW::Protocols::AVC {
class FCPTransport;
}

namespace ASFW::Audio::Oxford::Apogee {

/// Reuses the FW-137 validator so both register transports express route
/// liveness the same way.
using RouteValidator = Oxford::RouteValidator;

//==============================================================================
// Transport A - AV/C vendor commands over FCP
//==============================================================================

namespace VendorFcp {

using ResultCallback = std::function<void(IOReturn, const ApogeeVendorCommand&)>;
using SequenceCallback = std::function<void(IOReturn, const std::vector<ApogeeVendorCommand>&)>;

/// Frame one vendor command, submit it, and decode the response.
///
/// `isStatus` selects the AV/C ctype: a status query omits the value operands
/// and expects them back, a control command carries them.
void Send(Protocols::AVC::FCPTransport* transport,
          const ApogeeVendorCommand& command,
          bool isStatus,
          ResultCallback callback);

/// Run commands strictly in order, one in flight at a time, stopping at the
/// first failure. The Duet's params groups are read and written as sequences,
/// and a partially applied group is worse than a failed one - so an error
/// aborts rather than continuing with the rest.
void ExecuteSequence(Protocols::AVC::FCPTransport* transport,
                     const std::vector<ApogeeVendorCommand>& commands,
                     bool isStatus,
                     SequenceCallback callback);

} // namespace VendorFcp

//==============================================================================
// Transport B - plain async block reads to Duet meter registers
//==============================================================================

namespace MeterRegisters {

/// apogee.rs:128-130.
inline constexpr uint64_t kBaseAddress = 0xFFFFF0080000ULL;
inline constexpr uint32_t kInputOffset = 0x0004;
inline constexpr uint32_t kMixerOffset = 0x0404;

/// Two int32 levels; four for the mixer (2 stream-in + 2 mixer-out).
inline constexpr uint32_t kInputBlockBytes = 8;
inline constexpr uint32_t kMixerBlockBytes = 16;

[[nodiscard]] constexpr Async::FWAddress AddressFor(uint32_t offset) noexcept {
    const uint64_t address = kBaseAddress + offset;
    return Async::FWAddress{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((address >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(address & 0xFFFFFFFFU),
    }};
}

[[nodiscard]] constexpr Async::FWAddress InputAddress() noexcept {
    return AddressFor(kInputOffset);
}

[[nodiscard]] constexpr Async::FWAddress MixerAddress() noexcept {
    return AddressFor(kMixerOffset);
}

/// IEEE 1394 payloads are big-endian.
[[nodiscard]] constexpr int32_t DecodeLevel(std::span<const uint8_t> payload,
                                            size_t offset) noexcept {
    return static_cast<int32_t>((static_cast<uint32_t>(payload[offset]) << 24U) |
                                (static_cast<uint32_t>(payload[offset + 1U]) << 16U) |
                                (static_cast<uint32_t>(payload[offset + 2U]) << 8U) |
                                static_cast<uint32_t>(payload[offset + 3U]));
}

/// Both decoders reject a short block rather than reading past it, and leave
/// `out` untouched when they do.
[[nodiscard]] bool DecodeInput(std::span<const uint8_t> payload, InputMeterState& out) noexcept;
[[nodiscard]] bool DecodeMixer(std::span<const uint8_t> payload, MixerMeterState& out) noexcept;

void ReadInput(Async::IFireWireBusOps& busOps,
               const Discovery::DeviceRouteToken& route,
               RouteValidator isRouteCurrent,
               std::function<void(IOReturn, InputMeterState)> callback);

void ReadMixer(Async::IFireWireBusOps& busOps,
               const Discovery::DeviceRouteToken& route,
               RouteValidator isRouteCurrent,
               std::function<void(IOReturn, MixerMeterState)> callback);

} // namespace MeterRegisters

} // namespace ASFW::Audio::Oxford::Apogee
