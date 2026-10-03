// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCDiscoveryDocument.cpp - see AVCDiscoveryDocument.hpp.

#include "AVCDiscoveryDocument.hpp"
#include "../../Protocols/AVC/Commands/AudioNames.hpp"
#include "../../Protocols/AVC/Commands/CommandNames.hpp"
#include "../../Protocols/AVC/Descriptors/AudioControlBits.hpp"
#include "../../Protocols/AVC/Descriptors/DescriptorNames.hpp"
#include "../../Protocols/AVC/Descriptors/MusicCapabilityNames.hpp"

#include <algorithm>
#include <cstring>
#include <span>

namespace ASFW::UserClient::Wire {
namespace {
namespace E = ASFW::AVC::DiscoveryEngine;
namespace A = ASFW::AVC;
namespace G = Protocols::AVC::Graph;

/// Minimal JSON writer: commas are tracked per open container. Strings from
/// the device are escaped byte by byte, so the output is valid ASCII JSON.
class Json {
public:
    Json& Open(char bracket) { Comma(); out_ += bracket; first_.push_back(true); return *this; }
    Json& Close(char bracket) { out_ += bracket; first_.pop_back(); return *this; }
    Json& Key(std::string_view key) { Comma(); Quote(key); out_ += ':'; pendingValue_ = true; return *this; }
    Json& String(std::string_view text) { Comma(); Quote(text); return *this; }
    Json& Number(uint64_t value) { Comma(); out_ += std::to_string(value); return *this; }
    Json& Bool(bool value) { Comma(); out_ += value ? "true" : "false"; return *this; }
    Json& Null() { Comma(); out_ += "null"; return *this; }
    /// A number token written as given ("-1.0000"): for decimals the writer's integer Number cannot carry.
    Json& RawNumber(std::string_view token) { Comma(); out_ += token; return *this; }
    Json& Hex(std::span<const uint8_t> bytes) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const auto b : bytes) { text += kHex[b >> 4]; text += kHex[b & 0x0F]; }
        return String(text);
    }
    [[nodiscard]] std::string Take() { return std::move(out_); }

private:
    void Quote(std::string_view text) {
        out_ += '"';
        for (const char raw : text) {
            const auto c = static_cast<uint8_t>(raw);
            if (c == '"' || c == '\\') { out_ += '\\'; out_ += raw; }
            else if (c >= 0x20 && c < 0x7F) out_ += raw;
            else { // Control and non-ASCII bytes: Latin-1 code points.
                static constexpr char kHex[] = "0123456789abcdef";
                out_ += "\\u00"; out_ += kHex[c >> 4]; out_ += kHex[c & 0x0F];
            }
        }
        out_ += '"';
    }
    void Comma() {
        if (pendingValue_) { pendingValue_ = false; return; } // Value right after its key.
        if (first_.empty()) return;
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
    std::string out_;
    std::vector<bool> first_;
    bool pendingValue_{false};
};

const char* ErrorKindName(A::AvcErrorKind kind) {
    switch (kind) {
        case A::AvcErrorKind::kFrameTooShort: return "frameTooShort";
        case A::AvcErrorKind::kFrameTooLong: return "frameTooLong";
        case A::AvcErrorKind::kNotAResponse: return "notAResponse";
        case A::AvcErrorKind::kAddressMismatch: return "addressMismatch";
        case A::AvcErrorKind::kOpcodeMismatch: return "opcodeMismatch";
        case A::AvcErrorKind::kUnexpectedResponse: return "unexpectedResponse";
        case A::AvcErrorKind::kOperandsTooShort: return "operandsTooShort";
        case A::AvcErrorKind::kMalformedOperands: return "malformedOperands";
        case A::AvcErrorKind::kInvalidArgument: return "invalidArgument";
        case A::AvcErrorKind::kUnsupported: return "unsupported";
        case A::AvcErrorKind::kTimeout: return "timeout";
        case A::AvcErrorKind::kBusReset: return "busReset";
        case A::AvcErrorKind::kTransportError: return "transportError";
        case A::AvcErrorKind::kRefused: return "refused";
        case A::AvcErrorKind::kBusy: return "busy";
    }
    return "unknown"; // A value from a newer enum: still valid JSON.
}

const char* ParseErrorName(Protocols::AVC::Descriptors::ParseErrorKind kind) {
    using K = Protocols::AVC::Descriptors::ParseErrorKind;
    switch (kind) {
        case K::Truncated: return "truncated";
        case K::InvalidLength: return "invalidLength";
        case K::InvalidValue: return "invalidValue";
        case K::BudgetExceeded: return "budgetExceeded";
    }
    return "unknown";
}

const char* OutcomeName(Protocols::AVC::FcpExchangeOutcome outcome) {
    using O = Protocols::AVC::FcpExchangeOutcome;
    switch (outcome) {
        case O::kResponse: return "response";
        case O::kTimeout: return "timeout";
        case O::kBusReset: return "busReset";
        case O::kTransportError: return "transportError";
        case O::kResponseMismatch: return "responseMismatch";
        case O::kRefusedByFilter: return "refusedByFilter";
        case O::kBusy: return "busy";
        case O::kInvalid: return "invalid";
    }
    return "unknown";
}

void Error(Json& j, const std::optional<A::AvcError>& error) {
    if (!error) { j.Null(); return; }
    j.Open('{').Key("kind").String(ErrorKindName(error->kind)).Key("response");
    if (error->response) j.Number(static_cast<uint8_t>(*error->response)); else j.Null();
    j.Key("operandOffset").Number(error->operandOffset).Close('}');
}

std::string GuidText(uint64_t guid) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 60; shift >= 0; shift -= 4) text += kHex[(guid >> shift) & 0xF];
    return text;
}

void Subunit(Json& j, const A::SubunitId& id) {
    j.Open('{').Key("type").Number(static_cast<uint8_t>(id.type)).Key("id").Number(id.id).Key("typeName");
    if (const auto name = A::LookupName(A::names::kSubunitTypes, static_cast<uint32_t>(id.type))) j.String(*name); else j.Null();
    j.Close('}');
}

namespace D = Protocols::AVC::Descriptors;

/// The spec name of a code, or JSON null for one no table names (the reader shows the raw value).
template <size_t N>
void NameOrNull(Json& j, const std::array<A::NameEntry, N>& table, uint32_t value) {
    if (const auto name = A::LookupName(table, value)) j.String(*name); else j.Null();
}

void Endpoint(Json& j, const std::optional<D::MusicPlugEndpoint>& e) {
    if (!e) { j.Null(); return; }
    j.Open('{').Key("functionType").Number(e->functionType).Key("plugId").Number(e->plugId)
        .Key("functionBlockId").Number(e->functionBlockId).Key("streamPosition").Number(e->streamPosition)
        .Key("streamLocation").Number(e->streamLocation).Close('}');
}

void OptionalByte(Json& j, const std::optional<uint8_t>& value) {
    if (value) j.Number(*value); else j.Null();
}
/// An activity or capability byte as its name (bit 0 / bit 1 named by the caller), or null when absent.
void OptionalActivityName(Json& j, const std::optional<uint8_t>& value, const char* bit0, const char* bit1) {
    if (value) j.String(D::ActivityName(*value, bit0, bit1)); else j.Null();
}

/// The Audio subunits' identifier descriptors with their function block names resolved from the text database,
/// as the graph builder resolves them (the snapshot keeps the descriptor as read).
struct ResolvedAudio {
    A::SubunitId id;
    D::AudioSubunitIdentifier audio;
};
std::vector<ResolvedAudio> ResolveAudio(const E::DiscoverySnapshot& s) {
    std::vector<ResolvedAudio> out;
    for (const auto& c : s.contents) {
        if (!c.audio) continue;
        ResolvedAudio resolved{c.id, *c.audio};
        D::AudioSubunitDescriptorParser::ResolveNames(resolved.audio, c.text);
        out.push_back(std::move(resolved));
    }
    return out;
}
const D::AudioSubunitIdentifier* AudioOf(const std::vector<ResolvedAudio>& resolved, const A::SubunitId& id) {
    for (const auto& r : resolved) if (r.id == id) return &r.audio;
    return nullptr;
}
const std::string* AudioBlockName(const std::vector<ResolvedAudio>& resolved, const A::SubunitId& subunit, uint8_t type, uint8_t id) {
    const auto* audio = AudioOf(resolved, subunit);
    if (!audio) return nullptr;
    for (const auto& fb : audio->functionBlocks)
        if (static_cast<uint8_t>(fb.type) == type && fb.id == id) return &fb.name;
    return nullptr;
}

/// The Music status descriptor's facts (TA 2001007 §6.2) a reader shows: plugs with their clusters, the music
/// plugs and their routing, and per source plug the stream labels, MIDI streams and activity.
void MusicStatus(Json& j, const D::MusicSubunitStatus& m) {
    j.Open('{').Key("general");
    if (m.capabilities.hasGeneralCapability) {
        j.Open('{').Key("transmit").Number(m.capabilities.transmitCapabilityFlags)
            .Key("transmitName").String(D::TransferCapabilityName(m.capabilities.transmitCapabilityFlags))
            .Key("receive").Number(m.capabilities.receiveCapabilityFlags)
            .Key("receiveName").String(D::TransferCapabilityName(m.capabilities.receiveCapabilityFlags)).Key("latency");
        if (m.capabilities.latencyCapability) j.Number(*m.capabilities.latencyCapability); else j.Null();
        j.Close('}');
    } else {
        j.Null();
    }
    j.Key("declaredSourcePlugs"); OptionalByte(j, m.declaredSourcePlugs);
    j.Key("plugs").Open('[');
    for (const auto& plug : m.plugs) {
        j.Open('{').Key("id").Number(plug.plugId).Key("destination").Bool(plug.isDestination)
            .Key("usage").Number(plug.usage).Key("usageName"); NameOrNull(j, D::names::kMusicPlugUsages, plug.usage);
        j.Key("name").String(plug.name).Key("clusters").Open('[');
        for (const auto& c : plug.clusters) {
            j.Open('{').Key("name").String(c.name).Key("streamFormat").Number(c.streamFormatCode)
                .Key("portType").Number(c.portType).Key("portTypeName"); NameOrNull(j, D::names::kMusicPortTypes, c.portType);
            j.Key("channels").Number(c.channelCount).Key("signals").Open('[');
            for (const auto& sig : c.signals) j.Open('{').Key("musicPlugId").Number(sig.musicPlugId).Key("position").Number(sig.position).Close('}');
            j.Close(']').Close('}');
        }
        j.Close(']').Close('}');
    }
    j.Close(']').Key("musicPlugs").Open('[');
    for (const auto& mp : m.musicPlugs) {
        j.Open('{').Key("id").Number(mp.musicPlugId).Key("type").Number(mp.plugType).Key("typeName");
        NameOrNull(j, D::names::kMusicPlugTypes, mp.plugType);
        j.Key("routing").Number(mp.routingSupport).Key("routingName"); NameOrNull(j, D::names::kMusicRoutingSupports, mp.routingSupport);
        j.Key("name").String(mp.name).Key("source"); Endpoint(j, mp.source);
        j.Key("destination"); Endpoint(j, mp.destination);
        j.Close('}');
    }
    // Per source plug: everything the status descriptor says about the streams it carries.
    std::vector<uint8_t> sourcePlugs;
    for (const auto& [id, labels] : m.perPlugChannelNames) sourcePlugs.push_back(id);
    for (const auto& [id, midi] : m.perPlugMidiStreams) sourcePlugs.push_back(id);
    for (const auto& [id, activity] : m.perPlugActivity) sourcePlugs.push_back(id);
    std::ranges::sort(sourcePlugs);
    sourcePlugs.erase(std::ranges::unique(sourcePlugs).begin(), sourcePlugs.end());
    j.Close(']').Key("sourcePlugs").Open('[');
    for (const uint8_t id : sourcePlugs) {
        j.Open('{').Key("plug").Number(id).Key("audioLabels").Open('[');
        if (const auto it = m.perPlugChannelNames.find(id); it != m.perPlugChannelNames.end())
            for (const auto& label : it->second) j.String(label);
        j.Close(']').Key("midi");
        if (const auto it = m.perPlugMidiStreams.find(id); it != m.perPlugMidiStreams.end()) {
            j.Open('{').Key("declared").Number(it->second.declaredStreams).Key("labels").Open('[');
            for (const auto& label : it->second.labels) j.String(label);
            j.Close(']').Close('}');
        } else {
            j.Null();
        }
        j.Key("activity");
        if (const auto it = m.perPlugActivity.find(id); it != m.perPlugActivity.end()) {
            j.Open('{').Key("smpteTimeCode"); OptionalByte(j, it->second.smpteTimeCode);
            j.Key("smpteTimeCodeName"); OptionalActivityName(j, it->second.smpteTimeCode, "rx", "tx");
            j.Key("sampleCount"); OptionalByte(j, it->second.sampleCount);
            j.Key("sampleCountName"); OptionalActivityName(j, it->second.sampleCount, "rx", "tx");
            j.Key("audioSync"); OptionalByte(j, it->second.audioSync);
            j.Key("audioSyncName"); OptionalActivityName(j, it->second.audioSync, "bus", "external");
            j.Close('}');
        } else {
            j.Null();
        }
        j.Close('}');
    }
    j.Close(']').Close('}');
}

/// The Music identifier descriptor (TA 2001007 §5): the subunit's static capabilities.
void MusicIdentifier(Json& j, const D::MusicSubunitIdentifier& id) {
    j.Open('{').Key("generation").Number(id.generationId).Key("version").Number(id.version)
        .Key("versionText").String(D::VersionName(id.version))
        .Key("capabilityAttributes").Number(id.capabilityAttributes.empty() ? 0 : id.capabilityAttributes.front())
        .Key("capabilityNames").Open('[');
    for (const auto& name : D::CapabilityAttributeNames(id.capabilityAttributes)) j.String(name);
    j.Close(']').Key("general");
    if (id.general) {
        j.Open('{').Key("transmit").Number(id.general->transmit).Key("transmitName").String(D::TransferCapabilityName(id.general->transmit))
            .Key("receive").Number(id.general->receive).Key("receiveName").String(D::TransferCapabilityName(id.general->receive))
            .Key("latency").Number(id.general->latency).Close('}');
    } else {
        j.Null();
    }
    j.Key("audio");
    if (id.audio) {
        j.Open('[');
        for (const auto& f : *id.audio) {
            j.Open('{').Key("maxInputChannels").Number(f.maxInputChannels).Key("maxOutputChannels").Number(f.maxOutputChannels)
                .Key("fdf").Number(f.fdf).Key("fdfName"); NameOrNull(j, A::names::kCipSfcs, f.fdf);
            j.Key("am824Label").Number(f.am824Label).Key("am824LabelName");
            if (const auto label = D::Am824LabelName(f.am824Label)) j.String(*label); else j.Null();
            j.Close('}');
        }
        j.Close(']');
    } else {
        j.Null();
    }
    j.Key("midi");
    if (id.midi) {
        j.Open('{').Key("version").Number(id.midi->version).Key("revision").Number(id.midi->revision)
            .Key("versionText").String(std::to_string(id.midi->version) + "." + std::to_string(id.midi->revision))
            .Key("adaptationLayer").Number(id.midi->adaptationLayerVersion).Key("maxInputPorts").Number(id.midi->maxInputPorts)
            .Key("maxOutputPorts").Number(id.midi->maxOutputPorts).Close('}');
    } else {
        j.Null();
    }
    j.Key("smpteTimeCode"); OptionalByte(j, id.smpteTimeCode);
    j.Key("smpteTimeCodeName"); OptionalActivityName(j, id.smpteTimeCode, "rx", "tx");
    j.Key("sampleCount"); OptionalByte(j, id.sampleCount);
    j.Key("sampleCountName"); OptionalActivityName(j, id.sampleCount, "rx", "tx");
    j.Key("audioSync"); OptionalByte(j, id.audioSync);
    j.Key("audioSyncName"); OptionalActivityName(j, id.audioSync, "bus", "external");
    j.Close('}');
}

/// The spec names of the controls a Controls bitmap announces, bit order from the MSB (Audio Tables 8.3-8.22).
void ControlNames(Json& j, D::AudioFunctionBlockType type, uint8_t subType, std::span<const uint8_t> bitmap) {
    j.Open('[');
    const auto defined = D::ControlBitsOf(type, subType);
    for (size_t bit = 0; bit < bitmap.size() * 8; ++bit) {
        if (!(bitmap[bit / 8] & (0x80u >> (bit % 8)))) continue;
        const D::ControlBit* known = nullptr;
        for (const auto& entry : defined) if (entry.bit == bit) known = &entry;
        if (!known) { j.String("UNKNOWN(control_bit:" + std::to_string(bit) + ")"); continue; }
        if (known->control.selector == 0) { j.String(known->name); continue; }
        const auto* spec = A::Cmd::FindControl(known->control);
        j.String(spec ? spec->name : std::string_view{"UNKNOWN"});
    }
    j.Close(']');
}

/// The Audio subunit's identifier descriptor: its function blocks with their names (from the text database),
/// what feeds each, and what each can do.
void AudioContents(Json& j, const D::AudioSubunitIdentifier& a) {
    const auto nameOf = [&a](uint8_t type, uint8_t id) -> const std::string* {
        for (const auto& fb : a.functionBlocks)
            if (static_cast<uint8_t>(fb.type) == type && fb.id == id) return &fb.name;
        return nullptr;
    };
    const auto source = [&](const D::AudioSourceId& src) {
        j.Open('{').Key("type").Number(src.type).Key("id").Number(src.id).Key("kind");
        if (src.IsSubunitDestPlug()) j.String("subunitDestinationPlug");
        else if (!src.IsConnected()) j.String("notConnected");
        else if (src.IsFunctionBlock()) j.String("functionBlock");
        else j.String("other");
        j.Key("name");
        const auto* name = src.IsFunctionBlock() ? nameOf(src.type, src.id) : nullptr;
        if (name && !name->empty()) j.String(*name); else j.Null();
        j.Close('}');
    };
    j.Open('{').Key("sourcePlugLinks").Open('[');
    for (const auto& link : a.sourcePlugLinks) source(link);
    j.Close(']').Key("functionBlocks").Open('[');
    for (const auto& fb : a.functionBlocks) {
        const auto raw = static_cast<uint8_t>(fb.type);
        j.Open('{').Key("type").Number(raw).Key("typeName"); NameOrNull(j, A::Cmd::names::kFunctionBlockTypes, raw);
        j.Key("id").Number(fb.id).Key("name").String(fb.name).Key("inputs").Open('[');
        for (const auto& input : fb.inputSources) source(input);
        j.Close(']').Key("clusterChannels").Number(fb.clusterChannels);
        if (fb.type == D::AudioFunctionBlockType::kFeature) {
            const auto master = static_cast<uint16_t>(fb.masterControls);
            const std::array<uint8_t, 2> bits = {static_cast<uint8_t>(master >> 8), static_cast<uint8_t>(master & 0xFF)};
            j.Key("generalTag").Number(fb.generalTag).Key("controls").Open('{').Key("master").Number(fb.masterControls)
                .Key("channels").Open('[');
            for (const auto c : fb.channelControls) j.Number(c);
            j.Close(']').Key("masterNames"); ControlNames(j, fb.type, 0, bits);
            j.Close('}');
        } else if (fb.typeInfo) {
            const auto& info = *fb.typeInfo;
            j.Key("subType").Number(info.subType).Key("subTypeName");
            if (fb.type == D::AudioFunctionBlockType::kCodec) NameOrNull(j, A::Cmd::names::kCodecTypes, info.subType);
            else NameOrNull(j, A::Cmd::names::kProcessingTypes, info.subType);
            j.Key("controlNames");
            if (info.subType == static_cast<uint8_t>(A::Cmd::ProcessingType::kMixer) && fb.type == D::AudioFunctionBlockType::kProcessing) {
                size_t programmable = 0;
                for (size_t bit = 0; bit < info.controls.size() * 8; ++bit) programmable += info.ControlsBit(bit) ? 1 : 0;
                j.Open('[').Close(']').Key("programmableMixerControls").Number(programmable);
            } else {
                ControlNames(j, fb.type, info.subType, info.controls);
            }
            j.Key("modes").Open('[');
            for (const auto mode : info.modes) j.Number(mode);
            j.Close(']');
        } else if (fb.typeInfoError) {
            j.Key("typeInfoError").String(ParseErrorName(fb.typeInfoError->kind));
        }
        j.Close('}');
    }
    j.Close(']').Close('}');
}

/// What discovery read from each subunit's descriptors, parsed (the raw bytes are in `descriptors`).
void Contents(Json& j, const E::DiscoverySnapshot& s, const std::vector<ResolvedAudio>& resolved) {
    j.Key("contents").Open('[');
    for (const auto& c : s.contents) {
        j.Open('{').Key("subunit"); Subunit(j, c.id);
        j.Key("music");
        if (c.music) MusicStatus(j, *c.music); else j.Null();
        j.Key("musicIdentifier");
        if (c.musicIdentifier) MusicIdentifier(j, *c.musicIdentifier); else j.Null();
        j.Key("audio");
        if (const auto* audio = AudioOf(resolved, c.id)) AudioContents(j, *audio); else j.Null();
        j.Close('}');
    }
    j.Close(']');
}

/// A stream format as the driver decodes it: the rate, the entries of a compound AM824 format by name, and
/// the whole thing as text; any other format keeps its text only (the bytes are in the raw field).
void DecodedFormat(Json& j, const A::Cmd::StreamFormat& format) {
    j.Open('{').Key("text").String(A::Cmd::Describe(format));
    if (format.kind == A::Cmd::StreamFormat::Kind::kCompoundAm824) {
        const auto& c = format.compound;
        j.Key("kind").String("compoundAm824").Key("rate");
        if (const auto name = A::LookupName(A::names::kStreamFormatRates, static_cast<uint32_t>(c.rate))) j.String(*name); else j.Null();
        j.Key("rateHz");
        if (const auto hz = A::ToHz(c.rate)) j.Number(*hz); else j.Null();
        j.Key("syncSource").Bool(c.syncSource).Key("entries").Open('[');
        for (const auto& entry : c.Entries()) {
            j.Open('{').Key("count").Number(entry.count).Key("format");
            if (const auto name = A::LookupName(A::Cmd::names::kAm824Formats, static_cast<uint32_t>(entry.format))) j.String(*name); else j.Null();
            j.Key("code").Number(static_cast<uint8_t>(entry.format)).Close('}');
        }
        j.Close(']');
    } else {
        j.Key("kind").String("other");
    }
    j.Close('}');
}

/// A feature control's reply as the driver reads it: the control's spec name, and its value by what it means
/// ("on" for a boolean, dB for a volume), with the spec's text for it.
void DecodedFeature(Json& j, const E::FeatureStatus& f) {
    const auto selector = static_cast<uint8_t>(f.control);
    j.Key("controlName");
    if (const auto* spec = A::Cmd::FindControl(A::Cmd::FunctionBlockType::kFeature, A::Cmd::kAnySubType, selector)) j.String(spec->name); else j.Null();
    j.Key("decoded");
    const auto* spec = A::Cmd::FindControl(A::Cmd::FunctionBlockType::kFeature, A::Cmd::kAnySubType, selector);
    if (!f.value || !spec) { j.Null(); return; }
    A::Cmd::FunctionBlockControlReply reply;
    reply.controlData.assign(f.value->data.begin(), f.value->data.begin() + f.value->dataLength);
    const auto value = reply.Value(spec->kind);
    if (!value) { j.Null(); return; }
    j.Open('{').Key("text").String(A::Cmd::Describe(*value));
    if (const auto on = value->AsBoolean()) {
        j.Key("kind").String("boolean").Key("on").Bool(*on);
    } else if (spec->kind == A::Cmd::AudioValueKind::kVolume) {
        j.Key("kind").String("volume");
        const auto volume = value->AsVolume();
        j.Key("valid").Bool(volume.has_value()).Key("negativeInfinity").Bool(volume && volume->IsNegativeInfinity());
        if (volume && !volume->IsNegativeInfinity()) {
            // The spec's own decimal text, "-1.0000 dB(0xff00)" without the unit and the raw value.
            const auto text = A::Cmd::Describe(*value);
            j.Key("db").RawNumber(text.substr(0, text.find(' ')));
        }
    } else {
        j.Key("kind").String("other");
    }
    j.Close('}');
}

void Snapshot(Json& j, const E::DiscoverySnapshot& s) {
    const auto resolved = ResolveAudio(s);
    j.Key("complete").Bool(s.complete).Key("cancelled").Bool(s.cancelled).Key("terminalError");
    Error(j, s.terminalError);

    const auto& unit = s.unit;
    j.Key("unit").Open('{')
        .Key("unitType").Number(static_cast<uint8_t>(unit.info.unitType))
        .Key("unitId").Number(unit.info.unitId)
        .Key("companyId").Number(A::ToOui(unit.info.companyId))
        .Key("isoInputs").Number(unit.unitPlugs.isochronousInputs)
        .Key("isoOutputs").Number(unit.unitPlugs.isochronousOutputs)
        .Key("externalInputs").Number(unit.unitPlugs.externalInputs)
        .Key("externalOutputs").Number(unit.unitPlugs.externalOutputs)
        .Key("subunits").Open('[');
    for (const auto& sub : unit.subunits) {
        j.Open('{').Key("type").Number(static_cast<uint8_t>(sub.id.type)).Key("id").Number(sub.id.id).Key("typeName");
        if (const auto name = A::LookupName(A::names::kSubunitTypes, static_cast<uint32_t>(sub.id.type))) j.String(*name); else j.Null();
        j.Key("destinationPlugs").Number(sub.plugs.destinationPlugs)
            .Key("sourcePlugs").Number(sub.plugs.sourcePlugs)
            .Key("plugsDiscovered").Bool(sub.plugsDiscovered).Close('}');
    }
    j.Close(']').Close('}');

    j.Key("plugs").Open('[');
    for (const auto& p : s.plugs) {
        j.Open('{').Key("address").Number(p.address.Byte()).Key("subunit");
        if (p.address.IsUnit()) j.Null(); else Subunit(j, A::SubunitId{p.address.Type(), p.address.Id()});
        j.Key("direction").String(p.direction == A::Cmd::PlugDirection::kInput ? "input" : "output")
            .Key("id").Number(p.id.value).Key("signalFormat");
        if (p.signalFormat) {
            const std::array<uint8_t, 4> raw{p.signalFormat->fmt, p.signalFormat->fdf[0],
                                             p.signalFormat->fdf[1], p.signalFormat->fdf[2]};
            j.Hex(raw);
        } else j.Null();
        j.Key("current");
        if (p.current) j.Hex(p.current->Raw()); else j.Null();
        j.Key("formations").Open('[');
        for (const auto& f : p.formations) j.Hex(f.Raw());
        j.Close(']').Key("currentDecoded");
        if (p.current) DecodedFormat(j, *p.current); else j.Null();
        j.Key("formationsDecoded").Open('[');
        for (const auto& f : p.formations) DecodedFormat(j, f);
        j.Close(']').Key("route");
        if (p.route) {
            // The bytes as the device sent them, then the same answer by spec name (TA 2002010 Tables 7.6-7.10):
            // a reader of this document sees `ready`, not 0x31.
            const auto status = p.route->Status();
            const auto kind = A::Cmd::DestinationKindOf(p.route->destination);
            j.Open('{').Key("source").Hex(p.route->source.bytes)
                .Key("destination").Hex(p.route->destination.bytes)
                .Key("firstOperand").Number(p.route->first.Raw())
                .Key("sourceName").String(A::Cmd::Describe(p.route->source, A::Cmd::SignalRole::kSource))
                .Key("destinationName").String(A::Cmd::Describe(p.route->destination, A::Cmd::SignalRole::kDestination))
                .Key("status").String(A::Cmd::Describe(status))
                .Key("outputStatusName");
            if (const auto output = status.Status()) {
                if (const auto name = A::LookupName(A::Cmd::names::kOutputStatuses, static_cast<uint32_t>(*output))) j.String(*name); else j.Null();
            } else {
                j.Null();
            }
            j.Key("deviations").String(kind ? A::Cmd::Describe(A::Cmd::CheckStatusAgainstSpec(status, *kind))
                                               : std::string("n/a"))
                .Close('}');
        } else j.Null();
        j.Close('}');
    }
    j.Close(']');

    j.Key("descriptors").Open('[');
    for (const auto& d : s.descriptors) {
        j.Open('{').Key("subunit"); Subunit(j, d.subunit);
        j.Key("specifier").Hex(std::span<const uint8_t>(d.specifier.bytes.data(), d.specifier.length))
            .Key("specifierText").String(A::Cmd::Describe(d.specifier)).Key("bytes").Number(d.bytes.size()).Key("data").Hex(d.bytes)
            .Key("primaryError"); Error(j, d.primaryError);
        j.Key("cleanupError"); Error(j, d.cleanupError);
        j.Key("parseError");
        if (d.parseError) j.Open('{').Key("offset").Number(d.parseError->offset)
                              .Key("kind").String(ParseErrorName(d.parseError->kind)).Close('}');
        else j.Null();
        j.Close('}');
    }
    j.Close(']');

    j.Key("features").Open('[');
    for (const auto& f : s.features) {
        j.Open('{').Key("subunit"); Subunit(j, f.subunit);
        j.Key("block").Number(f.blockId).Key("channel").Number(f.channel)
            .Key("control").Number(static_cast<uint8_t>(f.control)).Key("value");
        if (f.value) j.Hex(std::span<const uint8_t>(f.value->data.data(), f.value->dataLength)); else j.Null();
        j.Key("error"); Error(j, f.error);
        j.Key("blockName");
        if (const auto* name = AudioBlockName(resolved, f.subunit, static_cast<uint8_t>(A::Cmd::FunctionBlockType::kFeature), f.blockId); name && !name->empty()) j.String(*name); else j.Null();
        DecodedFeature(j, f);
        j.Close('}');
    }
    j.Close(']');

    j.Key("selectors").Open('[');
    for (const auto& sel : s.selectors) {
        j.Open('{').Key("subunit"); Subunit(j, sel.subunit);
        j.Key("block").Number(sel.blockId).Key("current");
        if (sel.value) j.Number(sel.value->inputPlug); else j.Null();
        // The block's name, what each of its inputs is, and what the current one is.
        const auto* audio = AudioOf(resolved, sel.subunit);
        const D::AudioFunctionBlockInfo* block = audio ? audio->FindBlock(D::AudioFunctionBlockType::kSelector, sel.blockId) : nullptr;
        j.Key("name");
        if (block && !block->name.empty()) j.String(block->name); else j.Null();
        j.Key("inputs").Open('[');
        if (block) {
            for (const auto& input : block->inputSources) {
                j.Open('{').Key("typeName");
                if (const auto name = A::LookupName(A::Cmd::names::kFunctionBlockTypes, input.type)) j.String(*name);
                else if (input.IsSubunitDestPlug()) j.String("subunit destination plug");
                else if (!input.IsConnected()) j.String("not connected");
                else j.Null();
                j.Key("id").Number(input.id).Key("name");
                const auto* name = input.IsFunctionBlock() ? AudioBlockName(resolved, sel.subunit, input.type, input.id) : nullptr;
                if (name && !name->empty()) j.String(*name); else j.Null();
                j.Close('}');
            }
        }
        j.Close(']').Close('}');
    }
    j.Close(']');

    j.Key("confirmedClockRoutes").Open('[');
    for (const auto& r : s.confirmedClockRoutes)
        j.Open('{').Key("source").Hex(r.source.bytes).Key("destination").Hex(r.destination.bytes).Close('}');
    j.Close(']');

    j.Key("failedProbes").Open('[');
    for (const auto& o : s.outcomes) {
        if (!o.error) continue;
        j.Open('{').Key("address").Number(o.address.Byte()).Key("addressText").String(A::Describe(o.address))
            .Key("opcode").Number(static_cast<uint8_t>(o.opcode))
            .Key("opcodeName").String(A::Describe(o.address.Type(), o.opcode)).Key("error"); Error(j, o.error);
        j.Close('}');
    }
    j.Close(']').Key("probeCount").Number(s.outcomes.size());

    j.Key("textReferences").Open('[');
    for (const auto& t : s.textReferences) {
        static constexpr const char* kKinds[] = {"duplicate", "cycle", "budgetExceeded"};
        const auto kind = static_cast<size_t>(t.kind);
        j.Open('{').Key("subunit"); Subunit(j, t.subunit);
        j.Key("target").Number(t.target).Key("kind").String(kind < 3 ? kKinds[kind] : "unknown").Close('}');
    }
    j.Close(']');

    Contents(j, s, resolved);

    const auto extension = [&j](const E::ExtensionPlug& plug) {
        j.Open('{').Key("currentRate").Number(plug.currentRateHz).Key("formations").Open('[');
        for (const auto& f : plug.formations)
            j.Open('{').Key("rate").Number(f.rateHz).Key("pcm").Number(f.pcmChannels)
                .Key("midi").Number(f.midiChannels).Close('}');
        j.Close(']').Key("slotMap").Open('[');
        for (uint32_t i = 0; i < plug.pcmSlots.slotCount; ++i) j.Number(plug.pcmSlots.slotForChannel[i]);
        j.Close(']').Close('}');
    };
    j.Key("extension").Open('{').Key("playback"); extension(s.extension.playback);
    j.Key("capture"); extension(s.extension.capture);
    j.Close('}');
}

void Stream(Json& j, const G::StreamGraph& s) {
    j.Open('{').Key("subunitPlug").Number(s.subunitPlugId)
        .Key("selection").Number(static_cast<uint8_t>(s.selectionEvidence))
        .Key("channels").Number(s.channelCount).Key("dataBlockSize").Number(s.dataBlockSize)
        .Key("midi").Number(s.midiStreamCount).Key("rate").Number(s.currentSampleRate)
        .Key("rates").Open('[');
    for (const auto rate : s.supportedSampleRates) j.Number(rate);
    j.Close(']').Key("slotMap").Open('[');
    for (uint32_t i = 0; i < s.slotMap.slotCount; ++i) j.Number(s.slotMap.slotForChannel[i]);
    j.Close(']').Key("slotMapValidation").Number(static_cast<uint8_t>(s.slotMapValidation))
        .Key("usingFallbackMap").Bool(s.usingFallbackMap).Key("routeAmbiguous").Bool(s.routeAmbiguous)
        .Key("clusters").Number(s.clusters.size()).Key("channelNames").Open('[');
    for (const auto& name : s.channelNames) j.String(name);
    j.Close(']').Close('}');
}

void Graph(Json& j, const G::DeviceGraph& g) {
    j.Open('{').Key("playback"); Stream(j, g.playback);
    j.Key("capture"); Stream(j, g.capture);
    j.Key("clockSources").Open('[');
    for (const auto& c : g.clockSources)
        j.Open('{').Key("name").String(c.name).Key("current").Bool(c.isCurrent).Close('}');
    j.Close(']').Key("selectors").Open('[');
    for (const auto& sel : g.selectors) {
        j.Open('{').Key("block").Number(sel.functionBlockId).Key("name").String(sel.name).Key("current");
        if (sel.currentInput) j.Number(*sel.currentInput); else j.Null();
        j.Close('}');
    }
    j.Close(']').Key("routes").Number(g.routes.size()).Close('}');
}

void Exchanges(Json& j, const Protocols::AVC::FcpExchangeLog& log) {
    j.Open('{').Key("session").Number(log.session).Key("dropped").Number(log.dropped).Key("records").Open('[');
    for (const auto& r : log.records) {
        j.Open('{').Key("sequence").Number(r.sequence).Key("generation").Number(r.generation)
            .Key("outcome").String(OutcomeName(r.outcome)).Key("interim").Bool(r.interim)
            .Key("retries").Number(r.retries).Key("elapsedUs").Number(r.elapsedNs / 1000)
            .Key("command").Hex(r.command).Key("response").Hex(r.response).Close('}');
    }
    j.Close(']').Close('}');
}
} // namespace

std::string BuildAVCDiscoveryDocument(const E::DiscoverySnapshot* snapshot, const G::DeviceGraph* graph,
                                      const Protocols::AVC::FcpExchangeLog& exchanges) {
    Json j;
    j.Open('{').Key("format").String("asfw.avc.discovery").Key("version").Number(kAVCDiscoveryDocumentVersion);
    if (snapshot) {
        j.Key("session").Number(snapshot->session.value).Key("route").Open('{')
            .Key("guid").String(GuidText(snapshot->route.guid)) // Hex text: JSON numbers are doubles..Key("generation").Number(snapshot->route.generation.value)
            .Key("node").Number(snapshot->route.nodeId).Close('}');
        j.Key("snapshot").Open('{');
        Snapshot(j, *snapshot);
        j.Close('}');
    } else {
        j.Key("session").Number(0).Key("route").Null().Key("snapshot").Null();
    }
    j.Key("graph");
    if (graph) Graph(j, *graph); else j.Null();
    j.Key("exchanges");
    Exchanges(j, exchanges);
    j.Close('}');
    return j.Take();
}

std::vector<uint8_t> SerializeDiscoveryPage(std::string_view document, uint32_t session, uint32_t generation,
                                            uint32_t offset, size_t maxBytes) {
    AVCDiscoveryPageWire header{};
    header.session = session;
    header.generation = generation;
    header.totalBytes = static_cast<uint32_t>(document.size());
    header.offset = offset;
    header.checksum = Fnv1a32(document);
    const size_t room = maxBytes > sizeof(header) ? maxBytes - sizeof(header) : 0;
    const size_t start = std::min<size_t>(offset, document.size());
    header.length = static_cast<uint32_t>(std::min(room, document.size() - start));
    std::vector<uint8_t> out(sizeof(header) + header.length);
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), document.data() + start, header.length);
    return out;
}

} // namespace ASFW::UserClient::Wire
