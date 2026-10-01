// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kAlesisDefinitions{
    Definition(DeviceDefinitionId::AlesisMultiMix, kAlesisVendorId,
               kAlesisMultiMixModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::AlesisMultiMix,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported, kAlesisVendorName,
               kAlesisMultiMixModelName, std::nullopt, BootloaderCuePolicy::None,
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking}}),
    // Generic DICE: its stream counts come from its registers like the
    // MultiMix's. Not run on hardware.
    Definition(DeviceDefinitionId::AlesisIo, kAlesisVendorId, kAlesisIoModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::GenericDice,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported,
               kAlesisVendorName, kAlesisIoModelName, std::nullopt,
               BootloaderCuePolicy::None,
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking}}),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
