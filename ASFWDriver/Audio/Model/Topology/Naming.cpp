// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#include "Naming.hpp"

#include "Format.hpp"
#include <unordered_map>

namespace ASFW::AudioModel {
namespace {

constexpr const char* kindText(SignalKind kind) {
    switch (kind) {
        case SignalKind::AnalogLine:       return "Line";
        case SignalKind::AnalogMicXlr:     return "XLR";
        case SignalKind::AnalogInstrument: return "Inst";
        case SignalKind::Headphone:        return "Headphone";
        case SignalKind::SpdifCoaxial:     return "S/PDIF";
        case SignalKind::SpdifOptical:     return "Opt S/PDIF";
        case SignalKind::Adat:             return "ADAT";
        case SignalKind::HostStream:       return "";
        case SignalKind::Unknown:          return "";
    }
    return "";
}

// An endpoint port whose direction is Output sources signal into the graph.
constexpr bool sourcesIntoGraph(PortDirection direction) {
    return direction == PortDirection::Output;
}

std::string channelSpan(uint32_t index, uint32_t channels) {
    if (channels <= 1) {
        return detail::Format("{}", index);
    }
    if (channels == 2) {
        return detail::Format("{}/{}", index, index + 1);
    }
    return detail::Format("{}-{}", index, index + channels - 1);
}

struct Lookup {
    std::unordered_map<PortId, const Port*> ports;
    std::unordered_map<NodeId, const Node*> nodes;
    std::unordered_map<PortId, PortId> driverOf;
    std::unordered_map<PortId, const Bus*> busOf;

    explicit Lookup(const Topology& topology) {
        for (const auto& port : topology.ports) ports.emplace(port.id, &port);
        for (const auto& node : topology.nodes) nodes.emplace(node.id, &node);
        for (const auto& link : topology.fixedLinks) driverOf.insert_or_assign(link.destination, link.source);
        for (const auto& bus : topology.buses) {
            for (auto portId : bus.ports) busOf.insert_or_assign(portId, &bus);
        }
    }

    const Node* ownerOf(PortId portId) const {
        auto portIt = ports.find(portId);
        if (portIt == ports.end()) return nullptr;
        auto nodeIt = nodes.find(portIt->second->owner);
        return nodeIt == nodes.end() ? nullptr : nodeIt->second;
    }
};

// Bounded so a malformed fixture cannot spin here.
constexpr int kMaxHops = 32;

std::optional<PortId> walk(const Lookup& lookup, PortId start) {
    PortId current = start;

    for (int hop = 0; hop < kMaxHops; ++hop) {
        const Node* owner = lookup.ownerOf(current);
        if (owner == nullptr) return std::nullopt;
        if (std::holds_alternative<EndpointNode>(owner->body)) return current;

        auto driver = lookup.driverOf.find(current);
        if (driver == lookup.driverOf.end()) return std::nullopt;
        const PortId source = driver->second;

        const Node* sourceOwner = lookup.ownerOf(source);
        if (sourceOwner == nullptr) return std::nullopt;
        if (std::holds_alternative<EndpointNode>(sourceOwner->body)) return source;

        // A processor is transparent for identity purposes: keep walking from
        // its first input. A router or mixer is not -- the signal there is a
        // selection or a sum, so it is named by its bus instead.
        if (const auto* processor = std::get_if<ProcessorNode>(&sourceOwner->body)) {
            if (processor->inputs.empty()) return std::nullopt;
            current = processor->inputs.front();
            continue;
        }
        return source;
    }

    return std::nullopt;
}

} // namespace

std::string canonicalName(SignalIdentity signal, PortDirection direction, uint32_t channels) {
    const bool intoGraph = sourcesIntoGraph(direction);

    if (signal.kind == SignalKind::HostStream) {
        return detail::Format("{} {}", intoGraph ? "Playback" : "Capture",
                           channelSpan(signal.index, channels));
    }
    if (signal.kind == SignalKind::Unknown) {
        return detail::Format("Signal {}", channelSpan(signal.index, channels));
    }

    return detail::Format("{} {} {}", kindText(signal.kind), intoGraph ? "In" : "Out",
                       channelSpan(signal.index, channels));
}

std::string displayName(const Port& port) {
    if (port.signal.kind != SignalKind::Unknown) {
        return canonicalName(port.signal, port.direction, port.channels);
    }
    return port.name;
}

std::optional<PortId> traceToEndpoint(const Topology& topology, PortId port) {
    const Lookup lookup(topology);
    auto reached = walk(lookup, port);
    if (!reached.has_value()) return std::nullopt;

    const Node* owner = lookup.ownerOf(*reached);
    if (owner == nullptr || !std::holds_alternative<EndpointNode>(owner->body)) {
        return std::nullopt;
    }
    return reached;
}

std::optional<std::string> sourceLabel(const Topology& topology, PortId port) {
    const Lookup lookup(topology);
    auto reached = walk(lookup, port);
    if (!reached.has_value()) return std::nullopt;

    if (const Node* owner = lookup.ownerOf(*reached);
        owner != nullptr && std::holds_alternative<EndpointNode>(owner->body)) {
        auto portIt = lookup.ports.find(*reached);
        if (portIt == lookup.ports.end()) return std::nullopt;
        return displayName(*portIt->second);
    }

    // Not a connector: the signal is a router selection or a mixer sum, so it is
    // named by the bus it belongs to.
    if (auto busIt = lookup.busOf.find(*reached); busIt != lookup.busOf.end()) {
        return busIt->second->name;
    }
    return std::nullopt;
}

} // namespace ASFW::AudioModel
