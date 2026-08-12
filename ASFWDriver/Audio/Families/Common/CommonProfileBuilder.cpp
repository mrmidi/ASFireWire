// SPDX-License-Identifier: Apache-2.0

#include "CommonProfileBuilder.hpp"

#include <algorithm>
#include <cstdio>

namespace ASFW::Audio::Families::Common {

std::expected<Devices::ResolvedAudioEndpointProfile, Devices::ProfileBuildError>
BuildBase(const Devices::ProfileBuildContext& context,
          const AudioStreamRuntimeCaps& caps,
          std::span<const uint32_t> supportedRates) noexcept {
    if (!context.endpointId || !context.staticPlan.unit ||
        context.staticPlan.unit.device != context.device.instanceId) {
        return std::unexpected(Devices::ProfileBuildError::InvalidGeometry);
    }

    Devices::ResolvedAudioEndpointProfile profile{};
    profile.endpointId = context.endpointId;
    profile.deviceInstanceId = context.device.instanceId;
    profile.unitInstanceId = context.staticPlan.unit;
    profile.observedGuid = context.device.ObservedGuid();
    profile.definitionId = context.staticPlan.candidates.size() == 1
                               ? context.staticPlan.candidates.front()
                               : DeviceProfiles::Audio::DeviceDefinitionId::Unknown;
    profile.exactVariantId = context.staticPlan.exactVariantId;
    if (context.staticPlan.equivalenceClassId != 0) {
        profile.equivalenceClassId = context.staticPlan.equivalenceClassId;
    }
    profile.familyProvider = context.staticPlan.family;
    profile.profileBuilder = context.staticPlan.profileBuilder;
    profile.candidateDefinitionIds = context.staticPlan.candidates;
    profile.matchProvenance = context.staticPlan.provenance;
    profile.vendorName = context.staticPlan.vendorName;
    profile.deviceName = context.staticPlan.modelName.empty()
                             ? "FireWire Audio"
                             : context.staticPlan.modelName;
    profile.runtimeCaps = caps;
    profile.currentSampleRateHz = caps.sampleRateHz != 0 ? caps.sampleRateHz : 48000U;
    for (const uint32_t rate : supportedRates) {
        if (rate == 0 ||
            std::find(profile.supportedRates.begin(),
                      profile.supportedRates.begin() + profile.supportedRateCount,
                      rate) != profile.supportedRates.begin() + profile.supportedRateCount) {
            continue;
        }
        if (profile.supportedRateCount == profile.supportedRates.size()) break;
        profile.supportedRates[profile.supportedRateCount++] = rate;
    }
    if (profile.supportedRateCount == 0) {
        profile.supportedRates[0] = profile.currentSampleRateHz;
        profile.supportedRateCount = 1;
    }

    for (const auto& facet : context.adapterFacets) {
        if (std::find_if(profile.facets.begin(), profile.facets.end(),
                         [&facet](const Devices::FacetDescriptor& existing) {
                             return existing.kind == facet.kind &&
                                    existing.schemaId == facet.schemaId;
                         }) == profile.facets.end()) {
            profile.facets.push_back(facet);
        }
    }

    char identity[96]{};
    if (context.staticPlan.persistentKeyRecipe ==
            DeviceProfiles::Audio::PersistentKeyRecipeId::ReliableObservedEui64 &&
        context.staticPlan.guidReliability ==
            DeviceProfiles::Audio::GuidReliability::ReliableWhenUnique &&
        context.device.ObservedGuid() != 0 &&
        context.device.quarantineReason == Discovery::QuarantineReason::None) {
        std::snprintf(identity, sizeof(identity), "eui64-%016llx",
                      context.device.ObservedGuid());
        profile.persistentKey = Devices::PersistentDeviceKey{
            .value = identity,
            .trustworthy = true,
        };
        profile.identityStatus = Devices::PersistentIdentityStatus::Persistent;
    } else {
        std::snprintf(identity, sizeof(identity), "instance-%llu",
                      context.device.instanceId.value);
        profile.identityStatus = Devices::PersistentIdentityStatus::Ephemeral;
    }

    char uid[160]{};
    std::snprintf(uid, sizeof(uid), "ASFW:%s:unit-%u:endpoint-0", identity,
                  context.staticPlan.unit.unitDirectoryOffset);
    profile.coreAudioUid = uid;
    return profile;
}

void AddDefaultTiming(Devices::ResolvedAudioEndpointProfile& profile,
                      uint32_t anchorTimeoutMs) noexcept {
    const uint8_t count = profile.supportedRateCount != 0
                              ? profile.supportedRateCount
                              : uint8_t{1};
    profile.timingCount = count;
    for (uint8_t i = 0; i < count; ++i) {
        const uint32_t rate = profile.supportedRates[i] != 0
                                  ? profile.supportedRates[i]
                                  : profile.currentSampleRateHz;
        profile.timing[i] = Devices::RateTimingPolicy{
            .sampleRateHz = rate,
            .inputLatencyFrames = 32,
            .outputLatencyFrames = 32,
            .inputSafetyFrames = 16,
            .outputSafetyFrames = 16,
            .rxTransferDelayTicks = 12800,
            .txTransferDelayTicks = 12800,
            .anchorTimeoutMs = anchorTimeoutMs,
        };
    }
}

} // namespace ASFW::Audio::Families::Common
