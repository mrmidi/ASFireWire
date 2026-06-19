#include "DiceProfileRegistry.hpp"

#include "Profiles/GenericDiceProfile.hpp"

namespace ASFW::Protocols::Audio::DICE {

namespace {
// Stateless catch-all owned here so GenericProfile() can never return null.
Profiles::GenericDiceProfile gGenericProfile{};
} // namespace

bool DiceProfileRegistry::RegisterProfile(
    const IDiceDeviceProfile* profile) noexcept {
    if (profile == nullptr || profileCount_ >= kMaxProfiles) {
        return false;
    }
    profiles_[profileCount_++] = profile;
    return true;
}

const IDiceDeviceProfile* DiceProfileRegistry::FindProfile(
    const DiceDeviceIdentity& identity) const noexcept {
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

} // namespace ASFW::Protocols::Audio::DICE
