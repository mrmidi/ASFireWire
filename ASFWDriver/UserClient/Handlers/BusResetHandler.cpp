//
//  BusResetHandler.cpp
//  ASFWDriver
//
//  Handler for bus reset related UserClient methods
//

#include "BusResetHandler.hpp"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../../Diagnostics/ControllerMetrics.hpp"
#include "../../Diagnostics/MetricsSink.hpp"
#include "../../Async/AsyncSubsystem.hpp"
#include "../../Debug/BusResetPacketCapture.hpp"
#include "../WireFormats/BusResetWireFormats.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/OSData.h>
#include <algorithm>
#include <cstring>

namespace ASFW::UserClient {

BusResetHandler::BusResetHandler(ASFWDriver* driver)
    : driver_(driver) {
}

kern_return_t BusResetHandler::GetBusResetCount(IOUserClientMethodArguments* args) {
    // Return bus reset count, generation, and timestamp
    // Output: 3 scalar uint64_t values
    if (!args || args->scalarOutputCount < 3) {
        return kIOReturnBadArgument;
    }

    // Get real metrics from ControllerCore
    using namespace ASFW::Driver;
    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (!controller) {
        // Driver not fully initialized yet
        args->scalarOutput[0] = 0;
        args->scalarOutput[1] = 0;
        args->scalarOutput[2] = 0;
        args->scalarOutputCount = 3;
        return kIOReturnSuccess;
    }

    auto& metrics = controller->Metrics().BusReset();
    uint32_t generation = 0;
    if (auto topo = controller->LatestTopology()) {
        generation = topo->generation;
    }

    args->scalarOutput[0] = metrics.resetCount;
    args->scalarOutput[1] = generation;
    args->scalarOutput[2] = metrics.lastResetCompletion;
    args->scalarOutputCount = 3;

    return kIOReturnSuccess;
}

kern_return_t BusResetHandler::GetBusResetHistory(IOUserClientMethodArguments* args) {
    // Return array of bus reset packet snapshots
    // Input: startIndex, count
    // Output: OSData with BusResetPacketWire array
    if (!args || args->scalarInputCount < 2) {
        return kIOReturnBadArgument;
    }

    const uint64_t startIndex = args->scalarInput[0];
    const uint64_t requestCount = args->scalarInput[1];

    if (requestCount == 0 || requestCount > 32) {
        return kIOReturnBadArgument;
    }

    using namespace ASFW::Async;
    using namespace ASFW::Debug;

    // Get capture from driver's async subsystem
    auto* asyncSys = static_cast<AsyncSubsystem*>(driver_->GetAsyncSubsystem());
    if (!asyncSys) {
        // Return empty if not available
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    auto* capture = asyncSys->GetBusResetCapture();
    if (!capture) {
        // Return empty if not available
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    // Determine how many packets to return
    size_t totalCount = capture->GetCount();
    if (startIndex >= totalCount) {
        // startIndex out of range, return empty
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    size_t availableCount = totalCount - startIndex;
    size_t returnCount = std::min(availableCount, static_cast<size_t>(requestCount));

    // Allocate buffer for wire format packets
    size_t dataSize = returnCount * sizeof(Wire::BusResetPacketWire);
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(dataSize));
    if (!data) {
        return kIOReturnNoMemory;
    }

    // Copy packets from capture to wire format
    for (size_t i = 0; i < returnCount; i++) {
        auto snapshot = capture->GetSnapshot(startIndex + i);
        if (!snapshot) break;  // Shouldn't happen, but be safe

        Wire::BusResetPacketWire wire{};
        wire.captureTimestamp = snapshot->captureTimestamp;
        wire.generation = snapshot->generation;
        wire.eventCode = snapshot->eventCode;
        wire.tCode = snapshot->tCode;
        wire.cycleTime = snapshot->cycleTime;

        // Copy quadlets
        for (int q = 0; q < 4; q++) {
            wire.rawQuadlets[q] = snapshot->rawQuadlets[q];
            wire.wireQuadlets[q] = snapshot->wireQuadlets[q];
        }

        // Copy context info
        std::strncpy(wire.contextInfo, snapshot->contextInfo, sizeof(wire.contextInfo) - 1);
        wire.contextInfo[sizeof(wire.contextInfo) - 1] = '\0';

        // Append to OSData
        if (!data->appendBytes(&wire, sizeof(wire))) {
            data->release();
            return kIOReturnNoMemory;
        }
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;

    return kIOReturnSuccess;
}

kern_return_t BusResetHandler::ClearHistory(IOUserClientMethodArguments* args) {
    // Clear bus reset packet history
    using namespace ASFW::Async;

    auto* asyncSys = static_cast<AsyncSubsystem*>(driver_->GetAsyncSubsystem());
    if (!asyncSys) {
        return kIOReturnSuccess;  // Nothing to clear
    }

    auto* capture = asyncSys->GetBusResetCapture();
    if (capture) {
        capture->Clear();
    }

    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
