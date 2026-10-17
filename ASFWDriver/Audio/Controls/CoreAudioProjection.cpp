// SPDX-License-Identifier: Apache-2.0
#include "CoreAudioProjection.hpp"
#include "../Model/Topology/Validate.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <map>
#include <tuple>

namespace ASFW::Audio::Controls {
namespace {
namespace AM = AudioModel;
struct Mapping { AM::ParameterSemantic semantic; uint32_t classId; CoreAudioControlKind kind; };
constexpr std::array mappings{
    Mapping{AM::ParameterSemantic::Level, 'vlme', CoreAudioControlKind::Level},
    Mapping{AM::ParameterSemantic::Mute, 'mute', CoreAudioControlKind::Boolean},
    Mapping{AM::ParameterSemantic::PhantomPower, 'phan', CoreAudioControlKind::Boolean},
    Mapping{AM::ParameterSemantic::PhaseInvert, 'phsi', CoreAudioControlKind::Boolean},
    Mapping{AM::ParameterSemantic::NominalLevel, 'nlvl', CoreAudioControlKind::Selector},
    Mapping{AM::ParameterSemantic::Source, 'dsrc', CoreAudioControlKind::Selector},
    Mapping{AM::ParameterSemantic::ClockSource, 'clck', CoreAudioControlKind::Selector},
    Mapping{AM::ParameterSemantic::HighPassFilter, 'hipf', CoreAudioControlKind::Selector},
};
struct Placement { uint32_t scope, element; auto operator<=>(const Placement&) const = default; };
using Placements = std::set<Placement>;

// Walk channel-preserving paths only. Every recursive frame owns its visited
// set: forks cannot hide a cycle or suppress another path's result.
void Walk(const AM::Topology& t, const AM::DeviceState& state, AM::PortId port, bool downstream,
          std::set<AM::PortId> visited, Placements& result) {
    if (!visited.insert(port).second) return;
    const auto p = std::ranges::find(t.ports, port, &AM::Port::id);
    if (p == t.ports.end() || p->channels != 1) return;
    const auto n = std::ranges::find(t.nodes, p->owner, &AM::Node::id);
    if (n == t.nodes.end()) return;
    if (const auto* endpoint = std::get_if<AM::EndpointNode>(&n->body);
        endpoint && endpoint->kind == AM::EndpointKind::Host &&
        p->signal.kind == AM::SignalKind::HostStream && p->signal.index > 0) {
        if (downstream && p->direction == AM::PortDirection::Input)
            result.insert({'inpt', p->signal.index});
        else if (!downstream && p->direction == AM::PortDirection::Output)
            result.insert({'outp', p->signal.index});
        return;
    }
    for (const auto& link : t.fixedLinks) {
        if (downstream && link.source == port) Walk(t, state, link.destination, downstream, visited, result);
        if (!downstream && link.destination == port) Walk(t, state, link.source, downstream, visited, result);
    }
    if (const auto* body = std::get_if<AM::ProcessorNode>(&n->body); body && body->channelPreserving) {
        const auto& from = downstream ? body->inputs : body->outputs;
        const auto& to = downstream ? body->outputs : body->inputs;
        if (from.size() != to.size()) return;
        const auto found = std::ranges::find(from, port);
        if (found != from.end()) Walk(t, state, to[static_cast<size_t>(found - from.begin())], downstream, visited, result);
    }
    if (!downstream) if (const auto* mixer = std::get_if<AM::MixerNode>(&n->body);
        mixer && std::ranges::find(mixer->outputs, port) != mixer->outputs.end()) {
        // A mixer output depends on every connected source. Only node-wide
        // controls covering the complete stream can become a master; a
        // per-channel control remains ambiguous when the mix combines channels.
        for (const auto input : mixer->inputs) Walk(t, state, input, false, visited, result);
    }
    if (const auto* body = std::get_if<AM::RouterNode>(&n->body)) {
        std::set<AM::PortId> destinations;
        for (const auto& bundle : body->legalBundles) {
            const auto current = state.routers.find(n->id);
            const bool stableSelector = downstream && body->outputs.size() == 1;
            if (!stableSelector && (current == state.routers.end() ||
                std::ranges::find(current->second.activeBundles, bundle.id) == current->second.activeBundles.end())) continue;
            for (const auto& route : bundle.routes) {
                if (downstream && route.input == port) destinations.insert(route.output);
                if (!downstream && route.output == port) destinations.insert(route.input);
            }
        }
        if (destinations.size() == 1) Walk(t, state, *destinations.begin(), downstream, visited, result);
    }
}

std::optional<Placement> Place(const AM::Topology& t, const AM::DeviceState& state, const AM::ParameterTarget& target) {
    Placements found;
    if (const auto* port = std::get_if<AM::PortId>(&target)) {
        Walk(t, state, *port, true, {}, found);
        Walk(t, state, *port, false, {}, found);
        if (found.size() == 1) return *found.begin();
        return {};
    }
    const auto* id = std::get_if<AM::NodeId>(&target);
    if (!id) return {}; // mixer crosspoints are app-only
    const auto node = std::ranges::find(t.nodes, *id, &AM::Node::id);
    if (node == t.nodes.end()) return {};
    for (const auto& port : t.ports) if (port.owner == *id) {
        Walk(t, state, port.id, true, {}, found);
        Walk(t, state, port.id, false, {}, found);
    }
    if (found.empty()) return {};
    const auto scope = found.begin()->scope;
    if (std::ranges::any_of(found, [scope](const auto& p) { return p.scope != scope; })) return {};
    if (found.size() == 1) return *found.begin();
    // A node-wide control can be HAL master only when it covers every host
    // element of this direction, not merely two adjacent channels in a wider stream.
    Placements all;
    for (const auto& p : t.ports) {
        const auto n = std::ranges::find(t.nodes, p.owner, &AM::Node::id);
        if (n == t.nodes.end()) continue;
        const auto* e = std::get_if<AM::EndpointNode>(&n->body);
        if (e && e->kind == AM::EndpointKind::Host && p.signal.kind == AM::SignalKind::HostStream &&
            ((scope == 'inpt' && p.direction == AM::PortDirection::Input) ||
             (scope == 'outp' && p.direction == AM::PortDirection::Output)))
            all.insert({scope, p.signal.index});
    }
    if (all == found) return Placement{scope, 0};
    return {};
}
bool ValidRange(const AM::ScalarDomain& d) {
    return std::isfinite(d.min) && std::isfinite(d.max) && d.min < d.max &&
        (!d.step || (std::isfinite(*d.step) && *d.step > 0));
}
} // namespace

std::optional<double> ScalarToPosition(const AM::ScalarDomain& d, double value) {
    if (!ValidRange(d) || !std::isfinite(value) || value < d.min || value > d.max) return {};
    return (value - d.min) / (d.max - d.min);
}
std::optional<double> PositionToScalar(const AM::ScalarDomain& d, double value) {
    if (!ValidRange(d) || !std::isfinite(value) || value < 0 || value > 1) return {};
    double result = d.min + value * (d.max - d.min);
    if (d.step) {
        const double highest = std::floor((d.max - d.min) / *d.step + 1e-9);
        result = d.min + std::clamp(std::round((result - d.min) / *d.step), 0.0, highest) * *d.step;
    }
    return result;
}

std::vector<CompositeInputType> ProjectInputTypes(const AM::Topology& t, const AM::DeviceState& state) {
    std::vector<CompositeInputType> result;
    if (state.topologyRevision != t.revision) return result;
    for (const auto& node : t.nodes) {
        const auto* router = std::get_if<AM::RouterNode>(&node.body);
        if (!router || router->outputs.size() != 1 || router->inputs.empty()) continue;
        Placements placement;
        Walk(t, state, router->outputs.front(), true, {}, placement);
        if (placement.size() != 1 || placement.begin()->scope != 'inpt') continue;
        CompositeInputType control{node.id, router->outputs.front(), placement.begin()->element};
        std::map<std::string, size_t> labelCounts;
        std::map<int64_t, std::string> connectors;
        for (const auto& bundle : router->legalBundles) {
            if (bundle.routes.size() != 1 || bundle.routes.front().output != control.output) {
                control.choices.clear(); break;
            }
            const auto input = bundle.routes.front().input;
            // Connector identity belongs to the upstream endpoint, reached
            // through fixed links. Do not infer it from a device/vendor name.
            AM::PortId source = input;
            std::set<AM::PortId> visited;
            while (visited.insert(source).second) {
                std::vector<AM::PortId> previous;
                for (const auto& link : t.fixedLinks) if (link.destination == source) previous.push_back(link.source);
                if (previous.size() != 1) break;
                source = previous.front();
            }
            const auto port = std::ranges::find(t.ports, source, &AM::Port::id);
            if (port == t.ports.end()) { control.choices.clear(); break; }
            std::string connector;
            switch (port->signal.kind) {
                case AM::SignalKind::AnalogMicXlr: connector = "XLR"; break;
                case AM::SignalKind::AnalogInstrument: connector = "Inst"; break;
                case AM::SignalKind::AnalogLine: connector = "Line"; break;
                default: break;
            }
            if (connector.empty()) { control.choices.clear(); break; }
            const auto level = std::ranges::find_if(t.parameters, [&](const auto& p) {
                return p.target == AM::ParameterTarget{source} && p.semantic == AM::ParameterSemantic::NominalLevel &&
                    std::holds_alternative<AM::EnumDomain>(p.domain);
            });
            const auto add = [&](std::string name, std::optional<AM::ParameterId> parameter, std::optional<int64_t> value) {
                const auto item = static_cast<int64_t>(control.choices.size());
                ++labelCounts[name]; connectors[item] = connector;
                control.choices.push_back({item, std::move(name), bundle.id, source, parameter, value});
                const auto rs = state.routers.find(node.id);
                if (rs == state.routers.end() || rs->second.activeBundles != std::vector{bundle.id}) return;
                if (parameter) {
                    const auto ps = state.parameters.find(*parameter);
                    if (ps == state.parameters.end() || ps->second.value != AM::ParameterValue{*value}) return;
                }
                control.selected = item;
            };
            if (level != t.parameters.end())
                for (const auto& item : std::get<AM::EnumDomain>(level->domain).values)
                    add(item.name, level->id, item.value);
            else add(connector, {}, {});
        }
        for (auto& choice : control.choices)
            if (labelCounts[choice.name] > 1) choice.name = connectors[choice.value] + " " + choice.name;
        if (!control.choices.empty()) result.push_back(std::move(control));
    }
    return result;
}

std::vector<CoreAudioControl> ProjectCoreAudioControls(const AM::Topology& t, const AM::DeviceState& state) {
    std::vector<CoreAudioControl> result;
    if (state.topologyRevision != t.revision) return result;
    const auto inputTypes = ProjectInputTypes(t, state);
    for (const auto& p : t.parameters) {
        if (std::ranges::any_of(inputTypes, [&](const auto& type) {
            return std::ranges::any_of(type.choices, [&](const auto& choice) { return choice.levelParameter == p.id; });
        })) continue;
        const auto mapping = std::ranges::find(mappings, p.semantic, &Mapping::semantic);
        if (mapping == mappings.end()) continue;
        const auto s = state.parameters.find(p.id);
        if (s == state.parameters.end() || !AM::validateParameterState(p, s->second)) continue;
        const auto domain = s->second.domainOverride ? AM::ParameterDomain{*s->second.domainOverride} : p.domain;
        if ((mapping->kind == CoreAudioControlKind::Level && !std::holds_alternative<AM::ScalarDomain>(domain)) ||
            (mapping->kind == CoreAudioControlKind::Boolean && !std::holds_alternative<AM::BooleanDomain>(domain)) ||
            (mapping->kind == CoreAudioControlKind::Selector && !std::holds_alternative<AM::EnumDomain>(domain))) continue;
        const auto placement = Place(t, state, p.target);
        if (!placement) continue; // no guessed channel numbers or private scope
        result.push_back({p.id, mapping->kind, mapping->classId, placement->scope, placement->element,
            p.access == AM::ParameterAccess::ReadWrite, p.name, domain, s->second.value,
            mapping->kind == CoreAudioControlKind::Level && placement->scope == 'inpt'});
    }
    for (const auto& type : inputTypes) {
        if (!type.selected) continue;
        AM::EnumDomain domain;
        for (const auto& choice : type.choices) domain.values.push_back({choice.value, choice.name});
        result.push_back({InputTypeParameterId(type.router), CoreAudioControlKind::Selector, 'nlvl', 'inpt',
            type.element, std::get<AM::RouterNode>(std::ranges::find(t.nodes, type.router, &AM::Node::id)->body).writable, "Input Type", std::move(domain), *type.selected});
    }
    // An ambiguous class/scope/element cannot represent two independent values.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, size_t> counts;
    for (const auto& c : result) ++counts[{c.classId, c.scope, c.element}];
    std::erase_if(result, [&](const auto& c) {
        return counts.at({c.classId, c.scope, c.element}) > 1;
    });
    return result;
}
} // namespace ASFW::Audio::Controls
