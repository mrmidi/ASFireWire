#include "SelfIDTopologyNormalizer.hpp"

#include <algorithm>

namespace ASFW::Driver {

std::expected<PhysicalTopologyGraph, TopologyBuildError>
SelfIDTopologyNormalizer::BuildPhysicalGraph(const std::vector<SelfIDNodeRecord>& records,
                                             uint8_t localPhysicalId) {
    if (records.empty()) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::EmptySequenceSet,
            "No Self-ID node records are available"});
    }

    const uint8_t rootId = records.back().physicalId;

    PhysicalTopologyGraph graph{};
    graph.rootId = rootId;
    graph.localId = localPhysicalId;
    graph.nodeCount = static_cast<uint8_t>(records.size());
    graph.nodes.reserve(records.size());

    for (const SelfIDNodeRecord& record : records) {
        TopologyNodeRecord node{};
        node.physicalId = record.physicalId;
        node.linkActive = record.linkActive;
        node.contender = record.contender;
        node.initiatedReset = record.initiatedReset;
        node.gapCount = record.gapCount;
        node.powerClass = record.powerClass;
        node.maxSpeedMbps = record.maxSpeedMbps;
        node.baseRaw = record.baseRaw;
        node.reportedPorts = record.ports;
        node.portCount = record.portCount;
        node.isRoot = record.physicalId == rootId;
        node.isLocal = record.physicalId == localPhysicalId;
        graph.nodes.push_back(node);
    }

    std::vector<uint8_t> unresolvedChildren;
    unresolvedChildren.reserve(records.size());

    // IEEE 1394-2008 Annex P, Table P.1:
    // Process self-ID records in increasing physical_ID order. For every
    // connected child port on the current node, pop the physical_ID of a node
    // with an unresolved parent connection from the stack. Child ports are
    // processed in decreasing port-number order.
    for (uint8_t id = 0; id <= rootId; ++id) {
        if (id >= graph.nodes.size() || graph.nodes[id].physicalId != id) {
            return std::unexpected(TopologyBuildError{
                TopologyBuildErrorCode::NonContiguousPhysicalIds,
                "Physical IDs are not contiguous from 0 to root"});
        }

        TopologyNodeRecord& current = graph.nodes[id];

        for (int port = static_cast<int>(kMaxPhyPorts) - 1; port >= 0; --port) {
            if (current.reportedPorts[static_cast<size_t>(port)] != PortState::Child) {
                continue;
            }

            if (unresolvedChildren.empty()) {
                return std::unexpected(TopologyBuildError{
                    TopologyBuildErrorCode::ChildPortWithEmptyStack,
                    "Child port encountered but unresolved-child stack is empty"});
            }

            const uint8_t childId = unresolvedChildren.back();
            unresolvedChildren.pop_back();

            if (childId >= graph.nodes.size()) {
                return std::unexpected(TopologyBuildError{
                    TopologyBuildErrorCode::NonContiguousPhysicalIds,
                    "Unresolved child physical ID is outside graph node array"});
            }

            TopologyNodeRecord& child = graph.nodes[childId];
            const std::optional<uint8_t> childParentPort = FirstUnresolvedParentPort(child);
            if (!childParentPort.has_value()) {
                return std::unexpected(TopologyBuildError{
                    TopologyBuildErrorCode::PoppedNodeHasNoUnresolvedParent,
                    "Popped child node has no unresolved Parent port"});
            }

            ConnectBidirectional(current,
                                 static_cast<uint8_t>(port),
                                 child,
                                 *childParentPort);
        }

        if (id < rootId) {
            if (!FirstUnresolvedParentPort(current).has_value()) {
                return std::unexpected(TopologyBuildError{
                    TopologyBuildErrorCode::NonRootWithoutParentPort,
                    "Non-root node has no Parent port after child links were resolved"});
            }

            unresolvedChildren.push_back(id);
        }
    }

    if (!unresolvedChildren.empty()) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::UnresolvedStackAfterRoot,
            "Unresolved child stack is not empty after processing the root node"});
    }

    // IRM election: highest physical ID contender.
    graph.irmId = kInvalidPhysicalId;
    for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
        if (it->contender) {
            graph.irmId = it->physicalId;
            it->isIRM = true;
            break;
        }
    }

    return graph;
}

std::expected<NormalizedTopologyGraph, TopologyBuildError>
SelfIDTopologyNormalizer::NormalizeFromLocal(const PhysicalTopologyGraph& physical,
                                             uint8_t localPhysicalId) {
    if (localPhysicalId == kInvalidPhysicalId || localPhysicalId >= physical.nodes.size()) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::LocalNodeUnavailable,
            "Local physical ID is unavailable or outside physical graph"});
    }

    NormalizedTopologyGraph normalized{};
    normalized.observerPhysicalId = localPhysicalId;
    normalized.nodes.resize(physical.nodes.size());

    for (const TopologyNodeRecord& physicalNode : physical.nodes) {
        NormalizedNode& node = normalized.nodes[physicalNode.physicalId];
        node.physicalId = physicalNode.physicalId;
        node.isObserver = physicalNode.physicalId == localPhysicalId;
    }

    std::vector<bool> visited(physical.nodes.size(), false);
    std::vector<uint8_t> stack;
    stack.push_back(localPhysicalId);
    visited[localPhysicalId] = true;

    while (!stack.empty()) {
        const uint8_t currentId = stack.back();
        stack.pop_back();

        const TopologyNodeRecord& current = physical.nodes[currentId];

        for (uint8_t port = 0; port < kMaxPhyPorts; ++port) {
            const TopologyPortLink& link = current.links[static_cast<size_t>(port)];
            if (!link.connected) {
                continue;
            }

            const uint8_t remoteId = link.remoteNodeId;
            const uint8_t remotePort = link.remotePort;

            if (remoteId >= physical.nodes.size()) {
                return std::unexpected(TopologyBuildError{
                    TopologyBuildErrorCode::ReciprocalLinkMissing,
                    "Physical link references a remote node outside the graph"});
            }

            if (visited[remoteId]) {
                continue;
            }

            visited[remoteId] = true;
            stack.push_back(remoteId);

            NormalizedNode& currentNorm = normalized.nodes[currentId];
            NormalizedNode& remoteNorm = normalized.nodes[remoteId];

            currentNorm.ports[static_cast<size_t>(port)] = NormalizedPortLink{
                .connected = true,
                .remotePhysicalId = remoteId,
                .remotePort = remotePort,
                .normalizedState = PortState::Child,
            };

            remoteNorm.ports[static_cast<size_t>(remotePort)] = NormalizedPortLink{
                .connected = true,
                .remotePhysicalId = currentId,
                .remotePort = port,
                .normalizedState = PortState::Parent,
            };
        }
    }

    for (bool nodeVisited : visited) {
        if (!nodeVisited) {
            return std::unexpected(TopologyBuildError{
                TopologyBuildErrorCode::EdgeCountMismatch,
                "Physical graph is disconnected from local observer"});
        }
    }

    return normalized;
}

std::optional<uint8_t>
SelfIDTopologyNormalizer::FirstUnresolvedParentPort(const TopologyNodeRecord& node) noexcept {
    for (uint8_t port = 0; port < kMaxPhyPorts; ++port) {
        const size_t index = static_cast<size_t>(port);
        if (node.reportedPorts[index] != PortState::Parent) {
            continue;
        }
        if (!node.links[index].connected) {
            return port;
        }
    }

    return std::nullopt;
}

void SelfIDTopologyNormalizer::ConnectBidirectional(TopologyNodeRecord& parent,
                                                    uint8_t parentPort,
                                                    TopologyNodeRecord& child,
                                                    uint8_t childPort) noexcept {
    parent.links[static_cast<size_t>(parentPort)] = TopologyPortLink{
        .connected = true,
        .remoteNodeId = child.physicalId,
        .remotePort = childPort,
    };

    child.links[static_cast<size_t>(childPort)] = TopologyPortLink{
        .connected = true,
        .remoteNodeId = parent.physicalId,
        .remotePort = parentPort,
    };
}

} // namespace ASFW::Driver
