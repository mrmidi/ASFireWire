// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioDeviceCatalog.hpp"

#include <optional>
#include <set>
#include <vector>

namespace ASFW::DeviceProfiles::Audio {

namespace {

[[nodiscard]] constexpr bool Valid(const MaskedValue32& value) noexcept {
    return value.mask != 0 && (value.value & ~value.mask) == 0;
}

[[nodiscard]] constexpr bool Valid(const MaskedValue64& value) noexcept {
    return value.mask != 0 && (value.value & ~value.mask) == 0;
}

[[nodiscard]] bool ClauseHasConstraint(const IdentityMatchClause& clause) noexcept {
    return clause.observedGuid || clause.busInfoWord || clause.nodeVendorOui ||
           clause.rootVendorId || clause.rootModelId || clause.unitVendorId ||
           clause.unitModelId || clause.unitSpecifierId || clause.unitVersion ||
           clause.rootVendorName || clause.rootModelName || clause.unitVendorName ||
           clause.unitModelName;
}

[[nodiscard]] bool ClauseIsValid(const IdentityMatchClause& clause) noexcept {
    if (!ClauseHasConstraint(clause)) return false;
    if (clause.observedGuid && !Valid(*clause.observedGuid)) return false;
    if (clause.busInfoWord &&
        (clause.busInfoWord->index > 63 || !Valid(clause.busInfoWord->value))) return false;
    for (const auto* value : {&clause.nodeVendorOui, &clause.rootVendorId,
                              &clause.rootModelId, &clause.unitVendorId,
                              &clause.unitModelId, &clause.unitSpecifierId,
                              &clause.unitVersion}) {
        if (*value && !Valid(**value)) return false;
    }
    return true;
}

template <typename T>
[[nodiscard]] constexpr bool MaskedConstraintsOverlap(
    const std::optional<T>& first,
    const std::optional<T>& second) noexcept {
    if (!first || !second) return true;
    return ((first->value ^ second->value) & (first->mask & second->mask)) == 0;
}

template <typename T>
[[nodiscard]] constexpr bool ExactConstraintsOverlap(
    const std::optional<T>& first,
    const std::optional<T>& second) noexcept {
    return !first || !second || *first == *second;
}

enum ClauseConstraintBit : uint16_t {
    kObservedGuid = 1U << 0U,
    kBusInfoWord = 1U << 1U,
    kNodeVendorOui = 1U << 2U,
    kRootVendorId = 1U << 3U,
    kRootModelId = 1U << 4U,
    kUnitVendorId = 1U << 5U,
    kUnitModelId = 1U << 6U,
    kUnitSpecifierId = 1U << 7U,
    kUnitVersion = 1U << 8U,
    kRootVendorName = 1U << 9U,
    kRootModelName = 1U << 10U,
    kUnitVendorName = 1U << 11U,
    kUnitModelName = 1U << 12U,
};

[[nodiscard]] constexpr uint16_t ConstraintBits(
    const IdentityMatchClause& clause) noexcept {
    return (clause.observedGuid ? kObservedGuid : 0U) |
           (clause.busInfoWord ? kBusInfoWord : 0U) |
           (clause.nodeVendorOui ? kNodeVendorOui : 0U) |
           (clause.rootVendorId ? kRootVendorId : 0U) |
           (clause.rootModelId ? kRootModelId : 0U) |
           (clause.unitVendorId ? kUnitVendorId : 0U) |
           (clause.unitModelId ? kUnitModelId : 0U) |
           (clause.unitSpecifierId ? kUnitSpecifierId : 0U) |
           (clause.unitVersion ? kUnitVersion : 0U) |
           (clause.rootVendorName ? kRootVendorName : 0U) |
           (clause.rootModelName ? kRootModelName : 0U) |
           (clause.unitVendorName ? kUnitVendorName : 0U) |
           (clause.unitModelName ? kUnitModelName : 0U);
}

[[nodiscard]] bool ClausesProvablyOverlap(const IdentityMatchClause& first,
                                          const IdentityMatchClause& second) noexcept {
    const uint16_t firstBits = ConstraintBits(first);
    const uint16_t secondBits = ConstraintBits(second);
    // If neither clause is a refinement of the other, independent Config-ROM
    // identity surfaces could still conflict at runtime, but the catalog cannot
    // prove that intersection statically. Runtime resolution remains fail-closed.
    if ((firstBits & secondBits) != firstBits &&
        (firstBits & secondBits) != secondBits) {
        return false;
    }

    const bool busOverlap = !first.busInfoWord || !second.busInfoWord ||
        first.busInfoWord->index != second.busInfoWord->index ||
        MaskedConstraintsOverlap(
            std::optional{first.busInfoWord->value},
            std::optional{second.busInfoWord->value});
    return MaskedConstraintsOverlap(first.observedGuid, second.observedGuid) &&
           busOverlap &&
           MaskedConstraintsOverlap(first.nodeVendorOui, second.nodeVendorOui) &&
           MaskedConstraintsOverlap(first.rootVendorId, second.rootVendorId) &&
           MaskedConstraintsOverlap(first.rootModelId, second.rootModelId) &&
           MaskedConstraintsOverlap(first.unitVendorId, second.unitVendorId) &&
           MaskedConstraintsOverlap(first.unitModelId, second.unitModelId) &&
           MaskedConstraintsOverlap(first.unitSpecifierId, second.unitSpecifierId) &&
           MaskedConstraintsOverlap(first.unitVersion, second.unitVersion) &&
           ExactConstraintsOverlap(first.rootVendorName, second.rootVendorName) &&
           ExactConstraintsOverlap(first.rootModelName, second.rootModelName) &&
           ExactConstraintsOverlap(first.unitVendorName, second.unitVendorName) &&
           ExactConstraintsOverlap(first.unitModelName, second.unitModelName);
}

[[nodiscard]] constexpr bool KnownFamily(AudioFamilyProviderId id) noexcept {
    return id >= AudioFamilyProviderId::GenericAvc &&
           id <= AudioFamilyProviderId::kLastValid;
}

[[nodiscard]] constexpr bool KnownProbe(ProbePolicyId id) noexcept {
    return id >= ProbePolicyId::GenericAvc && id <= ProbePolicyId::kLastValid;
}

[[nodiscard]] constexpr bool KnownBuilder(ProfileBuilderId id) noexcept {
    return id >= ProfileBuilderId::GenericAvc && id <= ProfileBuilderId::kLastValid;
}

[[nodiscard]] constexpr bool KnownProtocol(ProtocolImplementationId id) noexcept {
    return id > ProtocolImplementationId::None &&
           id <= ProtocolImplementationId::kLastValid;
}

[[nodiscard]] constexpr bool ProtocolMatchesFamily(
    ProtocolImplementationId implementation, AudioFamilyProviderId family) noexcept {
    switch (implementation) {
        case ProtocolImplementationId::DiceTcat:
        case ProtocolImplementationId::DiceSPro24Dsp:
        case ProtocolImplementationId::DiceWeissInt:
            return family == AudioFamilyProviderId::DICE;
        case ProtocolImplementationId::ApogeeDuet:
        case ProtocolImplementationId::MackieOnyx:
            return family == AudioFamilyProviderId::OXFW;
        case ProtocolImplementationId::FireworksOnyx400F:
            return family == AudioFamilyProviderId::Fireworks;
        case ProtocolImplementationId::BeBoBPhase88:
        case ProtocolImplementationId::BeBoBGeneric:
            return family == AudioFamilyProviderId::BeBoB;
        case ProtocolImplementationId::MotuV2:
            return family == AudioFamilyProviderId::MotuRegister;
        case ProtocolImplementationId::None:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool ProbeMatchesFamily(
    ProbePolicyId probe, AudioFamilyProviderId family) noexcept {
    switch (family) {
        case AudioFamilyProviderId::DICE:
            return probe == ProbePolicyId::DiceTcat;
        case AudioFamilyProviderId::OXFW:
            return probe == ProbePolicyId::OxfwAvc;
        case AudioFamilyProviderId::Fireworks:
            return probe == ProbePolicyId::FireworksEfc;
        case AudioFamilyProviderId::BeBoB:
            return probe == ProbePolicyId::BeBoBPlug0;
        case AudioFamilyProviderId::MotuRegister:
            return probe == ProbePolicyId::MotuRegister;
        case AudioFamilyProviderId::GenericAvc:
            return probe == ProbePolicyId::GenericAvc;
        case AudioFamilyProviderId::None:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool CompatibleOverlap(
    const AudioDeviceDefinition& first,
    const AudioDeviceDefinition& second) noexcept {
    return first.equivalenceClassId != 0 &&
           first.equivalenceClassId == second.equivalenceClassId &&
           first.family == second.family &&
           first.probePolicy == second.probePolicy &&
           first.protocolImplementation == second.protocolImplementation &&
           first.commonEquivalenceProfileBuilder != ProfileBuilderId::None &&
           first.commonEquivalenceProfileBuilder == second.commonEquivalenceProfileBuilder;
}

} // namespace

std::vector<CatalogValidationIssue> AudioDeviceCatalog::Validate() noexcept {
    return ValidateDefinitions(Definitions());
}

std::vector<CatalogValidationIssue> AudioDeviceCatalog::ValidateDefinitions(
    std::span<const AudioDeviceDefinition> definitions) noexcept {
    std::vector<CatalogValidationIssue> issues;
    std::set<DeviceDefinitionId> seenIds;
    for (size_t definitionIndex = 0; definitionIndex < definitions.size(); ++definitionIndex) {
        const auto& definition = definitions[definitionIndex];
        if (definition.id == DeviceDefinitionId::Unknown ||
            !seenIds.insert(definition.id).second) {
            issues.push_back({definition.id, definition.id, "duplicate definition id"});
        }
        if (definition.clauseCount == 0 ||
            definition.clauseCount > definition.clauses.size()) {
            issues.push_back({definition.id, definition.id, "invalid clause count"});
            continue;
        }
        for (uint8_t clause = 0; clause < definition.clauseCount; ++clause) {
            if (!ClauseIsValid(definition.clauses[clause])) {
                issues.push_back({definition.id, definition.id,
                                  "empty clause or invalid mask"});
            }
        }
        if (definition.support == SupportDisposition::Supported &&
            (!KnownFamily(definition.family) ||
             !KnownBuilder(definition.profileBuilder) ||
             !KnownProbe(definition.probePolicy) ||
             !KnownProtocol(definition.protocolImplementation))) {
            issues.push_back({definition.id, definition.id,
                              "supported definition lacks provider/probe/profile/protocol"});
        }
        if (definition.support == SupportDisposition::Supported &&
            (!ProtocolMatchesFamily(definition.protocolImplementation, definition.family) ||
             !ProbeMatchesFamily(definition.probePolicy, definition.family))) {
            issues.push_back({definition.id, definition.id,
                              "supported definition has incompatible family/probe/protocol"});
        }
        if (definition.support != SupportDisposition::Supported &&
            definition.protocolImplementation != ProtocolImplementationId::None) {
            issues.push_back({definition.id, definition.id,
                              "unsupported definition names a protocol implementation"});
        }
        if (definition.equivalenceClassId != 0 &&
            !KnownBuilder(definition.commonEquivalenceProfileBuilder)) {
            issues.push_back({definition.id, definition.id,
                              "equivalence definition lacks common profile"});
        }

        for (size_t priorIndex = 0; priorIndex < definitionIndex; ++priorIndex) {
            const auto& prior = definitions[priorIndex];
            bool overlappingClause = false;
            for (uint8_t clause = 0; clause < definition.clauseCount && !overlappingClause;
                 ++clause) {
                for (uint8_t priorClause = 0; priorClause < prior.clauseCount;
                     ++priorClause) {
                    if (ClausesProvablyOverlap(definition.clauses[clause],
                                               prior.clauses[priorClause])) {
                        overlappingClause = true;
                        break;
                    }
                }
            }
            if (overlappingClause && !CompatibleOverlap(definition, prior)) {
                issues.push_back({prior.id, definition.id,
                                  "provably overlapping incompatible match clauses"});
            }
        }
    }
    return issues;
}

} // namespace ASFW::DeviceProfiles::Audio
