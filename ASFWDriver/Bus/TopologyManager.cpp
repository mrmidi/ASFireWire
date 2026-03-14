#include "TopologyManager.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "../Logging/Logging.hpp"
#include "TopologyTypes.hpp"

namespace {

using namespace ASFW::Driver;

constexpr size_t kMaxPorts = 128;

// keep small internal aggregators local to the compilation unit
struct NodeAccumulator {
    uint8_t phyId{0};
    bool haveBase{false};
    bool linkActive{false};
    bool contender{false};
    bool initiatedReset{false};
    uint8_t gapCount{0};
    uint8_t powerClass{0};
    uint32_t speedCode{0};
    std::vector<PortState> ports;
};

void StorePort(NodeAccumulator& node, size_t index, PortState state) {
    if (index >= kMaxPorts) {
        return; // Silently ignore ports beyond cap
    }
    if (node.ports.size() <= index) {
        node.ports.resize(index + 1, PortState::NotPresent);
    }
    node.ports[index] = state;
}

// Per IEEE 1394-1995 §8.4.3.2: Root node identification
struct NodeIDRegisterInfo {
    std::optional<uint8_t> localNodeId;
    std::optional<uint16_t> busNumber;
    uint16_t busBase16{0};
};

using TopologyBuildError = TopologyManager::TopologyBuildError;
using TopologyBuildErrorCode = TopologyManager::TopologyBuildErrorCode;

[[nodiscard]] bool NodeHasPortState(const TopologyNode& node, PortState state) {
    return std::find(node.portStates.begin(), node.portStates.end(), state) != node.portStates.end();
}

[[nodiscard]] const TopologyNode* FindNodeById(const std::vector<TopologyNode>& nodes,
                                               const uint8_t nodeId) {
    const auto it = std::find_if(nodes.begin(), nodes.end(), [nodeId](const TopologyNode& node) {
        return node.nodeId == nodeId;
    });
    return it == nodes.end() ? nullptr : &*it;
}

[[nodiscard]] bool HasReciprocalParentLink(const TopologyNode& child, const uint8_t parentNodeId) {
    return std::find(child.parentNodeIds.begin(), child.parentNodeIds.end(), parentNodeId) !=
           child.parentNodeIds.end();
}

template <typename Predicate>
[[nodiscard]] std::optional<uint8_t> FindLastMatchingNodeId(const std::vector<TopologyNode>& nodes,
                                                            Predicate predicate) {
    const auto it = std::find_if(nodes.rbegin(), nodes.rend(), predicate);
    if (it == nodes.rend()) {
        return std::nullopt;
    }
    return it->nodeId;
}

std::optional<uint8_t> FindRootNode(const std::vector<TopologyNode>& nodes) {
    if (const auto rootId = FindLastMatchingNodeId(
            nodes, [](const TopologyNode& node) {
                return node.linkActive && node.portCount > 0 &&
                       !NodeHasPortState(node, PortState::Parent);
            });
        rootId.has_value()) {
        return rootId;
    }

    if (const auto rootId = FindLastMatchingNodeId(
            nodes, [](const TopologyNode& node) {
                return node.linkActive && node.portCount > 0 && node.isIRMCandidate;
            });
        rootId.has_value()) {
        return rootId;
    }

    if (const auto rootId = FindLastMatchingNodeId(
            nodes, [](const TopologyNode& node) { return node.linkActive && node.portCount > 0; });
        rootId.has_value()) {
        return rootId;
    }

    return FindLastMatchingNodeId(nodes,
                                  [](const TopologyNode& node) { return node.linkActive; });
}

std::optional<uint8_t> FindIRMNode(const std::vector<TopologyNode>& nodes) {
    std::optional<uint8_t> irmId;
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (it->isIRMCandidate) {
            irmId = it->nodeId;
            break;
        }
    }
    return irmId;
}

uint8_t CalculateOptimumGapCount(const std::map<uint8_t, NodeAccumulator>& accumulators) {
    uint8_t maxGap = 0;
    for (const auto& entry : accumulators) {
        if (entry.second.haveBase && entry.second.gapCount > maxGap) {
            maxGap = entry.second.gapCount;
        }
    }
    return maxGap > 63 ? 63 : maxGap;
}

uint8_t CalculateMaxHops(const std::vector<TopologyNode>& nodes, uint8_t rootNodeId) {
    if (nodes.empty()) {
        return 0;
    }

    std::map<uint8_t, uint8_t> hopCount;
    std::vector<uint8_t> queue;

    hopCount[rootNodeId] = 0;
    queue.push_back(rootNodeId);

    uint8_t maxHops = 0;
    size_t queueHead = 0;

    while (queueHead < queue.size()) {
        const uint8_t currentNodeId = queue[queueHead++];
        const uint8_t currentHops = hopCount[currentNodeId];
        const TopologyNode* currentNode = FindNodeById(nodes, currentNodeId);

        if (!currentNode) {
            continue;
        }

        for (const uint8_t childId : currentNode->childNodeIds) {
            if (hopCount.find(childId) != hopCount.end()) {
                continue;
            }

            const uint8_t childHops = currentHops + 1;
            hopCount[childId] = childHops;
            queue.push_back(childId);
            maxHops = std::max(maxHops, childHops);
        }
    }

    return maxHops;
}

[[nodiscard]] bool HasExplicitTreePorts(const std::vector<TopologyNode>& nodes) {
    for (const auto& node : nodes) {
        for (const auto state : node.portStates) {
            if (state == PortState::Parent || state == PortState::Child) {
                return true;
            }
        }
    }
    return false;
}

void ValidateTopology(const std::vector<TopologyNode>& nodes, std::vector<std::string>& warnings) {
    if (nodes.empty()) {
        return;
    }

    if (!HasExplicitTreePorts(nodes)) {
        return;
    }

    uint32_t rootCount = 0;
    for (const auto& node : nodes) {
        if (node.parentNodeIds.empty()) {
            rootCount++;
        }
    }

    if (rootCount == 0) {
        warnings.push_back("No root node found (all nodes have parents - cycle detected)");
    } else if (rootCount > 1) {
        warnings.push_back("Multiple root nodes found (" + std::to_string(rootCount) +
                         ") - forest instead of tree");
    }

    for (const auto& parent : nodes) {
        for (const uint8_t childId : parent.childNodeIds) {
            const TopologyNode* child = FindNodeById(nodes, childId);

            if (!child) {
                warnings.push_back("Node " + std::to_string(parent.nodeId) +
                                 " has child " + std::to_string(childId) +
                                 " which doesn't exist");
                continue;
            }

            if (!HasReciprocalParentLink(*child, parent.nodeId)) {
                warnings.push_back("Node " + std::to_string(parent.nodeId) +
                                 " → " + std::to_string(childId) +
                                 " missing reciprocal parent link");
            }
        }
    }

    uint32_t totalEdges = 0;
    for (const auto& node : nodes) {
        totalEdges += static_cast<uint32_t>(node.childNodeIds.size());
    }

    const uint32_t expectedEdges = static_cast<uint32_t>(nodes.size()) - 1;
    if (totalEdges != expectedEdges) {
        warnings.push_back("Edge count mismatch: " + std::to_string(totalEdges) +
                         " edges for " + std::to_string(nodes.size()) +
                         " nodes (expected " + std::to_string(expectedEdges) + ")");
    }
}

[[nodiscard]] bool HasContiguousNodeCoverage(const std::vector<TopologyNode>& nodes) {
    if (nodes.empty()) {
        return false;
    }

    uint8_t expectedNodeId = 0;
    for (const auto& node : nodes) {
        if (node.nodeId != expectedNodeId) {
            return false;
        }
        ++expectedNodeId;
    }
    return true;
}

[[nodiscard]] std::string JoinWarnings(const std::vector<std::string>& warnings) {
    return std::accumulate(
        warnings.begin(), warnings.end(), std::string{},
        [](std::string acc, const std::string& warning) {
            if (!acc.empty()) {
                acc.append("; ");
            }
            acc.append(warning);
            return acc;
        });
}

void ResetTreeLinks(std::vector<TopologyNode>& nodes) {
    for (auto& node : nodes) {
        node.parentNodeIds.clear();
        node.childNodeIds.clear();
    }
}

[[nodiscard]] bool IsAlreadyConnected(const TopologyNode& child, uint8_t parentNodeId) {
    return std::find(child.parentNodeIds.begin(), child.parentNodeIds.end(), parentNodeId) !=
           child.parentNodeIds.end();
}

[[nodiscard]] std::optional<size_t> FindUnlinkedChildNodeIndex(
    const std::vector<TopologyNode>& nodes, size_t parentIndex, uint8_t parentNodeId) {
    for (size_t candidateIndex = 0; candidateIndex < nodes.size(); ++candidateIndex) {
        if (candidateIndex == parentIndex) {
            continue;
        }

        const auto& candidate = nodes[candidateIndex];
        if (!NodeHasPortState(candidate, PortState::Child)) {
            continue;
        }
        if (!IsAlreadyConnected(candidate, parentNodeId)) {
            return candidateIndex;
        }
    }

    return std::nullopt;
}

void ConnectTreeNodes(TopologyNode& parent, TopologyNode& child, uint32_t& edgesConstructed) {
    parent.childNodeIds.push_back(child.nodeId);
    child.parentNodeIds.push_back(parent.nodeId);
    ++edgesConstructed;
}

void RecordOrphanedParentPort(const TopologyNode& node, size_t portIndex,
                              std::vector<std::string>& warnings,
                              uint32_t& orphanedPorts) {
    ++orphanedPorts;
    warnings.push_back("Orphaned Parent port on node " + std::to_string(node.nodeId) + " port " +
                       std::to_string(portIndex));
}

void AppendTreeLinkWarnings(size_t nodeCount, uint32_t edgesConstructed, uint32_t orphanedPorts,
                            std::vector<std::string>& warnings) {
    if (nodeCount > 0 && edgesConstructed != (nodeCount - 1)) {
        warnings.push_back("Edge count " + std::to_string(edgesConstructed) + " != expected " +
                           std::to_string(nodeCount - 1) + " for tree structure");
    }

    if (orphanedPorts > 0) {
        warnings.push_back("Found " + std::to_string(orphanedPorts) + " orphaned Parent ports");
    }
}

void BuildTreeLinks(std::vector<TopologyNode>& nodes, std::vector<std::string>& warnings) {
    ResetTreeLinks(nodes);
    if (!HasExplicitTreePorts(nodes)) {
        return;
    }

    uint32_t edgesConstructed = 0;
    uint32_t orphanedPorts = 0;

    for (size_t parentIndex = 0; parentIndex < nodes.size(); ++parentIndex) {
        auto& parent = nodes[parentIndex];
        for (size_t portIndex = 0; portIndex < parent.portStates.size(); ++portIndex) {
            if (parent.portStates[portIndex] != PortState::Parent) {
                continue;
            }

            const auto childIndex =
                FindUnlinkedChildNodeIndex(nodes, parentIndex, parent.nodeId);
            if (!childIndex.has_value()) {
                RecordOrphanedParentPort(parent, portIndex, warnings, orphanedPorts);
                continue;
            }

            ConnectTreeNodes(parent, nodes[*childIndex], edgesConstructed);
        }
    }

    AppendTreeLinkWarnings(nodes.size(), edgesConstructed, orphanedPorts, warnings);
}

[[nodiscard]] NodeIDRegisterInfo DecodeNodeIDRegister(uint32_t nodeIDReg) {
    NodeIDRegisterInfo info{};
    if ((nodeIDReg & 0x80000000u) == 0) {
        return info;
    }

    const uint16_t nodeID = static_cast<uint16_t>(nodeIDReg & 0xFFFFu);
    const uint8_t nodeNum = static_cast<uint8_t>(nodeID & 0x3Fu);
    info.busBase16 = static_cast<uint16_t>(nodeID & 0xFFC0u);
    info.busNumber = static_cast<uint16_t>((nodeID >> 6) & 0x3FFu);
    if (nodeNum != 63) {
        info.localNodeId = nodeNum;
    }
    return info;
}

void ApplyBaseQuadlet(NodeAccumulator& node, uint32_t raw) {
    node.haveBase = true;
    node.linkActive = IsLinkActive(raw);
    node.contender = IsContender(raw);
    node.initiatedReset = IsInitiatedReset(raw);
    node.gapCount = ExtractGapCount(raw);
    node.powerClass = static_cast<uint8_t>(ExtractPowerClass(raw));
    node.speedCode = ExtractSpeedCode(raw);
    node.ports.clear();
    node.ports.reserve(3);
    StorePort(node, 0, ExtractPortState(raw, 0));
    StorePort(node, 1, ExtractPortState(raw, 1));
    StorePort(node, 2, ExtractPortState(raw, 2));
    if (!HasMorePackets(raw)) {
        node.ports.resize(3);
    }
}

void ApplyExtendedQuadlet(NodeAccumulator& node, uint32_t raw) {
    const uint32_t sequence = ExtractSeq(raw);
    const size_t baseIndex = 3u + static_cast<size_t>(sequence) * 4u;
    for (size_t slot = 0; slot < 4; ++slot) {
        const size_t portIndex = baseIndex + slot;
        const uint32_t code = (raw >> (slot * 2)) & 0x3u;
        StorePort(node, portIndex, DecodePort(code));
    }
}

[[nodiscard]] std::map<uint8_t, NodeAccumulator> BuildAccumulators(
    const SelfIDCapture::Result& result) {
    std::map<uint8_t, NodeAccumulator> accumulators;

    for (const auto& seq : result.sequences) {
        const size_t start = seq.first;
        const unsigned int quadletCount = seq.second;
        for (unsigned int quadletIndex = 0; quadletIndex < quadletCount; ++quadletIndex) {
            const uint32_t raw = result.quads[start + quadletIndex];
            const uint8_t phyId = ExtractPhyID(raw);
            auto& node = accumulators[phyId];
            node.phyId = phyId;

            if (quadletIndex == 0) {
                ApplyBaseQuadlet(node, raw);
                continue;
            }

            ApplyExtendedQuadlet(node, raw);
        }
    }

    return accumulators;
}

[[nodiscard]] uint8_t CountPresentPorts(const NodeAccumulator& node) {
    return static_cast<uint8_t>(
        std::count_if(node.ports.begin(), node.ports.end(), [](PortState state) {
            return state != PortState::NotPresent;
        }));
}

[[nodiscard]] std::optional<uint8_t> FindParentPortIndex(const NodeAccumulator& node) {
    for (size_t index = 0; index < node.ports.size(); ++index) {
        if (node.ports[index] == PortState::Parent) {
            return static_cast<uint8_t>(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] TopologyNode BuildTopologyNode(const NodeAccumulator& node) {
    TopologyNode topo{};
    topo.nodeId = node.phyId;
    topo.isIRMCandidate = node.contender;
    topo.linkActive = node.linkActive;
    topo.initiatedReset = node.initiatedReset;
    topo.gapCount = node.gapCount;
    topo.powerClass = node.powerClass;
    topo.maxSpeedMbps = DecodeSpeed(node.speedCode);
    topo.portCount = CountPresentPorts(node);
    topo.portStates = node.ports;
    if (const auto parentPort = FindParentPortIndex(node); parentPort.has_value()) {
        topo.parentPort = *parentPort;
    }
    return topo;
}

[[nodiscard]] TopologySnapshot InitializeSnapshot(const SelfIDCapture::Result& result,
                                                  uint64_t timestamp) {
    TopologySnapshot snapshot{};
    snapshot.generation = result.generation;
    snapshot.capturedAt = timestamp;
    snapshot.selfIDData.rawQuadlets = result.quads;
    snapshot.selfIDData.sequences = result.sequences;
    snapshot.selfIDData.generation = result.generation;
    snapshot.selfIDData.captureTimestamp = timestamp;
    snapshot.selfIDData.valid = result.valid;
    snapshot.selfIDData.timedOut = result.timedOut;
    snapshot.selfIDData.crcError = result.crcError;
    return snapshot;
}

void AppendTopologyNodes(const std::map<uint8_t, NodeAccumulator>& accumulators,
                         TopologySnapshot& snapshot) {
    snapshot.nodes.reserve(accumulators.size());
    for (const auto& [_, node] : accumulators) {
        if (!node.haveBase) {
            continue;
        }
        snapshot.nodes.push_back(BuildTopologyNode(node));
    }

    std::sort(snapshot.nodes.begin(), snapshot.nodes.end(),
              [](const TopologyNode& lhs, const TopologyNode& rhs) {
                  return lhs.nodeId < rhs.nodeId;
              });
}

void PopulateSnapshotAnalysis(TopologySnapshot& snapshot,
                              const std::map<uint8_t, NodeAccumulator>& accumulators,
                              const SelfIDCapture::Result& result,
                              const NodeIDRegisterInfo& nodeInfo) {
    snapshot.nodeCount = static_cast<uint8_t>(snapshot.nodes.size());
    snapshot.rootNodeId = FindRootNode(snapshot.nodes);
    snapshot.irmNodeId = FindIRMNode(snapshot.nodes);
    snapshot.localNodeId = nodeInfo.localNodeId;
    snapshot.busBase16 = nodeInfo.busBase16;
    snapshot.busNumber = nodeInfo.busNumber;
    snapshot.gapCount = CalculateOptimumGapCount(accumulators);

    const auto gaps = TopologyManager::ExtractGapCounts(result.quads);
    snapshot.gapCountConsistent =
        gaps.empty() ||
        std::adjacent_find(gaps.begin(), gaps.end(),
                           [](uint8_t lhs, uint8_t rhs) { return lhs != rhs; }) == gaps.end();
}

void MarkRootAndComputeHops(TopologySnapshot& snapshot) {
    if (!snapshot.rootNodeId.has_value()) {
        snapshot.maxHopsFromRoot = 0;
        return;
    }

    for (auto& node : snapshot.nodes) {
        if (node.nodeId == *snapshot.rootNodeId) {
            node.isRoot = true;
            break;
        }
    }
    snapshot.maxHopsFromRoot = CalculateMaxHops(snapshot.nodes, *snapshot.rootNodeId);
}

[[nodiscard]] std::expected<void, TopologyBuildError> ValidateSnapshot(
    const TopologySnapshot& snapshot, const std::vector<std::string>& warnings) {
    if (!HasContiguousNodeCoverage(snapshot.nodes)) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::MissingNodeCoverage,
            "Self-ID node coverage is not contiguous from the lowest observed node ID"});
    }

    if (!warnings.empty()) {
        return std::unexpected(
            TopologyBuildError{TopologyBuildErrorCode::TreeValidationFailed,
                               JoinWarnings(warnings)});
    }

    if (!snapshot.rootNodeId.has_value()) {
        return std::unexpected(
            TopologyBuildError{TopologyBuildErrorCode::NoRootNode,
                               "No root node could be derived from the validated Self-ID tree"});
    }

    return {};
}

void LogTopologySummary(const TopologySnapshot& snapshot) {
    const std::string rootStr =
        snapshot.rootNodeId.has_value() ? std::to_string(*snapshot.rootNodeId) : std::string("none");
    const std::string irmStr =
        snapshot.irmNodeId.has_value() ? std::to_string(*snapshot.irmNodeId) : std::string("none");
    const std::string localStr = snapshot.localNodeId.has_value()
                                     ? std::to_string(*snapshot.localNodeId)
                                     : std::string("none");
    const std::string busStr =
        snapshot.busNumber.has_value() ? std::to_string(*snapshot.busNumber) : std::string("none");

    ASFW_LOG(Topology, "=== 🗺️ Topology Snapshot ===");
    ASFW_LOG(Topology,
             "🧮 gen=%u nodes=%u root=%{public}s IRM=%{public}s local=%{public}s bus=%{public}s gap=%u maxHops=%u",
             snapshot.generation,
             snapshot.nodeCount,
             rootStr.c_str(),
             irmStr.c_str(),
             localStr.c_str(),
             busStr.c_str(),
             snapshot.gapCount,
             snapshot.maxHopsFromRoot);
}

unsigned int LogResetInitiators(const TopologySnapshot& snapshot) {
    unsigned int resetInitiators = 0;
    for (const auto& node : snapshot.nodes) {
        if (!node.initiatedReset) {
            continue;
        }
        ++resetInitiators;
        ASFW_LOG(Topology, "🌀 Node %u initiated bus reset", node.nodeId);
    }
    return resetInitiators;
}

[[nodiscard]] unsigned int CountActivePorts(const TopologySnapshot& snapshot) {
    unsigned int totalActivePorts = 0;
    for (const auto& node : snapshot.nodes) {
        if (node.linkActive) {
            totalActivePorts += node.portCount;
        }
    }
    return totalActivePorts;
}

void LogTopologyWarnings(const TopologySnapshot& snapshot) {
    if (!snapshot.irmNodeId.has_value()) {
        ASFW_LOG(Topology, "⚠️  WARNING: No IRM candidate found (no contender nodes)");
    }
    if (!snapshot.busNumber.has_value()) {
        ASFW_LOG(Topology, "⚠️  WARNING: Bus number is unknown (NodeID.IDValid=0) — defer async reads until valid");
    }

    const unsigned int resetInitiators = LogResetInitiators(snapshot);
    if (resetInitiators > 1) {
        ASFW_LOG(Topology,
                 "⚠️  WARNING: Multiple nodes (%u) initiated bus reset - check cabling/power",
                 resetInitiators);
    }

    if (CountActivePorts(snapshot) == 0 && snapshot.nodeCount > 0) {
        ASFW_LOG(Topology, "⚠️  WARNING: Zero active ports detected - nodes may be isolated");
    }
}

#if ASFW_DEBUG_TOPOLOGY
const char* PortStateEmoji(PortState state) {
    switch (state) {
        case PortState::Parent: return "⬆️";
        case PortState::Child: return "⬇️";
        case PortState::NotActive: return "⚪️";
        case PortState::NotPresent:
        default: return "▫️";
    }
}

const char* PortStateToString(PortState state) {
    switch (state) {
        case PortState::Parent: return "parent";
        case PortState::Child: return "child";
        case PortState::NotActive: return "inactive";
        case PortState::NotPresent:
        default: return "absent";
    }
}

std::string SummarizePorts(const std::vector<PortState>& ports) {
    std::string summary;
    for (size_t idx = 0; idx < ports.size(); ++idx) {
        const PortState state = ports[idx];
        if (state == PortState::NotPresent) {
            continue;
        }
        if (!summary.empty()) {
            summary.push_back(' ');
        }
        summary.append("p");
        summary.append(std::to_string(idx));
        summary.append("=");
        summary.append(PortStateToString(state));
        summary.append(PortStateEmoji(state));
    }
    if (summary.empty()) {
        summary = "none";
    }
    return summary;
}
#endif

} // namespace

namespace ASFW::Driver {

TopologyManager::TopologyManager() = default;

void TopologyManager::Reset() {
    latest_.reset();
}

void TopologyManager::InvalidateForBusReset() {
    latest_.reset();
    ClearBadIRMFlags();
}

const char* TopologyManager::TopologyBuildErrorCodeString(TopologyBuildErrorCode code) noexcept {
    switch (code) {
    case TopologyBuildErrorCode::InvalidSelfID:
        return "InvalidSelfID";
    case TopologyBuildErrorCode::EmptySequenceSet:
        return "EmptySequenceSet";
    case TopologyBuildErrorCode::MissingNodeCoverage:
        return "MissingNodeCoverage";
    case TopologyBuildErrorCode::NoRootNode:
        return "NoRootNode";
    case TopologyBuildErrorCode::TreeValidationFailed:
        return "TreeValidationFailed";
    }
    return "Unknown";
}

std::expected<TopologySnapshot, TopologyManager::TopologyBuildError>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
TopologyManager::UpdateFromSelfID(const SelfIDCapture::Result& result, uint64_t timestamp,
                                  uint32_t nodeIDReg) {
    if (!result.valid || result.quads.empty()) {
        ASFW_LOG(Topology, "Self-ID result invalid (crc=%d timeout=%d)",
               result.crcError, result.timedOut);
        return std::unexpected(TopologyBuildError{TopologyBuildErrorCode::InvalidSelfID,
                                                  "Self-ID result invalid"});
    }

    if (result.sequences.empty()) {
        ASFW_LOG(Topology, "Self-ID has quadlets but no valid sequences - invalid data");
        return std::unexpected(TopologyBuildError{TopologyBuildErrorCode::EmptySequenceSet,
                                                  "Self-ID sequence set is empty"});
    }

    const NodeIDRegisterInfo nodeInfo = DecodeNodeIDRegister(nodeIDReg);
    std::vector<std::string> warnings;
    const auto accumulators = BuildAccumulators(result);
    TopologySnapshot snapshot = InitializeSnapshot(result, timestamp);
    AppendTopologyNodes(accumulators, snapshot);

    // Build tree structure by matching parent/child ports (IEEE 1394-2008 Annex P)
    BuildTreeLinks(snapshot.nodes, warnings);

    // Validate topology consistency (tree structure requirements)
    ValidateTopology(snapshot.nodes, warnings);

    // Perform topology analysis per IEEE 1394-1995 §8.4
    PopulateSnapshotAnalysis(snapshot, accumulators, result, nodeInfo);
    MarkRootAndComputeHops(snapshot);

    if (auto validation = ValidateSnapshot(snapshot, warnings); !validation.has_value()) {
        return std::unexpected(validation.error());
    }

    // Log topology analysis results with rich context
    LogTopologySummary(snapshot);

#if ASFW_DEBUG_TOPOLOGY
    for (const auto& topoNode : snapshot.nodes) {
        const auto accIt = accumulators.find(topoNode.nodeId);
        const std::string portSummary = (accIt != accumulators.end()) ? SummarizePorts(accIt->second.ports)
                                                                       : std::string("unknown");

        std::string badges;
        if (topoNode.isRoot) {
            badges += "👑";
        }
        if (snapshot.irmNodeId && topoNode.nodeId == *snapshot.irmNodeId) {
            badges += "🏛️";
        }
        if (snapshot.localNodeId && topoNode.nodeId == *snapshot.localNodeId) {
            badges += "📍";
        }
        if (badges.empty()) {
            badges = "•";
        }

        const char* linkEmoji = topoNode.linkActive ? "✅" : "⬜️";
        const char* resetEmoji = topoNode.initiatedReset ? "🌀" : "";
        const char* contenderEmoji = topoNode.isIRMCandidate ? "🗳️" : "";

        ASFW_LOG_TOPOLOGY_DETAIL(
            "%{public}s Node %u: link=%{public}s speed=%uMb ports=%u (%{public}s) power=%{public}s gap=%u %{public}s%{public}s",
            badges.c_str(),
            topoNode.nodeId,
            linkEmoji,
            topoNode.maxSpeedMbps,
            topoNode.portCount,
            portSummary.c_str(),
            PowerClassToString(static_cast<PowerClass>(topoNode.powerClass)),
            topoNode.gapCount,
            contenderEmoji,
            resetEmoji);
    }
#endif
    ASFW_LOG(Topology, "=== End Topology Snapshot ===");
    LogTopologyWarnings(snapshot);

    snapshot.warnings = warnings;

    latest_ = snapshot;
    return snapshot;
}

std::optional<TopologySnapshot> TopologyManager::LatestSnapshot() const {
    if (latest_.has_value()) {
        // ASFW_LOG(Topology, "LatestSnapshot() called: returning gen=%u nodes=%u",
        //          latest_->generation, latest_->nodeCount);
    } else {
        // ASFW_LOG(Topology, "LatestSnapshot() called: no snapshot available (latest_ is nullopt)");
    }
    return latest_;
}

std::optional<TopologySnapshot> TopologyManager::CompareAndSwap(std::optional<TopologySnapshot> previous) {
    if (!latest_.has_value()) {
        return std::nullopt;
    }
    if (previous.has_value() && previous->capturedAt == latest_->capturedAt) {
        return std::nullopt;
    }
    return latest_;
}

// ============================================================================
// Bad IRM Tracking
// ============================================================================

void TopologyManager::MarkNodeAsBadIRM(uint8_t nodeID) {
    if (nodeID >= 63) {
        ASFW_LOG(Topology, "MarkNodeAsBadIRM: Invalid node ID %u (must be 0-62)", nodeID);
        return;
    }

    // Resize vector if needed (max 63 nodes on bus)
    if (badIRMFlags_.size() < 63) {
        badIRMFlags_.resize(63, false);
    }

    if (!badIRMFlags_[nodeID]) {
        ASFW_LOG(Topology, "⚠️  Node %u marked as bad IRM (failed verification)", nodeID);
        badIRMFlags_[nodeID] = true;
    }
}

bool TopologyManager::IsNodeBadIRM(uint8_t nodeID) const {
    if (nodeID >= badIRMFlags_.size()) {
        return false;
    }
    return badIRMFlags_[nodeID];
}

void TopologyManager::ClearBadIRMFlags() {
    if (!badIRMFlags_.empty()) {
        ASFW_LOG(Topology, "Clearing bad IRM flags (bus reset)");
        badIRMFlags_.clear();
        badIRMFlags_.resize(63, false);
    }
}

// ============================================================================
// Gap Count Extraction
// ============================================================================

std::vector<uint8_t> TopologyManager::ExtractGapCounts(const std::vector<uint32_t>& selfIDs) {
    std::vector<uint8_t> gaps;

    if (selfIDs.empty()) {
        return gaps;
    }

    // The base Self-ID quadlet carries the gap count in bits[21:16]. Extended
    // quadlets reuse high bits for sequence metadata, so we only extract from
    // the non-extended packet-0 form described by the wire-format helpers.
    constexpr uint32_t kGapCountMask = 0x003F0000;    // bits[21:16]
    constexpr uint32_t kGapCountShift = 16;

    for (uint32_t packet : selfIDs) {
        if (!IsSelfIDTag(packet) || IsExtended(packet)) {
            continue;
        }

        uint8_t gapCount = static_cast<uint8_t>((packet & kGapCountMask) >> kGapCountShift);
        gaps.push_back(gapCount);
    }

    return gaps;
}

} // namespace ASFW::Driver
