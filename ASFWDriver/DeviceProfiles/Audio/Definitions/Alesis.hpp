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
               SupportDisposition::Supported, kAlesisVendorName,
               kAlesisMultiMixModelName, std::nullopt, BootloaderCuePolicy::None,
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .clampCaptureStreamsToOne = true}),
    // Recognition only -- its geometry has never been captured, so it names no
    // builder and nothing streams it. The row exists to hold the capture-stream
    // clamp, which FFADO applies to this model as well as the MultiMix; losing
    // that when the predicate went would have been losing evidence, not
    // deleting dead code.
    Definition(DeviceDefinitionId::AlesisIo, kAlesisVendorId, kAlesisIoModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None,
               ProfileBuilderId::None, SupportDisposition::RecognizedUnsupported,
               kAlesisVendorName, kAlesisIoModelName, std::nullopt,
               BootloaderCuePolicy::None,
               DeviceStreamTraits{.forcedStreamMode = ForcedStreamMode::Blocking,
                                  .clampCaptureStreamsToOne = true}),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
