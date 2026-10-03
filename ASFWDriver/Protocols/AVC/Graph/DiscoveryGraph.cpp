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
std::optional<StreamGraph> ExtensionGeometry(const E::ExtensionPlug& facts, bool input,
                                            const std::optional<StreamGraph>& generic) {
    if (facts.formations.empty()) return generic;
    const uint32_t rate = generic ? generic->currentSampleRate : facts.currentRateHz;
    if (!rate || (facts.currentRateHz && facts.currentRateHz != rate)) return {};
    const auto found = std::find_if(facts.formations.begin(), facts.formations.end(),
                                   [rate](const auto& f) { return f.rateHz == rate; });
    if (found == facts.formations.end() || found->pcmChannels > Common::kMaxPcmSlots || found->midiChannels > kMaxMidiDataChannels) return {};
    const auto rateCode = A::StreamFormatRateFromHz(rate); if (!rateCode) return {};
    A::Cmd::StreamFormat format; format.kind = A::Cmd::StreamFormat::Kind::kCompoundAm824;
    format.compound.rate = *rateCode; format.compound.entryCount = found->midiChannels ? 2 : 1;
    format.compound.entries[0] = {static_cast<uint8_t>(found->pcmChannels), A::Cmd::Am824Format::kMultiBitLinearAudioRaw};
    format.compound.entries[1] = {static_cast<uint8_t>(found->midiChannels), A::Cmd::Am824Format::kMidiConformant};
    auto stream = BuildUnitStreamGeometry(format, input); if (!stream) return {};
    stream->supportedSampleRates.clear();
    for (const auto& f : facts.formations)
        if (f.pcmChannels == found->pcmChannels && f.midiChannels == found->midiChannels && A::StreamFormatRateFromHz(f.rateHz))
            if (std::find(stream->supportedSampleRates.begin(), stream->supportedSampleRates.end(), f.rateHz) == stream->supportedSampleRates.end())
                stream->supportedSampleRates.push_back(f.rateHz);
    return stream;
}
bool ValidMap(const Common::PcmSlotMap& map, const StreamGraph& stream) {
    if (!map.FitsWithin(stream.channelCount, stream.dataBlockSize)) return false;
    std::array<bool, 256> seen{};
    for (uint32_t channel = 0; channel < stream.channelCount; ++channel) {
        const auto slot = map.SlotFor(channel);
        if (slot >= seen.size() || seen[slot]) return false;
        seen[slot] = true;
    }
    return true;
}

}
DeviceGraph BuildDiscoveryGraph(const E::DiscoverySnapshot& snapshot, std::string modelName) {
    GraphBuildOptions options; options.modelName = std::move(modelName); options.allowDefaultPlugSelection = false;
    const E::SubunitContents* music = nullptr;
    const E::SubunitContents* audio = nullptr;
    bool playbackAmbiguous = false;
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
            if (playbackAmbiguous || (music && music != &c) || options.playbackSubunitDestPlugId) {
                playbackAmbiguous = true;
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
            if (!status.value || status.attribute != A::Cmd::ControlAttribute::kCurrent) continue;
            confirmed.status.state = FeatureStatusState::kConfirmed;
            const auto control = static_cast<ConfirmedFeatureControl>(static_cast<uint8_t>(status.control) - 1);
            if (status.channel == 0) confirmed.status.master.push_back(control);
            else if (status.channel <= confirmed.status.channels.size()) confirmed.status.channels[status.channel - 1].push_back(control);
        }
        options.confirmedFeatureControls.push_back(std::move(confirmed));
    }
    DeviceGraph graph = music ? AvcGraphBuilder::BuildGraph(*music->music, audio ? &*audio->audio : nullptr, options) : DeviceGraph{};
    // Identifier advertises supported modes; status advertises current modes.
    // TA 2001007 Tables 5.5/5.6 and Figure 6.3. Prefer the identifier when read.
    if (music && music->musicIdentifier && music->musicIdentifier->general) {
        graph.transmitModes = music->musicIdentifier->general->transmit;
        graph.receiveModes = music->musicIdentifier->general->receive;
        graph.supportsBlockingTransmit = (*graph.transmitModes & Descriptors::kMusicCapabilityBlockingBit) != 0;
    }
    graph.modelName = options.modelName;
    for (const auto& status : snapshot.features) {
        if (!status.value) continue;
        auto it = std::find_if(graph.featureChannels.begin(), graph.featureChannels.end(), [&](const auto& item) {
            return item.subunit == status.subunit.id && item.block == status.blockId && item.channel == status.channel;
        });
        if (it == graph.featureChannels.end()) {
            graph.featureChannels.push_back({status.subunit.id, status.blockId, status.channel});
            it = std::prev(graph.featureChannels.end());
        }
        if (status.control == A::Cmd::FeatureControl::kMute && status.attribute == A::Cmd::ControlAttribute::kCurrent &&
            status.value->dataLength == 1 && (status.value->data[0] == A::Cmd::kBooleanTrue || status.value->data[0] == A::Cmd::kBooleanFalse))
            it->mute = status.value->AsMute();
        if (status.control != A::Cmd::FeatureControl::kVolume || status.value->dataLength != 2) continue;
        const auto volume = status.value->AsVolume();
        if (!volume.IsValid()) continue;
        switch (status.attribute) {
        case A::Cmd::ControlAttribute::kCurrent: it->volume = volume.Raw(); break;
        case A::Cmd::ControlAttribute::kMinimum: it->minimum = volume.Raw(); break;
        case A::Cmd::ControlAttribute::kMaximum: it->maximum = volume.Raw(); break;
        case A::Cmd::ControlAttribute::kResolution: it->resolution = volume.Raw(); break;
        default: break;
        }
    }

    for (bool input : {true, false}) {
        auto& stream = input ? graph.playback : graph.capture;
        auto geometry = Geometry(Find(snapshot, A::SubunitAddress::Unit(), input, 0), input);
        if (!geometry && music && stream.selectionEvidence != StreamSelectionEvidence::kUnresolved)
            geometry = Geometry(Find(snapshot, music->id.ToAddress(), input, stream.subunitPlugId), input);
        const auto& extension = input ? snapshot.extension.playback : snapshot.extension.capture;
        geometry = ExtensionGeometry(extension, input, geometry);
        if (!geometry) continue;
        const auto bridgeMap = [&] {
            if (extension.pcmSlots.slotCount && ValidMap(extension.pcmSlots, *geometry)) {
                geometry->slotMap = extension.pcmSlots;
                geometry->slotMapValidation = SlotMapValidation::kValidated;
                geometry->usingFallbackMap = false;
            }
        };
        bridgeMap();
        if (music && stream.channelCount == geometry->channelCount && stream.selectionEvidence != StreamSelectionEvidence::kUnresolved) {
            if (const auto* descriptor = music->music->FindPlug(stream.subunitPlugId, input)) {
                const auto validated = AvcGraphBuilder::BuildStreamGraph(*descriptor, *music->music, geometry->dataBlockSize);
                const bool descriptorValid = !validated.usingFallbackMap && ValidMap(validated.slotMap, *geometry);
                stream.slotMap = descriptorValid ? validated.slotMap : geometry->slotMap;
                stream.slotMapValidation = descriptorValid ? validated.slotMapValidation : geometry->slotMapValidation;
                stream.usingFallbackMap = descriptorValid ? false : geometry->usingFallbackMap;
                stream.dataBlockSize = geometry->dataBlockSize; stream.currentSampleRate = geometry->currentSampleRate;
                stream.supportedSampleRates = geometry->supportedSampleRates; stream.midiStreamCount = geometry->midiStreamCount;
                continue;
            }
        }
        stream = std::move(*geometry);
    }
    for (auto& selector : graph.selectors)
        for (const auto& status : snapshot.selectors)
            if (status.subunit.id == selector.audioSubunitId && status.blockId == selector.functionBlockId) {
                if (status.value) selector.currentInput = status.value->inputPlug;
            }
    graph.playback.routeAmbiguous = playbackAmbiguous;
    for (const auto& plug : snapshot.plugs) if (plug.route)
        graph.routes.push_back({plug.route->source.bytes, plug.route->destination.bytes});
    // Join channel-level Music routes to confirmed inter-subunit SIGNAL SOURCE
    // routes. Music endpoint layout cross-checked with FFADO
    // avc_descriptor_music.cpp:451-461; Audio source IDs retain type + ID.
    if (music) for (bool playback : {true, false}) {
        const auto& stream = playback ? graph.playback : graph.capture;
        auto& bindings = playback ? graph.playbackAudioChannels : graph.captureAudioChannels;
        if (stream.routeAmbiguous || stream.selectionEvidence == StreamSelectionEvidence::kUnresolved) continue;
        for (const auto& channel : stream.channels) {
            const auto* detail = music->music->FindMusicPlug(channel.musicPlugId);
            if (!detail) continue;
            const auto& endpoint = playback ? detail->destination : detail->source;
            const auto expectedType = playback ? Descriptors::MusicPlugEndpoint::kSubunitSourcePlug : Descriptors::MusicPlugEndpoint::kSubunitDestinationPlug;
            if (!endpoint || endpoint->functionType != expectedType ||
                endpoint->streamPosition == Descriptors::MusicPlugEndpoint::kUnset) continue;
            const std::array<uint8_t, 2> musicEndpoint{music->id.ToAddress().Byte(), endpoint->plugId};
            std::optional<AudioStreamChannelBinding> binding;
            bool ambiguous = false;
            for (const auto& route : graph.routes) {
                if ((playback ? route.source : route.destination) != musicEndpoint) continue;
                const auto& peer = playback ? route.destination : route.source;
                const auto address = A::SubunitAddress::FromByte(peer[0]);
                if (address.Type() != A::SubunitType::kAudio) continue;
                AudioStreamChannelBinding candidate{channel.logicalIndex, address.Id(), peer[1], endpoint->streamPosition};
                if (binding && (binding->audioSubunitId != candidate.audioSubunitId || binding->plugId != candidate.plugId || binding->position != candidate.position)) ambiguous = true;
                binding = candidate;
            }
            if (binding && !ambiguous) bindings.push_back(*binding);
        }
    }
    for (const auto& route : snapshot.confirmedClockRoutes) {
        ClockSourceInfo clock;
        clock.endpoint.subunitId = route.source.Subunit().Id(); clock.endpoint.endpointId = route.source.PlugId();
        clock.endpoint.kind = route.source.IsUnit() ? (route.source.IsExternalUnitPlug() ?
            ClockEndpointKind::kUnitExternalInput : ClockEndpointKind::kUnitIsochronousInput) : ClockEndpointKind::kMusicSubunitPlug;
        clock.name = "Source " + std::to_string(route.source.bytes[0]) + ":" + std::to_string(route.source.PlugId());
        for (const auto& plug : snapshot.plugs)
            if (plug.route && plug.route->destination == route.destination && plug.route->source == route.source) clock.isCurrent = true;
        graph.clockSources.push_back(std::move(clock));
    }
    for (const auto& outcome : snapshot.outcomes) {
        graph.probeResults.push_back({outcome.address.Byte(), static_cast<uint8_t>(outcome.opcode),
            outcome.error, outcome.responseCode, outcome.command, outcome.responseOperands, outcome.responseAddress, outcome.responseOpcode});
    }
    return graph;
}
} // namespace ASFW::Protocols::AVC::Graph
