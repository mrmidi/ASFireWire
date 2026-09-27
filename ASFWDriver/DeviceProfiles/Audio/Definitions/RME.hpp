// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../AudioDeviceCatalog.hpp"
#include "../AudioDeviceIds.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// RME's former Fireface devices use an RME-specific unit specifier/version,
// not the AV/C unit identity. These personas use a proprietary register
// protocol, so AV/C/FCP remains blocked even after direct-register support.
constexpr AudioDeviceDefinition RmeDefinition(DeviceDefinitionId id,
                                               uint32_t unitVersion,
                                               const char* modelName) {
    return AudioDeviceDefinition{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {IdentityMatchClause{
                        .rootVendorId = MaskedValue32{kRmeVendorId},
                        .rootModelId = MaskedValue32{kRmeRootModelId},
                        .unitSpecifierId = MaskedValue32{kRmeUnitSpecifierId},
                        .unitVersion = MaskedValue32{unitVersion},
                    },
                    IdentityMatchClause{}},
        .clauseCount = 1,
        .family = AudioFamilyProviderId::None,
        .probePolicy = ProbePolicyId::NoAutomaticTraffic,
        .profileBuilder = ProfileBuilderId::None,
        .protocolImplementation = ProtocolImplementationId::None,
        .support = SupportDisposition::RecognizedUnsupported,
        .guidReliability = GuidReliability::ReliableWhenUnique,
        .vendorName = kRmeVendorName,
        .modelName = modelName,
    };
}

inline constexpr std::array kRmeDefinitions{
    RmeDefinition(DeviceDefinitionId::RmeFireface400,
                  kRmeFireface400UnitVersion, kRmeFireface400ModelName),
    RmeDefinition(DeviceDefinitionId::RmeFireface800,
                  kRmeFireface800UnitVersion, kRmeFireface800ModelName),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
