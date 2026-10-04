// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../Protocols/AVC/Graph/AvcDeviceGraph.hpp"
#include <algorithm>
#include <optional>
#include <vector>

namespace ASFW::Protocols::AVC {
// Core Audio scope assignment is presentation policy. It never changes AV/C
// channel numbering, control ranges or wire encoding. Processors/mixers may
// reorder or combine channels: do not infer a mapping through them.
struct AvcControlPlacement { uint32_t scope{static_cast<uint32_t>('ptru')}, element{}; };
namespace ControlMapping {
using namespace Graph;
using Source = Descriptors::AudioSourceId;
inline const ControlBlockInfo* Find(const DeviceGraph& graph, uint8_t subunit, Source source) {
    const ControlBlockInfo* result = nullptr;
    for (const auto& block : graph.controls)
        if (block.audioSubunitId == subunit && block.id == source.id && static_cast<uint8_t>(block.type) == source.type) {
            if (result) return nullptr;
            result = &block;
        }
    return result;
}
// Follow only fixed, one-input, channel-preserving feature chains. A selector
// with several declared inputs stays internal even if today's choice is known:
// changing routing must not invalidate an already-published channel assignment.
inline std::optional<Source> Upstream(const DeviceGraph& graph, uint8_t subunit,
    Source source, uint8_t width, std::vector<Source>& visited,
    std::optional<Source> mustPass = {}) {
    bool passed = !mustPass;
    while (source.IsFunctionBlock()) {
        if (std::ranges::find(visited, source) != visited.end()) return {};
        visited.push_back(source);
        if (mustPass && source == *mustPass) passed = true;
        const auto* block = Find(graph, subunit, source);
        if (!block || block->inputSources.size() != 1) return {};
        if (block->type == Descriptors::AudioFunctionBlockType::kFeature) {
            if (block->channelCount != width) return {};
        } else if (block->type != Descriptors::AudioFunctionBlockType::kSelector) return {};
        source = block->inputSources.front();
    }
    return passed && source.IsSubunitDestPlug() ? std::optional{source} : std::nullopt;
}
inline std::optional<uint32_t> Element(const std::vector<AudioStreamChannelBinding>& channels,
    uint32_t total, uint8_t subunit, uint8_t plug, uint8_t width, uint8_t channel) {
    if (!width || channel > width) return {};
    std::vector<uint32_t> map(width, UINT32_MAX);
    for (const auto& binding : channels) {
        if (binding.audioSubunitId != subunit || binding.plugId != plug) continue;
        if (binding.position >= width || binding.logicalIndex >= total || map[binding.position] != UINT32_MAX) return {};
        map[binding.position] = binding.logicalIndex;
    }
    if (std::ranges::find(map, UINT32_MAX) != map.end()) return {};
    auto distinct = map;
    std::ranges::sort(distinct);
    if (std::adjacent_find(distinct.begin(), distinct.end()) != distinct.end()) return {};
    // Channel 0 is a device-wide master only if it controls the entire stream.
    if (channel == 0) return width == total ? std::optional<uint32_t>{0} : std::nullopt;
    return map[channel - 1] + 1;
}
}
inline AvcControlPlacement PlaceAvcControl(const Graph::DeviceGraph& graph,
    const Graph::ControlBlockInfo& block, uint8_t channel) {
    using namespace ControlMapping;
    const uint32_t token = (uint32_t{block.audioSubunitId} << 16) | (uint32_t{block.id} << 8) | channel;
    const AvcControlPlacement internal{static_cast<uint32_t>('ptru'), token + 1};
    if (block.type != Descriptors::AudioFunctionBlockType::kFeature || !block.channelCount) return internal;
    const Source feature{static_cast<uint8_t>(block.type), block.id};
    std::optional<AvcControlPlacement> output, input;
    if (!graph.playback.routeAmbiguous) {
        std::vector<Source> visited;
        const auto root = Upstream(graph, block.audioSubunitId, feature, block.channelCount, visited);
        if (root) if (auto element = Element(graph.playbackAudioChannels, graph.playback.channelCount,
            block.audioSubunitId, root->id, block.channelCount, channel))
            output = AvcControlPlacement{static_cast<uint32_t>('outp'), *element};
    }
    if (!graph.capture.routeAmbiguous) {
        for (const auto& plug : graph.audioSourcePlugs) {
            if (plug.audioSubunitId != block.audioSubunitId) continue;
            std::vector<Source> visited;
            if (!Upstream(graph, plug.audioSubunitId, plug.source, block.channelCount, visited, feature)) continue;
            const auto element = Element(graph.captureAudioChannels, graph.capture.channelCount,
                block.audioSubunitId, plug.plugId, block.channelCount, channel);
            if (!element) continue;
            if (input) return internal; // feature fans out to multiple capture plugs
            input = AvcControlPlacement{static_cast<uint32_t>('inpt'), *element};
        }
    }
    // A shared gain controlling both directions cannot be represented as two
    // independent HAL controls without linked-value synchronization.
    if (output && input) return internal;
    return output ? *output : input ? *input : internal;
}
// A mixer output master need not have the PCM stream's width: selectors can
// distribute its outputs among physical plugs. Resolve only the output-facing
// feature frontier, never through a processing/mixer block into its input gains.
inline bool IsAvcOutputMaster(const Graph::DeviceGraph& graph, const Graph::ControlBlockInfo& feature) {
    if (feature.type != Descriptors::AudioFunctionBlockType::kFeature || !feature.channelCount ||
        feature.volumePurpose == 2 || feature.volumePurpose == 3) return false;
    const auto fixed = PlaceAvcControl(graph, feature, 0);
    if (fixed.scope == static_cast<uint32_t>('outp') && fixed.element == 0) return true;
    if (graph.capture.routeAmbiguous || graph.playback.routeAmbiguous ||
        graph.captureAudioChannels.size() != graph.capture.channelCount || !graph.capture.channelCount) return false;
    std::vector<bool> seen(graph.capture.channelCount, false);
    for (const auto& channel : graph.captureAudioChannels) {
        if (channel.logicalIndex >= seen.size() || seen[channel.logicalIndex]) return false;
        seen[channel.logicalIndex] = true;
    }
    const ControlMapping::Source target{static_cast<uint8_t>(feature.type), feature.id};
    const auto reaches = [&](auto&& self, ControlMapping::Source source,
                             std::vector<ControlMapping::Source> visited) -> bool {
        if (source == target) return true;
        if (!source.IsFunctionBlock() || std::ranges::find(visited, source) != visited.end()) return false;
        visited.push_back(source);
        const auto* block = ControlMapping::Find(graph, feature.audioSubunitId, source);
        if (!block || block->type != Descriptors::AudioFunctionBlockType::kSelector) return false;
        return std::ranges::any_of(block->inputSources, [&](auto input) { return self(self, input, visited); });
    };
    bool output = false;
    for (const auto& plug : graph.audioSourcePlugs) {
        if (plug.audioSubunitId != feature.audioSubunitId || !reaches(reaches, plug.source, {})) continue;
        const bool capture = std::ranges::any_of(graph.captureAudioChannels, [&](const auto& channel) {
            return channel.audioSubunitId == plug.audioSubunitId && channel.plugId == plug.plugId;
        });
        if (capture) return false; // a shared capture/output gain is not an output-only master
        output = true;
    }
    return output;
}
} // namespace ASFW::Protocols::AVC
