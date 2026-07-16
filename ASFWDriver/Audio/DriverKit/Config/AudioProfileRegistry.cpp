// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.cpp
// Global profile registry dispatcher.

#include "AudioProfileRegistry.hpp"
#include "AVC/ApogeeDuetProfile.hpp"
#include "AVC/Phase88Profile.hpp"
#include "DICE/DiceProfileRegistry.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio {

const IAudioDeviceProfile* AudioProfileRegistry::FindProfile(uint32_t vendorId,
                                                             uint32_t modelId,
                                                             uint64_t guid) noexcept {
    static AVC::Profiles::ApogeeDuetProfile apogeeDuetProfile{};
    if (vendorId == DeviceProfiles::Audio::kApogeeVendorId &&
        modelId == DeviceProfiles::Audio::kApogeeDuetModelId) {
        return &apogeeDuetProfile;
    }

    static AVC::Profiles::Phase88Profile phase88Profile{};
    if (vendorId == DeviceProfiles::Audio::kTerraTecVendorId &&
        modelId == DeviceProfiles::Audio::kPhase88RackFwModelId) {
        return &phase88Profile;
    }

    // Map identity to the DICE family structures
    DICE::DiceDeviceIdentity identity{
        .guid = guid,
        .vendorId = vendorId,
        .modelId = modelId
    };

    // Instantiate/access the DICE registry singleton.
    // The constructor of DiceProfileRegistry pre-populates all known DICE profiles.
    static DICE::DiceProfileRegistry diceRegistry{};

    if (const auto* profile = diceRegistry.FindProfile(identity)) {
        return profile;
    }

    // The generic profile is a DICE fallback only. Exact AV/C adapters above
    // must never inherit its name or wire geometry.
    return diceRegistry.GenericProfile();
}

} // namespace ASFW::Isoch::Audio
