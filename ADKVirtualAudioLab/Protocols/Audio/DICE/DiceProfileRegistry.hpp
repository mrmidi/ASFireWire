#pragma once

#include "DiceDeviceProfile.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

class DiceProfileRegistry final {
public:
    DiceProfileRegistry() noexcept = default;

    bool RegisterProfile(const IDiceDeviceProfile* profile) noexcept;

    const IDiceDeviceProfile* FindProfile(const DiceDeviceIdentity& identity) const noexcept;
    const IDiceDeviceProfile* GenericProfile() const noexcept;

    uint32_t ProfileCount() const noexcept;

private:
    static constexpr uint32_t kMaxProfiles = 16;

    const IDiceDeviceProfile* profiles_[kMaxProfiles]{};
    uint32_t profileCount_{0};
};

} // namespace ASFW::Protocols::Audio::DICE