// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBDeviceProfiles.hpp - BridgeCo BeBoB device identity and support metadata.
//
// Single source of truth for BeBoB device recognition. All known BeBoB devices live
// here with an explicit support level; no vendor-ID-range matching (a single OUI can
// include both BeBoB-era and DICE devices, e.g. PreSonus 0x000a92). Exact vendor+model
// only.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::DeviceProfiles::Audio::BeBoB {

enum class SupportLevel : uint8_t {
    kVerified,       // full protocol, HW-tested
    kDiscoveryOnly,  // generic protocol, streaming works, no mixer/clock customization
    kExperimental,   // enabled only with explicit flag
};

struct Device {
    uint32_t vendorId;
    uint32_t modelId;
    const char* name;
    SupportLevel support;
};

inline constexpr std::array kDevices{
    // TerraTec PHASE 88 Rack FW — HW-verified (2026-07-16).
    // cross-validated: linux-sound-firewire-stack/firewire/bebob/bebob.c:427-428,
    // libffado-2.5.0/src/bebob/terratec/terratec_device.h:24-36.
    Device{kTerraTecVendorId, kPhase88RackFwModelId, kPhase88RackFwModelName,
           SupportLevel::kVerified},
    // Known BeBoB devices without HW verification. Discovery-only until captured
    // from real hardware. Device IDs cross-validated with Linux bebob_id_table
    // (linux-sound-firewire-stack/firewire/bebob/bebob.c:366-485) and
    // libffado-2.5.0/src/bebob/bebob_avdevice.cpp.
    // (Additional entries added as hardware becomes available.)
};

[[nodiscard]] constexpr bool IsBeBoBDevice(uint32_t vendorId, uint32_t modelId) noexcept {
    for (const auto& d : kDevices) {
        if (d.vendorId == vendorId && d.modelId == modelId) return true;
    }
    return false;
}

[[nodiscard]] constexpr SupportLevel SupportLevelFor(uint32_t vendorId,
                                                     uint32_t modelId) noexcept {
    for (const auto& d : kDevices) {
        if (d.vendorId == vendorId && d.modelId == modelId) return d.support;
    }
    return SupportLevel::kDiscoveryOnly;
}

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    for (const auto& d : kDevices) {
        if (d.vendorId == query.vendorId && d.modelId == query.modelId) {
            return DeviceIdentityHint{.vendorId = query.vendorId,
                                      .modelId = query.modelId,
                                      .vendorName = kTerraTecVendorName,
                                      .modelName = d.name,
                                      .source = MatchSource::VendorModel};
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (!IsBeBoBDevice(query.vendorId, query.modelId)) {
        return std::nullopt;
    }
    // All BeBoB devices are AV/C-driven with CMP; the protocol family is AVC.
    // cross-validated: linux-sound-firewire-stack/firewire/bebob/bebob.c:184-260.
    return AudioProfileHint{.family = AudioProtocolFamily::AVC,
                            .mode = AudioIntegrationMode::kAVCDriven,
                            .source = MatchSource::VendorModel};
}

} // namespace ASFW::DeviceProfiles::Audio::BeBoB
