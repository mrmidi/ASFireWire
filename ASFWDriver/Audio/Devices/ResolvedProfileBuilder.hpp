// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AudioFamilyProvider.hpp"

#include <expected>
#include <span>

namespace ASFW::Audio::Devices {

enum class ProfileBuildError : uint8_t {
    WrongProbeFacts,
    UnsupportedBuilder,
    InvalidGeometry,
    InsufficientSafeEvidence,
};

struct ProfileBuildContext final {
    AudioEndpointId endpointId{};
    const Discovery::DeviceRecord& device;
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& staticPlan;
    const FamilyProbeFacts& probeFacts;
    std::span<const FacetDescriptor> adapterFacets{};
};

class ResolvedProfileBuilder final {
public:
    [[nodiscard]] static std::expected<
        DeviceProfiles::Audio::StaticAudioEndpointPlan, ProfileBuildError>
    ApplyProbeEvidence(
        const DeviceProfiles::Audio::StaticAudioEndpointPlan& staticPlan,
        const FamilyProbeFacts& probeFacts) noexcept;

    [[nodiscard]] static std::expected<ResolvedAudioEndpointProfile, ProfileBuildError>
    Build(const ProfileBuildContext& context) noexcept;
};

} // namespace ASFW::Audio::Devices
