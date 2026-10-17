// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include "Id.hpp"
#include "Parameter.hpp"
#include "Router.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ASFW::AudioModel {

struct RouterState {
    std::vector<RouteBundleId> activeBundles;
};

/// Where a runtime value came from (AUAA §18). A bus reset or reconnect
/// invalidates Cached values first: the hardware never confirmed them.
enum class StateOrigin : uint8_t {
    Observed, ///< Read back from the hardware.
    Cached,   ///< Written by the host; the hardware cannot be read back.
    Derived,  ///< Computed from other observations or events.
};

struct ParameterState {
    ParameterValue value;
    StateOrigin origin{StateOrigin::Observed};
};

struct DeviceState {
    uint64_t topologyRevision{};

    std::unordered_map<NodeId, RouterState> routers;
    std::unordered_map<ParameterId, ParameterState> parameters;
    std::unordered_map<MeterId, double> meters;
};

} // namespace ASFW::AudioModel
