// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ControlSnapshotDocument.cpp - see ControlSnapshotDocument.hpp.

#include "ControlSnapshotDocument.hpp"
#include "JsonWriter.hpp"

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
        j.Close('}');
    }
    j.Close(']');

    j.Key("parameters").Open('[');
    for (const auto& parameter : topology.parameters) {
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
        j.Key("domain").Open('{');
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
            } else {
                j.Key("kind").String("enum");
                j.Key("values").Open('[');
                for (const auto& item : domain.values) {
                    j.Open('{');
                    j.Key("value").RawNumber(std::to_string(item.value));
                    j.Key("name").String(item.name);
                    j.Close('}');
                }
                j.Close(']');
            }
        }, parameter.domain);
        j.Close('}');
        if (const auto found = state.parameters.find(parameter.id); found != state.parameters.end()) {
            j.Key("value");
            std::visit([&](const auto& value) {
                using V = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<V, bool>) j.Bool(value);
                else if constexpr (std::is_same_v<V, double>) Decimal(j, value);
                else j.RawNumber(std::to_string(value));
            }, found->second.value);
            j.Key("origin").String(OriginName(found->second.origin));
        } else {
            j.Key("value").Null();
            j.Key("origin").Null();
        }
        j.Close('}');
    }
    j.Close(']');
    j.Close('}');
    return j.Take();
}

} // namespace ASFW::UserClient::Wire
