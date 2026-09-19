// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../AudioDeviceCatalog.hpp"
#include "../AudioDeviceIds.hpp"

#include <optional>

namespace ASFW::DeviceProfiles::Audio::Definitions {

constexpr IdentityMatchClause Root(uint32_t vendor, uint32_t model) {
    return IdentityMatchClause{.rootVendorId = MaskedValue32{vendor},
                               .rootModelId = MaskedValue32{model}};
}

constexpr IdentityMatchClause GuidEncoded(uint32_t vendor, uint32_t model) {
    constexpr uint64_t kOuiMask = 0xFFFF'FF00'0000'0000ULL;
    constexpr uint64_t kModelMask = 0x0000'0000'0FC0'0000ULL;
    return IdentityMatchClause{
        .observedGuid = MaskedValue64{
            (static_cast<uint64_t>(vendor) << 40U) |
                (static_cast<uint64_t>(model & 0x3FU) << 22U),
            kOuiMask | kModelMask},
    };
}

constexpr AudioDeviceDefinition Definition(
    DeviceDefinitionId id, uint32_t vendor, uint32_t model,
    AudioFamilyProviderId family, ProbePolicyId probe, ProfileBuilderId builder,
    SupportDisposition support, const char* vendorName, const char* modelName,
    std::optional<uint32_t> guidModel = std::nullopt,
    BootloaderCuePolicy bootloaderCue = BootloaderCuePolicy::None,
    DeviceStreamTraits streamTraits = {}) {
    auto rootClause = Root(vendor, model);
    auto guidClause = IdentityMatchClause{};

    const auto constrainSelectedUnit = [family](IdentityMatchClause& clause) constexpr {
        switch (family) {
            case AudioFamilyProviderId::DICE:
                clause.unitVersion = MaskedValue32{0x000001};
                break;
            case AudioFamilyProviderId::BeBoB:
                clause.unitSpecifierId = MaskedValue32{0x00A02D};
                break;
            case AudioFamilyProviderId::Fireworks:
                clause.unitSpecifierId = MaskedValue32{0x00A02D};
                clause.unitVersion = MaskedValue32{0x010000};
                break;
            case AudioFamilyProviderId::OXFW:
            case AudioFamilyProviderId::GenericAvc:
                clause.unitSpecifierId = MaskedValue32{0x00A02D};
                clause.unitVersion = MaskedValue32{0x010001};
                break;
            case AudioFamilyProviderId::MotuRegister:
                break;
            case AudioFamilyProviderId::None:
                break;
        }
    };
    constrainSelectedUnit(rootClause);

    AudioDeviceDefinition result{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {rootClause, IdentityMatchClause{}},
        .clauseCount = 1,
        .family = family,
        .probePolicy = probe,
        .profileBuilder = builder,
        .support = support,
        .guidReliability = GuidReliability::ReliableWhenUnique,
        .streamTraits = streamTraits,
        .bootloaderCue = bootloaderCue,
        .vendorName = vendorName,
        .modelName = modelName,
    };
    if (guidModel.has_value()) {
        guidClause = GuidEncoded(vendor, *guidModel);
        constrainSelectedUnit(guidClause);
        result.clauses[1] = guidClause;
        result.clauseCount = 2;
    }
    return result;
}

constexpr AudioDeviceDefinition MotuDefinition(DeviceDefinitionId id,
                                               uint32_t swVersion,
                                               ProfileBuilderId builder,
                                               SupportDisposition support,
                                               const char* modelName) {
    return AudioDeviceDefinition{
        .id = id,
        .variantId = static_cast<uint32_t>(id),
        .clauses = {IdentityMatchClause{
                        .rootVendorId = MaskedValue32{kMotuVendorId},
                        .rootModelId = MaskedValue32{0},
                        .unitSpecifierId = MaskedValue32{kMotuVendorId},
                        .unitVersion = MaskedValue32{swVersion},
                    },
                    IdentityMatchClause{}},
        .clauseCount = 1,
        .family = AudioFamilyProviderId::MotuRegister,
        .probePolicy = support == SupportDisposition::Supported
                           ? ProbePolicyId::MotuRegister
                           : ProbePolicyId::None,
        .profileBuilder = builder,
        .support = support,
        .guidReliability = GuidReliability::ReliableWhenUnique,
        .vendorName = kMotuVendorName,
        .modelName = modelName,
    };
}

constexpr DeviceStreamTraits kDiceTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
};

constexpr DeviceStreamTraits kCmpBlockingTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
    .startShape = StreamStartShape::CmpReceiveThenTransmit,
    .cmpChoosesIsoChannel = true,
};

constexpr DeviceStreamTraits kMackieBlockingTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
};

constexpr DeviceStreamTraits kCmpBlockingUntrustedStrideTraits{
    .forcedStreamMode = ForcedStreamMode::Blocking,
    .startShape = StreamStartShape::CmpReceiveThenTransmit,
    .cmpChoosesIsoChannel = true,
    .captureTrustConfiguredStride = true,
    .startRatePinHz = 44100U,
};

} // namespace ASFW::DeviceProfiles::Audio::Definitions
