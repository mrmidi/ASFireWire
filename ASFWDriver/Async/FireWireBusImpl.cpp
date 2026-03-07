#include "FireWireBusImpl.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../Logging/Logging.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <queue>
#include <vector>

namespace ASFW::Async {

namespace {

void CompleteStaleGenerationAsync(IAsyncControllerPort& async,
                                  InterfaceCompletionCallback callback) {
    async.PostToWorkloop(^{
      if (callback) {
          callback(AsyncStatus::kStaleGeneration, std::span<const uint8_t>{});
      }
    });
}

[[nodiscard]] bool HasCurrentGeneration(IAsyncControllerPort& async, FW::Generation generation) {
    const auto busState = async.GetBusStateSnapshot();
    const FW::Generation current{busState.generation16};
    if (generation == current) {
        return true;
    }
    return false;
}

[[nodiscard]] uint16_t ResolveDestinationNodeId(const char* operation, FW::NodeId node,
                                                FWAddress addr) {
    const uint16_t addrNodeIdRaw = addr.nodeID;
    const uint8_t addrNodeNumber = static_cast<uint8_t>(addrNodeIdRaw & 0x3Fu);
    if (addrNodeIdRaw != 0 && addrNodeNumber != node.value) {
        static std::atomic<bool> sLoggedNodeMismatch{false};
        if (!sLoggedNodeMismatch.exchange(true, std::memory_order_relaxed)) {
            ASFW_LOG_V2(Async,
                        "FireWireBusImpl::%{public}s: FWAddress.nodeID mismatch "
                        "(addr.nodeID=0x%04x nodeId=%u); using nodeId",
                        operation, addrNodeIdRaw, node.value);
        }
    }

    return static_cast<uint16_t>(node.value);
}

[[nodiscard]] CompletionCallback AdaptInterfaceCompletion(InterfaceCompletionCallback callback) {
    return [callback = std::move(callback)](AsyncHandle, AsyncStatus status, uint8_t,
                                            std::span<const uint8_t> payload) {
        if (callback) {
            callback(status, payload);
        }
    };
}

} // namespace

FireWireBusImpl::FireWireBusImpl(IAsyncControllerPort& async, Driver::TopologyManager& topo)
    : async_(async), topo_(topo) {}

AsyncHandle FireWireBusImpl::ReadBlock(FW::Generation gen, FW::NodeId node, FWAddress addr,
                                       uint32_t length, FW::FwSpeed speed,
                                       InterfaceCompletionCallback callback) {
    if (!HasCurrentGeneration(async_, gen)) {
        CompleteStaleGenerationAsync(async_, std::move(callback));
        return AsyncHandle{0};
    }

    ReadParams params{.destinationID = ResolveDestinationNodeId("ReadBlock", node, addr),
                      .addressHigh = addr.addressHi,
                      .addressLow = addr.addressLo,
                      .length = length,
                      .speedCode = static_cast<uint8_t>(speed)};
    return async_.Read(params, AdaptInterfaceCompletion(std::move(callback)));
}

AsyncHandle FireWireBusImpl::WriteBlock(FW::Generation gen, FW::NodeId node, FWAddress addr,
                                        std::span<const uint8_t> data, FW::FwSpeed speed,
                                        InterfaceCompletionCallback callback) {
    if (!HasCurrentGeneration(async_, gen)) {
        CompleteStaleGenerationAsync(async_, std::move(callback));
        return AsyncHandle{0};
    }

    WriteParams params{.destinationID = ResolveDestinationNodeId("WriteBlock", node, addr),
                       .addressHigh = addr.addressHi,
                       .addressLow = addr.addressLo,
                       .payload = data.data(),
                       .length = static_cast<uint32_t>(data.size()),
                       .speedCode = static_cast<uint8_t>(speed)};
    return async_.Write(params, AdaptInterfaceCompletion(std::move(callback)));
}

AsyncHandle FireWireBusImpl::Lock(FW::Generation gen, FW::NodeId node, FWAddress addr,
                                  FW::LockOp op, std::span<const uint8_t> operand,
                                  uint32_t responseLength, FW::FwSpeed speed,
                                  InterfaceCompletionCallback callback) {
    if (!HasCurrentGeneration(async_, gen)) {
        CompleteStaleGenerationAsync(async_, std::move(callback));
        return AsyncHandle{0};
    }

    LockParams params{};
    params.destinationID = ResolveDestinationNodeId("Lock", node, addr);
    params.addressHigh = addr.addressHi;
    params.addressLow = addr.addressLo;
    params.operand = operand.data();
    params.operandLength = static_cast<uint32_t>(operand.size());
    params.responseLength = responseLength;
    params.speedCode = static_cast<uint8_t>(speed);

    const uint16_t extendedTCode = static_cast<uint16_t>(op);

    return async_.Lock(params, extendedTCode, AdaptInterfaceCompletion(std::move(callback)));
}

bool FireWireBusImpl::Cancel(AsyncHandle handle) { return async_.Cancel(handle); }

FW::FwSpeed FireWireBusImpl::GetSpeed(FW::NodeId nodeId) const {
    // Get the latest topology snapshot
    auto snapshot = topo_.LatestSnapshot();
    if (!snapshot) {
        return FW::FwSpeed::S100; // Default to S100 if no topology available
    }

    // Find the node in the topology
    for (const auto& node : snapshot->nodes) {
        if (node.nodeId == nodeId.value) {
            // Convert maxSpeedMbps to FwSpeed enum
            switch (node.maxSpeedMbps) {
            case 100:
                return FW::FwSpeed::S100;
            case 200:
                return FW::FwSpeed::S200;
            case 400:
                return FW::FwSpeed::S400;
            case 800:
                return FW::FwSpeed::S800;
            default:
                return FW::FwSpeed::S100;
            }
        }
    }

    return FW::FwSpeed::S100; // Default if node not found
}

uint32_t FireWireBusImpl::HopCount(FW::NodeId nodeA, FW::NodeId nodeB) const {
    // Special case: same node
    if (nodeA.value == nodeB.value) {
        return 0;
    }

    // Get the latest topology snapshot
    auto snapshot = topo_.LatestSnapshot();
    if (!snapshot || snapshot->nodes.empty()) {
        return UINT32_MAX; // Unknown
    }

    // Build a map from nodeId to TopologyNode for fast lookup
    std::map<uint8_t, const Driver::TopologyNode*> nodeMap;
    for (const auto& node : snapshot->nodes) {
        nodeMap[node.nodeId] = &node;
    }

    // Check that both nodes exist
    if (nodeMap.find(nodeA.value) == nodeMap.end() || nodeMap.find(nodeB.value) == nodeMap.end()) {
        return UINT32_MAX; // Unknown
    }

    // BFS to find shortest path from nodeA to nodeB
    std::map<uint8_t, uint32_t> distance;
    std::queue<uint8_t> queue;

    distance[nodeA.value] = 0;
    queue.push(nodeA.value);

    while (!queue.empty()) {
        uint8_t current = queue.front();
        queue.pop();

        if (current == nodeB.value) {
            return distance[current];
        }

        const auto* currentNode = nodeMap[current];
        if (!currentNode)
            continue;

        // Visit parent nodes
        for (uint8_t parentId : currentNode->parentNodeIds) {
            if (distance.find(parentId) == distance.end()) {
                distance[parentId] = distance[current] + 1;
                queue.push(parentId);
            }
        }

        // Visit child nodes
        for (uint8_t childId : currentNode->childNodeIds) {
            if (distance.find(childId) == distance.end()) {
                distance[childId] = distance[current] + 1;
                queue.push(childId);
            }
        }
    }

    return UINT32_MAX; // No path found
}

FW::Generation FireWireBusImpl::GetGeneration() const {
    const auto state = async_.GetBusStateSnapshot();
    return FW::Generation{state.generation16};
}

FW::NodeId FireWireBusImpl::GetLocalNodeID() const {
    const auto state = async_.GetBusStateSnapshot();
    uint8_t nodeId = static_cast<uint8_t>(state.localNodeID & 0x3Fu); // Extract low 6 bits
    return FW::NodeId{nodeId};
}

} // namespace ASFW::Async
