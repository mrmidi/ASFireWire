// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Exact TerraTec BridgeCo / BeBoB identities. This is metadata only.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::TerraTec {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kTerraTecVendorId || query.modelId != kPhase88RackFwModelId) {
        return std::nullopt;
    }
    return DeviceIdentityHint{.vendorId = query.vendorId,
                              .modelId = query.modelId,
                              .vendorName = kTerraTecVendorName,
                              .modelName = kPhase88RackFwModelName,
                              .source = MatchSource::VendorModel};
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kTerraTecVendorId || query.modelId != kPhase88RackFwModelId) {
        return std::nullopt;
    }
    // Exact identity from the attached TerraTec Config ROM and FFADO's BeBoB
    // table (libffado-2.5.0/configuration:186-194). Geometry is probe-derived;
    // never use the generic DICE backend for this device.
    return AudioProfileHint{.family = AudioProtocolFamily::AVC,
                            .mode = AudioIntegrationMode::kAVCDriven,
                            .source = MatchSource::VendorModel};
}

} // namespace ASFW::DeviceProfiles::Audio::TerraTec
