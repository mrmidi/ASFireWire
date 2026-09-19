// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioDeviceCatalog.hpp"

#include <algorithm>
#include <vector>

namespace ASFW::DeviceProfiles::Audio {

namespace {

[[nodiscard]] const Discovery::UnitIdentityEvidence*
FindUnit(const Discovery::DeviceIdentityEvidence& device, uint32_t directoryOffset) noexcept {
    const auto it = std::ranges::find_if(
        device.units,
        [directoryOffset](const Discovery::UnitIdentityEvidence& unit) {
            return unit.unitDirectoryOffset == directoryOffset;
        });
    return it != device.units.end() ? &*it : nullptr;
}

[[nodiscard]] const Discovery::UnitIdentityEvidence*
FindUnit(const Discovery::DeviceRecord& device, uint32_t directoryOffset) noexcept {
    return FindUnit(device.identity, directoryOffset);
}

} // namespace

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::ResolveWithDefinitions(
    const Discovery::DeviceIdentityEvidence& device,
    const Discovery::UnitIdentityEvidence& unit,
    std::span<const AudioDeviceDefinition> definitions,
    std::span<const AudioSafetyRule> safetyRules,
    bool allowGenericAvcFallback,
    Discovery::DeviceInstanceId instanceId) noexcept {
    for (const auto& rule : safetyRules) {
        for (uint8_t i = 0; i < rule.clauseCount; ++i) {
            if (rule.clauses[i].Matches(device, unit)) {
                return std::unexpected(CatalogResolutionError::HazardousIdentity);
            }
        }
    }

    struct Match {
        const AudioDeviceDefinition* definition{nullptr};
        uint8_t clauseIndex{0};
    };
    std::vector<Match> matches;
    for (const auto& definition : definitions) {
        for (uint8_t i = 0; i < definition.clauseCount; ++i) {
            if (definition.clauses[i].Matches(device, unit)) {
                matches.push_back(Match{&definition, i});
                break;
            }
        }
    }

    if (matches.empty()) {
        // 1394 TA general AV/C units advertise this exact specifier/version
        // pair. DICE uses a vendor specifier with interface version 0x000001,
        // so an unknown DICE unit cannot fall through into an FCP probe.
        // Cross-validated with protocols/ta1394/general/README.md:78 and Linux
        // firewire/dice/dice.c:248-262.
        if (!allowGenericAvcFallback ||
            unit.specifierId.value_or(0) != 0x00A02D ||
            unit.version.value_or(0) != 0x010001) {
            return std::unexpected(CatalogResolutionError::NoMatch);
        }
        return StaticAudioEndpointPlan{
            .unit = Discovery::UnitInstanceId{instanceId,
                                               unit.unitDirectoryOffset},
            .unitVersion = unit.version.value_or(0U),
            .exactVariantId = std::nullopt,
            .family = AudioFamilyProviderId::GenericAvc,
            .probePolicy = ProbePolicyId::GenericAvc,
            .support = SupportDisposition::GenericFallback,
            .guidReliability = GuidReliability::ReliableWhenUnique,
            .persistentKeyRecipe = PersistentKeyRecipeId::ReliableObservedEui64,
            .candidates = {DeviceDefinitionId::GenericAvc},
            .candidatePlans = {{DeviceDefinitionId::GenericAvc,
                                0,
                                ProfileBuilderId::GenericAvc,
                                {},
                                device.rootVendorName,
                                device.rootModelName.empty()
                                    ? "Generic AV/C Audio"
                                    : device.rootModelName}},
            .provenance = {{DeviceDefinitionId::GenericAvc, 0}},
            .profileBuilder = ProfileBuilderId::GenericAvc,
            .vendorName = device.rootVendorName,
            .modelName = device.rootModelName.empty()
                             ? "Generic AV/C Audio"
                             : device.rootModelName,
        };
    }

    const auto& first = *matches.front().definition;
    if (matches.size() > 1U) {
        if (first.equivalenceClassId == 0) {
            return std::unexpected(CatalogResolutionError::AmbiguousIdentity);
        }
        for (const auto& match : matches) {
            if (match.definition->equivalenceClassId != first.equivalenceClassId ||
                match.definition->family != first.family ||
                match.definition->probePolicy != first.probePolicy ||
                match.definition->support != first.support ||
                first.commonEquivalenceProfileBuilder == ProfileBuilderId::None ||
                match.definition->commonEquivalenceProfileBuilder !=
                    first.commonEquivalenceProfileBuilder) {
                return std::unexpected(CatalogResolutionError::AmbiguousIdentity);
            }
        }
    }

    StaticAudioEndpointPlan plan{
        .unit = Discovery::UnitInstanceId{instanceId,
                                           unit.unitDirectoryOffset},
        .unitVersion = unit.version.value_or(0U),
        .exactVariantId = matches.size() == 1U && first.variantId != 0
                              ? std::optional<uint32_t>{first.variantId}
                              : std::nullopt,
        .family = first.family,
        .probePolicy = first.probePolicy,
        .support = first.support,
        .guidReliability = first.guidReliability,
        .persistentKeyRecipe = first.persistentKeyRecipe,
        .equivalenceClassId = first.equivalenceClassId,
        .profileBuilder = matches.size() > 1U
                              ? first.commonEquivalenceProfileBuilder
                              : first.profileBuilder,
        .commonEquivalenceProfileBuilder = first.commonEquivalenceProfileBuilder,
        .streamTraits = first.streamTraits,
        .bootloaderCue = first.bootloaderCue,
        .vendorName = first.vendorName != nullptr ? first.vendorName : "",
        .modelName = first.modelName != nullptr ? first.modelName : "",
    };
    for (const auto& match : matches) {
        plan.candidates.push_back(match.definition->id);
        plan.candidatePlans.push_back(CandidateEndpointPlan{
            .definitionId = match.definition->id,
            .variantId = match.definition->variantId,
            .profileBuilder = match.definition->profileBuilder,
            .probeConstraint = match.definition->probeConstraint,
            .vendorName = match.definition->vendorName != nullptr
                              ? match.definition->vendorName
                              : "",
            .modelName = match.definition->modelName != nullptr
                             ? match.definition->modelName
                             : "",
        });
        plan.provenance.push_back(MatchProvenance{match.definition->id,
                                                  match.clauseIndex});
    }
    return plan;
}

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::ResolveWithDefinitions(
    const Discovery::DeviceRecord& device,
    const Discovery::UnitIdentityEvidence& unit,
    std::span<const AudioDeviceDefinition> definitions,
    std::span<const AudioSafetyRule> safetyRules,
    bool allowGenericAvcFallback) noexcept {
    if (!device.instanceId || FindUnit(device, unit.unitDirectoryOffset) == nullptr) {
        return std::unexpected(CatalogResolutionError::InvalidUnit);
    }
    return ResolveWithDefinitions(device.identity, unit, definitions, safetyRules,
                                  allowGenericAvcFallback,
                                  device.instanceId);
}

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::Resolve(const Discovery::DeviceIdentityEvidence& device,
                            const Discovery::UnitIdentityEvidence& unit,
                            Discovery::DeviceInstanceId instanceId) noexcept {
    return ResolveWithDefinitions(device, unit, Definitions(), SafetyRules(), true, instanceId);
}

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::Resolve(const Discovery::DeviceRecord& device,
                            const Discovery::UnitIdentityEvidence& unit) noexcept {
    return ResolveWithDefinitions(device, unit, Definitions(), SafetyRules(), true);
}

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::Resolve(const Discovery::DeviceIdentityEvidence& device) noexcept {
    if (MatchAnySafetyRule(device).has_value()) {
        return std::unexpected(CatalogResolutionError::HazardousIdentity);
    }

    if (device.units.empty()) {
        const Discovery::UnitIdentityEvidence emptyUnit{};
        return Resolve(device, emptyUnit, Discovery::DeviceInstanceId{});
    }

    std::vector<StaticAudioEndpointPlan> curatedPlans;
    std::optional<StaticAudioEndpointPlan> genericPlan;

    for (const auto& unit : device.units) {
        auto result = Resolve(device, unit, Discovery::DeviceInstanceId{});
        if (!result.has_value()) {
            if (result.error() == CatalogResolutionError::NoMatch) {
                continue;
            }
            return std::unexpected(result.error());
        }
        if (result->support == SupportDisposition::GenericFallback) {
            if (!genericPlan.has_value()) {
                genericPlan = std::move(*result);
            }
        } else {
            curatedPlans.push_back(std::move(*result));
        }
    }

    if (!curatedPlans.empty()) {
        const auto& first = curatedPlans.front();
        if (curatedPlans.size() > 1U) {
            for (size_t i = 1; i < curatedPlans.size(); ++i) {
                const auto& plan = curatedPlans[i];
                const bool sameEquivalence = (plan.equivalenceClassId != 0 &&
                                              plan.equivalenceClassId == first.equivalenceClassId);
                const bool sameCandidates = (plan.candidates == first.candidates);
                if (!sameEquivalence && !sameCandidates) {
                    return std::unexpected(CatalogResolutionError::AmbiguousIdentity);
                }
            }
        }
        return first;
    }

    if (genericPlan.has_value()) {
        return *genericPlan;
    }

    return std::unexpected(CatalogResolutionError::NoMatch);
}

std::expected<StaticAudioEndpointPlan, CatalogResolutionError>
AudioDeviceCatalog::Resolve(const Discovery::DeviceRecord& device) noexcept {
    auto plan = Resolve(device.identity);
    if (plan.has_value()) {
        plan->unit.device = device.instanceId;
    }
    return plan;
}

Discovery::AvcCommandFilterId AudioDeviceCatalog::CommandFilterFor(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    if (MatchAnySafetyRule(device).has_value()) {
        return Discovery::AvcCommandFilterId::BlockAll;
    }

    const auto plan = Resolve(device);
    if (!plan.has_value()) {
        switch (plan.error()) {
            case CatalogResolutionError::HazardousIdentity:
            case CatalogResolutionError::InvalidUnit:
            case CatalogResolutionError::AmbiguousIdentity:
                return Discovery::AvcCommandFilterId::BlockAll;
            case CatalogResolutionError::NoMatch:
                return Discovery::AvcCommandFilterId::Unrestricted;
        }
    }

    if (plan->probePolicy == ProbePolicyId::BeBoBFilteredCommandSet) {
        return Discovery::AvcCommandFilterId::MAudioSpecialBeBoB;
    }
    if (plan->support == SupportDisposition::Quarantined) {
        return Discovery::AvcCommandFilterId::BlockAll;
    }
    return Discovery::AvcCommandFilterId::Unrestricted;
}

ProfileBuilderId AudioDeviceCatalog::ProfileBuilderFor(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    const auto plan = Resolve(device);
    if (!plan.has_value()) {
        return ProfileBuilderId::None;
    }
    return plan->profileBuilder;
}

DeviceStreamTraits AudioDeviceCatalog::StreamTraitsFor(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    const auto plan = Resolve(device);
    if (!plan.has_value()) {
        return DeviceStreamTraits{};
    }
    return plan->streamTraits;
}

std::optional<const AudioSafetyRule*>
AudioDeviceCatalog::MatchSafetyRule(
    const Discovery::DeviceIdentityEvidence& device,
    const Discovery::UnitIdentityEvidence& unit) noexcept {
    for (const auto& rule : SafetyRules()) {
        for (uint8_t i = 0; i < rule.clauseCount; ++i) {
            if (rule.clauses[i].Matches(device, unit)) {
                return &rule;
            }
        }
    }
    return std::nullopt;
}

std::optional<const AudioSafetyRule*>
AudioDeviceCatalog::MatchAnySafetyRule(
    const Discovery::DeviceIdentityEvidence& device) noexcept {
    for (const auto& unit : device.units) {
        if (const auto rule = MatchSafetyRule(device, unit); rule.has_value()) {
            return rule;
        }
    }
    if (device.units.empty()) {
        const Discovery::UnitIdentityEvidence emptyUnit{};
        return MatchSafetyRule(device, emptyUnit);
    }
    return std::nullopt;
}

} // namespace ASFW::DeviceProfiles::Audio
