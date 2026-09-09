// SPDX-License-Identifier: Apache-2.0
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
    if (!IsMotuUnit(query)) {
        return std::nullopt;
    }
    // Only the 828mk2 is hardware-verified. The other protocol-v2 siblings resolve an
    // identity (above) so they are named in diagnostics, but stay kNone — no protocol is
    // constructed for them until their chunk layouts are confirmed on real hardware.
    if (query.unitSwVersion == kMotu828mk2SwVersion) {
        return AudioProfileHint{.family = AudioProtocolFamily::VendorSpecific,
                                .mode = AudioIntegrationMode::kHardcodedNub,
                                .source = MatchSource::ConfigROM};
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio::Motu
