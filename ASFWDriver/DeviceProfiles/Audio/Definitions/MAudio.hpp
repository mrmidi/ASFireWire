// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// A DICE row whose selected unit carries M-Audio's own interface version.
// Definition() pins every DICE row to version 0x000001, which the ProFire's
// unit directory does not carry; the unit is pinned to the exact specifier and
// version the hardware reports instead, so a non-DICE M-Audio unit that shares
// the OUI and model id still cannot match.
constexpr AudioDeviceDefinition MAudioProFireDefinition(DeviceDefinitionId id, uint32_t model,
                                                        const char* modelName) {
    auto definition = Definition(id, kMAudioVendorId, model, AudioFamilyProviderId::DICE,
                                 ProbePolicyId::DiceTcat, ProfileBuilderId::GenericDice,
                                 ProtocolImplementationId::DiceTcat,
                                 SupportDisposition::Supported, kMAudioVendorName, modelName,
                                 std::nullopt, BootloaderCuePolicy::None, kDiceTraits);
    definition.clauses[0].unitSpecifierId = MaskedValue32{kMAudioVendorId};
    definition.clauses[0].unitVersion = MaskedValue32{kMAudioProFireUnitVersion};
    return definition;
}

inline constexpr std::array kMAudioDefinitions{
    // ---- M-Audio "special firmware" ----
    // The bootloader persona is never an audio endpoint. The two operational
    // personas use a fixed formation and the narrow special-firmware FCP gate;
    // no generic or BridgeCo AV/C discovery may run against them.
    //
    // Without a definition, an 1814 is an ordinary AV/C unit, and AVCDiscovery
    // opens on it with generic UNIT_INFO / SUBUNIT_INFO -- two of the four
    // shapes AVC_DEVICE_HAZARDS.md H1 records as freeze-capable on this
    // firmware. ProbePolicyId::BeBoBFilteredCommandSet resolves through
    // CommandFilterFor() to AvcCommandFilterId::MAudioSpecialBeBoB, and
    // FCPTransport::SubmitCommand then refuses every frame the allowlist in
    // AVCCommandFilter.hpp does not name. So recognising these devices is what
    // makes them safe to have on the bus at all.
    //
    // Never an audio endpoint: CommandFilterFor blocks all FCP traffic and the
    // probe policy selects no bootstrap. The cue policy is consumed by the
    // guarded BeBoB bootloader preparation (Protocols/BeBoB/Bootloader).
    Definition(DeviceDefinitionId::MAudioFireWire1814Bootloader, kMAudioVendorId,
               kMAudioFireWire1814BootloaderModelId, AudioFamilyProviderId::None,
               ProbePolicyId::NoAutomaticTraffic, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioFireWire1814BootloaderModelName, std::nullopt,
               BootloaderCuePolicy::BeBoBStartFirmware),
    Definition(DeviceDefinitionId::MAudioFireWire1814, kMAudioVendorId,
               kMAudioFireWire1814ModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::MAudioFireWire1814,
               ProtocolImplementationId::BeBoBMAudioSpecial,
               SupportDisposition::Supported, kMAudioVendorName,
               kMAudioFireWire1814ModelName, std::nullopt, BootloaderCuePolicy::None,
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking},
                                  .resource = {.irmChannelMask = kAnyIsoChannel},
                                  .start = {.startShape = StreamStartShape::MAudioSpecial,
                                            .startRatePinHz = 48000U}}),
    Definition(DeviceDefinitionId::MAudioProjectMix, kMAudioVendorId,
               kMAudioProjectMixModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::MAudioProjectMix,
               ProtocolImplementationId::BeBoBMAudioSpecial,
               SupportDisposition::Supported, kMAudioVendorName,
               kMAudioProjectMixModelName, std::nullopt, BootloaderCuePolicy::None,
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking},
                                  .resource = {.irmChannelMask = kAnyIsoChannel},
                                  .start = {.startShape = StreamStartShape::MAudioSpecial,
                                            .startRatePinHz = 48000U}}),

    // ---- ProFire (DICE II / TCAT) ----
    // Not BeBoB: the ProFire line is a standard TCAT DICE device behind a
    // non-standard unit version (AudioDeviceIds.hpp). Linux's snd-dice binds it
    // on vendor + model and reads its stream formats from the device, so it
    // takes the generic DICE profile: channels and rates come from its registers.
    MAudioProFireDefinition(DeviceDefinitionId::MAudioProFire2626, kMAudioProFire2626ModelId,
                            kMAudioProFire2626ModelName),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
