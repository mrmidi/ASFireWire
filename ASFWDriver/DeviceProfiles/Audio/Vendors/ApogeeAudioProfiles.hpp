// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeAudioProfiles.hpp - Apogee FireWire audio device knowledge (Oxford / AV/C).
// Knows ONLY Apogee devices; performs no runtime protocol construction.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Apogee {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kApogeeVendorId && query.modelId == kApogeeDuetModelId) {
        return DeviceIdentityHint{.vendorId = query.vendorId,
                                  .modelId = query.modelId,
                                  .vendorName = kApogeeVendorName,
                                  .modelName = kApogeeDuetModelName,
                                  .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kApogeeVendorId && query.modelId == kApogeeDuetModelId) {
        return AudioProfileHint{.family = AudioProtocolFamily::Oxford,
                                .mode = AudioIntegrationMode::kAVCDriven,
                                .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Apogee
