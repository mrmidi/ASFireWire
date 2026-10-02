// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryGraph.hpp"
#include "AvcStreamGeometry.hpp"
#include <algorithm>
namespace ASFW::Protocols::AVC::Graph {
namespace A = ASFW::AVC;
namespace E = A::DiscoveryEngine;
namespace {
const E::PlugContents* Find(const E::DiscoverySnapshot& snapshot, A::SubunitAddress address,
                          bool input, uint8_t id) {
    const auto it = std::find_if(snapshot.plugs.begin(), snapshot.plugs.end(), [&](const auto& p) {
        return p.address == address && p.id.value == id &&
            (p.direction == A::Cmd::PlugDirection::kInput) == input;
    });
    return it == snapshot.plugs.end() ? nullptr : &*it;
}
std::optional<StreamGraph> Geometry(const E::PlugContents* p, bool input) {
    if (!p) return {};
    std::optional<uint32_t> rate;
    if (p->signalFormat) if (auto code = A::Cmd::SfcOf(*p->signalFormat)) rate = A::ToHz(*code);
    const A::Cmd::StreamFormat* format = nullptr;
    if (p->current && (!rate || A::ToHz(p->current->compound.rate) == rate)) format = &*p->current;
    if (!format && rate) for (const auto& f : p->formations)
        if (A::ToHz(f.compound.rate) == rate) { format = &f; break; }
    if (!format) return {};
    auto stream = BuildUnitStreamGeometry(*format, input);
    if (!stream) return {};
    stream->supportedSampleRates.clear();
    for (const auto& f : p->formations) {
        auto candidate = BuildUnitStreamGeometry(f, input);
        if (candidate && candidate->channelCount == stream->channelCount && candidate->dataBlockSize == stream->dataBlockSize &&
            std::find(stream->supportedSampleRates.begin(), stream->supportedSampleRates.end(), candidate->currentSampleRate) == stream->supportedSampleRates.end())
            stream->supportedSampleRates.push_back(candidate->currentSampleRate);
    }
    if (stream->supportedSampleRates.empty()) stream->supportedSampleRates = {stream->currentSampleRate};
    return stream;
}
}
DeviceGraph BuildDiscoveryGraph(const E::DiscoverySnapshot& snapshot, std::string modelName) {
    GraphBuildOptions options; options.modelName = std::move(modelName); options.allowDefaultPlugSelection = false;
    const E::SubunitContents* music = nullptr;
    const E::SubunitContents* audio = nullptr;
    // Select the music identity from observed unit routes. Never cross-match
    // equal plug numbers belonging to different subunits.
    const auto* captureUnit = Find(snapshot, A::SubunitAddress::Unit(), false, 0);
    if (captureUnit && captureUnit->route && !captureUnit->route->source.IsUnit())
        for (const auto& c : snapshot.contents)
            if (c.music && c.id.ToAddress() == captureUnit->route->source.Subunit()) {
                music = &c; options.captureSubunitSourcePlugId = captureUnit->route->source.PlugId();
            }
    for (const auto& c : snapshot.contents) {
        if (c.audio && !audio) { audio = &c; options.audioSubunitId = c.id.id; }
        if (!c.music) continue;
        for (const auto& plug : snapshot.plugs) if (plug.address == c.id.ToAddress() &&
            plug.direction == A::Cmd::PlugDirection::kInput && plug.route &&
            plug.route->source == A::Cmd::SignalAddress::UnitIsochronousPlug(0)) {
            if ((music && music != &c) || options.playbackSubunitDestPlugId) {
                // Ambiguous route remains unresolved; unit-format geometry can
                // still be represented independently of descriptor selection.
                options.playbackSubunitDestPlugId.reset(); break;
            }
            music = &c; options.playbackSubunitDestPlugId = plug.id.value;
        }
    }
    if (!music) {
        // Inventory remains available when routes are unavailable; no implicit
        // selection is made by BuildGraph with allowDefaultPlugSelection=false.
        for (const auto& c : snapshot.contents) if (c.music) { music = &c; break; }
    }
    if (audio) for (const auto& block : audio->audio->functionBlocks) {
        if (block.type != Descriptors::AudioFunctionBlockType::kFeature) continue;
        GraphBuildOptions::ConfirmedFeatureControls confirmed{audio->id.id, block.id};
        confirmed.status.channels.resize(block.channelControls.size());
        for (const auto& status : snapshot.features) if (status.subunit == audio->id && status.blockId == block.id) {
            if (confirmed.status.state == FeatureStatusState::kNotProbed) confirmed.status.state = FeatureStatusState::kUnsupported;
            if (!status.value) continue;
            confirmed.status.state = FeatureStatusState::kConfirmed;
            const auto control = static_cast<ConfirmedFeatureControl>(static_cast<uint8_t>(status.control) - 1);
            if (status.channel == 0) confirmed.status.master.push_back(control);
            else if (status.channel <= confirmed.status.channels.size()) confirmed.status.channels[status.channel - 1].push_back(control);
        }
        options.confirmedFeatureControls.push_back(std::move(confirmed));
    }
    DeviceGraph graph = music ? AvcGraphBuilder::BuildGraph(*music->music, audio ? &*audio->audio : nullptr, options) : DeviceGraph{};
    graph.modelName = options.modelName;
    for (bool input : {true, false}) {
        auto& stream = input ? graph.playback : graph.capture;
        auto geometry = Geometry(Find(snapshot, A::SubunitAddress::Unit(), input, 0), input);
        if (!geometry && music && stream.selectionEvidence != StreamSelectionEvidence::kUnresolved)
            geometry = Geometry(Find(snapshot, music->id.ToAddress(), input, stream.subunitPlugId), input);
        if (!geometry) continue;
        if (music && stream.channelCount == geometry->channelCount && stream.selectionEvidence != StreamSelectionEvidence::kUnresolved) {
            if (const auto* descriptor = music->music->FindPlug(stream.subunitPlugId, input)) {
                const auto validated = AvcGraphBuilder::BuildStreamGraph(*descriptor, *music->music, geometry->dataBlockSize);
                stream.slotMap = validated.slotMap; stream.slotMapValidation = validated.slotMapValidation;
                stream.usingFallbackMap = validated.usingFallbackMap;
                stream.dataBlockSize = geometry->dataBlockSize; stream.currentSampleRate = geometry->currentSampleRate;
                stream.supportedSampleRates = geometry->supportedSampleRates; stream.midiStreamCount = geometry->midiStreamCount;
                continue;
            }
        }
        stream = std::move(*geometry);
    }
    return graph;
}
} // namespace ASFW::Protocols::AVC::Graph
