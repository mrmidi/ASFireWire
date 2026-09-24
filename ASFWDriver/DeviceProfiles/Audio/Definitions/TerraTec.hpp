// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kTerraTecDefinitions{
    Definition(DeviceDefinitionId::TerraTecPhase88, kTerraTecVendorId,
               kPhase88RackFwModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBPlug0, ProfileBuilderId::TerraTecPhase88,
               ProtocolImplementationId::BeBoBPhase88,
               SupportDisposition::Supported, kTerraTecVendorName,
               kPhase88RackFwModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
