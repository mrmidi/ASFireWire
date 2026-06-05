// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// MidasAudioProfiles.hpp - Midas FireWire audio device knowledge (DICE/TCAT).
// Knows ONLY Midas devices; performs no runtime protocol construction. Header-only
// constexpr matchers, mirroring the other vendor providers.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Midas {

// Midas FireWire interfaces are the DICE/TCAT-based Venice F-series (F16/F24/F32). They
// share IEEE OUI 0x10c73f; the only model id observed so far is 0x000001 (FFADO labels
// that entry "Venice F32"). We therefore recognize the whole vendor and present one
// honest "Venice" name instead of guessing the variant.
//
// Recognition is IDENTITY-ONLY and fail-closed: the audio profile reports integration
// mode kNone (a recognized-but-deferred DICE device, exactly like the deferred Focusrite
// multistream models). The driver never publishes a CoreAudio endpoint for these
// devices. Standard DICE stream caps come back empty (runtime_caps_not_ready) until the
// EAP / current-config probing path and clock setup exist; publishing a guessed endpoint
// would mislead testers and disturb hardware state. See the Midas EAP probing design note.

/// Direct vendor_id identity match (display names). Matches the whole Midas OUI.
[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kMidasVendorId) {
        return DeviceIdentityHint{.vendorId = query.vendorId,
                                  .modelId = query.modelId,
                                  .vendorName = kMidasVendorName,
                                  .modelName = kVeniceModelName,
                                  .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

/// Audio family + integration mode for a recognized Midas device.
[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId == kMidasVendorId) {
        // Recognized DICE device whose multistream bring-up is deferred (mode kNone):
        // fail closed, do not publish audio until the EAP/current-config path is proven.
        return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                .mode = AudioIntegrationMode::kNone,
                                .source = MatchSource::VendorModel};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Midas
