// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcControlProjection.cpp - see AvcControlProjection.hpp.

#include "AvcControlProjection.hpp"

#include <string>

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
        // Channel k (1-based on the wire) has input/output port index k-1.
        const uint8_t width = block.channelCount == 0 ? 1 : block.channelCount;
        std::vector<AM::PortId> outputs;
        for (uint8_t ch = 1; ch <= width; ++ch) {
            const auto label = "Ch " + std::to_string(ch);
            body.inputs.push_back(AddPort(nodeId, AM::PortDirection::Input, label + " In"));
            outputs.push_back(AddPort(nodeId, AM::PortDirection::Output, label + " Out"));
        }
        body.outputs = outputs;
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
        const auto out = AddPort(nodeId, AM::PortDirection::Output, "Out");
        body.outputs.push_back(out);
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto in = AddPort(nodeId, AM::PortDirection::Input, "In " + std::to_string(i + 1));
            body.inputs.push_back(in);
            body.legalBundles.push_back(AM::RouteBundle{
                .id = AM::RouteBundleId{static_cast<uint32_t>(i + 1)},
                .routes = {AM::Route{.input = in, .output = out}}});
        }
        body.constraints = AM::RouterConstraints{.maxActiveBundles = 1, .maxSourcesPerOutput = 1};
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
    return builder.Take();
}

} // namespace ASFW::Audio::Controls
