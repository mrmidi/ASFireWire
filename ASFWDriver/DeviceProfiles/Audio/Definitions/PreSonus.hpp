// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kPreSonusDefinitions{
    Definition(DeviceDefinitionId::PreSonusStudioLive1602, kPreSonusVendorId,
               kStudioLive1602ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::PreSonusStudioLive1602,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported, kPreSonusVendorName,
               kStudioLive1602ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::PreSonusStudioLive1642, kPreSonusVendorId,
               kStudioLive1642ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kPreSonusVendorName,
               kStudioLive1642ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    // Anna's 24.4.2 (GUID 0x000A9204049204CB): profile landed in #122, the
    // DeviceProtocolFactory clause it was missing in #124. Its playback side is
    // asymmetric (16 + 10), which is what Stage 3 has to frame per stream.
    Definition(DeviceDefinitionId::PreSonusStudioLive2442, kPreSonusVendorId,
               kStudioLive2442ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::PreSonusStudioLive2442,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported, kPreSonusVendorName,
               kStudioLive2442ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::PreSonusStudioLive3242, kPreSonusVendorId,
               kStudioLive3242ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kPreSonusVendorName,
               kStudioLive3242ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
