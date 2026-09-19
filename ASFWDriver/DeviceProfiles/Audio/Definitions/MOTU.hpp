// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "CatalogHelpers.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

inline constexpr std::array kMotuDefinitions{
    // Only the 828mkII is hardware-verified (Config ROM captured 2026-07-26).
    // The UltraLite shares its chunk layout exactly (motu-protocol-v2.c:274-282
    // vs :302-310) and differs only in a fetching-mode write the protocol
    // already handles, so it rides along. The other three resolve an identity
    // so they are named in diagnostics, and stop there until their chunk
    // layouts are confirmed against real hardware.
    MotuDefinition(DeviceDefinitionId::Motu828mk2, kMotu828mk2SwVersion,
                   ProfileBuilderId::Motu828mk2, SupportDisposition::Supported,
                   kMotu828mk2ModelName),
    MotuDefinition(DeviceDefinitionId::MotuUltralite, kMotuUltraliteSwVersion,
                   ProfileBuilderId::MotuUltralite, SupportDisposition::Supported,
                   kMotuUltraliteModelName),
    MotuDefinition(DeviceDefinitionId::Motu896hd, kMotu896hdSwVersion,
                   ProfileBuilderId::None,
                   SupportDisposition::RecognizedUnsupported, kMotu896hdModelName),
    MotuDefinition(DeviceDefinitionId::MotuTraveler, kMotuTravelerSwVersion,
                   ProfileBuilderId::None,
                   SupportDisposition::RecognizedUnsupported, kMotuTravelerModelName),
    MotuDefinition(DeviceDefinitionId::Motu8pre, kMotu8preSwVersion,
                   ProfileBuilderId::None,
                   SupportDisposition::RecognizedUnsupported, kMotu8preModelName),
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
