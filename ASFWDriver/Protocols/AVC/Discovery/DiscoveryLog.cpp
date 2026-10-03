// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiscoveryLog.cpp - See DiscoveryLog.hpp.

#include "DiscoveryLog.hpp"

#include "../Commands/CommandNames.hpp"
#include "../Core/AvcNames.hpp"
#include "../Descriptors/DescriptorNames.hpp"

#include <algorithm>
#include <string_view>

namespace ASFW::AVC::DiscoveryEngine {

namespace {

namespace D = ASFW::Protocols::AVC::Descriptors;
namespace G = ASFW::Protocols::AVC::Graph;

using ASFW::AVC::Describe;
using ASFW::AVC::Hex;
using ASFW::AVC::HexBytes;
using ASFW::AVC::names::kErrorKinds;

constexpr std::string_view kContinuation = " (cont ";

[[nodiscard]] const char* YesNo(bool value) { return value ? "yes" : "no"; }

/// "Music(0x0c)#0": the type by name and the instance. The address byte is printed once, on the
/// "subunit" line; repeating it on every line only costs ring space.
[[nodiscard]] std::string Label(SubunitAddress address) {
    if (address.IsUnit()) return Describe(SubunitType::kUnit);
    return Describe(address.Type()) + "#" + std::to_string(address.Id());
}
/// The unit is held as type Unit with id 0; its address is the unit address, not 0xf8.
[[nodiscard]] std::string Label(const SubunitId& id) {
    return id.type == SubunitType::kUnit ? Label(SubunitAddress::Unit()) : Label(id.ToAddress());
}

[[nodiscard]] std::string Quoted(const std::string& text) { return "\"" + text + "\""; }

[[nodiscard]] std::string DescribeError(const std::optional<AvcError>& error) {
    if (!error) return "none";
    std::string text = Describe(error->kind);
    if (error->response) text += " response=" + Describe(*error->response);
    if (error->operandOffset != 0) text += " at operand " + std::to_string(error->operandOffset);
    return text;
}

/// One line per fact; a fact longer than the ring message is wrapped at ", " or a space into numbered
/// continuation lines that repeat the context.
class Lines {
public:
    void Add(const std::string& context, const std::string& body) {
        const std::string head = std::string(kDiscoveryLogTag) + " " + context;
        if (head.size() + 1 + body.size() <= kMaxDiscoveryLogLine) {
            lines_.push_back(head + " " + body);
            return;
        }
        std::string rest = body;
        unsigned part = 1;
        while (!rest.empty()) {
            const std::string label = part == 1 ? head : head + std::string(kContinuation) + std::to_string(part) + ")";
            const size_t budget = kMaxDiscoveryLogLine > label.size() + 1 ? kMaxDiscoveryLogLine - label.size() - 1 : 1;
            if (rest.size() <= budget) {
                lines_.push_back(label + " " + rest);
                break;
            }
            size_t cut = rest.rfind(", ", budget - 1);  // the comma stays on this line, so it must fit
            if (cut != std::string::npos && cut > 0) {
                cut += 1;  // keep the comma on this line
            } else if ((cut = rest.rfind(' ', budget)) == std::string::npos || cut == 0) {
                cut = budget;
            }
            lines_.push_back(label + " " + rest.substr(0, cut));
            const size_t next = rest.find_first_not_of(' ', cut);
            rest = next == std::string::npos ? std::string() : rest.substr(next);
            ++part;
        }
    }
    [[nodiscard]] std::vector<std::string> Take() { return std::move(lines_); }

private:
    std::vector<std::string> lines_;
};

// ---------------------------------------------------------------------------
// Pieces
// ---------------------------------------------------------------------------

[[nodiscard]] std::string PlugContext(const PlugContents& plug) {
    return "plug " + Label(plug.address) + " " + Cmd::Describe(plug.direction) + " #" + std::to_string(plug.id.value);
}

/// The CCM STATUS answer for a plug: both addresses, the status byte by field, and any departure from the spec.
void DescribeRoute(const std::string& context, const Cmd::SignalSource& route, Lines& out) {
    const auto status = route.Status();
    out.Add(context, "route source=" + Cmd::Describe(route.source, Cmd::SignalRole::kSource) +
                         " destination=" + Cmd::Describe(route.destination, Cmd::SignalRole::kDestination));
    std::string text = "route_status first_operand=" + Hex(route.first.Raw()) + " " + Cmd::Describe(status);
    if (const auto virtualOutput = route.AsVirtualOutput()) {
        text += " virtual_output_channel=" + std::to_string(virtualOutput->isochronousChannel) +
                " virtual_output_input_plug=iPCR[" + std::to_string(virtualOutput->inputPlug.Raw()) + "]";
    }
    out.Add(context, text);
    const auto kind = Cmd::DestinationKindOf(route.destination);
    out.Add(context, "route_check destination_kind=" + (kind ? Cmd::Describe(*kind) : std::string("n/a")) + " deviations=" +
                         (kind ? Cmd::Describe(Cmd::CheckStatusAgainstSpec(status, *kind)) : std::string("n/a")));
}

[[nodiscard]] std::string DescribeFeatureReply(const Cmd::FeatureReply& reply) {
    std::string text = "attribute=" + Cmd::Describe(reply.attribute) + " data=[" +
                       HexBytes(std::span<const uint8_t>{reply.data.data(), reply.dataLength}) + "]";
    if (reply.control == Cmd::FeatureControl::kMute && reply.dataLength > 0) {
        const uint8_t raw = reply.data[0];
        text += raw == Cmd::kBooleanTrue ? " muted(on)" : raw == Cmd::kBooleanFalse ? " not muted(off)"
                                                                                     : " UNKNOWN(mute_value:" + Hex(raw) + ")";
    } else if (reply.control == Cmd::FeatureControl::kVolume && reply.dataLength >= 2) {
        const auto volume = reply.AsVolume();
        text += " volume_raw=" + Hex(static_cast<uint16_t>(volume.Raw()), 4);
        text += volume.IsValid() ? (volume.IsNegativeInfinity() ? " -inf dB" : " " + std::to_string(volume.Raw() / 256) + " dB")
                                 : " invalid";
    }
    return text;
}

[[nodiscard]] std::string DescribeCapabilityBits(uint8_t bits) {
    std::string text;
    if ((bits & D::kMusicCapabilityNonBlockingBit) != 0) text += "non-blocking";
    if ((bits & D::kMusicCapabilityBlockingBit) != 0) text += std::string(text.empty() ? "" : "+") + "blocking";
    if (text.empty()) text = "none";
    return text + "(" + Hex(bits) + ")";
}

[[nodiscard]] std::string DescribeFormation(const Formation& f) {
    return std::to_string(f.rateHz) + " Hz " + std::to_string(f.pcmChannels) + " PCM + " + std::to_string(f.midiChannels) + " MIDI";
}

// The graph's own enums are ours, not wire values: plain names.
[[nodiscard]] const char* Name(G::StreamSelectionEvidence value) {
    switch (value) {
        case G::StreamSelectionEvidence::kUnresolved: return "unresolved";
        case G::StreamSelectionEvidence::kSignalSourceInquiry: return "SIGNAL SOURCE inquiry";
        case G::StreamSelectionEvidence::kDescriptorDefaultAssumption: return "descriptor default assumption";
        case G::StreamSelectionEvidence::kUnitPlugFormat: return "unit plug format";
    }
    return "UNKNOWN(stream_selection_evidence)";
}
[[nodiscard]] const char* Name(G::SlotMapValidation value) {
    switch (value) {
        case G::SlotMapValidation::kNoDataBlockSize: return "no data block size";
        case G::SlotMapValidation::kValidated: return "validated";
        case G::SlotMapValidation::kRejectedFallback: return "rejected, fallback map";
    }
    return "UNKNOWN(slot_map_validation)";
}
[[nodiscard]] const char* Name(G::ClockEndpointKind value) {
    switch (value) {
        case G::ClockEndpointKind::kUnitIsochronousInput: return "unit isochronous input";
        case G::ClockEndpointKind::kUnitIsochronousOutput: return "unit isochronous output";
        case G::ClockEndpointKind::kUnitExternalInput: return "unit external input";
        case G::ClockEndpointKind::kMusicSubunitPlug: return "music subunit plug";
        case G::ClockEndpointKind::kAudioSubunitPlug: return "audio subunit plug";
        case G::ClockEndpointKind::kAudioFunctionBlock: return "audio function block";
    }
    return "UNKNOWN(clock_endpoint_kind)";
}
[[nodiscard]] const char* Name(TextReferenceKind value) {
    switch (value) {
        case TextReferenceKind::Duplicate: return "duplicate";
        case TextReferenceKind::Cycle: return "cycle";
        case TextReferenceKind::BudgetExceeded: return "budget exceeded";
    }
    return "UNKNOWN(text_reference_kind)";
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void DescribeUnit(const DiscoverySnapshot& s, Lines& out) {
    out.Add("discovery", "guid=" + Hex64(s.route.guid) + " node=" + std::to_string(s.route.nodeId) +
                             " generation=" + std::to_string(s.route.generation.value) +
                             " session=" + std::to_string(s.session.value) + " complete=" + YesNo(s.complete) +
                             " cancelled=" + YesNo(s.cancelled) + " terminal_error=" + DescribeError(s.terminalError));
    const auto& info = s.unit.info;
    out.Add("unit", "unit_type=" + Describe(info.unitType) + " unit=" + std::to_string(info.unitId) +
                        " company_id=" + Hex(ToOui(info.companyId), 6));
    const auto& p = s.unit.unitPlugs;
    const auto& a = s.unit.unitAsyncPlugs;
    out.Add("unit plugs", "isochronous_in=" + std::to_string(p.isochronousInputs) +
                              " isochronous_out=" + std::to_string(p.isochronousOutputs) +
                              " external_in=" + std::to_string(p.externalInputs) +
                              " external_out=" + std::to_string(p.externalOutputs) +
                              " async_in=" + std::to_string(a.asynchronousInputs) +
                              " async_out=" + std::to_string(a.asynchronousOutputs));
    for (const auto& sub : s.unit.subunits) {
        out.Add("subunit " + Describe(sub.id.ToAddress()),
                "destination_plugs=" + std::to_string(sub.plugs.destinationPlugs) +
                    " source_plugs=" + std::to_string(sub.plugs.sourcePlugs) + " plugs_discovered=" + YesNo(sub.plugsDiscovered));
    }
}

/// A stream format as facts: a compound AM824 format is a header line and one line per entry, so a
/// device with many entries never wraps; any other format is one line (with its bytes when no table names it).
void AddFormat(Lines& out, const std::string& context, const std::string& label, const Cmd::StreamFormat& format) {
    if (format.kind != Cmd::StreamFormat::Kind::kCompoundAm824) {
        out.Add(context, label + " " + Cmd::Describe(format));
        return;
    }
    out.Add(context, label + " " + Cmd::DescribeCompoundHeader(format.compound));
    size_t index = 0;
    for (const auto& entry : format.compound.Entries()) {
        out.Add(context, label + " entry[" + std::to_string(index++) + "] " + Cmd::Describe(entry));
    }
}

void DescribePlugs(const DiscoverySnapshot& s, Lines& out) {
    for (const auto& plug : s.plugs) {
        const std::string context = PlugContext(plug);
        if (plug.signalFormat) out.Add(context, "signal_format " + Cmd::Describe(*plug.signalFormat));
        if (plug.current) AddFormat(out, context, "current_format", *plug.current);
        size_t index = 0;
        for (const auto& formation : plug.formations) {
            AddFormat(out, context, "supported_format[" + std::to_string(index++) + "]", formation);
        }
        if (plug.route) DescribeRoute(context, *plug.route, out);
    }
    for (const auto& route : s.confirmedClockRoutes) {
        out.Add("clock_route",
                "confirmed by SPECIFIC INQUIRY source=" + Cmd::Describe(route.source, Cmd::SignalRole::kSource) +
                    " destination=" + Cmd::Describe(route.destination, Cmd::SignalRole::kDestination));
    }
}

void DescribeMusic(const SubunitContents& c, Lines& out) {
    const auto& m = *c.music;
    const std::string context = "music " + Label(c.id);
    out.Add(context, "descriptor_length=" + std::to_string(m.declaredLength));
    size_t blockIndex = 0;
    for (const auto& b : m.topLevelBlocks) {
        out.Add(context, "top_level_block[" + std::to_string(blockIndex++) + "] " + D::DescribeInfoBlockType(b.type) +
                             " bytes=" + std::to_string(b.totalBytes) + " primary=" + std::to_string(b.primaryLength));
    }
    const auto& caps = m.capabilities;
    if (caps.hasGeneralCapability) {
        out.Add(context, "general_status transmit=" + DescribeCapabilityBits(caps.transmitCapabilityFlags) +
                             " receive=" + DescribeCapabilityBits(caps.receiveCapabilityFlags) +
                             " latency=" + (caps.latencyCapability ? Hex(*caps.latencyCapability, 8) : std::string("n/a")));
    }
    // The capability readings below come from top-level blocks 8101-8105, a layout of the identifier
    // descriptor (TA 2001007 §5.2). They are unverified on any device (audit F7); logged only when read.
    if (caps.hasAudioCapability) {
        out.Add(context, "capability_from_top_level_block audio max_input_channels=" +
                             std::to_string(caps.maxAudioInputChannels.value_or(0)) +
                             " max_output_channels=" + std::to_string(caps.maxAudioOutputChannels.value_or(0)) +
                             " (unverified, F7)");
    }
    if (caps.hasMidiCapability) {
        out.Add(context, "capability_from_top_level_block MIDI version=" + std::to_string(caps.midiVersionMajor) + "." +
                             std::to_string(caps.midiVersionMinor) + " adaptation_layer=" + Hex(caps.midiAdaptationLayerVersion) +
                             " max_input_ports=" + std::to_string(caps.maxMidiInputPorts.value_or(0)) +
                             " max_output_ports=" + std::to_string(caps.maxMidiOutputPorts.value_or(0)) + " (unverified, F7)");
    }
    if (caps.hasSmpteTimeCodeCapability || caps.hasSampleCountCapability || caps.hasAudioSyncCapability) {
        out.Add(context, "capability_from_top_level_block smpte=" + Hex(caps.smpteTimeCodeCapabilityFlags) +
                             " sample_count=" + Hex(caps.sampleCountCapabilityFlags) + " audio_sync=" +
                             Hex(caps.audioSyncCapabilityFlags) + " (unverified, F7)");
    }
    if (m.hasRoutingStatus) {
        out.Add(context, "routing_status destination_plugs=" + std::to_string(m.numDestPlugs) +
                             " source_plugs=" + std::to_string(m.numSrcPlugs));
    }
    for (const auto& plug : m.plugs) {
        const std::string plugContext = context + " plug " + (plug.isDestination ? "destination" : "source") + " #" + std::to_string(plug.plugId);
        out.Add(plugContext, "usage=" + D::DescribeMusicPlugUsage(plug.usage) + " name=" + Quoted(plug.name) +
                                 " clusters=" + std::to_string(plug.clusters.size()));
        size_t index = 0;
        for (const auto& cluster : plug.clusters) {
            std::string signals;
            for (const auto& signal : cluster.signals) {
                if (!signals.empty()) signals += ", ";
                signals += "plug " + std::to_string(signal.musicPlugId) + "@" + std::to_string(signal.position);
            }
            const std::string clusterLabel = "cluster[" + std::to_string(index++) + "]";
            out.Add(plugContext, clusterLabel + " stream_format=" +
                                     Describe(static_cast<Cmd::Am824Format>(cluster.streamFormatCode)) +
                                     " port_type=" + D::DescribeMusicPortType(cluster.portType) +
                                     " channels=" + std::to_string(cluster.channelCount) + " name=" + Quoted(cluster.name));
            out.Add(plugContext, clusterLabel + " signals=[" + signals + "]");
        }
    }
    for (const auto& plug : m.musicPlugs) {
        const std::string plugContext = context + " music_plug " + std::to_string(plug.musicPlugId);
        out.Add(plugContext, "type=" + D::DescribeMusicPlugType(plug.plugType) +
                                 " routing_support=" + D::DescribeMusicRoutingSupport(plug.routingSupport) +
                                 " name=" + Quoted(plug.name));
        if (plug.source) out.Add(plugContext, "source " + D::DescribeEndpoint(*plug.source));
        if (plug.destination) out.Add(plugContext, "destination " + D::DescribeEndpoint(*plug.destination));
    }
    std::vector<uint8_t> labelled;
    for (const auto& [plugId, labels] : m.perPlugChannelNames) labelled.push_back(plugId);
    std::ranges::sort(labelled);
    for (const uint8_t plugId : labelled) {
        size_t position = 0;
        for (const auto& label : m.perPlugChannelNames.at(plugId)) {
            out.Add(context + " source plug #" + std::to_string(plugId),
                    "audio_stream_label[" + std::to_string(position++) + "]=" + Quoted(label));
        }
    }
}

void DescribeAudio(const SubunitContents& c, Lines& out) {
    const auto& a = *c.audio;
    const std::string context = "audio " + Label(c.id);
    std::string roots;
    for (const uint16_t id : a.rootListIds) roots += (roots.empty() ? "" : ", ") + Hex(id, 4);
    out.Add(context, "generation_id=" + std::to_string(a.generationId) + " list_id_size=" + std::to_string(a.sizeOfListId) +
                         " object_id_size=" + std::to_string(a.sizeOfObjectId) +
                         " object_position_size=" + std::to_string(a.sizeOfObjectPosition) + " root_lists=[" + roots +
                         "] text_database_entries=" + std::to_string(c.text.size()));
    size_t link = 0;
    for (const auto& source : a.sourcePlugLinks) {
        out.Add(context, "subunit_source_plug[" + std::to_string(link++) + "] fed_by " + D::Describe(source));
    }
    for (const auto& fb : a.functionBlocks) {
        const std::string fbContext = context + " function_block " + D::Describe(fb.type) + " #" + std::to_string(fb.id);
        out.Add(fbContext, "name=" + Quoted(fb.name) + " name_index=" +
                               (fb.nameIndex == D::kNoNameIndex ? std::string("none") : std::to_string(fb.nameIndex)));
        for (const auto& source : fb.inputSources) out.Add(fbContext, "input " + D::Describe(source));
        if (fb.type == D::AudioFunctionBlockType::kFeature) {
            // The bitmap is a descriptor hint only; STATUS decides which controls exist (audit F8).
            std::string channels;
            for (size_t i = 0; i < fb.channelControls.size(); ++i) channels += (i ? ", " : "") + Hex(fb.channelControls[i], 4);
            out.Add(fbContext, "cluster_channels=" + std::to_string(fb.clusterChannels) + " general_tag=" +
                                   std::to_string(fb.generalTag));
            out.Add(fbContext, "advertised_controls master=" + Hex(fb.masterControls, 4) + " channels=[" + channels +
                                   "] (hint, bit order unverified, F8)");
        } else if (fb.type == D::AudioFunctionBlockType::kProcessing) {
            out.Add(fbContext, "process_type=" + Hex(fb.processType));
        }
    }
}

void DescribeDescriptors(const DiscoverySnapshot& s, Lines& out) {
    for (const auto& d : s.descriptors) {
        out.Add("descriptor " + Label(d.subunit),
                Cmd::Describe(d.specifier) + " bytes=" + std::to_string(d.bytes.size()) +
                    " primary_error=" + DescribeError(d.primaryError) + " cleanup_error=" + DescribeError(d.cleanupError) +
                    " parse_error=" + (d.parseError ? D::DescribeParseError(*d.parseError) : std::string("none")));
    }
    for (const auto& r : s.textReferences) {
        out.Add("text_reference " + Label(r.subunit), "target=" + Hex(r.target, 4) + " kind=" + Name(r.kind));
    }
}

void DescribeStatuses(const DiscoverySnapshot& s, Lines& out) {
    for (const auto& f : s.features) {
        const std::string context = "feature_status " + Label(f.subunit) + " fb=" + std::to_string(f.blockId) +
                                    " channel=" + std::to_string(f.channel) + " control=" + Cmd::Describe(f.control);
        out.Add(context, f.value ? DescribeFeatureReply(*f.value) : "error=" + DescribeError(f.error));
    }
    for (const auto& sel : s.selectors) {
        out.Add("selector_status " + Label(sel.subunit) + " fb=" + std::to_string(sel.blockId),
                sel.value ? "input_plug=" + std::to_string(sel.value->inputPlug) : std::string("no answer"));
    }
}

void DescribeOutcomes(const DiscoverySnapshot& s, Lines& out) {
    size_t failed = 0;
    std::vector<std::pair<std::string, size_t>> failures;  // one line per distinct failure, in first-seen order
    for (const auto& o : s.outcomes) {
        if (!o.error) continue;
        ++failed;
        const std::string key = "probe_failed " + Label(o.address) + " opcode=" + Describe(o.address.Type(), o.opcode) + " error=" + DescribeError(o.error);
        const auto found = std::ranges::find_if(failures, [&key](const auto& f) { return f.first == key; });
        if (found == failures.end()) failures.emplace_back(key, 1); else ++found->second;
    }
    for (const auto& [key, count] : failures) out.Add(key.substr(0, key.find(' ')), key.substr(key.find(' ') + 1) + " count=" + std::to_string(count));
    out.Add("probes", "total=" + std::to_string(s.outcomes.size()) + " failed=" + std::to_string(failed));
}

void DescribeExtension(const DiscoverySnapshot& s, Lines& out) {
    const auto side = [&out](const char* name, const ExtensionPlug& plug) {
        if (plug.currentRateHz == 0 && plug.formations.size() == 0 && plug.pcmSlots.slotCount == 0) {
            out.Add(std::string("extension ") + name, "none reported");
            return;
        }
        const std::string context = std::string("extension ") + name;
        out.Add(context, "current_rate=" + std::to_string(plug.currentRateHz) + " Hz pcm_slots=" +
                             std::to_string(plug.pcmSlots.slotCount) + " pcm_channels=" + std::to_string(plug.pcmSlots.channelCount));
        size_t index = 0;
        for (const auto& f : plug.formations) out.Add(context, "formation[" + std::to_string(index++) + "] " + DescribeFormation(f));
    };
    side("playback", s.extension.playback);
    side("capture", s.extension.capture);
}

void DescribeStream(const char* name, const G::StreamGraph& g, Lines& out) {
    std::string rates;
    for (const uint32_t rate : g.supportedSampleRates) rates += (rates.empty() ? "" : ", ") + std::to_string(rate);
    const std::string context = std::string("graph ") + name;
    out.Add(context, "subunit_plug=" + std::to_string(g.subunitPlugId) + " selected_by=" + Name(g.selectionEvidence) +
                         " route_ambiguous=" + YesNo(g.routeAmbiguous));
    out.Add(context, "data_block_size=" + std::to_string(g.dataBlockSize) + " pcm_channels=" + std::to_string(g.channelCount) +
                         " midi_streams=" + std::to_string(g.midiStreamCount) + " slot_map=" + Name(g.slotMapValidation) +
                         " fallback_map=" + YesNo(g.usingFallbackMap));
    out.Add(context, "current_rate=" + std::to_string(g.currentSampleRate) + " rates=[" + rates + "]");
    for (const auto& ch : g.channels) {
        out.Add(std::string("graph ") + name + " channel " + std::to_string(ch.logicalIndex),
                "slot=" + std::to_string(ch.slotIndex) + " name=" + Quoted(ch.name) + " cluster=" + Quoted(ch.clusterName) +
                    " format=" + Describe(static_cast<Cmd::Am824Format>(ch.formatCode)) +
                    " music_plug=" + (ch.musicPlugId == G::kUnsetMusicPlugId ? std::string("none") : std::to_string(ch.musicPlugId)));
    }
}

void DescribeGraph(const G::DeviceGraph& g, Lines& out) {
    out.Add("graph", "model=" + Quoted(g.modelName) + " supports_blocking_transmit=" + YesNo(g.supportsBlockingTransmit));
    DescribeStream("playback", g.playback, out);
    DescribeStream("capture", g.capture, out);
    for (const auto& c : g.clockSources) {
        out.Add("graph clock_source", "name=" + Quoted(c.name) + " endpoint=" + Name(c.endpoint.kind) +
                                          " subunit=" + std::to_string(c.endpoint.subunitId) +
                                          " endpoint_id=" + std::to_string(c.endpoint.endpointId) + " current=" + YesNo(c.isCurrent));
    }
    for (const auto& sync : g.syncDestinations) {
        out.Add("graph sync_destination", "music_plug=" + std::to_string(sync.subunitPlugId) + " name=" + Quoted(sync.name));
    }
    for (const auto& sel : g.selectors) {
        const std::string context = "graph selector audio_subunit=" + std::to_string(sel.audioSubunitId) + " fb=" +
                                    std::to_string(sel.functionBlockId);
        out.Add(context, "name=" + Quoted(sel.name) + " current_input=" +
                             (sel.currentInput ? std::to_string(*sel.currentInput) : std::string("unconfirmed")));
        for (const auto& source : sel.declaredInputs) out.Add(context, "declared_input " + D::Describe(source));
    }
    out.Add("graph", "control_blocks=" + std::to_string(g.controls.size()) + " routing_edges=" + std::to_string(g.routes.size()));
}

} // namespace

std::vector<std::string> DescribeDiscovery(const DiscoverySnapshot& snapshot, const G::DeviceGraph* graph) {
    Lines out;
    DescribeUnit(snapshot, out);
    DescribePlugs(snapshot, out);
    DescribeDescriptors(snapshot, out);
    for (const auto& c : snapshot.contents) {
        if (c.music) DescribeMusic(c, out);
        if (c.audio) DescribeAudio(c, out);
    }
    DescribeStatuses(snapshot, out);
    DescribeOutcomes(snapshot, out);
    DescribeExtension(snapshot, out);
    if (graph != nullptr) DescribeGraph(*graph, out);
    return out.Take();
}

} // namespace ASFW::AVC::DiscoveryEngine
