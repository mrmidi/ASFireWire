// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kMAudioDefinitions{
    // ---- M-Audio "special firmware" ----
    // These three exist here for their probe policy, not for audio: this branch
    // has no MAudioSpecialProtocol, so none of them names a builder and none
    // will be streamed. What they carry is the bound on what may be *sent*.
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
    // The bootloader persona is not an audio endpoint and never becomes one.
    // Its BootloaderCuePolicy has no consumer on this branch; it is carried so
    // the identity is complete and so the merge with `midi` stays textual.
    Definition(DeviceDefinitionId::MAudioFireWire1814Bootloader, kMAudioVendorId,
               kMAudioFireWire1814BootloaderModelId, AudioFamilyProviderId::None,
               ProbePolicyId::NoAutomaticTraffic, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioFireWire1814BootloaderModelName, std::nullopt,
               BootloaderCuePolicy::BeBoBStartFirmware),
    Definition(DeviceDefinitionId::MAudioFireWire1814, kMAudioVendorId,
               kMAudioFireWire1814ModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioFireWire1814ModelName),
    Definition(DeviceDefinitionId::MAudioProjectMix, kMAudioVendorId,
               kMAudioProjectMixModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBFilteredCommandSet, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kMAudioVendorName,
               kMAudioProjectMixModelName),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
