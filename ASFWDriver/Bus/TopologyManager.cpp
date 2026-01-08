#include "TopologyManager.hpp"

#include <algorithm>
#include <array>
#include <map>
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
std::optional<uint8_t> FindRootNode(const std::vector<TopologyNode>& nodes) {
    std::optional<uint8_t> rootId;

    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (it->linkActive && it->portCount > 0) {
            bool hasParentPort = false;
            for (const auto& state : it->portStates) {
                if (state == PortState::Parent) {
                    hasParentPort = true;
                    break;
                }
            }
            if (!hasParentPort) {
                rootId = it->nodeId;
                break;
            }
        }
    }

    if (!rootId.has_value()) {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            if (it->linkActive && it->portCount > 0 && it->isIRMCandidate) {
                rootId = it->nodeId;
                break;
            }
        }
    }

    if (!rootId.has_value()) {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            if (it->linkActive && it->portCount > 0) {
                rootId = it->nodeId;
                break;
            }
        }
    }

    return rootId;
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

        const TopologyNode* currentNode = nullptr;
        for (const auto& node : nodes) {
            if (node.nodeId == currentNodeId) {
                currentNode = &node;
                break;
            }
        }

        if (!currentNode) {
            continue;
        }

        for (const uint8_t childId : currentNode->childNodeIds) {
            if (hopCount.find(childId) == hopCount.end()) {
                const uint8_t childHops = currentHops + 1;
                hopCount[childId] = childHops;
                queue.push_back(childId);

                if (childHops > maxHops) {
                    maxHops = childHops;
                }
            }
        }
    }

    return maxHops;
}

void ValidateTopology(const std::vector<TopologyNode>& nodes, std::vector<std::string>& warnings) {
    if (nodes.empty()) {
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
            const TopologyNode* child = nullptr;
            for (const auto& node : nodes) {
                if (node.nodeId == childId) {
                    child = &node;
                    break;
                }
            }

            if (!child) {
                warnings.push_back("Node " + std::to_string(parent.nodeId) +
                                 " has child " + std::to_string(childId) +
                                 " which doesn't exist");
                continue;
            }

            bool hasReciprocalLink = std::find(
                child->parentNodeIds.begin(),
                child->parentNodeIds.end(),
                parent.nodeId
            ) != child->parentNodeIds.end();

            if (!hasReciprocalLink) {
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

void BuildTreeLinks(std::vector<TopologyNode>& nodes, std::vector<std::string>& warnings) {
    for (auto& node : nodes) {
        node.parentNodeIds.clear();
        node.childNodeIds.clear();
    }

    uint32_t edgesConstructed = 0;
    uint32_t orphanedPorts = 0;

    for (size_t i = 0; i < nodes.size(); ++i) {
        TopologyNode& nodeA = nodes[i];

        for (size_t portA = 0; portA < nodeA.portStates.size(); ++portA) {
            if (nodeA.portStates[portA] == PortState::Parent) {
                bool foundMatch = false;

                for (size_t j = 0; j < nodes.size(); ++j) {
                    if (i == j) continue;
                    TopologyNode& nodeB = nodes[j];

                    for (size_t portB = 0; portB < nodeB.portStates.size(); ++portB) {
                        if (nodeB.portStates[portB] == PortState::Child) {
                            bool alreadyConnected = std::find(
                                nodeB.parentNodeIds.begin(),
                                nodeB.parentNodeIds.end(),
                                nodeA.nodeId
                            ) != nodeB.parentNodeIds.end();

                            if (!alreadyConnected) {
                                nodeA.childNodeIds.push_back(nodeB.nodeId);
                                nodeB.parentNodeIds.push_back(nodeA.nodeId);
                                edgesConstructed++;
                                foundMatch = true;
                                break;
                            }
                        }
                    }
                    if (foundMatch) break;
                }

                if (!foundMatch) {
                    orphanedPorts++;
                    warnings.push_back("Orphaned Parent port on node " +
                                     std::to_string(nodeA.nodeId) + " port " +
                                     std::to_string(portA));
                }
            }
        }
    }

    if (nodes.size() > 0 && edgesConstructed != (nodes.size() - 1)) {
        warnings.push_back("Edge count " + std::to_string(edgesConstructed) +
                         " != expected " + std::to_string(nodes.size() - 1) +
                         " for tree structure");
    }

    if (orphanedPorts > 0) {
        warnings.push_back("Found " + std::to_string(orphanedPorts) +
                         " orphaned Parent ports");
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

std::optional<TopologySnapshot> TopologyManager::UpdateFromSelfID(const SelfIDCapture::Result& result,
                                                                 uint64_t timestamp,
                                                                 uint32_t nodeIDReg) {
    if (!result.valid || result.quads.empty()) {
        ASFW_LOG(Topology, "Self-ID result invalid (crc=%d timeout=%d)",
               result.crcError, result.timedOut);
        return latest_;
    }
    
    if (result.sequences.empty()) {
        ASFW_LOG(Topology, "Self-ID has quadlets but no valid sequences - invalid data");
        return latest_;
    }
    
    // Extract bus/node from NodeID register if valid
    // OHCI NodeID[31] = IDValid; low 16 bits are IEEE 1394 Node_ID: [15:6]=bus, [5:0]=node.
    std::optional<uint8_t> localNodeId;
    std::optional<uint16_t> busNumber;
    uint16_t busBase16 = 0;
    if (nodeIDReg & 0x80000000u) {  // IDValid
        const uint16_t node_id_16 = static_cast<uint16_t>(nodeIDReg & 0xFFFFu);
        const uint8_t nodeNum = static_cast<uint8_t>(node_id_16 & 0x3Fu);
        const uint16_t busNum = static_cast<uint16_t>((node_id_16 >> 6) & 0x3FFu);
        busBase16 = static_cast<uint16_t>(node_id_16 & 0xFFC0u); // (bus<<6)
        if (nodeNum != 63) {
            localNodeId = nodeNum;
        }
        busNumber = busNum;
    }

    std::vector<std::string> warnings;
    std::map<uint8_t, NodeAccumulator> accumulators;

    // Iterate pre-parsed and validated Self-ID sequences (start index + quadlet count)
    for (const auto& seq : result.sequences) {
        const size_t start = seq.first;
        const unsigned int quadlet_count = seq.second;
        for (unsigned int i = 0; i < quadlet_count; ++i) {
            const uint32_t raw = result.quads[start + i];

            // base quadlet (i == 0) contains primary fields
            const uint8_t phyId = ExtractPhyID(raw);
            auto& node = accumulators[phyId];
            node.phyId = phyId;

            if (i == 0) {
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
            } else {
                // extended quadlet: sequence number is encoded in the quad, but
                // enumerator already validated sequence ordering.
                const uint32_t sequence = ExtractSeq(raw);
                const size_t baseIndex = 3u + static_cast<size_t>(sequence) * 4u;
                for (size_t slot = 0; slot < 4; ++slot) {
                    const size_t portIndex = baseIndex + slot;
                    const uint32_t code = (raw >> (slot * 2)) & 0x3u;
                    StorePort(node, portIndex, DecodePort(code));
                }
            }
        }
    }

    TopologySnapshot snapshot;
    snapshot.generation = result.generation;
    snapshot.capturedAt = timestamp;
    
    // Store Self-ID raw data for GUI export
    snapshot.selfIDData.rawQuadlets = result.quads;
    snapshot.selfIDData.sequences = result.sequences;
    snapshot.selfIDData.generation = result.generation;
    snapshot.selfIDData.captureTimestamp = timestamp;
    snapshot.selfIDData.valid = result.valid;
    snapshot.selfIDData.timedOut = result.timedOut;
    snapshot.selfIDData.crcError = result.crcError;

    snapshot.nodes.reserve(accumulators.size());
    for (auto& entry : accumulators) {
        const auto& node = entry.second;
        if (!node.haveBase) {
            continue;
        }

        TopologyNode topo{};
        topo.nodeId = node.phyId;
        topo.isIRMCandidate = node.contender;
        topo.linkActive = node.linkActive;
        topo.initiatedReset = node.initiatedReset;
        topo.gapCount = node.gapCount;
        topo.powerClass = node.powerClass;
        topo.maxSpeedMbps = DecodeSpeed(node.speedCode);
        topo.portCount = static_cast<uint8_t>(std::count_if(node.ports.begin(), node.ports.end(), [](PortState state) {
            return state != PortState::NotPresent;
        }));
        topo.portStates = node.ports;  // Copy port states for GUI export
        
        // Determine parent port for tree structure
        for (size_t i = 0; i < node.ports.size(); ++i) {
            if (node.ports[i] == PortState::Parent) {
                topo.parentPort = static_cast<uint8_t>(i);
                break;
            }
        }
        
        snapshot.nodes.push_back(std::move(topo));
    }

    std::sort(snapshot.nodes.begin(), snapshot.nodes.end(), [](const TopologyNode& lhs, const TopologyNode& rhs) {
        return lhs.nodeId < rhs.nodeId;
    });

    // Build tree structure by matching parent/child ports (IEEE 1394-2008 Annex P)
    BuildTreeLinks(snapshot.nodes, warnings);

    // Validate topology consistency (tree structure requirements)
    ValidateTopology(snapshot.nodes, warnings);

    // Perform topology analysis per IEEE 1394-1995 §8.4
    snapshot.nodeCount = static_cast<uint8_t>(snapshot.nodes.size());
    snapshot.rootNodeId = FindRootNode(snapshot.nodes);
    snapshot.irmNodeId = FindIRMNode(snapshot.nodes);
    snapshot.localNodeId = localNodeId;
    snapshot.busBase16 = busBase16;
    snapshot.busNumber = busNumber;
    snapshot.gapCount = CalculateOptimumGapCount(accumulators);
    
    // Mark root node in topology
    if (snapshot.rootNodeId.has_value()) {
        for (auto& node : snapshot.nodes) {
            if (node.nodeId == *snapshot.rootNodeId) {
                node.isRoot = true;
                break;
            }
        }
        // Calculate maximum hop count from root (BFS traversal)
        snapshot.maxHopsFromRoot = CalculateMaxHops(snapshot.nodes, *snapshot.rootNodeId);
    } else {
        snapshot.maxHopsFromRoot = 0;
    }

    // Log topology analysis results with rich context
    const std::string rootStr = snapshot.rootNodeId.has_value() ? std::to_string(*snapshot.rootNodeId) : std::string("none");
    const std::string irmStr = snapshot.irmNodeId.has_value() ? std::to_string(*snapshot.irmNodeId) : std::string("none");
    const std::string localStr = snapshot.localNodeId.has_value() ? std::to_string(*snapshot.localNodeId) : std::string("none");
    const std::string busStr = snapshot.busNumber.has_value() ? std::to_string(*snapshot.busNumber) : std::string("none");

    ASFW_LOG(Topology, "=== 🗺️ Topology Snapshot ===");
    ASFW_LOG(Topology, "🧮 gen=%u nodes=%u root=%{public}s IRM=%{public}s local=%{public}s bus=%{public}s gap=%u maxHops=%u",
             snapshot.generation,
             snapshot.nodeCount,
             rootStr.c_str(),
             irmStr.c_str(),
             localStr.c_str(),
             busStr.c_str(),
             snapshot.gapCount,
             snapshot.maxHopsFromRoot);

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

    if (!snapshot.rootNodeId.has_value()) {
        ASFW_LOG(Topology, "⚠️  WARNING: No root node found (no active nodes with ports)");
    }
    if (!snapshot.irmNodeId.has_value()) {
        ASFW_LOG(Topology, "⚠️  WARNING: No IRM candidate found (no contender nodes)");
    }
    if (!snapshot.busNumber.has_value()) {
        ASFW_LOG(Topology, "⚠️  WARNING: Bus number is unknown (NodeID.IDValid=0) — defer async reads until valid");
    }
    
    unsigned int resetInitiators = 0;
    for (const auto& node : snapshot.nodes) {
        if (node.initiatedReset) {
            resetInitiators++;
            ASFW_LOG(Topology, "🌀 Node %u initiated bus reset", node.nodeId);
        }
    }

    if (resetInitiators > 1) {
        ASFW_LOG(Topology, "⚠️  WARNING: Multiple nodes (%u) initiated bus reset - check cabling/power", resetInitiators);
    }

    unsigned int totalActivePorts = 0;
    for (const auto& node : snapshot.nodes) {
        if (node.linkActive) {
            totalActivePorts += node.portCount;
        }
    }
    if (totalActivePorts == 0 && snapshot.nodeCount > 0) {
        ASFW_LOG(Topology, "⚠️  WARNING: Zero active ports detected - nodes may be isolated");
    }

    for (const auto& warning : warnings) {
        ASFW_LOG(Topology, "⚠️ %{public}s", warning.c_str());
    }
    
    // Store warnings in snapshot for GUI export
    snapshot.warnings = warnings;

    latest_ = snapshot;
    return latest_;
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

    // Per IEEE 1394-1995 §8.4.6.2.2, Self-ID packet format:
    // Bits[31:30] = 10 (Self-ID packet identifier)
    // Bits[29:24] = Physical ID (node ID)
    // Bits[23:22] = Packet number (00 for packet 0)
    // Bits[21:16] = Gap count (6 bits) ← We extract this
    // Bits[15:0]  = Other fields (link active, contender, etc.)
    //
    // Gap count is in bits 16-21 of packet 0 (the first packet in each sequence)

    constexpr uint32_t kSelfIDIdentifier = 0x2;       // bits[31:30] = 10
    constexpr uint32_t kPacketNumber0 = 0x0;          // bits[23:22] = 00
    constexpr uint32_t kGapCountMask = 0x003F0000;    // bits[21:16]
    constexpr uint32_t kGapCountShift = 16;

    for (uint32_t packet : selfIDs) {
        // Check if this is a Self-ID packet (bits 31:30 == 10)
        uint32_t identifier = (packet >> 30) & 0x3;
        if (identifier != kSelfIDIdentifier) {
            continue;  // Not a Self-ID packet (might be padding)
        }

        // Check if this is packet 0 in the sequence (bits 23:22 == 00)
        uint32_t packetNum = (packet >> 22) & 0x3;
        if (packetNum != kPacketNumber0) {
            continue;  // Packet 1, 2, or 3 - gap count only in packet 0
        }

        // Extract gap count (bits 21:16)
        uint8_t gapCount = static_cast<uint8_t>((packet & kGapCountMask) >> kGapCountShift);
        gaps.push_back(gapCount);
    }

    return gaps;
}

} // namespace ASFW::Driver
