// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcControlProjection.cpp - see AvcControlProjection.hpp.

#include "AvcControlProjection.hpp"

#include <string>
#include <map>
#include <tuple>
#include <algorithm>

namespace ASFW::Audio::Controls {
namespace {

namespace AM = ASFW::AudioModel;
namespace G = ASFW::Protocols::AVC::Graph;
using BlockType = ASFW::Protocols::AVC::Descriptors::AudioFunctionBlockType;

/// Node ids carry the AV/C address so they are stable across projections of
/// the same graph: [subunit:8][block type:8][block id:8], plus one so that no
/// id is zero.
constexpr AM::NodeId BlockNodeId(uint8_t subunit, uint8_t type, uint8_t block) noexcept {
    return AM::NodeId{((uint32_t{subunit} << 16) | (uint32_t{type} << 8) | block) + 1};
}

std::string BlockName(const G::ControlBlockInfo& block, const char* kind) {
    if (!block.name.empty()) return block.name;
    return std::string(kind) + " " + std::to_string(block.id);
}

class Builder {
public:
    explicit Builder(uint64_t revision) {
        model_.topology.revision = revision;
        model_.state.topologyRevision = revision;
    }

    AM::PortId AddPort(AM::NodeId owner, AM::PortDirection direction, std::string name) {
        const AM::PortId id{nextPort_++};
        model_.topology.ports.push_back(AM::Port{.id = id, .owner = owner, .direction = direction,
                                                 .channels = 1, .name = std::move(name)});
        return id;
    }

    void AddFeature(const G::DeviceGraph& graph, const G::ControlBlockInfo& block) {
        const auto nodeId = BlockNodeId(block.audioSubunitId, static_cast<uint8_t>(block.type), block.id);
        AM::ProcessorNode body;
        body.channelPreserving = true;
        // Channel k (1-based on the wire) has input/output port index k-1.
        const uint8_t width = block.channelCount == 0 ? 1 : block.channelCount;
        std::vector<AM::PortId> outputs;
        for (uint8_t ch = 1; ch <= width; ++ch) {
            const auto label = "Ch " + std::to_string(ch);
            body.inputs.push_back(AddPort(nodeId, AM::PortDirection::Input, label + " In"));
            outputs.push_back(AddPort(nodeId, AM::PortDirection::Output, label + " Out"));
        }
        body.outputs = outputs;
        blockPorts_[{block.audioSubunitId, static_cast<uint8_t>(block.type), block.id}] = {body.inputs, body.outputs};
        model_.topology.nodes.push_back(AM::Node{.id = nodeId, .name = BlockName(block, "Feature"),
                                                 .body = std::move(body)});

        for (const auto& channel : graph.featureChannels) {
            if (channel.subunit != block.audioSubunitId || channel.block != block.id) continue;
            if (channel.channel > width) continue; // descriptor and STATUS disagree: publish nothing
            const AM::ParameterTarget target = channel.channel == 0
                ? AM::ParameterTarget{nodeId}
                : AM::ParameterTarget{outputs[channel.channel - 1]};
            const std::string where = channel.channel == 0 ? "Master" : "Ch " + std::to_string(channel.channel);
            AddVolume(channel, target, where);
            AddMute(channel, target, where);
        }
    }

    void AddSelector(const G::ControlBlockInfo& block, const G::AudioSelectorInfo* selector) {
        const auto nodeId = BlockNodeId(block.audioSubunitId, static_cast<uint8_t>(block.type), block.id);
        const auto& inputs = selector ? selector->declaredInputs : block.inputSources;
        if (inputs.empty()) return;
        AM::RouterNode body;
        const uint32_t width = std::max<uint32_t>(1, block.channelCount);
        for (uint32_t ch = 0; ch < width; ++ch)
            body.outputs.push_back(AddPort(nodeId, AM::PortDirection::Output, "Out " + std::to_string(ch + 1)));
        std::vector<std::vector<AM::PortId>> inputGroups;
        for (size_t i = 0; i < inputs.size(); ++i) {
            AM::RouteBundle bundle{.id = AM::RouteBundleId{static_cast<uint32_t>(i + 1)}};
            std::vector<AM::PortId> group;
            for (uint32_t ch = 0; ch < width; ++ch) {
                const auto in = AddPort(nodeId, AM::PortDirection::Input,
                    "In " + std::to_string(i + 1) + " Ch " + std::to_string(ch + 1));
                body.inputs.push_back(in);
                group.push_back(in);
                bundle.routes.push_back({in, body.outputs[ch]});
            }
            inputGroups.push_back(std::move(group));
            body.legalBundles.push_back(std::move(bundle));
        }
        selectorInputs_[{block.audioSubunitId, static_cast<uint8_t>(block.type), block.id}] = std::move(inputGroups);
        body.constraints = AM::RouterConstraints{.maxActiveBundles = 1, .maxSourcesPerOutput = 1};
        blockPorts_[{block.audioSubunitId, static_cast<uint8_t>(block.type), block.id}] = {body.inputs, body.outputs};
        // Selector input plug numbers index the declared inputs from 0, as
        // FFADO reads and writes them (libffado bebob_avdevice.cpp:291, :338)
        // and as the app's discovery view shows them.
        AM::RouterState state;
        if (selector && selector->currentInput && *selector->currentInput < inputs.size())
            state.activeBundles.push_back(AM::RouteBundleId{static_cast<uint32_t>(*selector->currentInput + 1)});
        model_.state.routers.emplace(nodeId, std::move(state));
        model_.topology.nodes.push_back(AM::Node{.id = nodeId, .name = BlockName(block, "Selector"),
                                                 .body = std::move(body)});
    }


    void Connect(const G::DeviceGraph& graph) {
        // Descriptor sources denote whole plugs. A link is emitted only when
        // both sides expose exactly the same channel geometry.
        using Source = ASFW::Protocols::AVC::Descriptors::AudioSourceId;
        const auto sourcePorts = [&](uint8_t subunit, Source source) -> std::vector<AM::PortId> {
            if (source.IsFunctionBlock()) {
                const auto found = blockPorts_.find({subunit, source.type, source.id});
                return found == blockPorts_.end() ? std::vector<AM::PortId>{} : found->second.second;
            }
            if (!source.IsSubunitDestPlug()) return {};
            const auto key = std::tuple{subunit, source.id};
            if (const auto found = destinationPorts_.find(key); found != destinationPorts_.end()) return found->second;
            std::vector<AM::PortId> ports;
            std::vector<G::AudioStreamChannelBinding> channels;
            for (const auto& c : graph.playbackAudioChannels)
                if (c.audioSubunitId == subunit && c.plugId == source.id) channels.push_back(c);
            if (channels.empty()) return {}; // no physical identity/width evidence
            std::ranges::sort(channels, {}, &G::AudioStreamChannelBinding::position);
            for (size_t k = 0; k < channels.size(); ++k)
                if (channels[k].position != k || channels[k].logicalIndex >= graph.playback.channelCount) return {};
            const AM::NodeId node{0x10000000u | (uint32_t{subunit} << 8) | source.id};
            model_.topology.nodes.push_back({node, "Host Playback", AM::EndpointNode{AM::EndpointKind::Host}});
            for (const auto& c : channels) {
                const auto id = AddPort(node, AM::PortDirection::Output, "Playback " + std::to_string(c.logicalIndex + 1));
                model_.topology.ports.back().signal = {AM::SignalKind::HostStream, c.logicalIndex + 1};
                ports.push_back(id);
            }
            destinationPorts_.emplace(key, ports);
            return ports;
        };
        const auto connect = [&](const auto& sources, const auto& destinations) {
            if (sources.size() != destinations.size()) return;
            for (size_t k = 0; k < sources.size(); ++k) {
                const auto port = [&](AM::PortId id) -> const AM::Port& {
                    return *std::ranges::find(model_.topology.ports, id, &AM::Port::id);
                };
                if (port(sources[k]).channels != port(destinations[k]).channels) return;
            }
            for (size_t k = 0; k < sources.size(); ++k)
                model_.topology.fixedLinks.push_back({sources[k], destinations[k]});
        };
        for (const auto& block : graph.controls) {
            const auto found = blockPorts_.find({block.audioSubunitId, static_cast<uint8_t>(block.type), block.id});
            if (found == blockPorts_.end()) continue;
            if (block.type == BlockType::kFeature && block.inputSources.size() == 1) {
                connect(sourcePorts(block.audioSubunitId, block.inputSources.front()), found->second.first);
            } else if (block.type == BlockType::kSelector) {
                const auto selector = std::ranges::find_if(graph.selectors, [&](const auto& item) {
                    return item.audioSubunitId == block.audioSubunitId && item.functionBlockId == block.id;
                });
                const auto& inputs = selector == graph.selectors.end() ? block.inputSources : selector->declaredInputs;
                const auto& groups = selectorInputs_.at(found->first);
                for (size_t k = 0; k < inputs.size() && k < groups.size(); ++k)
                    connect(sourcePorts(block.audioSubunitId, inputs[k]), groups[k]);
            }
        }
        for (const auto& plug : graph.audioSourcePlugs) {
            std::vector<G::AudioStreamChannelBinding> channels;
            for (const auto& c : graph.captureAudioChannels)
                if (c.audioSubunitId == plug.audioSubunitId && c.plugId == plug.plugId) channels.push_back(c);
            if (channels.empty()) continue;
            std::ranges::sort(channels, {}, &G::AudioStreamChannelBinding::position);
            bool valid = true;
            for (size_t k = 0; k < channels.size(); ++k)
                if (channels[k].position != k || channels[k].logicalIndex >= graph.capture.channelCount) valid = false;
            if (!valid) continue;
            const auto sources = sourcePorts(plug.audioSubunitId, plug.source);
            if (sources.size() != channels.size()) continue;
            const AM::NodeId node{0x20000000u | (uint32_t{plug.audioSubunitId} << 8) | plug.plugId};
            model_.topology.nodes.push_back({node, "Host Capture", AM::EndpointNode{AM::EndpointKind::Host}});
            std::vector<AM::PortId> ports;
            for (const auto& c : channels) {
                ports.push_back(AddPort(node, AM::PortDirection::Input, "Capture " + std::to_string(c.logicalIndex + 1)));
                model_.topology.ports.back().signal = {AM::SignalKind::HostStream, c.logicalIndex + 1};
            }
            connect(sources, ports);
        }
    }

    AvcControlModel Take() { return std::move(model_); }

private:
    void AddVolume(const G::FeatureChannelState& channel, const AM::ParameterTarget& target,
                   const std::string& where) {
        if (!channel.volume || !channel.minimum || !channel.maximum || !channel.resolution) return;
        const Model::AvcVolumeRange range{*channel.minimum, *channel.maximum, *channel.resolution};
        // Same rule as the CoreAudio publication (AvcAudioConfig): no level is
        // published whose current value lies outside its own range.
        if (!range.Valid() || *channel.volume == INT16_MIN || *channel.volume < range.minimum ||
            *channel.volume > range.maximum) return;
        const auto id = AvcParameterId(channel.subunit, channel.block, channel.channel, AvcBindingKind::kFeatureVolume);
        model_.topology.parameters.push_back(AM::Parameter{
            .id = id, .target = target, .semantic = AM::ParameterSemantic::Level,
            .domain = AM::ScalarDomain{.min = Model::AvcVolumeRange::Decibels(range.minimum),
                                       .max = Model::AvcVolumeRange::Decibels(range.maximum),
                                       .step = Model::AvcVolumeRange::Decibels(range.resolution),
                                       .unit = AM::ScalarUnit::Decibels},
            .name = where + " Volume"});
        model_.state.parameters.emplace(id, AM::ParameterState{
            .value = static_cast<double>(Model::AvcVolumeRange::Decibels(*channel.volume)),
            .origin = AM::StateOrigin::Observed});
        model_.bindings.push_back(AvcParameterBinding{.parameter = id, .kind = AvcBindingKind::kFeatureVolume,
                                                      .subunit = channel.subunit, .block = channel.block,
                                                      .channel = channel.channel, .range = range});
    }

    void AddMute(const G::FeatureChannelState& channel, const AM::ParameterTarget& target,
                 const std::string& where) {
        if (!channel.mute) return;
        const auto id = AvcParameterId(channel.subunit, channel.block, channel.channel, AvcBindingKind::kFeatureMute);
        model_.topology.parameters.push_back(AM::Parameter{
            .id = id, .target = target, .semantic = AM::ParameterSemantic::Mute,
            .domain = AM::BooleanDomain{}, .name = where + " Mute"});
        model_.state.parameters.emplace(id, AM::ParameterState{.value = *channel.mute,
                                                               .origin = AM::StateOrigin::Observed});
        model_.bindings.push_back(AvcParameterBinding{.parameter = id, .kind = AvcBindingKind::kFeatureMute,
                                                      .subunit = channel.subunit, .block = channel.block,
                                                      .channel = channel.channel});
    }

    using BlockKey = std::tuple<uint8_t, uint8_t, uint8_t>;
    std::map<BlockKey, std::vector<std::vector<AM::PortId>>> selectorInputs_;
    std::map<BlockKey, std::pair<std::vector<AM::PortId>, std::vector<AM::PortId>>> blockPorts_;
    std::map<std::tuple<uint8_t, uint8_t>, std::vector<AM::PortId>> destinationPorts_;
    AvcControlModel model_;
    uint32_t nextPort_{1};
};

} // namespace

AvcControlModel ProjectAvcControls(const G::DeviceGraph& graph, uint64_t revision) {
    Builder builder(revision);
    for (const auto& block : graph.controls) {
        if (block.type == BlockType::kFeature) {
            builder.AddFeature(graph, block);
        } else if (block.type == BlockType::kSelector) {
            const G::AudioSelectorInfo* selector = nullptr;
            for (const auto& candidate : graph.selectors)
                if (candidate.audioSubunitId == block.audioSubunitId && candidate.functionBlockId == block.id)
                    selector = &candidate;
            builder.AddSelector(block, selector);
        }
        // Processing (mixer) and codec blocks: not projected until their
        // controls are decoded; showing an empty node would claim nothing.
    }
    builder.Connect(graph);
    return builder.Take();
}

} // namespace ASFW::Audio::Controls
