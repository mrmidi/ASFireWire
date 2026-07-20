// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RMEAudioProfiles.hpp - RME FireWire audio device identity and staged support policy.
//
// Stage 4B recognizes the Fireface 800, publishes a Core Audio endpoint, verifies STF,
// reserves the host-to-device IRM route, and programs RX packet geometry. Streaming
// remains deliberately blocked: no device TX allocation or isoch start is performed.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::RME {

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kRMEVendorId || query.modelId != kFireface800ModelId) {
        return std::nullopt;
    }

    return DeviceIdentityHint{.vendorId = query.vendorId,
                              .modelId = query.modelId,
                              .vendorName = kRMEVendorName,
                              .modelName = kFireface800ModelName,
                              .source = MatchSource::VendorModel};
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kRMEVendorId || query.modelId != kFireface800ModelId) {
        return std::nullopt;
    }

    // Publish a Core Audio endpoint with the verified 192 kHz / 12x12 geometry,
    // but keep transport disabled until the RME-specific isoch backend is implemented.
    return AudioProfileHint{.family = AudioProtocolFamily::VendorSpecific,
                            .mode = AudioIntegrationMode::kReadOnlyNub,
                            .source = MatchSource::VendorModel};
}

} // namespace ASFW::DeviceProfiles::Audio::RME
