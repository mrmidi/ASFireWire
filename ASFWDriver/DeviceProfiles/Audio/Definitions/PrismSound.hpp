// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kPrismDefinitions{
    // Verified on hardware (ADA-8XR FW card behind an Apple TB3->TB2->FW800
    // chain, M-series Mac, 2026-10): enumerates and streams 8-in/8-out AM824
    // at 48 kHz via the generic path — reported as "Dream ADA-8" from its ROM
    // strings until this entry gives it its name. Linux runs it spec_normal.
    Definition(DeviceDefinitionId::PrismAda8Xr, kPrismVendorId,
               kPrismAda8XrModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBPlug0, ProfileBuilderId::GenericAvc,
               ProtocolImplementationId::GenericAvc,
               SupportDisposition::Supported, kPrismVendorName,
               kPrismAda8XrModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingTraits),
    // Not run on hardware; catalogued from the Linux snd-bebob entry. Linux
    // gives the Orpheus (BeBoB v3) two DBC quirks the ADA-8XR does not carry
    // (commit c4d860a0d256: CIP_EMPTY_HAS_WRONG_DBC /
    // CIP_SKIP_DBC_ZERO_CHECK) — if periodic dropouts show up on one, that is
    // the first knob to reach for.
    Definition(DeviceDefinitionId::PrismOrpheus, kPrismVendorId,
               kPrismOrpheusModelId, AudioFamilyProviderId::BeBoB,
               ProbePolicyId::BeBoBPlug0, ProfileBuilderId::GenericAvc,
               ProtocolImplementationId::GenericAvc,
               SupportDisposition::Supported, kPrismVendorName,
               kPrismOrpheusModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
