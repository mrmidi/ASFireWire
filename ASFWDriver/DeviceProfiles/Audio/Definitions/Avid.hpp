// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// Avid Mbox Pro, 3rd generation (2011). DICE II / TCAT.
//
// The unit directory advertises specifier_id == the vendor OUI with version
// 0x000001, so the generic TA 61883 classifier cannot place it: the catalog
// entry is what makes it a DICE device. Stream geometry is not stated here --
// the device advertises its layout through EAP and the family provider reads it.
inline constexpr std::array kAvidDefinitions{
    Definition(DeviceDefinitionId::AvidMboxPro, kAvidVendorId, kMboxProModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::AvidMboxPro,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported, kAvidVendorName,
               kMboxProModelName, std::nullopt, BootloaderCuePolicy::None,
               kDiceTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
