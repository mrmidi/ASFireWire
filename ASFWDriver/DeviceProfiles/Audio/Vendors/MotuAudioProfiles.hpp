// SPDX-License-Identifier: Apache-2.0
// Modified in 2026 by Rafal Zalech to add original MOTU UltraLite support.
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioProfiles.hpp - MOTU FireWire audio device knowledge (vendor register protocol).
// Knows ONLY MOTU devices; performs no runtime protocol construction.
//
// Unlike every other provider here, MOTU cannot be matched on (vendor_id, model_id): the
// root directory carries model_id 0. Identity comes from the unit directory's
// Unit_Spec_Id (== the OUI) plus Unit_Sw_Version, matching Linux
// sound/firewire/motu/motu.c:151-181 (IEEE1394_MATCH_VENDOR_ID | SPECIFIER_ID | VERSION).

#pragma once

#include "../../Common/DeviceProfileTypes.hpp"
#include "../AudioDeviceIds.hpp"
#include "../AudioProfileTypes.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Motu {

/// True when the query carries a MOTU unit directory (OUI in both vendor and specifier).
[[nodiscard]] constexpr bool IsMotuUnit(const DeviceProfileQuery& query) noexcept {
    return query.vendorId == kMotuVendorId && query.unitSpecId == kMotuVendorId;
}

[[nodiscard]] constexpr const char* ModelNameForSwVersion(uint32_t swVersion) noexcept {
    switch (swVersion) {
    case kMotu828mk2SwVersion: return kMotu828mk2ModelName;
    case kMotu896hdSwVersion: return kMotu896hdModelName;
    case kMotuTravelerSwVersion: return kMotuTravelerModelName;
    case kMotuUltraliteSwVersion: return kMotuUltraliteModelName;
    case kMotu8preSwVersion: return kMotu8preModelName;
    default: return nullptr;
    }
}

[[nodiscard]] constexpr std::optional<DeviceIdentityHint>
LookupIdentity(const DeviceProfileQuery& query) noexcept {
    if (!IsMotuUnit(query)) {
        return std::nullopt;
    }
    const char* const modelName = ModelNameForSwVersion(query.unitSwVersion);
    if (modelName == nullptr) {
        return std::nullopt;
    }
    // modelId is reported as the software version: it is the only stable model
    // discriminator MOTU publishes, and downstream identity is keyed on it.
    return DeviceIdentityHint{.vendorId = query.vendorId,
                              .modelId = query.unitSwVersion,
                              .vendorName = kMotuVendorName,
                              .modelName = modelName,
                              .source = MatchSource::ConfigROM};
}

[[nodiscard]] constexpr std::optional<AudioProfileHint>
LookupAudioProfile(const DeviceProfileQuery& query) noexcept {
    uint32_t swVersion = 0;
    if (IsMotuUnit(query)) {
        swVersion = query.unitSwVersion;
    } else if (query.vendorId == kMotuVendorId &&
               ModelNameForSwVersion(query.modelId) != nullptr) {
        // Discovery canonicalizes a MOTU unit's software version into modelId after
        // parsing its unit directory. Accept that internal query shape as well.
        swVersion = query.modelId;
    } else {
        return std::nullopt;
    }
    // Audio integration is enabled only for models whose register plane and stream
    // geometry have been confirmed. Other protocol-v2 siblings resolve an identity
    // above but stay kNone.
    if (swVersion == kMotu828mk2SwVersion) {
        return AudioProfileHint{.family = AudioProtocolFamily::VendorSpecific,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::ConfigROM};
    }
    // Original UltraLite protocol-v2 hardware uses the same register plane and
    // a fixed 14-channel packed-PCM stream in both directions.
    if (swVersion == kMotuUltraliteSwVersion) {
        return AudioProfileHint{.family = AudioProtocolFamily::VendorSpecific,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::ConfigROM};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Motu
