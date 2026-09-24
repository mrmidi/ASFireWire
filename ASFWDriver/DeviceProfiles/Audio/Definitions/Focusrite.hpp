// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kFocusriteDefinitions{
    Definition(DeviceDefinitionId::FocusriteSPro14, kFocusriteVendorId, kSPro14ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::FocusriteSPro14,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported,
               kFocusriteVendorName, kSPro14ModelName, kSPro14ModelId,
               BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro24, kFocusriteVendorId, kSPro24ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::FocusriteSPro24,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported,
               kFocusriteVendorName, kSPro24ModelName, kSPro24ModelId,
               BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro24Dsp, kFocusriteVendorId,
               kSPro24DspModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::DiceTcat, ProfileBuilderId::FocusriteSPro24Dsp,
               ProtocolImplementationId::DiceSPro24Dsp,
               SupportDisposition::Supported, kFocusriteVendorName,
               kSPro24DspModelName, kSPro24DspModelId, BootloaderCuePolicy::None,
               // The DSP model is the one device whose wire format depends on its
               // runtime configuration rather than its identity: 8 PCM in 9 slots
               // is raw 24-in-32, anything else is AM824.
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking, .rawPcm24In32WhenEightInNineSlots = true}}),
    Definition(DeviceDefinitionId::FocusriteSPro40, kFocusriteVendorId, kSPro40ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::FocusriteSPro40,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported,
               kFocusriteVendorName, kSPro40ModelName, kSPro40ModelId,
               BootloaderCuePolicy::None, kDiceTraits),
    // `midi` marks the Liquid Saffire 56 Supported because it carries a
    // FocusriteLiquidS56 builder. This branch does not: the LS56 bring-up work
    // is uncommitted, so a Supported row here would resolve to a builder that
    // does not exist -- the #115 failure exactly. Flipping the two fields is
    // the whole change once the builder lands.
    Definition(DeviceDefinitionId::FocusriteLiquidS56, kFocusriteVendorId,
               kLiquidS56ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kFocusriteVendorName,
               kLiquidS56ModelName, kLiquidS56ModelId, BootloaderCuePolicy::None,
               kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro26, kFocusriteVendorId, kSPro26ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::None, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kFocusriteVendorName,
               kSPro26ModelName, kSPro26ModelId, BootloaderCuePolicy::None,
               kDiceTraits),
    Definition(DeviceDefinitionId::FocusriteSPro40Tcd3070, kFocusriteVendorId,
               kSPro40Tcd3070ModelId, AudioFamilyProviderId::DICE,
               ProbePolicyId::None, ProfileBuilderId::None,
               ProtocolImplementationId::None,
               SupportDisposition::RecognizedUnsupported, kFocusriteVendorName,
               kSPro40Tcd3070ModelName, kFocusriteGuidModelSPro40Tcd3070,
               BootloaderCuePolicy::None, kDiceTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
