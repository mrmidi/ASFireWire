// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// AlesisAudioProfiles.hpp - Alesis FireWire audio device knowledge (DICE/TCAT).
// Knows ONLY Alesis devices; performs no runtime protocol construction.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Alesis {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kAlesisVendorId && query.modelId == kAlesisMultiMixModelId) {
        return DeviceIdentityHint{.vendorId = query.vendorId,
                                  .modelId = query.modelId,
                                  .vendorName = kAlesisVendorName,
                                  .modelName = kAlesisMultiMixModelName,
                                  .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kAlesisVendorId && query.modelId == kAlesisMultiMixModelId) {
        return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Alesis
