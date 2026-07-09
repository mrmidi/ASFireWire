// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.cpp
// Global profile registry dispatcher.

#include "AudioProfileRegistry.hpp"
#include "DICE/DiceProfileRegistry.hpp"

namespace ASFW::Isoch::Audio {

const IAudioDeviceProfile* AudioProfileRegistry::FindProfile(uint32_t vendorId,
                                                             uint32_t modelId,
                                                             uint64_t guid) noexcept {
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

    // Fall back to the Generic DICE profile if no specific vendor profile matched
    return diceRegistry.GenericProfile();
}

} // namespace ASFW::Isoch::Audio
