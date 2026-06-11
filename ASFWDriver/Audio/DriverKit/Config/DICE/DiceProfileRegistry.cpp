// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfileRegistry.cpp
// Matcher registry database for DICE profiles.

#include "DiceProfileRegistry.hpp"
#include "Isoch/Profiles/FocusriteSaffireProfile.hpp"
#include "Isoch/Profiles/GenericDiceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE {

namespace {
Profiles::GenericDiceProfile gGenericProfile{};
Profiles::FocusriteSaffireProfile gFocusriteProfile{};
} // namespace

DiceProfileRegistry::DiceProfileRegistry() noexcept {
    (void)RegisterProfile(&gFocusriteProfile);
}

bool DiceProfileRegistry::RegisterProfile(const IDiceDeviceProfile* profile) noexcept {
    if (profile == nullptr || profileCount_ >= kMaxProfiles) {
        return false;
    }
    profiles_[profileCount_++] = profile;
    return true;
}

const IDiceDeviceProfile* DiceProfileRegistry::FindProfile(const DiceDeviceIdentity& identity) const noexcept {
    for (uint32_t i = 0; i < profileCount_; ++i) {
        if (profiles_[i] != nullptr && profiles_[i]->Matches(identity)) {
            return profiles_[i];
        }
    }
    return nullptr;
}

const IDiceDeviceProfile* DiceProfileRegistry::GenericProfile() const noexcept {
    return &gGenericProfile;
}

uint32_t DiceProfileRegistry::ProfileCount() const noexcept {
    return profileCount_;
}

} // namespace ASFW::Isoch::Audio::DICE
