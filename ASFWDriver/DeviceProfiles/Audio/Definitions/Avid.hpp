// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// Digidesign Digi 002 / 003: recognised, not yet streamed (issue #249).
//
// The model id lives in the unit directory only (the root directory has none),
// so the generic Definition() helper, which constrains the root model, cannot
// express these rows. The 002 and 003 share model ids; the per-product unit
// specifier tells them apart (AudioDeviceIds.hpp). They use a vendor register
// protocol, not AV/C: the unit is not a TA 61883 unit, so no FCP may be sent,
// and NoAutomaticTraffic makes CommandFilterFor() return BlockAll. Every
// captured GUID has a zero serial field, so two identical units would share a
// GUID.
constexpr AudioDeviceDefinition Digi00xDefinition(DeviceDefinitionId id,
                                                  uint32_t unitSpecifierId,
                                                  uint32_t unitModelId,
                                                  const char* modelName) {
    return AudioDeviceDefinition{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {IdentityMatchClause{
                        .rootVendorId = MaskedValue32{kAvidVendorId},
                        .unitModelId = MaskedValue32{unitModelId},
                        .unitSpecifierId = MaskedValue32{unitSpecifierId},
                        .unitVersion = MaskedValue32{kDigi00xUnitVersion},
                    },
                    IdentityMatchClause{}},
        .clauseCount = 1,
        .family = AudioFamilyProviderId::None,
        .probePolicy = ProbePolicyId::NoAutomaticTraffic,
        .profileBuilder = ProfileBuilderId::None,
        .protocolImplementation = ProtocolImplementationId::None,
        .support = SupportDisposition::RecognizedUnsupported,
        .guidReliability = GuidReliability::KnownNonUnique,
        .persistentKeyRecipe = PersistentKeyRecipeId::None,
        .vendorName = kDigidesignVendorName,
        .modelName = modelName,
    };
}

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
    Digi00xDefinition(DeviceDefinitionId::Digi002Console, kDigi002ConsoleSpecifierId,
                      kDigi00xConsoleModelId, kDigi002ConsoleModelName),
    Digi00xDefinition(DeviceDefinitionId::Digi002Rack, kDigi002RackSpecifierId,
                      kDigi00xRackModelId, kDigi002RackModelName),
    Digi00xDefinition(DeviceDefinitionId::Digi003Console, kDigi003ConsoleSpecifierId,
                      kDigi00xConsoleModelId, kDigi003ConsoleModelName),
    Digi00xDefinition(DeviceDefinitionId::Digi003Rack, kDigi003RackSpecifierId,
                      kDigi00xRackModelId, kDigi003RackModelName),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
