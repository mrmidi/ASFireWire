// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Shared profile predicate for the M-Audio 1814 / ProjectMix firmware path.
// Keep this at the family boundary so transport, AudioDriverKit, and the
// duplex coordinator make the same narrow opt-in decision.

#pragma once

#include "../../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

namespace ASFW::Audio::Families::BeBoB::MAudio {

[[nodiscard]] constexpr bool UsesSpecialDuplexPolicy(
    DeviceProfiles::Audio::ProfileBuilderId profileBuilder) noexcept {
    using ProfileBuilderId = DeviceProfiles::Audio::ProfileBuilderId;
    return profileBuilder == ProfileBuilderId::MAudioFireWire1814 ||
           profileBuilder == ProfileBuilderId::MAudioProjectMix;
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
