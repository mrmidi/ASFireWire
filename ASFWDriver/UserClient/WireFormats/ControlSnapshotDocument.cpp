// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ControlSnapshotDocument.cpp - see ControlSnapshotDocument.hpp.

#include "ControlSnapshotDocument.hpp"
#include "JsonWriter.hpp"
#include "../../Audio/Controls/CoreAudioProjection.hpp"
#include <algorithm>

#include <cmath>
#include <cstdio>
#include <type_traits>
#include <variant>

namespace ASFW::UserClient::Wire {
namespace {
namespace AM = ASFW::AudioModel;

const char* SemanticName(AM::ParameterSemantic semantic) {
    switch (semantic) {
        case AM::ParameterSemantic::Level: return "level";
        case AM::ParameterSemantic::Mute: return "mute";
        case AM::ParameterSemantic::PhantomPower: return "phantomPower";
        case AM::ParameterSemantic::PhaseInvert: return "phaseInvert";
        case AM::ParameterSemantic::Pan: return "pan";
        case AM::ParameterSemantic::Balance: return "balance";
        case AM::ParameterSemantic::Solo: return "solo";
        case AM::ParameterSemantic::NominalLevel: return "nominalLevel";
        case AM::ParameterSemantic::ClockSource: return "clockSource";
        case AM::ParameterSemantic::Dim: return "dim";
        case AM::ParameterSemantic::Source: return "source";
        case AM::ParameterSemantic::HighPassFilter: return "highPassFilter";
        case AM::ParameterSemantic::Unknown: break;
    }
    return "unknown";
}

const char* UnitName(AM::ScalarUnit unit) {
    switch (unit) {
        case AM::ScalarUnit::Decibels: return "dB";
        case AM::ScalarUnit::Hertz: return "Hz";
        case AM::ScalarUnit::Percent: return "%";
        case AM::ScalarUnit::Normalized: return "normalized";
        case AM::ScalarUnit::Generic: break;
    }
    return "generic";
}

const char* OriginName(AM::StateOrigin origin) {
    switch (origin) {
        case AM::StateOrigin::Observed: return "observed";
        case AM::StateOrigin::Cached: return "cached";
        case AM::StateOrigin::Derived: return "derived";
    }
    return "observed";
}

/// Enough digits to read back the same double. A rounded step would move the
/// app's grid off the one SetControlParameter checks: an AV/C volume step is
/// 1/256 dB, which "%.4f" turned into 0.0039.
void Decimal(Json& j, double value) {
    if (!std::isfinite(value)) { j.Null(); return; }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    j.RawNumber(buffer);
}

void PortList(Json& j, const std::vector<AM::PortId>& ports) {
    j.Open('[');
    for (const auto port : ports) j.Number(port.value);
    j.Close(']');
}

void Domain(Json& j, const AM::ParameterDomain& value) {
    j.Open('{');
        std::visit([&](const auto& domain) {
            using D = std::decay_t<decltype(domain)>;
            if constexpr (std::is_same_v<D, AM::BooleanDomain>) {
                j.Key("kind").String("bool");
            } else if constexpr (std::is_same_v<D, AM::ScalarDomain>) {
                j.Key("kind").String("scalar");
                j.Key("min"); Decimal(j, domain.min);
                j.Key("max"); Decimal(j, domain.max);
                if (domain.step) { j.Key("step"); Decimal(j, *domain.step); }
                j.Key("unit").String(UnitName(domain.unit));
            } else if constexpr (std::is_same_v<D, AM::EnumDomain>) {
                j.Key("kind").String("enum");
                j.Key("values").Open('[');
                for (const auto& item : domain.values) {
                    j.Open('{');
                    j.Key("value").RawNumber(std::to_string(item.value));
                    j.Key("name").String(item.name);
                    j.Close('}');
                }
                j.Close(']');
            } else {
                j.Key("kind").String("action");
            }
    }, value);
        j.Close('}');
}

} // namespace

std::string BuildControlSnapshotDocument(uint64_t guid, std::string_view family,
                                         const AM::Topology& topology, const AM::DeviceState& state, uint64_t sequence) {
    Json j;
    char guidText[24];
    std::snprintf(guidText, sizeof(guidText), "0x%016llx", static_cast<unsigned long long>(guid));
    j.Open('{');
    j.Key("schema").String(kControlSnapshotSchema);
    j.Key("guid").String(guidText);
    j.Key("family").String(family);
    j.Key("revision").Number(topology.revision);
    j.Key("sequence").Number(sequence);

    j.Key("nodes").Open('[');
    for (const auto& node : topology.nodes) {
        j.Open('{');
        j.Key("id").Number(node.id.value);
        j.Key("name").String(node.name);
        std::visit([&](const auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, AM::EndpointNode>) {
                j.Key("kind").String("endpoint");
                j.Key("host").Bool(body.kind == AM::EndpointKind::Host);
            } else {
                if constexpr (std::is_same_v<T, AM::RouterNode>) j.Key("kind").String("router");
                else if constexpr (std::is_same_v<T, AM::MixerNode>) j.Key("kind").String("mixer");
                else j.Key("kind").String("processor");
                j.Key("inputs"); PortList(j, body.inputs);
                j.Key("outputs"); PortList(j, body.outputs);
                if constexpr (std::is_same_v<T, AM::RouterNode>) {
                    j.Key("writable").Bool(body.writable);
                    j.Key("bundles").Open('[');
                    for (const auto& bundle : body.legalBundles) {
                        j.Open('{');
                        j.Key("id").Number(bundle.id.value);
                        j.Key("routes").Open('[');
                        for (const auto& route : bundle.routes)
                            j.Open('[').Number(route.input.value).Number(route.output.value).Close(']');
                        j.Close(']');
                        j.Close('}');
                    }
                    j.Close(']');
                    j.Key("activeBundles").Open('[');
                    if (const auto found = state.routers.find(node.id); found != state.routers.end())
                        for (const auto bundle : found->second.activeBundles) j.Number(bundle.value);
                    j.Close(']');
                }
                if constexpr (std::is_same_v<T, AM::MixerNode>) {
                    j.Key("crosspoints").Open('[');
                    for (const auto& crosspoint : body.crosspoints) {
                        j.Open('{');
                        j.Key("id").Number(crosspoint.id.value);
                        j.Key("in").Number(crosspoint.input.value);
                        j.Key("out").Number(crosspoint.output.value);
                        j.Close('}');
                    }
                    j.Close(']');
                }
            }
        }, node.body);
        j.Close('}');
    }
    j.Close(']');

    j.Key("ports").Open('[');
    for (const auto& port : topology.ports) {
        j.Open('{');
        j.Key("id").Number(port.id.value);
        j.Key("owner").Number(port.owner.value);
        j.Key("direction").String(port.direction == AM::PortDirection::Input ? "in" : "out");
        j.Key("channels").Number(port.channels);
        j.Key("name").String(port.name);
        j.Key("signalKind").Number(static_cast<uint8_t>(port.signal.kind));
        j.Key("signalIndex").Number(port.signal.index);
        j.Close('}');
    }
    j.Close(']');

    j.Key("fixedLinks").Open('[');
    for (const auto& link : topology.fixedLinks)
        j.Open('[').Number(link.source.value).Number(link.destination.value).Close(']');
    j.Close(']');

    const auto inputTypes = ASFW::Audio::Controls::ProjectInputTypes(topology, state);
    j.Key("parameters").Open('[');
    for (const auto& parameter : topology.parameters) {
        // App projection uses the same composite choices as CoreAudio. The
        // real connector/level parameters remain in the provider's model.
        if (std::ranges::any_of(inputTypes, [&](const auto& type) {
            return std::ranges::any_of(type.choices, [&](const auto& choice) { return choice.levelParameter == parameter.id; });
        })) continue;
        j.Open('{');
        j.Key("id").Number(parameter.id.value);
        j.Key("name").String(parameter.name);
        j.Key("semantic").String(SemanticName(parameter.semantic));
        j.Key("target").Open('{');
        std::visit([&](const auto& target) {
            using T = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<T, AM::NodeId>) j.Key("node").Number(target.value);
            else if constexpr (std::is_same_v<T, AM::PortId>) j.Key("port").Number(target.value);
            else j.Key("crosspoint").Number(target.value);
        }, parameter.target);
        j.Close('}');
        j.Key("access").String(parameter.access == AM::ParameterAccess::ReadOnly ? "readOnly" : "readWrite");
        j.Key("domain"); Domain(j, parameter.domain);
        const auto stateEntry = state.parameters.find(parameter.id);
        j.Key("applicable").Bool(stateEntry == state.parameters.end() || stateEntry->second.applicable);
        if (stateEntry != state.parameters.end() && stateEntry->second.domainOverride) {
            j.Key("domainOverride"); Domain(j, *stateEntry->second.domainOverride);
        }
        if (const auto found = state.parameters.find(parameter.id); found != state.parameters.end() && !std::holds_alternative<AM::ActionDomain>(parameter.domain)) {
            j.Key("value");
            std::visit([&](const auto& value) {
                using V = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<V, bool>) j.Bool(value);
                else if constexpr (std::is_same_v<V, double>) Decimal(j, value);
                else if constexpr (std::is_same_v<V, int64_t>) j.RawNumber(std::to_string(value));
                else j.Null();
            }, found->second.value);
            j.Key("origin").String(OriginName(found->second.origin));
        } else {
            j.Key("value").Null();
            j.Key("origin").Null();
        }
        j.Close('}');
    }
    const auto projected = ASFW::Audio::Controls::ProjectCoreAudioControls(topology, state);
    for (const auto& type : inputTypes) {
        const auto id = ASFW::Audio::Controls::InputTypeParameterId(type.router);
        const auto descriptor = std::ranges::find(projected, id, &ASFW::Audio::Controls::CoreAudioControl::parameter);
        j.Open('{');
        j.Key("id").Number(id.value); j.Key("name").String("Input Type");
        j.Key("semantic").String("nominalLevel");
        j.Key("target").Open('{').Key("node").Number(type.router.value).Close('}');
        j.Key("domain").Open('{').Key("kind").String("enum").Key("values").Open('[');
        for (const auto& choice : type.choices)
            j.Open('{').Key("value").RawNumber(std::to_string(choice.value)).Key("name").String(choice.name).Close('}');
        j.Close(']').Close('}');
        j.Key("access").String(descriptor != projected.end() && descriptor->settable ? "readWrite" : "readOnly");
        j.Key("applicable").Bool(true);
        j.Key("value"); if (type.selected) j.RawNumber(std::to_string(*type.selected)); else j.Null();
        j.Key("origin").String("derived"); j.Close('}');
    }
    j.Close(']');
    j.Close('}');
    return j.Take();
}

} // namespace ASFW::UserClient::Wire
