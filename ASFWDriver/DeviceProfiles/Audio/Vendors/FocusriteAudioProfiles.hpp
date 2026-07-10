// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FocusriteAudioProfiles.hpp - Focusrite FireWire audio device knowledge (DICE/TCAT).
// Knows ONLY Focusrite devices; performs no runtime protocol construction. Header-only
// constexpr matchers over static tables: no allocation, no link dependency.

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <cstdint>
#include <optional>

namespace ASFW::DeviceProfiles::Audio::Focusrite {

/// Direct vendor_id + model_id identity match (display names).
[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kFocusriteVendorId) {
        return std::nullopt;
    }

    const char* modelName = nullptr;
    switch (query.modelId) {
        case kSPro14ModelId:        modelName = kSPro14ModelName; break;
        case kSPro24ModelId:        modelName = kSPro24ModelName; break;
        case kSPro24DspModelId:     modelName = kSPro24DspModelName; break;
        case kSPro40ModelId:        modelName = kSPro40ModelName; break;
        case kLiquidS56ModelId:     modelName = kLiquidS56ModelName; break;
        case kSPro26ModelId:        modelName = kSPro26ModelName; break;
        case kSPro40Tcd3070ModelId: modelName = kSPro40Tcd3070ModelName; break;
        default:                    return std::nullopt;
    }

    return DeviceIdentityHint{.vendorId = query.vendorId,
                              .modelId = query.modelId,
                              .vendorName = kFocusriteVendorName,
                              .modelName = modelName,
                              .source = MatchSource::VendorModel};
}

/// Focusrite DICE devices encode the board model in GUID bits [27:22]. Used when the
/// Config ROM did not surface a usable model_id.
[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentityByGuid(const DeviceProfileQuery& query) noexcept {
    constexpr uint64_t kOuiMask    = 0x00FFFFFFULL;
    constexpr unsigned kOuiShift   = 40;
    constexpr unsigned kModelShift = 22;
    constexpr uint64_t kModelMask  = 0x3FULL;

    const auto vendorId = static_cast<uint32_t>((query.guid >> kOuiShift) & kOuiMask);
    if (vendorId != kFocusriteVendorId) {
        return std::nullopt;
    }

    auto modelId = static_cast<uint32_t>((query.guid >> kModelShift) & kModelMask);
    if (modelId == kFocusriteGuidModelSPro40Tcd3070) {
        modelId = kSPro40Tcd3070ModelId;
    }

    auto identity = LookupIdentity(
        DeviceProfileQuery{.guid = query.guid, .vendorId = vendorId, .modelId = modelId});
    if (identity.has_value()) {
        identity->source = MatchSource::GUID;
    }
    return identity;
}

/// Audio family + integration mode for a recognized Focusrite device.
[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    if (query.vendorId != kFocusriteVendorId) {
        return std::nullopt;
    }

    switch (query.modelId) {
        // Driven DICE/TCAT models.
        case kSPro14ModelId:
        case kSPro24ModelId:
        case kSPro24DspModelId:
            return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                    .mode = AudioIntegrationMode::kHardcodedNub,
                                    .source = MatchSource::VendorModel};
        // Recognized DICE models whose multistream bring-up is deferred (mode kNone).
        case kSPro40ModelId:
        case kLiquidS56ModelId:
        case kSPro26ModelId:
        case kSPro40Tcd3070ModelId:
            return AudioProfileHint{.family = AudioProtocolFamily::DICE,
                                    .mode = AudioIntegrationMode::kNone,
                                    .source = MatchSource::VendorModel};
        default:
            return std::nullopt;
    }
}

} // namespace ASFW::DeviceProfiles::Audio::Focusrite
