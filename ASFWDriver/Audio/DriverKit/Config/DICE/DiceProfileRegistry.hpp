// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfileRegistry.hpp
// Matcher registry database for DICE profiles.

#pragma once

#include "DiceDeviceProfile.hpp"
#include <cstdint>

namespace ASFW::Isoch::Audio::DICE {

class DiceProfileRegistry final {
public:
    DiceProfileRegistry() noexcept;

    bool RegisterProfile(const IDiceDeviceProfile* profile) noexcept;

    [[nodiscard]] const IDiceDeviceProfile* FindProfile(const DiceDeviceIdentity& identity) const noexcept;
    [[nodiscard]] const IDiceDeviceProfile* GenericProfile() const noexcept;

    [[nodiscard]] uint32_t ProfileCount() const noexcept;

private:
    static constexpr uint32_t kMaxProfiles = 16;

    const IDiceDeviceProfile* profiles_[kMaxProfiles]{};
    uint32_t profileCount_{0};
};

} // namespace ASFW::Isoch::Audio::DICE
