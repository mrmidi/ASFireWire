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
                   ProfileBuilderId::Motu828mk2,
                   ProtocolImplementationId::MotuV2,
                   SupportDisposition::Supported,
                   kMotu828mk2ModelName),
    MotuDefinition(DeviceDefinitionId::MotuUltralite, kMotuUltraliteSwVersion,
                   ProfileBuilderId::MotuUltralite,
                   ProtocolImplementationId::MotuV2,
                   SupportDisposition::Supported,
                   kMotuUltraliteModelName),
    MotuDefinition(DeviceDefinitionId::Motu896hd, kMotu896hdSwVersion,
                   ProfileBuilderId::None,
                   ProtocolImplementationId::None,
                   SupportDisposition::RecognizedUnsupported, kMotu896hdModelName),
    MotuDefinition(DeviceDefinitionId::MotuTraveler, kMotuTravelerSwVersion,
                   ProfileBuilderId::None,
                   ProtocolImplementationId::None,
                   SupportDisposition::RecognizedUnsupported, kMotuTravelerModelName),
    MotuDefinition(DeviceDefinitionId::Motu8pre, kMotu8preSwVersion,
                   ProfileBuilderId::None,
                   ProtocolImplementationId::None,
                   SupportDisposition::RecognizedUnsupported, kMotu8preModelName),
    // Protocol v3. Matched on Unit_Spec_Id + Unit_Sw_Version 0x000015 (828mk3
    // FireWire-only, Config ROM read on the device); its root directory has no
    // Model_Id, hence the unconstrained root model. Pinned to 48 kHz: the only
    // rate the hardware evidence covers.
    MotuDefinition(DeviceDefinitionId::Motu828mk3, kMotu828mk3SwVersion,
                   ProfileBuilderId::Motu828mk3,
                   ProtocolImplementationId::MotuV3,
                   SupportDisposition::Supported,
                   kMotu828Mk3ModelName,
                   DeviceStreamTraits{.start = {.startRatePinHz = 48000U}},
                   MotuRootModel::Unconstrained),
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
