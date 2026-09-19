// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kApogeeDefinitions{
    Definition(DeviceDefinitionId::ApogeeDuet, kApogeeVendorId, kApogeeDuetModelId,
               AudioFamilyProviderId::OXFW, ProbePolicyId::OxfwAvc,
               ProfileBuilderId::ApogeeDuet, SupportDisposition::Supported,
               kApogeeVendorName, kApogeeDuetModelName, std::nullopt,
               BootloaderCuePolicy::None,
               // Discovery reports and supports non-blocking, and host playback
               // works that way, but the observed device output cadence is
               // blocking -- forcing it keeps host and device aligned.
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .startShape = StreamStartShape::ApogeeInterleaved,
                                  .cmpChoosesIsoChannel = true,
                                  .startRatePinHz = 48000U}),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
