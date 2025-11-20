//
//  TopologyHandler.cpp
//  ASFWDriver
//
//  Handler for topology and Self-ID related UserClient methods
//

#include "TopologyHandler.hpp"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../WireFormats/TopologyWireFormats.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/OSData.h>
#include <cstring>

namespace ASFW::UserClient {

TopologyHandler::TopologyHandler(ASFWDriver* driver)
    : driver_(driver) {
}

kern_return_t TopologyHandler::GetSelfIDCapture(IOUserClientMethodArguments* args) {
    // Return Self-ID capture with raw quadlets and sequences
    // Input: generation (optional, 0 = latest)
    // Output: OSData with SelfIDMetricsWire + quadlets + sequences

    ASFW_LOG(UserClient, "kMethodGetSelfIDCapture called: args=%p", args);

    if (!args) {
        ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - args is NULL, returning BadArgument");
        return kIOReturnBadArgument;
    }

    ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - structureInput=%p structureOutput=%p maxSize=%llu",
             args->structureInput,
             args->structureOutput,
             args->structureOutputMaximumSize);

    using namespace ASFW::Driver;

    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (!controller) {
        ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - controller is NULL");
        return kIOReturnNotReady;
    }

    auto topo = controller->LatestTopology();
    if (!topo || !topo->selfIDData.valid) {
        // No valid Self-ID data available
        ASFW_LOG(UserClient, "kMethodGetSelfIDCapture - no valid Self-ID data (topo=%d valid=%d)",
                 topo.has_value() ? 1 : 0,
                 topo.has_value() ? (topo->selfIDData.valid ? 1 : 0) : 0);
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        ASFW_LOG(UserClient, "kMethodGetSelfIDCapture EXIT: setting structureOutput len=0 (no data yet)");
        return kIOReturnSuccess;
    }

    const auto& selfID = topo->selfIDData;

    // Calculate total size
    size_t headerSize = sizeof(Wire::SelfIDMetricsWire);
    size_t quadletsSize = selfID.rawQuadlets.size() * sizeof(uint32_t);
    size_t sequencesSize = selfID.sequences.size() * sizeof(Wire::SelfIDSequenceWire);
    size_t totalSize = headerSize + quadletsSize + sequencesSize;

    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) return kIOReturnNoMemory;

    // Write header
    Wire::SelfIDMetricsWire wire{};
    wire.generation = selfID.generation;
    wire.captureTimestamp = selfID.captureTimestamp;
    wire.quadletCount = static_cast<uint32_t>(selfID.rawQuadlets.size());
    wire.sequenceCount = static_cast<uint32_t>(selfID.sequences.size());
    wire.valid = selfID.valid ? 1 : 0;
    wire.timedOut = selfID.timedOut ? 1 : 0;
    wire.crcError = selfID.crcError ? 1 : 0;

    if (selfID.errorReason.has_value()) {
        std::strncpy(wire.errorReason, selfID.errorReason->c_str(), sizeof(wire.errorReason) - 1);
        wire.errorReason[sizeof(wire.errorReason) - 1] = '\0';
    } else {
        wire.errorReason[0] = '\0';
    }

    if (!data->appendBytes(&wire, sizeof(wire))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write quadlets
    if (!selfID.rawQuadlets.empty()) {
        if (!data->appendBytes(selfID.rawQuadlets.data(), quadletsSize)) {
            data->release();
            return kIOReturnNoMemory;
        }
    }

    // Write sequences
    for (const auto& seq : selfID.sequences) {
        Wire::SelfIDSequenceWire seqWire{};
        seqWire.startIndex = static_cast<uint32_t>(seq.first);
        seqWire.quadletCount = seq.second;
        if (!data->appendBytes(&seqWire, sizeof(seqWire))) {
            data->release();
            return kIOReturnNoMemory;
        }
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    ASFW_LOG(UserClient, "kMethodGetSelfIDCapture EXIT: setting structureOutput len=%zu (gen=%u quads=%u seqs=%u)",
             data ? data->getLength() : 0, wire.generation, wire.quadletCount, wire.sequenceCount);
    return kIOReturnSuccess;
}

kern_return_t TopologyHandler::GetTopologySnapshot(IOUserClientMethodArguments* args) {
    // Return complete topology snapshot with nodes and port states
    // Output: OSData with TopologySnapshotWire + nodes + port states + warnings

    ASFW_LOG(UserClient, "kMethodGetTopologySnapshot called: args=%p", args);

    if (!args) {
        ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - args is NULL, returning BadArgument");
        return kIOReturnBadArgument;
    }

    ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - structureInput=%p structureOutput=%p maxSize=%llu",
             args->structureInput,
             args->structureOutput,
             args->structureOutputMaximumSize);

    using namespace ASFW::Driver;

    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (!controller) {
        ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - controller is NULL");
        return kIOReturnNotReady;
    }

    auto topo = controller->LatestTopology();
    if (!topo) {
        // No topology available
        ASFW_LOG(UserClient, "kMethodGetTopologySnapshot - no topology available");
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        ASFW_LOG(UserClient, "kMethodGetTopologySnapshot EXIT: setting structureOutput len=0 (no data yet)");
        return kIOReturnSuccess;
    }

    // Calculate size for variable-length data
    size_t headerSize = sizeof(Wire::TopologySnapshotWire);
    size_t nodesBaseSize = topo->nodes.size() * sizeof(Wire::TopologyNodeWire);

    // Calculate port states size
    size_t portStatesSize = 0;
    for (const auto& node : topo->nodes) {
        portStatesSize += node.portStates.size();
    }

    // Calculate warnings size (null-terminated strings)
    size_t warningsSize = 0;
    for (const auto& warning : topo->warnings) {
        warningsSize += warning.length() + 1; // +1 for null terminator
    }

    size_t totalSize = headerSize + nodesBaseSize + portStatesSize + warningsSize;

    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) return kIOReturnNoMemory;

    // Write snapshot header
    Wire::TopologySnapshotWire snapWire{};
    snapWire.generation = topo->generation;
    snapWire.capturedAt = topo->capturedAt;
    snapWire.nodeCount = topo->nodeCount;
    snapWire.rootNodeId = topo->rootNodeId.value_or(0xFF);
    snapWire.irmNodeId = topo->irmNodeId.value_or(0xFF);
    snapWire.localNodeId = topo->localNodeId.value_or(0xFF);
    snapWire.gapCount = topo->gapCount;
    snapWire.warningCount = static_cast<uint8_t>(topo->warnings.size());
    snapWire.busBase16 = topo->busBase16;  // Serialize bus base for node ID construction

    if (!data->appendBytes(&snapWire, sizeof(snapWire))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write nodes
    for (const auto& node : topo->nodes) {
        Wire::TopologyNodeWire nodeWire{};
        nodeWire.nodeId = node.nodeId;
        nodeWire.portCount = node.portCount;
        nodeWire.gapCount = node.gapCount;
        nodeWire.powerClass = node.powerClass;
        nodeWire.maxSpeedMbps = node.maxSpeedMbps;
        nodeWire.isIRMCandidate = node.isIRMCandidate ? 1 : 0;
        nodeWire.linkActive = node.linkActive ? 1 : 0;
        nodeWire.initiatedReset = node.initiatedReset ? 1 : 0;
        nodeWire.isRoot = node.isRoot ? 1 : 0;
        nodeWire.parentPort = node.parentPort.value_or(0xFF);
        nodeWire.portStateCount = static_cast<uint8_t>(node.portStates.size());

        if (!data->appendBytes(&nodeWire, sizeof(nodeWire))) {
            data->release();
            return kIOReturnNoMemory;
        }

        // Write port states for this node
        for (auto portState : node.portStates) {
            uint8_t state = static_cast<uint8_t>(portState);
            if (!data->appendBytes(&state, sizeof(state))) {
                data->release();
                return kIOReturnNoMemory;
            }
        }
    }

    // Write warnings as null-terminated strings
    for (const auto& warning : topo->warnings) {
        const char* str = warning.c_str();
        size_t len = warning.length() + 1; // Include null terminator
        if (!data->appendBytes(str, len)) {
            data->release();
            return kIOReturnNoMemory;
        }
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    ASFW_LOG(UserClient, "kMethodGetTopologySnapshot EXIT: setting structureOutput len=%zu (gen=%u nodes=%u root=%u)",
             data ? data->getLength() : 0, snapWire.generation, snapWire.nodeCount, snapWire.rootNodeId);
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
