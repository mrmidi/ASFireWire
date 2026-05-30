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
    const size_t nodesBaseSize = topo.physical.nodes.size() * sizeof(Wire::TopologyNodeWire);

    size_t portStatesSize = 0;
    for (const auto& node : topo.physical.nodes) {
        portStatesSize += node.reportedPorts.size();
    }

    size_t warningsSize = 0;
    if (topo.graphStatus == ASFW::Driver::TopologyGraphStatus::Invalid) {
        warningsSize = topo.errorDetail.length() + 1;
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
    snapWire.rootNodeId = topo.rootNodeId;
    snapWire.irmNodeId = topo.irmNodeId;
    snapWire.localNodeId = topo.localNodeId;
    snapWire.gapCount = topo.gapCount;
    snapWire.warningCount = (topo.graphStatus == ASFW::Driver::TopologyGraphStatus::Invalid) ? 1 : 0;
    snapWire.busBase16 = topo.busBase16;

    if (!AppendValueOrRelease(data, snapWire)) {
        return nullptr;
    }

    for (const auto& node : topo.physical.nodes) {
        Wire::TopologyNodeWire nodeWire{};
        nodeWire.nodeId = node.physicalId;
        nodeWire.portCount = node.portCount;
        nodeWire.gapCount = node.gapCount;
        nodeWire.powerClass = node.powerClass;
        nodeWire.maxSpeedMbps = node.maxSpeedMbps;
        nodeWire.isIRMCandidate = node.contender ? 1 : 0;
        nodeWire.linkActive = node.linkActive ? 1 : 0;
        nodeWire.initiatedReset = node.initiatedReset ? 1 : 0;
        nodeWire.isRoot = node.isRoot ? 1 : 0;
        nodeWire.parentPort = 0xFF; // Not directly available in physical graph record anymore
        nodeWire.portStateCount = static_cast<uint8_t>(node.reportedPorts.size());

        if (!AppendValueOrRelease(data, nodeWire)) {
            return nullptr;
        }

        for (auto portState : node.reportedPorts) {
            const uint8_t state = static_cast<uint8_t>(portState);
            if (!AppendValueOrRelease(data, state)) {
                return nullptr;
            }
        }
    }

    if (topo.graphStatus == ASFW::Driver::TopologyGraphStatus::Invalid) {
        const char* str = topo.errorDetail.c_str();
        const size_t len = topo.errorDetail.length() + 1;
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
    if (!topo || topo->selfIdStatus != SelfIDStreamStatus::Valid) {
        // No valid Self-ID data available
        ASFW_LOG_V3(
            UserClient, "kMethodGetSelfIDCapture - no valid Self-ID data (topo=%d status=%u)",
            topo.has_value() ? 1 : 0, topo.has_value() ? static_cast<uint8_t>(topo->selfIdStatus) : 0);
        OSData* data = OSData::withCapacity(0);
        if (!data)
            return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        ASFW_LOG_V3(UserClient,
                    "kMethodGetSelfIDCapture EXIT: setting structureOutput len=0 (no data yet)");
        return kIOReturnSuccess;
    }

    // Calculate total size
    size_t headerSize = sizeof(Wire::SelfIDMetricsWire);
    size_t quadletsSize = topo->rawSelfIdQuadlets.size() * sizeof(uint32_t);
    size_t sequencesSize = 0; // Not directly stored in v2 snapshot currently
    size_t totalSize = headerSize + quadletsSize + sequencesSize;

    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data)
        return kIOReturnNoMemory;

    // Write header
    Wire::SelfIDMetricsWire wire{};
    wire.generation = topo->generation;
    wire.captureTimestamp = topo->capturedAt;
    wire.quadletCount = static_cast<uint32_t>(topo->rawSelfIdQuadlets.size());
    wire.sequenceCount = 0;
    wire.valid = (topo->selfIdStatus == SelfIDStreamStatus::Valid) ? 1 : 0;
    wire.timedOut = (topo->selfIdStatus == SelfIDStreamStatus::Timeout) ? 1 : 0;
    wire.crcError = (topo->selfIdStatus == SelfIDStreamStatus::CrcError) ? 1 : 0;

    if (topo->errorCode != TopologyBuildErrorCode::None) {
        strlcpy(wire.errorReason, topo->errorDetail.c_str(), sizeof(wire.errorReason));
    } else {
        wire.errorReason[0] = '\0';
    }

    if (!data->appendBytes(&wire, sizeof(wire))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write quadlets
    if (!topo->rawSelfIdQuadlets.empty()) {
        if (!data->appendBytes(topo->rawSelfIdQuadlets.data(), quadletsSize)) {
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
                topo->rootNodeId);
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
