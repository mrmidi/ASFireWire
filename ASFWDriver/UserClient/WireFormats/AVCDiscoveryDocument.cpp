// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCDiscoveryDocument.cpp - see AVCDiscoveryDocument.hpp.

#include "AVCDiscoveryDocument.hpp"

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
    j.Open('{').Key("type").Number(static_cast<uint8_t>(id.type)).Key("id").Number(id.id).Close('}');
}

void Snapshot(Json& j, const E::DiscoverySnapshot& s) {
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
        j.Open('{').Key("type").Number(static_cast<uint8_t>(sub.id.type)).Key("id").Number(sub.id.id)
            .Key("destinationPlugs").Number(sub.plugs.destinationPlugs)
            .Key("sourcePlugs").Number(sub.plugs.sourcePlugs)
            .Key("plugsDiscovered").Bool(sub.plugsDiscovered).Close('}');
    }
    j.Close(']').Close('}');

    j.Key("plugs").Open('[');
    for (const auto& p : s.plugs) {
        j.Open('{').Key("address").Number(p.address.Byte())
            .Key("direction").String(p.direction == A::Cmd::PlugDirection::kInput ? "input" : "output")
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
        j.Close(']').Key("route");
        if (p.route) j.Open('{').Key("source").Hex(p.route->source.bytes)
                         .Key("destination").Hex(p.route->destination.bytes).Close('}');
        else j.Null();
        j.Close('}');
    }
    j.Close(']');

    j.Key("descriptors").Open('[');
    for (const auto& d : s.descriptors) {
        j.Open('{').Key("subunit"); Subunit(j, d.subunit);
        j.Key("specifier").Hex(std::span<const uint8_t>(d.specifier.bytes.data(), d.specifier.length))
            .Key("bytes").Number(d.bytes.size()).Key("data").Hex(d.bytes)
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
        j.Close('}');
    }
    j.Close(']');

    j.Key("selectors").Open('[');
    for (const auto& sel : s.selectors) {
        j.Open('{').Key("subunit"); Subunit(j, sel.subunit);
        j.Key("block").Number(sel.blockId).Key("current");
        if (sel.value) j.Number(sel.value->inputPlug); else j.Null();
        j.Close('}');
    }
    j.Close(']');

    j.Key("confirmedClockRoutes").Open('[');
    for (const auto& r : s.confirmedClockRoutes)
        j.Open('{').Key("source").Hex(r.source.bytes).Key("destination").Hex(r.destination.bytes).Close('}');
    j.Close(']');

    j.Key("failedProbes").Open('[');
    for (const auto& o : s.outcomes) {
        if (!o.error) continue;
        j.Open('{').Key("address").Number(o.address.Byte())
            .Key("opcode").Number(static_cast<uint8_t>(o.opcode)).Key("error"); Error(j, o.error);
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
