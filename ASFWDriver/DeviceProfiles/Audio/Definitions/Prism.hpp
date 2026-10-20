// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kPrismDefinitions{
    Definition(DeviceDefinitionId::PrismSoundOrpheus, kPrismSoundVendorId,
               kOrpheusModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBPlug0, ProfileBuilderId::Orpheus,
               ProtocolImplementationId::Orpheus,
               SupportDisposition::Supported, kPrismSoundVendorName,
               kOrpheusModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
