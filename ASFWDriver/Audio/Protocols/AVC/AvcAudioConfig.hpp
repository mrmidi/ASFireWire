// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcAudioConfig.hpp - The audio endpoint an AV/C unit publishes.
//
// Two sources, one shape. A discovered unit publishes what its capability
// graph says. A unit whose policy forbids discovery traffic (M-Audio special
// firmware, Fireworks) publishes the fixed geometry of its catalog profile.
// The catalog's start-rate traits apply to both. Pure: no bus, no registry.

#pragma once

#include "../../../Protocols/AVC/Graph/AvcDeviceGraph.hpp"
#include "../../DriverKit/Config/IAudioDeviceProfile.hpp"
#include "../../Model/ASFWAudioDevice.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC {

struct AvcEndpointIdentity {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    std::string modelName;
};

/// The endpoint the graph describes, or nullopt when its geometry is unusable
/// (unresolved streams, mismatched current rates, too wide for the wire, no
/// rate the runtime can run). `runtimeRates`, when non-empty, are the rates the
/// device's runtime profile supports: the published rates are the graph's
/// rates among them.
[[nodiscard]] std::optional<::ASFW::Audio::Model::ASFWAudioDevice> BuildGraphAudioConfig(
    const AvcEndpointIdentity& identity,
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
    const Graph::DeviceGraph& graph,
    const std::vector<uint32_t>& runtimeRates = {});

/// The endpoint a catalog profile fixes, for a unit that is never probed.
[[nodiscard]] std::optional<::ASFW::Audio::Model::ASFWAudioDevice> BuildProfileOwnedAudioConfig(
    const AvcEndpointIdentity& identity,
    const DeviceProfiles::Audio::StaticAudioEndpointPlan& plan,
    const ::ASFW::Isoch::Audio::IAudioDeviceProfile& profile);

} // namespace ASFW::Protocols::AVC
