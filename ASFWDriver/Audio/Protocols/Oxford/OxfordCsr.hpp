// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfordCsr.hpp — Oxford Semiconductor FW970/971 ASIC control/status registers.
//
// These reads are chip-level, not device-level: every OXFW device implements
// them unchanged, so they must not live inside one vendor's overlay. Behaviour
// cross-validated against snd-firewire-ctl-services
// (protocols/oxfw/src/oxford.rs:8-19) — reference read only, no code copied.
//
// The hardware ID is load-bearing rather than cosmetic. Linux splits SYT
// trustworthiness by ASIC generation: the FW970 group carries per-device
// warnings while the FW971 group is documented as reliable with both
// transmission modes available (linux-sound-firewire-stack/firewire/oxfw/oxfw.c
// :328-360). Reading this register lets a device derive streaming policy at
// runtime instead of hardcoding a per-model quirk table.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Common/FWCommon.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"

namespace ASFW::Audio::Oxford {

//==============================================================================
// Register map (oxford.rs:8-10)
//==============================================================================

inline constexpr uint64_t kCsrRegisterBase = 0xFFFFF0000000ULL;
inline constexpr uint64_t kFirmwareIdOffset = 0x50000ULL;
inline constexpr uint64_t kHardwareIdOffset = 0x90020ULL;

/// Numeric hardware identifiers (oxford.rs:14-19).
inline constexpr uint32_t kHardwareIdFw970 = 0x39443841U; // '9','D','8','A'
inline constexpr uint32_t kHardwareIdFw971 = 0x39373100U; // '9','7','1','\0'

/// Which Oxford ASIC a hardware ID denotes.
enum class Asic : uint8_t {
    kUnknown = 0,
    kFw970,
    kFw971,
};

//==============================================================================
// Pure helpers
//==============================================================================

[[nodiscard]] constexpr Async::FWAddress AddressFor(uint64_t offset) noexcept {
    const uint64_t address = kCsrRegisterBase + offset;
    return Async::FWAddress{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((address >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(address & 0xFFFFFFFFU),
    }};
}

[[nodiscard]] constexpr Async::FWAddress FirmwareIdAddress() noexcept {
    return AddressFor(kFirmwareIdOffset);
}

[[nodiscard]] constexpr Async::FWAddress HardwareIdAddress() noexcept {
    return AddressFor(kHardwareIdOffset);
}

/// An unrecognised ID must stay unknown rather than defaulting to either ASIC —
/// a wrong generation would select the wrong SYT policy.
[[nodiscard]] constexpr Asic ClassifyHardwareId(uint32_t hardwareId) noexcept {
    switch (hardwareId) {
        case kHardwareIdFw970:
            return Asic::kFw970;
        case kHardwareIdFw971:
            return Asic::kFw971;
        default:
            return Asic::kUnknown;
    }
}

/// IEEE 1394 payloads are big-endian on the wire.
[[nodiscard]] constexpr uint32_t DecodeIdQuadlet(std::span<const uint8_t> payload) noexcept {
    if (payload.size() < 4U) {
        return 0U;
    }
    return (static_cast<uint32_t>(payload[0]) << 24U) |
           (static_cast<uint32_t>(payload[1]) << 16U) |
           (static_cast<uint32_t>(payload[2]) << 8U) |
           static_cast<uint32_t>(payload[3]);
}

//==============================================================================
// Async reads
//==============================================================================

/// Reports (status, id). `id` is meaningful only when status is success; a
/// failed read must not be reported as a zero ID, which would be
/// indistinguishable from a successful odd read classified as unknown.
using IdCallback = std::function<void(IOReturn, uint32_t)>;

/// Resolves the device's route at the moment of use, or nullopt when it has no
/// live route. The caller owns route-liveness policy; this layer only asks.
///
/// Deliberately a *provider*, not a validator over a caller-held token (FW-142).
/// Taking `(route, isRouteCurrent)` invited the caller to capture a route once
/// and validate that same value forever: `ApogeeDuetProtocol` snapshotted it at
/// construction, the registry rebound the device before the read was issued, and
/// every CSR read failed "route not current" against a device that was present
/// and answering vendor commands on the same node and generation. Resolving on
/// each use is what `FCPTransport::SubmitCommand` already does
/// (FCPTransport.cpp:206), which is precisely why FCP never went stale.
using RouteProvider = std::function<std::optional<Discovery::DeviceRouteToken>()>;

void ReadFirmwareId(Async::IFireWireBusOps& busOps,
                    RouteProvider currentRoute,
                    IdCallback callback);

void ReadHardwareId(Async::IFireWireBusOps& busOps,
                    RouteProvider currentRoute,
                    IdCallback callback);

} // namespace ASFW::Audio::Oxford
