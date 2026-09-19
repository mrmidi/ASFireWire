// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kMackieDefinitions{
    // One OUI, three production runs. The Oxford run's shared id 0x081216 is
    // verified on a real 820i (2026-08-17 capture: unit specifier 0x00A02D
    // version 0x010001, ROM strings "Loud Technologies Inc." / "Onyx-i"); the
    // 400F is an Echo Fireworks part on version 0x010000, so the two cannot
    // collide on the unit constraint even though they share a vendor.
    //
    // kOnyx820iModelId is deliberately absent: it is still the
    // kMackieModelIdPendingCapture sentinel (0xffffffff), which is not a valid
    // 24-bit model id, and a clause built from it would be a row that can never
    // match. It gets a definition when a DICE-run unit is captured.
    Definition(DeviceDefinitionId::MackieOnyxIOxfw, kMackieVendorId,
               kOnyxIOxfwModelId, AudioFamilyProviderId::OXFW,
               ProbePolicyId::OxfwAvc, ProfileBuilderId::MackieOnyxIOxfw,
               SupportDisposition::Supported, kMackieVendorName,
               kOnyxIOxfwModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingUntrustedStrideTraits),
    Definition(DeviceDefinitionId::MackieOnyx400F, kMackieVendorId,
               kOnyx400FModelId, AudioFamilyProviderId::Fireworks,
               ProbePolicyId::FireworksEfc, ProfileBuilderId::MackieOnyx400F,
               SupportDisposition::Supported, kMackieVendorName,
               kOnyx400FModelName, std::nullopt, BootloaderCuePolicy::None,
               kCmpBlockingUntrustedStrideTraits),
    Definition(DeviceDefinitionId::MackieOnyx1640iOxfw, kMackieVendorId,
               kOnyx1640iOxfwModelId, AudioFamilyProviderId::OXFW,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyx1640iModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
    Definition(DeviceDefinitionId::MackieOnyx1640iDice, kMackieVendorId,
               kOnyx1640iDiceModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyx1640iModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
    Definition(DeviceDefinitionId::MackieOnyxBlackbird, kMackieVendorId,
               kOnyxBlackbirdModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyxBlackbirdModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
    Definition(DeviceDefinitionId::MackieOnyx1200F, kMackieVendorId,
               kOnyx1200FModelId, AudioFamilyProviderId::Fireworks,
               ProbePolicyId::None, ProfileBuilderId::None,
               SupportDisposition::RecognizedUnsupported, kMackieVendorName,
               kOnyx1200FModelName, std::nullopt, BootloaderCuePolicy::None,
               kMackieBlockingTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
