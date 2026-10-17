// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include "Id.hpp"
#include "Signal.hpp"

#include <cstdint>
#include <string>

namespace ASFW::AudioModel {

enum class PortDirection : uint8_t {
    Input,
    Output,
};

struct Port {
    PortId id;
    NodeId owner;

    PortDirection direction;

    // A Port is a connectable signal boundary; a RouteBundle is the smallest
    // set of routes that must change atomically (AUAA 10.1).
    uint32_t channels{1};

    std::string name;

    /// Set on endpoint ports (those owned by an EndpointNode), where it is the
    /// authoritative identity and the display name is rendered from it.
    /// Interior ports leave this Unknown and keep an authored `name`.
    SignalIdentity signal{};
};

/// Build an endpoint port. Its name is left empty deliberately: endpoint names
/// are rendered from `signal` so that every device spells the same connector
/// the same way. See Naming.hpp.
inline Port endpointPort(PortId id,
                         NodeId owner,
                         PortDirection direction,
                         uint32_t channels,
                         SignalIdentity signal) {
    return Port{
        .id = id,
        .owner = owner,
        .direction = direction,
        .channels = channels,
        .name = {},
        .signal = signal,
    };
}

} // namespace ASFW::AudioModel
