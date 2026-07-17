// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.cpp
// Global profile registry dispatcher.

#include "AudioProfileRegistry.hpp"
#include "AVC/ApogeeDuetProfile.hpp"
#include "AVC/BeBoBProfile.hpp"
#include "AVC/Phase88Profile.hpp"
#include "DICE/DiceProfileRegistry.hpp"
#include "../../../Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.hpp"

#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio {

std::unordered_map<uint64_t, std::unique_ptr<IAudioDeviceProfile>>& AudioProfileRegistry::DynamicProfiles() {
    static std::unordered_map<uint64_t, std::unique_ptr<IAudioDeviceProfile>> profiles;
    return profiles;
}

const IAudioDeviceProfile* AudioProfileRegistry::FindProfile(uint32_t vendorId,
                                                             uint32_t modelId,
                                                             uint64_t guid) noexcept {
    // Per-GUID BeBoB profiles take precedence (registered during discovery).
    if (guid != 0) {
        auto& dynamic = DynamicProfiles();
        if (auto it = dynamic.find(guid); it != dynamic.end()) {
            return it->second.get();
        }
    }

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

const IAudioDeviceProfile* AudioProfileRegistry::RegisterBeBoBProfile(
    uint64_t guid, const void* discoveryModel) noexcept {
    if (guid == 0 || discoveryModel == nullptr) return nullptr;
    auto& dynamic = DynamicProfiles();
    if (dynamic.find(guid) != dynamic.end()) return dynamic[guid].get();
    const auto* model = static_cast<const Audio::BeBoB::DeviceModel*>(discoveryModel);
    dynamic[guid] = std::make_unique<AVC::Profiles::BeBoBProfile>(*model);
    return dynamic[guid].get();
}

void AudioProfileRegistry::UnregisterProfile(uint64_t guid) noexcept {
    DynamicProfiles().erase(guid);
}

} // namespace ASFW::Isoch::Audio
