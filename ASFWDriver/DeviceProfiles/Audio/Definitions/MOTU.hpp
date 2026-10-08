// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kMotuDefinitions{
    // Best-effort FireWire-only models. USB/hybrid identities remain recognized.
    MotuDefinition(DeviceDefinitionId::Motu828mk2, kMotu828mk2SwVersion,
                   ProfileBuilderId::Motu828mk2,
                   ProtocolImplementationId::MotuRegister,
                   SupportDisposition::Supported,
                   kMotu828mk2ModelName),
    MotuDefinition(DeviceDefinitionId::MotuUltralite, kMotuUltraliteSwVersion,
                   ProfileBuilderId::MotuUltralite,
                   ProtocolImplementationId::MotuRegister,
                   SupportDisposition::Supported,
                   kMotuUltraliteModelName),
    MotuDefinition(DeviceDefinitionId::Motu896hd, kMotu896hdSwVersion,
                   ProfileBuilderId::Motu896hd,
                   ProtocolImplementationId::MotuRegister,
                   SupportDisposition::Supported, kMotu896hdModelName),
    MotuDefinition(DeviceDefinitionId::MotuTraveler, kMotuTravelerSwVersion,
                   ProfileBuilderId::MotuTraveler,
                   ProtocolImplementationId::MotuRegister,
                   SupportDisposition::Supported, kMotuTravelerModelName),
    MotuDefinition(DeviceDefinitionId::Motu8pre, kMotu8preSwVersion,
                   ProfileBuilderId::Motu8pre,
                   ProtocolImplementationId::MotuRegister,
                   SupportDisposition::Supported, kMotu8preModelName),
    MotuDefinition(DeviceDefinitionId::Motu828, 1, ProfileBuilderId::Motu828, ProtocolImplementationId::MotuRegister, SupportDisposition::Supported, "828"),
    MotuDefinition(DeviceDefinitionId::Motu896, 2, ProfileBuilderId::Motu896, ProtocolImplementationId::MotuRegister, SupportDisposition::Supported, "896"),
    MotuDefinition(DeviceDefinitionId::Motu828mk3, 21, ProfileBuilderId::Motu828mk3, ProtocolImplementationId::MotuRegister, SupportDisposition::Supported, "828mk3"),
    MotuDefinition(DeviceDefinitionId::Motu896mk3, 23, ProfileBuilderId::Motu896mk3, ProtocolImplementationId::MotuRegister, SupportDisposition::Supported, "896mk3"),
    MotuDefinition(DeviceDefinitionId::MotuUltraliteMk3, 25, ProfileBuilderId::MotuUltraliteMk3, ProtocolImplementationId::MotuRegister, SupportDisposition::Supported, "UltraLite mk3"),
    MotuDefinition(DeviceDefinitionId::MotuTravelerMk3, 27, ProfileBuilderId::MotuTravelerMk3, ProtocolImplementationId::MotuRegister, SupportDisposition::Supported, "Traveler mk3"),
    MotuDefinition(DeviceDefinitionId::MotuUltraliteMk3Hybrid, 48, ProfileBuilderId::None, ProtocolImplementationId::None, SupportDisposition::RecognizedUnsupported, "UltraLite mk3 Hybrid"),
    MotuDefinition(DeviceDefinitionId::MotuAudioExpress, 51, ProfileBuilderId::None, ProtocolImplementationId::None, SupportDisposition::RecognizedUnsupported, "Audio Express"),
    MotuDefinition(DeviceDefinitionId::Motu828mk3Hybrid, 53, ProfileBuilderId::None, ProtocolImplementationId::None, SupportDisposition::RecognizedUnsupported, "828mk3 Hybrid"),
    MotuDefinition(DeviceDefinitionId::Motu896mk3Hybrid, 55, ProfileBuilderId::None, ProtocolImplementationId::None, SupportDisposition::RecognizedUnsupported, "896mk3 Hybrid"),
    MotuDefinition(DeviceDefinitionId::MotuTrack16, 57, ProfileBuilderId::None, ProtocolImplementationId::None, SupportDisposition::RecognizedUnsupported, "Track16"),
    MotuDefinition(DeviceDefinitionId::Motu4pre, 69, ProfileBuilderId::None, ProtocolImplementationId::None, SupportDisposition::RecognizedUnsupported, "4pre"),
};

[[nodiscard]] constexpr const char* MotuModelNameForSwVersion(uint32_t swVersion) noexcept {
    for (const auto& def : kMotuDefinitions) {
        if (def.clauseCount > 0 && def.clauses[0].unitVersion &&
            def.clauses[0].unitVersion->value == swVersion) {
            return def.modelName;
        }
    }
    return nullptr;
}

} // namespace ASFW::DeviceProfiles::Audio::Definitions
