// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// Digidesign Digi 002 / 003 (issue #249): the Digi00x register family.
//
// The model id lives in the unit directory only (the root directory has none),
// so the generic Definition() helper, which constrains the root model, cannot
// express these rows. As Linux, one row per model id (Console, Rack) matches
// vendor + version + model; the 002 and 003 share model ids, so the unit
// specifier picks the product name (AudioDeviceIds.hpp). They use a vendor register
// protocol, not AV/C: the unit is not a TA 61883 unit, so no FCP may be sent
// (CommandFilterFor() returns BlockAll for the Digi00xRegister probe). Every
// captured GUID has a zero serial field, so two identical units would share a
// GUID.
//
// Stream traits, cross-validated with Linux sound/firewire/digi00x
// (digi00x-stream.c): the IRM may pick any channel for either direction, and
// the family driver writes both to the device (6 bits each); the device sends
// nothing until it receives, so host transmit starts first. Capture SYT is not
// a presentation time (amdtp-dot.c CIP_UNAWARE_SYT; a 002 Console sends 0x0000
// in every packet), so receive timing comes from packet continuity. Rates, geometry
// and the wire format come from the Digi00x profile builder.
inline constexpr std::array kDigi00xConsoleNames{
    SpecifierModelName{kDigi002ConsoleSpecifierId, kDigi002ConsoleModelName},
    SpecifierModelName{kDigi003ConsoleSpecifierId, kDigi003ConsoleModelName},
};
inline constexpr std::array kDigi00xRackNames{
    SpecifierModelName{kDigi002RackSpecifierId, kDigi002RackModelName},
    SpecifierModelName{kDigi003RackSpecifierId, kDigi003RackModelName},
};

constexpr AudioDeviceDefinition Digi00xDefinition(
    DeviceDefinitionId id, uint32_t unitModelId, const char* modelName,
    std::span<const SpecifierModelName> namesBySpecifier) {
    return AudioDeviceDefinition{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {IdentityMatchClause{
                        .rootVendorId = MaskedValue32{kAvidVendorId},
                        .unitModelId = MaskedValue32{unitModelId},
                        .unitVersion = MaskedValue32{kDigi00xUnitVersion},
                    },
                    IdentityMatchClause{}},
        .clauseCount = 1,
        .family = AudioFamilyProviderId::Digi00xRegister,
        .probePolicy = ProbePolicyId::Digi00xRegister,
        .profileBuilder = ProfileBuilderId::Digi00x,
        .protocolImplementation = ProtocolImplementationId::Digi00x,
        .support = SupportDisposition::Supported,
        .guidReliability = GuidReliability::KnownNonUnique,
        .persistentKeyRecipe = PersistentKeyRecipeId::None,
        .streamTraits = DeviceStreamTraits{
            .wire = StreamWirePolicy{.captureSytUnaware = true},
            .resource = IsochResourcePolicy{.irmChannelMask = kAnyIsoChannel},
            .start = StreamStartPolicy{.startShape = StreamStartShape::TransmitFirst},
        },
        .vendorName = kDigidesignVendorName,
        .modelName = modelName,
        .modelNamesBySpecifier = namesBySpecifier,
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
    Digi00xDefinition(DeviceDefinitionId::Digi00xConsole, kDigi00xConsoleModelId,
                      kDigi00xConsoleModelName, kDigi00xConsoleNames),
    Digi00xDefinition(DeviceDefinitionId::Digi00xRack, kDigi00xRackModelId,
                      kDigi00xRackModelName, kDigi00xRackNames),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
