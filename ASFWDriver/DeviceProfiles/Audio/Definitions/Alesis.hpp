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
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking}),
    // Recognition only -- its geometry has never been captured, so it names no
    // builder and nothing streams it.
    Definition(DeviceDefinitionId::AlesisIo, kAlesisVendorId, kAlesisIoModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None,
               ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported,
               kAlesisVendorName, kAlesisIoModelName, std::nullopt,
               BootloaderCuePolicy::None,
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking}),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
