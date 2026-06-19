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

// Topology snapshots are served through the diagnostics ABI (ASFWDiagTopology,
// selector kMethodDiagGetTopology). The legacy TopologyNodeWire serializer was
// retired in favor of that single, versioned, layout-shared path.

} // namespace ASFW::UserClient
