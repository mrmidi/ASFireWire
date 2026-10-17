// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Id.hpp"
#include "Mixer.hpp"
#include "Processor.hpp"
#include "Router.hpp"

#include <compare>
#include <string>
#include <variant>
#include <vector>

namespace ASFW::AudioModel {

enum class EndpointKind : uint8_t {
    Physical,
    Host,
};

struct EndpointNode {
    EndpointKind kind{EndpointKind::Physical};

    constexpr auto operator<=>(const EndpointNode&) const = default;
};

struct RouterNode {
    std::vector<PortId> inputs;
    std::vector<PortId> outputs;
    std::vector<RouteBundle> legalBundles;
    RouterConstraints constraints;
};

struct MixerNode {
    std::vector<PortId> inputs;
    std::vector<PortId> outputs;
    std::vector<MixerCrosspoint> crosspoints;
};

using NodeBody = std::variant<
    EndpointNode,
    RouterNode,
    MixerNode,
    ProcessorNode
>;

struct Node {
    NodeId id;
    std::string name;
    NodeBody body{EndpointNode{}};
};

} // namespace ASFW::AudioModel
