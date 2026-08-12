// SPDX-License-Identifier: Apache-2.0

#include "ResolvedProfileBuilder.hpp"
#include "../Families/AVC/GenericAvcProfileBuilder.hpp"
#include "../Families/BeBoB/BeBoBProfileBuilder.hpp"
#include "../Families/DICE/DiceProfileBuilder.hpp"
#include "../Families/OXFW/OxfwProfileBuilder.hpp"

#include <algorithm>
#include <type_traits>

namespace ASFW::Audio::Devices {

namespace {

[[nodiscard]] const AudioStreamRuntimeCaps*
Streams(const FamilyProbeFacts& facts) noexcept {
    return std::visit([](const auto& value) -> const AudioStreamRuntimeCaps* {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return nullptr;
        } else {
            return &value.streams;
        }
    }, facts);
}

[[nodiscard]] bool Matches(
    const DeviceProfiles::Audio::SafeProbeConstraint& constraint,
    const AudioStreamRuntimeCaps& streams) noexcept {
    const auto same = [](const std::optional<uint32_t>& expected,
                         uint32_t observed) noexcept {
        return !expected || *expected == observed;
    };
    return same(constraint.hostInputPcmChannels, streams.hostInputPcmChannels) &&
           same(constraint.hostOutputPcmChannels, streams.hostOutputPcmChannels) &&
           same(constraint.deviceToHostSlots, streams.deviceToHostAm824Slots) &&
           same(constraint.hostToDeviceSlots, streams.hostToDeviceAm824Slots) &&
           same(constraint.deviceToHostStreamCount,
                streams.deviceToHostStreamCount) &&
           same(constraint.hostToDeviceStreamCount,
                streams.hostToDeviceStreamCount);
}

} // namespace

std::expected<DeviceProfiles::Audio::StaticAudioEndpointPlan, ProfileBuildError>
ResolvedProfileBuilder::ApplyProbeEvidence(
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& staticPlan,
    const FamilyProbeFacts& probeFacts) noexcept {
    if (staticPlan.candidatePlans.empty()) return staticPlan;
    const auto* streams = Streams(probeFacts);
    if (!streams) {
        return std::unexpected(ProfileBuildError::InsufficientSafeEvidence);
    }

    auto resolved = staticPlan;
    resolved.candidatePlans.clear();
    resolved.candidates.clear();
    for (const auto& candidate : staticPlan.candidatePlans) {
        if (Matches(candidate.probeConstraint, *streams)) {
            resolved.candidatePlans.push_back(candidate);
            resolved.candidates.push_back(candidate.definitionId);
        }
    }
    if (resolved.candidatePlans.empty()) {
        return std::unexpected(ProfileBuildError::InsufficientSafeEvidence);
    }

    std::erase_if(resolved.provenance, [&resolved](const auto& provenance) {
        return std::ranges::none_of(
            resolved.candidatePlans,
            [&provenance](const auto& candidate) {
                return candidate.definitionId == provenance.definitionId;
            });
    });

    if (resolved.candidatePlans.size() == 1) {
        const auto& exact = resolved.candidatePlans.front();
        resolved.exactVariantId = exact.variantId != 0
                                      ? std::optional<uint32_t>{exact.variantId}
                                      : std::nullopt;
        resolved.profileBuilder = exact.profileBuilder;
        resolved.vendorName = exact.vendorName;
        resolved.modelName = exact.modelName;
        return resolved;
    }

    if (resolved.commonEquivalenceProfileBuilder ==
        DeviceProfiles::Audio::ProfileBuilderId::None) {
        return std::unexpected(ProfileBuildError::InsufficientSafeEvidence);
    }
    resolved.exactVariantId.reset();
    resolved.profileBuilder = resolved.commonEquivalenceProfileBuilder;
    const bool commonName = std::ranges::all_of(
        resolved.candidatePlans, [&resolved](const auto& candidate) {
            return candidate.modelName == resolved.candidatePlans.front().modelName;
        });
    if (!commonName) resolved.modelName = "Compatible FireWire Audio";
    return resolved;
}

std::expected<ResolvedAudioEndpointProfile, ProfileBuildError>
ResolvedProfileBuilder::Build(const ProfileBuildContext& context) noexcept {
    using DeviceProfiles::Audio::AudioFamilyProviderId;
    switch (context.staticPlan.family) {
        case AudioFamilyProviderId::GenericAvc:
            return Families::AVC::BuildProfile(context);
        case AudioFamilyProviderId::BeBoB:
            return Families::BeBoB::BuildProfile(context);
        case AudioFamilyProviderId::DICE:
            return Families::DICE::BuildProfile(context);
        case AudioFamilyProviderId::OXFW:
            return Families::OXFW::BuildProfile(context);
        case AudioFamilyProviderId::None:
        default:
            return std::unexpected(ProfileBuildError::UnsupportedBuilder);
    }
}

} // namespace ASFW::Audio::Devices
