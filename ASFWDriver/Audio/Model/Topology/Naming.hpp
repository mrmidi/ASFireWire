// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#pragma once

#include <cstdint>

#include "Topology.hpp"

#include <optional>
#include <string>

namespace ASFW::AudioModel {

/// Canonical spelling of a connector, identical for every device.
///
/// Direction is read from the graph's point of view: an endpoint port whose
/// direction is `Output` feeds signal *into* the graph and is therefore an
/// input connector (or a playback slot arriving from the host); `Input` drains
/// signal out of the graph and is an output connector (or a capture slot).
std::string canonicalName(SignalIdentity signal, PortDirection direction, uint32_t channels);

/// Display name for any port: rendered from `signal` when it is known,
/// otherwise the authored `name`.
std::string displayName(const Port& port);

/// Where the signal arriving at `port` comes from, as a label.
///
/// Walks back through fixed links, passing through processor nodes, until it
/// reaches either an endpoint port (labelled from its SignalIdentity) or a port
/// that belongs to a Bus (labelled from the bus). Returns nullopt when neither
/// applies, so callers can fall back rather than invent a name.
std::optional<std::string> sourceLabel(const Topology& topology, PortId port);

/// The endpoint port ultimately feeding `port`, if the walk reaches one.
std::optional<PortId> traceToEndpoint(const Topology& topology, PortId port);

} // namespace ASFW::AudioModel
