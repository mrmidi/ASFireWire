// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
// Ported from midi:ADKVirtualAudioLab/Core/AudioModel (AUAA semantic core, FW-202).
#include "Validate.hpp"

#include <cmath>
#include <algorithm>
#include "Format.hpp"
#include <unordered_map>
#include <unordered_set>

namespace ASFW::AudioModel {

namespace {

void validateStructure(const Topology& topology,
                       std::unordered_map<NodeId, const Node*>& nodeMap,
                       std::unordered_map<PortId, const Port*>& portMap,
                       std::unordered_map<CrosspointId, const MixerCrosspoint*>& crosspointMap,
                       std::vector<TopologyError>& errors) {
    // 1. Validate Nodes
    for (const auto& node : topology.nodes) {
        if (auto [it, inserted] = nodeMap.emplace(node.id, &node); !inserted) {
            errors.push_back({
                TopologyErrorKind::DuplicateId,
                detail::Format("Duplicate NodeId: {}", node.id.value)
            });
        }
    }

    // 2. Validate Ports
    for (const auto& port : topology.ports) {
        if (auto [it, inserted] = portMap.emplace(port.id, &port); !inserted) {
            errors.push_back({
                TopologyErrorKind::DuplicateId,
                detail::Format("Duplicate PortId: {}", port.id.value)
            });
        }

        if (port.channels == 0) {
            errors.push_back({
                TopologyErrorKind::InvalidChannelCount,
                detail::Format("Port {} '{}' has zero channels", port.id.value, port.name)
            });
        }

        auto nodeIt = nodeMap.find(port.owner);
        if (nodeIt == nodeMap.end()) {
            errors.push_back({
                TopologyErrorKind::NonexistentNode,
                detail::Format("Port {} '{}' references nonexistent owner NodeId {}",
                            port.id.value, port.name, port.owner.value)
            });
            continue;
        }

        // An endpoint port is a connector or a wire slot, so it must carry a
        // canonical identity: its name is rendered from that identity rather
        // than authored per device.
        const bool isEndpoint = std::holds_alternative<EndpointNode>(nodeIt->second->body);
        if (isEndpoint && port.signal.kind == SignalKind::Unknown) {
            errors.push_back({
                TopologyErrorKind::MissingSignalIdentity,
                detail::Format("Endpoint port {} '{}' has no SignalIdentity", port.id.value, port.name)
            });
        }
        if (isEndpoint && port.signal.index == 0) {
            errors.push_back({
                TopologyErrorKind::MissingSignalIdentity,
                detail::Format("Endpoint port {} '{}' has SignalIdentity index 0 (indices are 1-based)",
                            port.id.value, port.name)
            });
        }
        if (!isEndpoint && port.name.empty()) {
            errors.push_back({
                TopologyErrorKind::MissingSignalIdentity,
                detail::Format("Interior port {} has no name and no identity to render one from",
                            port.id.value)
            });
        }
    }

    // 3. Validate Node Bodies (Ports ownership and direction)
    for (const auto& node : topology.nodes) {
        std::visit([&](const auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, EndpointNode>) {
                // Endpoint has no dedicated port tables in body
            } else if constexpr (std::is_same_v<T, RouterNode>) {
                std::unordered_set<PortId> inSet;
                for (const auto& inPortId : body.inputs) {
                    if (auto [_, ins] = inSet.insert(inPortId); !ins) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Router Node {} has duplicate input PortId {}", node.id.value, inPortId.value)
                        });
                    }
                    auto pIt = portMap.find(inPortId);
                    if (pIt == portMap.end()) {
                        errors.push_back({
                            TopologyErrorKind::NonexistentPort,
                            detail::Format("Router Node {} input PortId {} does not exist", node.id.value, inPortId.value)
                        });
                    } else {
                        if (pIt->second->owner != node.id) {
                            errors.push_back({
                                TopologyErrorKind::ForeignPortReference,
                                detail::Format("Router Node {} input PortId {} is not owned by this node",
                                            node.id.value, inPortId.value)
                            });
                        }
                        if (pIt->second->direction != PortDirection::Input) {
                            errors.push_back({
                                TopologyErrorKind::InvalidPortDirection,
                                detail::Format("Router Node {} input PortId {} is not PortDirection::Input",
                                            node.id.value, inPortId.value)
                            });
                        }
                    }
                }

                std::unordered_set<PortId> outSet;
                for (const auto& outPortId : body.outputs) {
                    if (auto [_, ins] = outSet.insert(outPortId); !ins) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Router Node {} has duplicate output PortId {}", node.id.value, outPortId.value)
                        });
                    }
                    auto pIt = portMap.find(outPortId);
                    if (pIt == portMap.end()) {
                        errors.push_back({
                            TopologyErrorKind::NonexistentPort,
                            detail::Format("Router Node {} output PortId {} does not exist", node.id.value, outPortId.value)
                        });
                    } else {
                        if (pIt->second->owner != node.id) {
                            errors.push_back({
                                TopologyErrorKind::ForeignPortReference,
                                detail::Format("Router Node {} output PortId {} is not owned by this node",
                                            node.id.value, outPortId.value)
                            });
                        }
                        if (pIt->second->direction != PortDirection::Output) {
                            errors.push_back({
                                TopologyErrorKind::InvalidPortDirection,
                                detail::Format("Router Node {} output PortId {} is not PortDirection::Output",
                                            node.id.value, outPortId.value)
                            });
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, MixerNode>) {
                std::unordered_set<PortId> inSet;
                for (const auto& inPortId : body.inputs) {
                    if (auto [_, ins] = inSet.insert(inPortId); !ins) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Mixer Node {} has duplicate input PortId {}", node.id.value, inPortId.value)
                        });
                    }
                    auto pIt = portMap.find(inPortId);
                    if (pIt == portMap.end()) {
                        errors.push_back({
                            TopologyErrorKind::NonexistentPort,
                            detail::Format("Mixer Node {} input PortId {} does not exist", node.id.value, inPortId.value)
                        });
                    } else {
                        if (pIt->second->owner != node.id) {
                            errors.push_back({
                                TopologyErrorKind::ForeignPortReference,
                                detail::Format("Mixer Node {} input PortId {} is not owned by this node",
                                            node.id.value, inPortId.value)
                            });
                        }
                        if (pIt->second->direction != PortDirection::Input) {
                            errors.push_back({
                                TopologyErrorKind::InvalidPortDirection,
                                detail::Format("Mixer Node {} input PortId {} is not PortDirection::Input",
                                            node.id.value, inPortId.value)
                            });
                        }
                    }
                }

                std::unordered_set<PortId> outSet;
                for (const auto& outPortId : body.outputs) {
                    if (auto [_, ins] = outSet.insert(outPortId); !ins) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Mixer Node {} has duplicate output PortId {}", node.id.value, outPortId.value)
                        });
                    }
                    auto pIt = portMap.find(outPortId);
                    if (pIt == portMap.end()) {
                        errors.push_back({
                            TopologyErrorKind::NonexistentPort,
                            detail::Format("Mixer Node {} output PortId {} does not exist", node.id.value, outPortId.value)
                        });
                    } else {
                        if (pIt->second->owner != node.id) {
                            errors.push_back({
                                TopologyErrorKind::ForeignPortReference,
                                detail::Format("Mixer Node {} output PortId {} is not owned by this node",
                                            node.id.value, outPortId.value)
                            });
                        }
                        if (pIt->second->direction != PortDirection::Output) {
                            errors.push_back({
                                TopologyErrorKind::InvalidPortDirection,
                                detail::Format("Mixer Node {} output PortId {} is not PortDirection::Output",
                                            node.id.value, outPortId.value)
                            });
                        }
                    }
                }

                std::unordered_set<uint64_t> seenCrosspoints;
                for (const auto& cp : body.crosspoints) {
                    if (auto [it, inserted] = crosspointMap.emplace(cp.id, &cp); !inserted) {
                        errors.push_back({
                            TopologyErrorKind::DuplicateId,
                            detail::Format("Duplicate CrosspointId: {}", cp.id.value)
                        });
                    }

                    const uint64_t cpKey = (static_cast<uint64_t>(cp.input.value) << 32) | cp.output.value;
                    if (auto [_, inserted] = seenCrosspoints.insert(cpKey); !inserted) {
                        errors.push_back({
                            TopologyErrorKind::DuplicateRouteOrCrosspoint,
                            detail::Format("Mixer Node {} has duplicate crosspoint (input {}, output {})",
                                        node.id.value, cp.input.value, cp.output.value)
                        });
                    }

                    if (!inSet.contains(cp.input)) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Mixer Node {} crosspoint {} references input PortId {} not in mixer inputs",
                                        node.id.value, cp.id.value, cp.input.value)
                        });
                    }
                    if (!outSet.contains(cp.output)) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Mixer Node {} crosspoint {} references output PortId {} not in mixer outputs",
                                        node.id.value, cp.id.value, cp.output.value)
                        });
                    }
                }
            } else if constexpr (std::is_same_v<T, ProcessorNode>) {
                for (const auto& inPortId : body.inputs) {
                    auto pIt = portMap.find(inPortId);
                    if (pIt == portMap.end()) {
                        errors.push_back({
                            TopologyErrorKind::NonexistentPort,
                            detail::Format("Processor Node {} input PortId {} does not exist", node.id.value, inPortId.value)
                        });
                    } else {
                        if (pIt->second->owner != node.id) {
                            errors.push_back({
                                TopologyErrorKind::ForeignPortReference,
                                detail::Format("Processor Node {} input PortId {} is not owned by this node",
                                            node.id.value, inPortId.value)
                            });
                        }
                        if (pIt->second->direction != PortDirection::Input) {
                            errors.push_back({
                                TopologyErrorKind::InvalidPortDirection,
                                detail::Format("Processor Node {} input PortId {} is not PortDirection::Input",
                                            node.id.value, inPortId.value)
                            });
                        }
                    }
                }

                for (const auto& outPortId : body.outputs) {
                    auto pIt = portMap.find(outPortId);
                    if (pIt == portMap.end()) {
                        errors.push_back({
                            TopologyErrorKind::NonexistentPort,
                            detail::Format("Processor Node {} output PortId {} does not exist", node.id.value, outPortId.value)
                        });
                    } else {
                        if (pIt->second->owner != node.id) {
                            errors.push_back({
                                TopologyErrorKind::ForeignPortReference,
                                detail::Format("Processor Node {} output PortId {} is not owned by this node",
                                            node.id.value, outPortId.value)
                            });
                        }
                        if (pIt->second->direction != PortDirection::Output) {
                            errors.push_back({
                                TopologyErrorKind::InvalidPortDirection,
                                detail::Format("Processor Node {} output PortId {} is not PortDirection::Output",
                                            node.id.value, outPortId.value)
                            });
                        }
                    }
                }
            }
        }, node.body);
    }
}

void validateRouting(const Topology& topology,
                     [[maybe_unused]] const std::unordered_map<NodeId, const Node*>& nodeMap,
                     const std::unordered_map<PortId, const Port*>& portMap,
                     std::vector<TopologyError>& errors) {
    // 1. Validate Fixed Links
    std::unordered_set<PortId> connectedDestinations;
    for (const auto& link : topology.fixedLinks) {
        auto srcIt = portMap.find(link.source);
        auto dstIt = portMap.find(link.destination);

        if (srcIt == portMap.end()) {
            errors.push_back({
                TopologyErrorKind::NonexistentPort,
                detail::Format("FixedLink source PortId {} does not exist", link.source.value)
            });
        } else if (srcIt->second->direction != PortDirection::Output) {
            errors.push_back({
                TopologyErrorKind::InvalidPortDirection,
                detail::Format("FixedLink source PortId {} is not an Output", link.source.value)
            });
        }

        if (dstIt == portMap.end()) {
            errors.push_back({
                TopologyErrorKind::NonexistentPort,
                detail::Format("FixedLink destination PortId {} does not exist", link.destination.value)
            });
        } else if (dstIt->second->direction != PortDirection::Input) {
            errors.push_back({
                TopologyErrorKind::InvalidPortDirection,
                detail::Format("FixedLink destination PortId {} is not an Input", link.destination.value)
            });
        }

        if (srcIt != portMap.end() && dstIt != portMap.end()) {
            if (link.source == link.destination) {
                errors.push_back({
                    TopologyErrorKind::ForeignPortReference,
                    detail::Format("FixedLink connects PortId {} to itself", link.source.value)
                });
            }

            if (srcIt->second->channels != dstIt->second->channels) {
                errors.push_back({
                    TopologyErrorKind::IncompatibleChannelCount,
                    detail::Format("FixedLink channel mismatch: source {} has {} ch, destination {} has {} ch",
                                link.source.value, srcIt->second->channels,
                                link.destination.value, dstIt->second->channels)
                });
            }

            if (auto [_, inserted] = connectedDestinations.insert(link.destination); !inserted) {
                errors.push_back({
                    TopologyErrorKind::MultipleDriversOnInput,
                    detail::Format("Multiple FixedLinks drive destination PortId {}", link.destination.value)
                });
            }
        }
    }

    // 2. Validate Router Nodes (legalBundles and constraints)
    for (const auto& node : topology.nodes) {
        if (const auto* router = std::get_if<RouterNode>(&node.body)) {
            const std::unordered_set<PortId> inSet(router->inputs.begin(), router->inputs.end());
            const std::unordered_set<PortId> outSet(router->outputs.begin(), router->outputs.end());

            // RouteBundleId is router-local
            std::unordered_set<RouteBundleId> seenBundleIds;
            std::unordered_set<uint64_t> allBundleRoutes;

            for (const auto& bundle : router->legalBundles) {
                if (auto [_, inserted] = seenBundleIds.insert(bundle.id); !inserted) {
                    errors.push_back({
                        TopologyErrorKind::DuplicateId,
                        detail::Format("Router Node {} has duplicate RouteBundleId: {}", node.id.value, bundle.id.value)
                    });
                }

                if (bundle.routes.empty()) {
                    errors.push_back({
                        TopologyErrorKind::InvalidConstraint,
                        detail::Format("Router Node {} RouteBundle {} has empty routes", node.id.value, bundle.id.value)
                    });
                }

                std::unordered_set<uint64_t> seenRoutesInBundle;
                for (const auto& route : bundle.routes) {
                    const uint64_t routeKey = (static_cast<uint64_t>(route.input.value) << 32) | route.output.value;
                    if (!allBundleRoutes.insert(routeKey).second)
                        errors.push_back({TopologyErrorKind::DuplicateRouteOrCrosspoint,
                            "A route must belong to exactly one bundle"});
                    if (auto [_, inserted] = seenRoutesInBundle.insert(routeKey); !inserted) {
                        errors.push_back({
                            TopologyErrorKind::DuplicateRouteOrCrosspoint,
                            detail::Format("Router Node {} RouteBundle {} has duplicate route (input {}, output {})",
                                        node.id.value, bundle.id.value, route.input.value, route.output.value)
                        });
                    }

                    if (!inSet.contains(route.input)) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Router Node {} RouteBundle {} references input PortId {} not in router inputs",
                                        node.id.value, bundle.id.value, route.input.value)
                        });
                    }
                    if (!outSet.contains(route.output)) {
                        errors.push_back({
                            TopologyErrorKind::ForeignPortReference,
                            detail::Format("Router Node {} RouteBundle {} references output PortId {} not in router outputs",
                                        node.id.value, bundle.id.value, route.output.value)
                        });
                    }

                    auto inPortIt = portMap.find(route.input);
                    auto outPortIt = portMap.find(route.output);
                    if (inPortIt != portMap.end() && outPortIt != portMap.end()) {
                        if (inPortIt->second->channels != outPortIt->second->channels) {
                            errors.push_back({
                                TopologyErrorKind::IncompatibleChannelCount,
                                detail::Format("Router Node {} RouteBundle {} channel mismatch: input {} ({} ch) vs output {} ({} ch)",
                                            node.id.value, bundle.id.value, route.input.value, inPortIt->second->channels,
                                            route.output.value, outPortIt->second->channels)
                            });
                        }
                    }
                }
            }

            if (router->constraints.maxActiveBundles.has_value() && *router->constraints.maxActiveBundles == 0) {
                errors.push_back({
                    TopologyErrorKind::InvalidConstraint,
                    detail::Format("Router Node {} maxActiveBundles constraint is 0", node.id.value)
                });
            }
            if (router->constraints.maxActiveRoutes.has_value() && *router->constraints.maxActiveRoutes == 0) {
                errors.push_back({
                    TopologyErrorKind::InvalidConstraint,
                    detail::Format("Router Node {} maxActiveRoutes constraint is 0", node.id.value)
                });
            }
            if (router->constraints.maxSourcesPerOutput.has_value() && *router->constraints.maxSourcesPerOutput == 0) {
                errors.push_back({
                    TopologyErrorKind::InvalidConstraint,
                    detail::Format("Router Node {} maxSourcesPerOutput constraint is 0", node.id.value)
                });
            }
            if (router->constraints.maxDestinationsPerInput.has_value() && *router->constraints.maxDestinationsPerInput == 0) {
                errors.push_back({
                    TopologyErrorKind::InvalidConstraint,
                    detail::Format("Router Node {} maxDestinationsPerInput constraint is 0", node.id.value)
                });
            }
        }
    }
}

void validateParameters(const Topology& topology,
                        [[maybe_unused]] const std::unordered_map<NodeId, const Node*>& nodeMap,
                        const std::unordered_map<PortId, const Port*>& portMap,
                        const std::unordered_map<CrosspointId, const MixerCrosspoint*>& crosspointMap,
                        std::vector<TopologyError>& errors) {
    // 1. Validate Parameters
    std::unordered_set<ParameterId> parameterSet;
    for (const auto& param : topology.parameters) {
        if (auto [_, inserted] = parameterSet.insert(param.id); !inserted) {
            errors.push_back({
                TopologyErrorKind::DuplicateId,
                detail::Format("Duplicate ParameterId: {}", param.id.value)
            });
        }

        std::visit([&](const auto& targetId) {
            using T = std::decay_t<decltype(targetId)>;
            if constexpr (std::is_same_v<T, NodeId>) {
                if (!nodeMap.contains(targetId)) {
                    errors.push_back({
                        TopologyErrorKind::NonexistentNode,
                        detail::Format("Parameter {} '{}' targets nonexistent NodeId {}",
                                    param.id.value, param.name, targetId.value)
                    });
                }
            } else if constexpr (std::is_same_v<T, PortId>) {
                if (!portMap.contains(targetId)) {
                    errors.push_back({
                        TopologyErrorKind::NonexistentPort,
                        detail::Format("Parameter {} '{}' targets nonexistent PortId {}",
                                    param.id.value, param.name, targetId.value)
                    });
                }
            } else if constexpr (std::is_same_v<T, CrosspointId>) {
                if (!crosspointMap.contains(targetId)) {
                    errors.push_back({
                        TopologyErrorKind::NonexistentCrosspoint,
                        detail::Format("Parameter {} '{}' targets nonexistent CrosspointId {}",
                                    param.id.value, param.name, targetId.value)
                    });
                }
            }
        }, param.target);

        std::visit([&](const auto& domain) {
            using D = std::decay_t<decltype(domain)>;
            if constexpr (std::is_same_v<D, ScalarDomain>) {
                if (!std::isfinite(domain.min) || !std::isfinite(domain.max) || domain.min > domain.max) {
                    errors.push_back({
                        TopologyErrorKind::InvalidDomain,
                        detail::Format("Parameter {} '{}' ScalarDomain min ({}) > max ({})",
                                    param.id.value, param.name, domain.min, domain.max)
                    });
                }
                if (domain.step.has_value() && (!std::isfinite(*domain.step) || *domain.step <= 0)) {
                    errors.push_back({
                        TopologyErrorKind::InvalidDomain,
                        detail::Format("Parameter {} '{}' ScalarDomain step ({}) <= 0",
                                    param.id.value, param.name, *domain.step)
                    });
                }
            } else if constexpr (std::is_same_v<D, EnumDomain>) {
                if (domain.values.empty()) {
                    errors.push_back({
                        TopologyErrorKind::InvalidDomain,
                        detail::Format("Parameter {} '{}' EnumDomain has no values",
                                    param.id.value, param.name)
                    });
                }
                std::unordered_set<int64_t> seenEnumValues;
                for (const auto& item : domain.values) {
                    if (auto [_, inserted] = seenEnumValues.insert(item.value); !inserted) {
                        errors.push_back({
                            TopologyErrorKind::InvalidDomain,
                            detail::Format("Parameter {} '{}' EnumDomain has duplicate value {}",
                                        param.id.value, param.name, item.value)
                        });
                    }
                }
            }
        }, param.domain);
    }

    // 2. Validate Meters
    std::unordered_set<MeterId> meterSet;
    for (const auto& meter : topology.meters) {
        if (auto [_, inserted] = meterSet.insert(meter.id); !inserted) {
            errors.push_back({
                TopologyErrorKind::DuplicateId,
                detail::Format("Duplicate MeterId: {}", meter.id.value)
            });
        }

        std::visit([&](const auto& targetId) {
            using T = std::decay_t<decltype(targetId)>;
            if constexpr (std::is_same_v<T, NodeId>) {
                if (!nodeMap.contains(targetId)) {
                    errors.push_back({
                        TopologyErrorKind::NonexistentNode,
                        detail::Format("Meter {} '{}' targets nonexistent NodeId {}",
                                    meter.id.value, meter.name, targetId.value)
                    });
                }
            } else if constexpr (std::is_same_v<T, PortId>) {
                if (!portMap.contains(targetId)) {
                    errors.push_back({
                        TopologyErrorKind::NonexistentPort,
                        detail::Format("Meter {} '{}' targets nonexistent PortId {}",
                                    meter.id.value, meter.name, targetId.value)
                    });
                }
            }
        }, meter.target);

        if (meter.domain.min > meter.domain.max) {
            errors.push_back({
                TopologyErrorKind::InvalidDomain,
                detail::Format("Meter {} '{}' ScalarDomain min ({}) > max ({})",
                            meter.id.value, meter.name, meter.domain.min, meter.domain.max)
            });
        }
    }

    // 3. Validate Channels
    std::unordered_set<ChannelId> channelSet;
    for (const auto& ch : topology.channels) {
        if (auto [_, inserted] = channelSet.insert(ch.id); !inserted) {
            errors.push_back({
                TopologyErrorKind::DuplicateId,
                detail::Format("Duplicate ChannelId: {}", ch.id.value)
            });
        }
        for (const auto& pId : ch.ports) {
            if (!portMap.contains(pId)) {
                errors.push_back({
                    TopologyErrorKind::NonexistentPort,
                    detail::Format("Channel {} '{}' references nonexistent PortId {}",
                                ch.id.value, ch.name, pId.value)
                });
            }
        }
    }

    // 4. Validate Buses
    std::unordered_set<BusId> busSet;
    for (const auto& bus : topology.buses) {
        if (auto [_, inserted] = busSet.insert(bus.id); !inserted) {
            errors.push_back({
                TopologyErrorKind::DuplicateId,
                detail::Format("Duplicate BusId: {}", bus.id.value)
            });
        }
        for (const auto& pId : bus.ports) {
            if (!portMap.contains(pId)) {
                errors.push_back({
                    TopologyErrorKind::NonexistentPort,
                    detail::Format("Bus {} '{}' references nonexistent PortId {}",
                                bus.id.value, bus.name, pId.value)
                });
            }
        }
    }
}

} // namespace

std::vector<TopologyError> validateAll(const Topology& topology) {
    std::vector<TopologyError> errors;
    std::unordered_map<NodeId, const Node*> nodeMap;
    std::unordered_map<PortId, const Port*> portMap;
    std::unordered_map<CrosspointId, const MixerCrosspoint*> crosspointMap;

    validateStructure(topology, nodeMap, portMap, crosspointMap, errors);
    validateRouting(topology, nodeMap, portMap, errors);
    validateParameters(topology, nodeMap, portMap, crosspointMap, errors);

    return errors;
}

std::expected<void, TopologyError> validate(const Topology& topology) {
    auto errors = validateAll(topology);
    if (!errors.empty()) {
        return std::unexpected(errors.front());
    }
    return {};
}

std::expected<void, StateError> validateParameterValue(
    const Topology& topology,
    ParameterId id,
    const ParameterValue& value) {

    const Parameter* found = nullptr;
    for (const auto& param : topology.parameters) {
        if (param.id == id) {
            found = &param;
            break;
        }
    }

    if (found == nullptr) {
        return std::unexpected(StateError{
            StateErrorKind::NonexistentParameter,
            detail::Format("ParameterId {} does not exist in topology", id.value)
        });
    }

    return validateParameterValue(*found, value);
}

std::expected<void, StateError> validateParameterValue(
    const Parameter& parameter, const ParameterValue& value) {
    const auto* found = &parameter;
    const auto id = parameter.id;
    return std::visit([&](const auto& domain) -> std::expected<void, StateError> {
        using D = std::decay_t<decltype(domain)>;
        if constexpr (std::is_same_v<D, BooleanDomain>) {
            if (!std::holds_alternative<bool>(value)) {
                return std::unexpected(StateError{
                    StateErrorKind::InvalidParameterValue,
                    detail::Format("Parameter {} '{}' expects boolean value", id.value, found->name)
                });
            }
        } else if constexpr (std::is_same_v<D, ScalarDomain>) {
            if (!std::holds_alternative<double>(value)) {
                return std::unexpected(StateError{
                    StateErrorKind::InvalidParameterValue,
                    detail::Format("Parameter {} '{}' expects double scalar value", id.value, found->name)
                });
            }
            const double v = std::get<double>(value);
            if (!std::isfinite(domain.min) || !std::isfinite(domain.max) || domain.min > domain.max ||
                (domain.step && (!std::isfinite(*domain.step) || *domain.step <= 0)) ||
                !std::isfinite(v) || v < domain.min || v > domain.max) {
                return std::unexpected(StateError{
                    StateErrorKind::InvalidParameterValue,
                    detail::Format("Parameter {} '{}' value {} out of range [{}, {}] or non-finite",
                                id.value, found->name, v, domain.min, domain.max)
                });
            }
            if (domain.step.has_value() && *domain.step > 0.0) {
                const double offset = v - domain.min;
                const double step = *domain.step;
                const double numSteps = std::round(offset / step);
                if (std::abs(offset - (numSteps * step)) > 1e-4) {
                    return std::unexpected(StateError{
                        StateErrorKind::InvalidParameterValue,
                        detail::Format("Parameter {} '{}' value {} does not align with step {}",
                                    id.value, found->name, v, step)
                    });
                }
            }
        } else if constexpr (std::is_same_v<D, ActionDomain>) {
            if (!std::holds_alternative<ActionTrigger>(value))
                return std::unexpected(StateError{StateErrorKind::InvalidParameterValue,
                    "An action requires a one-shot trigger"});
        } else if constexpr (std::is_same_v<D, EnumDomain>) {
            if (!std::holds_alternative<int64_t>(value)) {
                return std::unexpected(StateError{
                    StateErrorKind::InvalidParameterValue,
                    detail::Format("Parameter {} '{}' expects int64_t enum value", id.value, found->name)
                });
            }
            const int64_t v = std::get<int64_t>(value);
            bool match = false;
            for (const auto& item : domain.values) {
                if (item.value == v) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                return std::unexpected(StateError{
                    StateErrorKind::InvalidParameterValue,
                    detail::Format("Parameter {} '{}' enum value {} is not in legal enum items",
                                id.value, found->name, v)
                });
            }
        }
        return {};
    }, found->domain);
}

std::expected<void, StateError> validateParameterState(
    const Parameter& parameter, const ParameterState& state) {
    Parameter effective = parameter;
    if (state.domainOverride) {
        const auto* base = std::get_if<ScalarDomain>(&parameter.domain);
        const auto& domain = *state.domainOverride;
        if (!base || domain.unit != base->unit || !std::isfinite(domain.min) ||
            !std::isfinite(domain.max) || domain.min > domain.max ||
            (domain.step && (!std::isfinite(*domain.step) || *domain.step <= 0)))
            return std::unexpected(StateError{StateErrorKind::InvalidParameterState,
                "A dynamic range must be finite, ordered, positive-step and retain the scalar unit"});
        effective.domain = domain;
    }
    return validateParameterValue(effective, state.value);
}

std::expected<void, StateError> validateParameterWrite(
    const Topology& topology, const DeviceState& state, ParameterId id,
    const ParameterValue& value) {
    if (state.topologyRevision != topology.revision)
        return std::unexpected(StateError{StateErrorKind::TopologyRevisionMismatch, "Stale control state"});
    const auto found = std::ranges::find(topology.parameters, id, &Parameter::id);
    if (found == topology.parameters.end())
        return std::unexpected(StateError{StateErrorKind::NonexistentParameter, "Unknown parameter"});
    if (found->access == ParameterAccess::ReadOnly)
        return std::unexpected(StateError{StateErrorKind::ReadOnlyParameter, "This parameter is read-only"});
    const auto current = state.parameters.find(id);
    if (current == state.parameters.end()) {
        if (std::holds_alternative<ActionDomain>(found->domain)) return validateParameterValue(*found, value);
        return std::unexpected(StateError{StateErrorKind::MissingParameter, "No confirmed parameter state"});
    }
    if (!current->second.applicable)
        return std::unexpected(StateError{StateErrorKind::ParameterNotApplicable, "This control has no effect in the current mode"});
    // Reject invalid provider metadata even when the requested value itself fits.
    if (auto legal = validateParameterState(*found, current->second); !legal) return legal;
    auto requested = current->second;
    requested.value = value;
    return validateParameterState(*found, requested);
}

std::expected<RouterState, StateError> prepareRouteWrite(
    const Topology& topology, const DeviceState& state, NodeId id, PortId output, std::optional<PortId> input) {
    if (state.topologyRevision != topology.revision)
        return std::unexpected(StateError{StateErrorKind::TopologyRevisionMismatch, "Stale router state"});
    const auto node = std::ranges::find(topology.nodes, id, &Node::id);
    const auto* router = node == topology.nodes.end() ? nullptr : std::get_if<RouterNode>(&node->body);
    const auto current = state.routers.find(id);
    if (!router || current == state.routers.end())
        return std::unexpected(StateError{StateErrorKind::NonexistentRouter, "Unknown router"});
    if (!router->writable)
        return std::unexpected(StateError{StateErrorKind::ReadOnlyParameter, "Routing is read-only"});
    if (std::ranges::find(router->outputs, output) == router->outputs.end())
        return std::unexpected(StateError{StateErrorKind::RoutingConstraintViolated, "Unknown router output"});
    auto requested = current->second;
    const RouteBundle* target = nullptr;
    for (const auto& bundle : router->legalBundles) for (const auto& route : bundle.routes)
        if (input && route.input == *input && route.output == output) {
            if (target) return std::unexpected(StateError{StateErrorKind::RoutingConstraintViolated, "Route belongs to multiple bundles"});
            target = &bundle;
        }
    if (input && !target)
        return std::unexpected(StateError{StateErrorKind::RoutingConstraintViolated, "Route is not legal"});
    std::erase_if(requested.activeBundles, [&](auto bundleId) {
        const auto bundle = std::ranges::find(router->legalBundles, bundleId, &RouteBundle::id);
        return bundle != router->legalBundles.end() && std::ranges::any_of(bundle->routes,
            [&](const auto& route) { return route.output == output; });
    });
    if (target) requested.activeBundles.push_back(target->id);
    if (const auto valid = validateRouterState(topology, id, requested); !valid) return std::unexpected(valid.error());
    return requested;
}

std::expected<void, StateError> validateRouterState(
    const Topology& topology,
    NodeId routerNodeId,
    const RouterState& routerState) {

    const RouterNode* router = nullptr;
    for (const auto& node : topology.nodes) {
        if (node.id == routerNodeId) {
            router = std::get_if<RouterNode>(&node.body);
            break;
        }
    }

    if (router == nullptr) {
        return std::unexpected(StateError{
            StateErrorKind::NonexistentRouter,
            detail::Format("NodeId {} is not a RouterNode in topology", routerNodeId.value)
        });
    }

    std::unordered_map<RouteBundleId, const RouteBundle*> legalMap;
    for (const auto& b : router->legalBundles) {
        legalMap[b.id] = &b;
    }

    std::unordered_set<RouteBundleId> seen;
    std::vector<const RouteBundle*> activeBundleList;
    for (const auto& bId : routerState.activeBundles) {
        if (!seen.insert(bId).second) {
            return std::unexpected(StateError{
                StateErrorKind::DuplicateActiveRouteBundle,
                detail::Format("Router Node {} has duplicate active RouteBundleId {}",
                            routerNodeId.value, bId.value)
            });
        }

        auto it = legalMap.find(bId);
        if (it == legalMap.end()) {
            return std::unexpected(StateError{
                StateErrorKind::NonexistentRouteBundle,
                detail::Format("Router Node {} has active RouteBundleId {} which is not in legalBundles",
                            routerNodeId.value, bId.value)
            });
        }
        activeBundleList.push_back(it->second);
    }

    // Check maxActiveBundles constraint
    if (router->constraints.maxActiveBundles.has_value()) {
        if (routerState.activeBundles.size() > *router->constraints.maxActiveBundles) {
            return std::unexpected(StateError{
                StateErrorKind::RoutingConstraintViolated,
                detail::Format("Router Node {} active bundles ({}) exceeds maxActiveBundles ({})",
                            routerNodeId.value, routerState.activeBundles.size(),
                            *router->constraints.maxActiveBundles)
            });
        }
    }

    // Collect all active routes
    std::unordered_map<PortId, uint32_t> sourcesPerOutput;
    std::unordered_map<PortId, uint32_t> destinationsPerInput;
    uint32_t totalRoutes = 0;

    for (const auto* bundle : activeBundleList) {
        for (const auto& r : bundle->routes) {
            ++totalRoutes;
            ++sourcesPerOutput[r.output];
            ++destinationsPerInput[r.input];
        }
    }

    // Check maxActiveRoutes
    if (router->constraints.maxActiveRoutes.has_value()) {
        if (totalRoutes > *router->constraints.maxActiveRoutes) {
            return std::unexpected(StateError{
                StateErrorKind::RoutingConstraintViolated,
                detail::Format("Router Node {} total active routes ({}) exceeds maxActiveRoutes ({})",
                            routerNodeId.value, totalRoutes,
                            *router->constraints.maxActiveRoutes)
            });
        }
    }

    // Check maxSourcesPerOutput
    if (router->constraints.maxSourcesPerOutput.has_value()) {
        for (const auto& [outPort, count] : sourcesPerOutput) {
            if (count > *router->constraints.maxSourcesPerOutput) {
                return std::unexpected(StateError{
                    StateErrorKind::RoutingConstraintViolated,
                    detail::Format("Router Node {} output PortId {} has {} active sources (max is {})",
                                routerNodeId.value, outPort.value, count,
                                *router->constraints.maxSourcesPerOutput)
                });
            }
        }
    }

    // Check maxDestinationsPerInput
    if (router->constraints.maxDestinationsPerInput.has_value()) {
        for (const auto& [inPort, count] : destinationsPerInput) {
            if (count > *router->constraints.maxDestinationsPerInput) {
                return std::unexpected(StateError{
                    StateErrorKind::RoutingConstraintViolated,
                    detail::Format("Router Node {} input PortId {} has {} active destinations (max is {})",
                                routerNodeId.value, inPort.value, count,
                                *router->constraints.maxDestinationsPerInput)
                });
            }
        }
    }

    return {};
}

std::expected<void, StateError> validateMeterValue(
    const Topology& topology,
    MeterId id,
    double value) {

    const Meter* targetMeter = nullptr;
    for (const auto& m : topology.meters) {
        if (m.id == id) {
            targetMeter = &m;
            break;
        }
    }

    if (!targetMeter) {
        return std::unexpected(StateError{
            StateErrorKind::NonexistentMeter,
            detail::Format("MeterId {} does not exist in topology", id.value)
        });
    }

    if (!std::isfinite(value) || value < targetMeter->domain.min || value > targetMeter->domain.max) {
        return std::unexpected(StateError{
            StateErrorKind::InvalidMeterValue,
            detail::Format("MeterId {} '{}' value {} out of domain [{}, {}] or non-finite",
                        id.value, targetMeter->name, value, targetMeter->domain.min, targetMeter->domain.max)
        });
    }

    return {};
}

// Complete device state snapshot validation
std::expected<void, StateError> validateState(
    const Topology& topology,
    const DeviceState& state) {

    // 1. Check revision match
    if (state.topologyRevision != topology.revision) {
        return std::unexpected(StateError{
            StateErrorKind::TopologyRevisionMismatch,
            detail::Format("DeviceState topologyRevision ({}) does not match Topology revision ({})",
                        state.topologyRevision, topology.revision)
        });
    }

    // 2. Check completeness for Parameters
    std::unordered_set<ParameterId> topoParams;
    for (const auto& param : topology.parameters) {
        topoParams.insert(param.id);
        auto it = state.parameters.find(param.id);
        if (it == state.parameters.end()) {
            if (std::holds_alternative<ActionDomain>(param.domain)) continue;
            return std::unexpected(StateError{
                StateErrorKind::MissingParameter,
                detail::Format("DeviceState is missing required ParameterId {} '{}'", param.id.value, param.name)
            });
        }
        if (auto res = validateParameterState(param, it->second); !res) {
            return res;
        }
    }
    for (const auto& [paramId, _] : state.parameters) {
        if (!topoParams.contains(paramId)) {
            return std::unexpected(StateError{
                StateErrorKind::NonexistentParameter,
                detail::Format("DeviceState contains extra ParameterId {} not in topology", paramId.value)
            });
        }
    }

    // 3. Check completeness for Routers
    std::unordered_set<NodeId> topoRouters;
    for (const auto& node : topology.nodes) {
        if (std::holds_alternative<RouterNode>(node.body)) {
            topoRouters.insert(node.id);
            auto it = state.routers.find(node.id);
            if (it == state.routers.end()) {
                return std::unexpected(StateError{
                    StateErrorKind::MissingRouter,
                    detail::Format("DeviceState is missing required Router NodeId {} '{}'", node.id.value, node.name)
                });
            }
            if (auto res = validateRouterState(topology, node.id, it->second); !res) {
                return res;
            }
        }
    }
    for (const auto& [nodeId, _] : state.routers) {
        if (!topoRouters.contains(nodeId)) {
            return std::unexpected(StateError{
                StateErrorKind::NonexistentRouter,
                detail::Format("DeviceState contains extra Router NodeId {} not in topology", nodeId.value)
            });
        }
    }

    // 4. Check completeness for Meters
    std::unordered_set<MeterId> topoMeters;
    for (const auto& meter : topology.meters) {
        topoMeters.insert(meter.id);
        auto it = state.meters.find(meter.id);
        if (it == state.meters.end()) {
            return std::unexpected(StateError{
                StateErrorKind::MissingMeter,
                detail::Format("DeviceState is missing required MeterId {} '{}'", meter.id.value, meter.name)
            });
        }
        if (auto res = validateMeterValue(topology, meter.id, it->second); !res) {
            return res;
        }
    }
    for (const auto& [meterId, _] : state.meters) {
        if (!topoMeters.contains(meterId)) {
            return std::unexpected(StateError{
                StateErrorKind::NonexistentMeter,
                detail::Format("DeviceState contains extra MeterId {} not in topology", meterId.value)
            });
        }
    }

    return {};
}

} // namespace ASFW::AudioModel
