// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kMidasDefinitions{
    Definition(DeviceDefinitionId::MidasVeniceF32, kMidasVendorId,
               kMidasVeniceModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::MidasVeniceF32,
               SupportDisposition::Supported, kMidasVendorName,
               kMidasVeniceModelName, std::nullopt, BootloaderCuePolicy::None,
               kDiceTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
