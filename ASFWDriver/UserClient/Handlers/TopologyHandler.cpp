//
//  TopologyHandler.cpp
//  ASFWDriver
//
//  Handler for topology and Self-ID related UserClient methods
//

#include "TopologyHandler.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../WireFormats/TopologyWireFormats.hpp"
#include "ASFWDriver.h"
#include "ControllerCoreAccess.hpp"

#include <DriverKit/OSData.h>
#include <cstring>

namespace ASFW::UserClient {

namespace {

void SetStructureOutput(IOUserClientMethodArguments* args, OSData* data) {
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
}

[[nodiscard]] OSData* CreateEmptyOutput() {
    return OSData::withCapacity(0);
}

[[nodiscard]] bool AppendBytesOrRelease(OSData* data, const uint8_t* bytes, size_t length) {
    if (data->appendBytes(bytes, length)) {
        return true;
    }

    data->release();
    return false;
}

template <typename T>
[[nodiscard]] bool AppendValueOrRelease(OSData* data, const T& value) {
    return AppendBytesOrRelease(data,
                                reinterpret_cast<const uint8_t*>(&value),
                                sizeof(value));
}

[[nodiscard]] OSData* SerializeTopologySnapshot(const ASFW::Driver::TopologySnapshot& topo) {
    const size_t headerSize = sizeof(Wire::TopologySnapshotWire);
    const size_t nodesBaseSize = topo.nodes.size() * sizeof(Wire::TopologyNodeWire);

    size_t portStatesSize = 0;
    for (const auto& node : topo.nodes) {
        portStatesSize += node.portStates.size();
    }

    size_t warningsSize = 0;
    for (const auto& warning : topo.warnings) {
        warningsSize += warning.length() + 1;
    }

    const size_t totalSize = headerSize + nodesBaseSize + portStatesSize + warningsSize;
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) {
        return nullptr;
    }

    Wire::TopologySnapshotWire snapWire{};
    snapWire.generation = topo.generation;
    snapWire.capturedAt = topo.capturedAt;
    snapWire.nodeCount = topo.nodeCount;
    snapWire.rootNodeId = topo.rootNodeId.value_or(0xFF);
    snapWire.irmNodeId = topo.irmNodeId.value_or(0xFF);
    snapWire.localNodeId = topo.localNodeId.value_or(0xFF);
    snapWire.gapCount = topo.gapCount;
    snapWire.warningCount = static_cast<uint8_t>(topo.warnings.size());
    snapWire.busBase16 = topo.busBase16;

    if (!AppendValueOrRelease(data, snapWire)) {
        return nullptr;
    }

    for (const auto& node : topo.nodes) {
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

        if (!AppendValueOrRelease(data, nodeWire)) {
            return nullptr;
        }

        for (auto portState : node.portStates) {
            const uint8_t state = static_cast<uint8_t>(portState);
            if (!AppendValueOrRelease(data, state)) {
                return nullptr;
            }
        }
    }

    for (const auto& warning : topo.warnings) {
        const char* str = warning.c_str();
        const size_t len = warning.length() + 1;
        if (!AppendBytesOrRelease(data,
                                  reinterpret_cast<const uint8_t*>(str),
                                  len)) {
            return nullptr;
        }
    }

    return data;
}

} // namespace

TopologyHandler::TopologyHandler(ASFWDriver* driver) : driver_(driver) {}

kern_return_t TopologyHandler::GetSelfIDCapture(IOUserClientMethodArguments* args) {
    // Return Self-ID capture with raw quadlets and sequences
    // Input: generation (optional, 0 = latest)
    // Output: OSData with SelfIDMetricsWire + quadlets + sequences

    ASFW_LOG_V3(UserClient, "kMethodGetSelfIDCapture called: args=%p", args);

    if (!args) {
        ASFW_LOG_V0(UserClient, "kMethodGetSelfIDCapture - args is NULL, returning BadArgument");
        return kIOReturnBadArgument;
    }

    ASFW_LOG_V3(UserClient,
                "kMethodGetSelfIDCapture - structureInput=%p structureOutput=%p maxSize=%llu",
                args->structureInput, args->structureOutput, args->structureOutputMaximumSize);

    using namespace ASFW::Driver;

    auto* controller = GetControllerCorePtr(driver_);
    if (!controller) {
        ASFW_LOG_V0(UserClient, "kMethodGetSelfIDCapture - controller is NULL");
        return kIOReturnNotReady;
    }

    auto topo = controller->LatestTopology();
    if (!topo || !topo->selfIDData.valid) {
        // No valid Self-ID data available
        ASFW_LOG_V3(
            UserClient, "kMethodGetSelfIDCapture - no valid Self-ID data (topo=%d valid=%d)",
            topo.has_value() ? 1 : 0, topo.has_value() ? (topo->selfIDData.valid ? 1 : 0) : 0);
        OSData* data = OSData::withCapacity(0);
        if (!data)
            return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        ASFW_LOG_V3(UserClient,
                    "kMethodGetSelfIDCapture EXIT: setting structureOutput len=0 (no data yet)");
        return kIOReturnSuccess;
    }

    const auto& selfID = topo->selfIDData;

    // Calculate total size
    size_t headerSize = sizeof(Wire::SelfIDMetricsWire);
    size_t quadletsSize = selfID.rawQuadlets.size() * sizeof(uint32_t);
    size_t sequencesSize = selfID.sequences.size() * sizeof(Wire::SelfIDSequenceWire);
    size_t totalSize = headerSize + quadletsSize + sequencesSize;

    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data)
        return kIOReturnNoMemory;

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
        strlcpy(wire.errorReason, selfID.errorReason->c_str(), sizeof(wire.errorReason));
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
    ASFW_LOG_V3(
        UserClient,
        "kMethodGetSelfIDCapture EXIT: setting structureOutput len=%zu (gen=%u quads=%u seqs=%u)",
        data ? data->getLength() : 0, wire.generation, wire.quadletCount, wire.sequenceCount);
    return kIOReturnSuccess;
}

kern_return_t TopologyHandler::GetTopologySnapshot(IOUserClientMethodArguments* args) {
    // Return complete topology snapshot with nodes and port states
    // Output: OSData with TopologySnapshotWire + nodes + port states + warnings

    ASFW_LOG_V3(UserClient, "kMethodGetTopologySnapshot called: args=%p", args);

    if (!args) {
        ASFW_LOG_V0(UserClient, "kMethodGetTopologySnapshot - args is NULL, returning BadArgument");
        return kIOReturnBadArgument;
    }

    ASFW_LOG_V3(UserClient,
                "kMethodGetTopologySnapshot - structureInput=%p structureOutput=%p maxSize=%llu",
                args->structureInput, args->structureOutput, args->structureOutputMaximumSize);

    using namespace ASFW::Driver;

    auto* controller = GetControllerCorePtr(driver_);
    if (!controller) {
        ASFW_LOG_V0(UserClient, "kMethodGetTopologySnapshot - controller is NULL");
        return kIOReturnNotReady;
    }

    auto topo = controller->LatestTopology();
    if (!topo) {
        // No topology available
        ASFW_LOG_V3(UserClient, "kMethodGetTopologySnapshot - no topology available");
        OSData* data = CreateEmptyOutput();
        if (!data)
            return kIOReturnNoMemory;
        SetStructureOutput(args, data);
        ASFW_LOG_V3(UserClient,
                    "kMethodGetTopologySnapshot EXIT: setting structureOutput len=0 (no data yet)");
        return kIOReturnSuccess;
    }

    OSData* data = SerializeTopologySnapshot(*topo);
    if (!data) {
        return kIOReturnNoMemory;
    }

    SetStructureOutput(args, data);
    ASFW_LOG_V3(UserClient,
                "kMethodGetTopologySnapshot EXIT: setting structureOutput len=%zu (gen=%u nodes=%u "
                "root=%u)",
                data ? data->getLength() : 0, topo->generation, topo->nodeCount,
                topo->rootNodeId.value_or(0xFF));
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
