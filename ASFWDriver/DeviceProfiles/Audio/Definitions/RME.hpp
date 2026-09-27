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
                                               const char* modelName,
                                               ProfileBuilderId builder,
                                               uint64_t channelMask) {
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
        .family = AudioFamilyProviderId::RmeRegister,
        .probePolicy = ProbePolicyId::RmeRegister,
        .profileBuilder = builder,
        .protocolImplementation = ProtocolImplementationId::RmeFireface,
        .support = SupportDisposition::Supported,
        .guidReliability = GuidReliability::ReliableWhenUnique,
        .streamTraits = DeviceStreamTraits{
            .wire = StreamWirePolicy{.forcedStreamMode = ForcedStreamMode::Blocking,
                                     .headerlessUpper24LE = true},
            .resource = IsochResourcePolicy{.irmChannelMask = channelMask},
            .start = StreamStartPolicy{.startShape = StreamStartShape::CmpReceiveThenTransmit,
                                       .startRatePinHz = 48000},
        },
        .vendorName = kRmeVendorName,
        .modelName = modelName,
    };
}

inline constexpr std::array kRmeDefinitions{
    RmeDefinition(DeviceDefinitionId::RmeFireface400,
                  kRmeFireface400UnitVersion, kRmeFireface400ModelName,
                  ProfileBuilderId::RmeFireface400, 0xffU),
    RmeDefinition(DeviceDefinitionId::RmeFireface800,
                  kRmeFireface800UnitVersion, kRmeFireface800ModelName,
                  ProfileBuilderId::RmeFireface800, kAnyIsoChannel),
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
