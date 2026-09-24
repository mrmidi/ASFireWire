// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

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
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .startShape = StreamStartShape::MAudioSpecial,
                                  .cmpChoosesIsoChannel = true,
                                  .startRatePinHz = 48000U}),
    Definition(DeviceDefinitionId::MAudioProjectMix, kMAudioVendorId,
               kMAudioProjectMixModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::MAudioProjectMix,
               ProtocolImplementationId::BeBoBMAudioSpecial,
               SupportDisposition::Supported, kMAudioVendorName,
               kMAudioProjectMixModelName, std::nullopt, BootloaderCuePolicy::None,
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .startShape = StreamStartShape::MAudioSpecial,
                                  .cmpChoosesIsoChannel = true,
                                  .startRatePinHz = 48000U}),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
