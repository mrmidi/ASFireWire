// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../AudioDeviceCatalog.hpp"
#include "../AudioDeviceIds.hpp"

#include <array>

namespace ASFW::DeviceProfiles::Audio::Definitions {

// RME's former Fireface devices use an RME-specific unit specifier/version,
// not the AV/C unit identity. On real hardware (IEEE 1212 Unit Directory key 0x17),
// model ID 0x101800 is published in the unit directory while root model ID is absent.
// Clause 0 matches real hardware (unit directory model ID), Clause 1 provides
// backwards compatibility for fixtures specifying root model ID.
// These personas use a proprietary register protocol, so AV/C/FCP remains blocked
// even after direct-register support.
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
                        .unitModelId = MaskedValue32{kRmeModelId},
                        .unitSpecifierId = MaskedValue32{kRmeUnitSpecifierId},
                        .unitVersion = MaskedValue32{unitVersion},
                    },
                    IdentityMatchClause{
                        .rootVendorId = MaskedValue32{kRmeVendorId},
                        .rootModelId = MaskedValue32{kRmeModelId},
                        .unitSpecifierId = MaskedValue32{kRmeUnitSpecifierId},
                        .unitVersion = MaskedValue32{unitVersion},
                    }},
        .clauseCount = 2,
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
