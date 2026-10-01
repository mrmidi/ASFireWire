// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// The DACs, the Vesta and the MAN301 take the INT202's wire policy: 2 channels each way on
// the wire (Linux dice-weiss.c:14-75), CoreAudio shown only the host->device
// side, host transmit first. Not run on hardware. The ADC2 and the AFI1 have
// inputs that matter and stay on the generic duplex policy.
inline constexpr DeviceStreamTraits kWeissDacTraits{
    .wire = {.forcedStreamMode = ForcedStreamMode::Blocking},
    .start = {.startShape = StreamStartShape::TransmitFirst},
};

inline constexpr std::array kWeissDefinitions{
    Definition(DeviceDefinitionId::WeissAdc2, kWeissVendorId, kWeissAdc2ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::GenericDice,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissAdc2ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissVesta, kWeissVendorId, kWeissVestaModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::WeissDac,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissVestaModelName, std::nullopt, BootloaderCuePolicy::None, kWeissDacTraits),
    Definition(DeviceDefinitionId::WeissDac2, kWeissVendorId, kWeissDac2ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::WeissDac,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissDac2ModelName, std::nullopt, BootloaderCuePolicy::None, kWeissDacTraits),
    Definition(DeviceDefinitionId::WeissAfi1, kWeissVendorId, kWeissAfi1ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::GenericDice,
               ProtocolImplementationId::DiceTcat,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissAfi1ModelName, std::nullopt, BootloaderCuePolicy::None, kDiceTraits),
    Definition(DeviceDefinitionId::WeissInt202, kWeissVendorId, kWeissInt202ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::WeissInt202,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported,
               kWeissVendorName, kWeissInt202ModelName, std::nullopt, BootloaderCuePolicy::None,
               // Output-only in CoreAudio but duplex on the wire: host transmit
               // goes first after GLOBAL_ENABLE so its AM824 packets establish
               // the device receive-clock path. dice-weiss.c:10-35.
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking},
                                  .start = {.startShape = StreamStartShape::TransmitFirst}}),
    Definition(DeviceDefinitionId::WeissDac202, kWeissVendorId, kWeissDac202ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::WeissDac,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissDac202ModelName, std::nullopt, BootloaderCuePolicy::None, kWeissDacTraits),
    Definition(DeviceDefinitionId::WeissMaya, kWeissVendorId, kWeissMayaModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::WeissDac,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissMayaModelName, std::nullopt, BootloaderCuePolicy::None, kWeissDacTraits),
    Definition(DeviceDefinitionId::WeissInt203, kWeissVendorId, kWeissInt203ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat,
               ProfileBuilderId::WeissInt203,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported,
               kWeissVendorName, kWeissInt203ModelName, std::nullopt, BootloaderCuePolicy::None,
               // Output-only in CoreAudio but duplex on the wire: host transmit
               // goes first after GLOBAL_ENABLE so its AM824 packets establish
               // the device receive-clock path. dice-weiss.c:10-35.
               DeviceStreamTraits{.wire = {.forcedStreamMode = ForcedStreamMode::Blocking},
                                  .start = {.startShape = StreamStartShape::TransmitFirst}}),
    Definition(DeviceDefinitionId::WeissMan301, kWeissVendorId, kWeissMan301ModelId,
               AudioFamilyProviderId::DICE, ProbePolicyId::DiceTcat, ProfileBuilderId::WeissDac,
               ProtocolImplementationId::DiceWeissInt,
               SupportDisposition::Supported, kWeissVendorName,
               kWeissMan301ModelName, std::nullopt, BootloaderCuePolicy::None, kWeissDacTraits),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
