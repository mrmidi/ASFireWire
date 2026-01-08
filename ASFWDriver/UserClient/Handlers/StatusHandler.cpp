//
//  StatusHandler.cpp
//  ASFWDriver
//
//  Handler for controller status related UserClient methods
//

#include "StatusHandler.hpp"
#include "ASFWDriver.h"
#include "ASFWDriverUserClient.h"
#include "../../Controller/ControllerCore.hpp" // ASFWDriver/Controller/ControllerCore.hpp
#include "../../Diagnostics/ControllerMetrics.hpp" // ASFWDriver/Diagnostics/ControllerMetrics.hpp
#include "../../Diagnostics/MetricsSink.hpp" // ASFWDriver/Diagnostics/MetricsSink.hpp
#include "../../Controller/ControllerStateMachine.hpp" // ASFWDriver/Controller/ControllerStateMachine.hpp
#include "../../Async/AsyncSubsystem.hpp" // ASFWDriver/Async/AsyncSubsystem.hpp
#include "../WireFormats/StatusWireFormats.hpp" // ASFWDriver/UserClient/WireFormats/StatusWireFormats.hpp
#include "../../Logging/Logging.hpp" // ASFWDriver/Logging/Logging.hpp

#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <cstdio>
#include <cstring>

namespace ASFW::UserClient {

StatusHandler::StatusHandler(ASFWDriver* driver)
    : driver_(driver) {
}

kern_return_t StatusHandler::GetControllerStatus(IOUserClientMethodArguments* args) {
    // Return comprehensive controller status
    // Output: ControllerStatusWire structure
    if (!args) {
        return kIOReturnBadArgument;
    }

    using namespace ASFW::Driver;
    Wire::ControllerStatusWire status{};
    status.version = Wire::kControllerStatusWireVersion;
    status.flags = 0;
    std::strncpy(status.stateName, "NotReady", sizeof(status.stateName));
    status.stateName[sizeof(status.stateName) - 1] = '\0';
    status.generation = 0;
    status.nodeCount = 0;
    status.localNodeID = 0xFFFFFFFFu;
    status.rootNodeID = 0xFFFFFFFFu;
    status.irmNodeID = 0xFFFFFFFFu;
    status.busResetCount = 0;
    status.lastBusResetTime = 0;
    status.uptimeNanoseconds = 0;

    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (controller) {
        auto stateStr = std::string(ToString(controller->StateMachine().CurrentState()));
        std::strncpy(status.stateName, stateStr.c_str(), sizeof(status.stateName) - 1);
        status.stateName[sizeof(status.stateName) - 1] = '\0';

        const auto& busResetMetrics = controller->Metrics().BusReset();
        status.busResetCount = busResetMetrics.resetCount;
        status.lastBusResetTime = busResetMetrics.lastResetCompletion;
        if (busResetMetrics.lastResetCompletion >= busResetMetrics.lastResetStart) {
            status.uptimeNanoseconds = busResetMetrics.lastResetCompletion - busResetMetrics.lastResetStart;
        } else {
            status.uptimeNanoseconds = busResetMetrics.lastResetCompletion;
        }

        if (auto topo = controller->LatestTopology()) {
            status.generation = topo->generation;
            status.nodeCount = topo->nodeCount;
            status.localNodeID = topo->localNodeId.has_value()
                ? static_cast<uint32_t>(*topo->localNodeId)
                : 0xFFFFFFFFu;
            status.rootNodeID = topo->rootNodeId.has_value()
                ? static_cast<uint32_t>(*topo->rootNodeId)
                : 0xFFFFFFFFu;
            status.irmNodeID = topo->irmNodeId.has_value()
                ? static_cast<uint32_t>(*topo->irmNodeId)
                : 0xFFFFFFFFu;

            if (topo->irmNodeId.has_value() && topo->localNodeId.has_value() &&
                topo->irmNodeId == topo->localNodeId) {
                status.flags |= Wire::ControllerStatusFlags::kIsIRM;
            }
            // TODO: Determine cycle-master role from hardware registers/topology
        }
    }

    if (auto* asyncSys = static_cast<ASFW::Async::AsyncSubsystem*>(driver_->GetAsyncSubsystem())) {
        if (auto snapshotOpt = asyncSys->GetStatusSnapshot()) {
            const auto& snapshot = *snapshotOpt;

            status.async.atRequest.descriptorVirt = snapshot.atRequest.descriptorVirt;
            status.async.atRequest.descriptorIOVA = snapshot.atRequest.descriptorIOVA;
            status.async.atRequest.descriptorCount = snapshot.atRequest.descriptorCount;
            status.async.atRequest.descriptorStride = snapshot.atRequest.descriptorStride;
            status.async.atRequest.commandPtr = snapshot.atRequest.commandPtr;

            status.async.atResponse = {
                snapshot.atResponse.descriptorVirt,
                snapshot.atResponse.descriptorIOVA,
                snapshot.atResponse.descriptorCount,
                snapshot.atResponse.descriptorStride,
                snapshot.atResponse.commandPtr,
                0
            };

            status.async.arRequest = {
                snapshot.arRequest.descriptorVirt,
                snapshot.arRequest.descriptorIOVA,
                snapshot.arRequest.descriptorCount,
                snapshot.arRequest.descriptorStride,
                snapshot.arRequest.commandPtr,
                0
            };

            status.async.arResponse = {
                snapshot.arResponse.descriptorVirt,
                snapshot.arResponse.descriptorIOVA,
                snapshot.arResponse.descriptorCount,
                snapshot.arResponse.descriptorStride,
                snapshot.arResponse.commandPtr,
                0
            };

            status.async.arRequestBuffers.bufferVirt = snapshot.arRequestBuffers.bufferVirt;
            status.async.arRequestBuffers.bufferIOVA = snapshot.arRequestBuffers.bufferIOVA;
            status.async.arRequestBuffers.bufferCount = snapshot.arRequestBuffers.bufferCount;
            status.async.arRequestBuffers.bufferSize = snapshot.arRequestBuffers.bufferSize;

            status.async.arResponseBuffers.bufferVirt = snapshot.arResponseBuffers.bufferVirt;
            status.async.arResponseBuffers.bufferIOVA = snapshot.arResponseBuffers.bufferIOVA;
            status.async.arResponseBuffers.bufferCount = snapshot.arResponseBuffers.bufferCount;
            status.async.arResponseBuffers.bufferSize = snapshot.arResponseBuffers.bufferSize;

            status.async.dmaSlabVirt = snapshot.dmaSlabVirt;
            status.async.dmaSlabIOVA = snapshot.dmaSlabIOVA;
            status.async.dmaSlabSize = snapshot.dmaSlabSize;
        }
    }

    OSData* data = OSData::withBytes(&status, sizeof(status));
    if (!data) {
        return kIOReturnNoMemory;
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;

    return kIOReturnSuccess;
}

kern_return_t StatusHandler::GetMetricsSnapshot(IOUserClientMethodArguments* args) {
    // Future: Return IOReporter data
    return kIOReturnUnsupported;
}

kern_return_t StatusHandler::Ping(IOUserClientMethodArguments* args) {
    if (!args) {
        return kIOReturnBadArgument;
    }

    using namespace ASFW::Driver;

    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (!controller) {
        return kIOReturnNotReady;
    }

    // Touch metrics subsystem to ensure readiness
    const auto& busMetrics = controller->Metrics().BusReset();

    char message[64];
    int written = std::snprintf(message, sizeof(message), "pong (resets=%u)", busMetrics.resetCount);
    if (written < 0) {
        return kIOReturnError;
    }

    const size_t payloadSize = static_cast<size_t>(written) + 1; // include null terminator
    OSData* data = OSData::withBytes(message, static_cast<uint32_t>(payloadSize));
    if (!data) {
        return kIOReturnNoMemory;
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t StatusHandler::RegisterStatusListener(IOUserClientMethodArguments* args,
                                                     ASFWDriverUserClient* userClient) {
    if (!args || !args->completion) {
        return kIOReturnBadArgument;
    }

    if (!userClient || !userClient->ivars || !userClient->ivars->driver) {
        return kIOReturnNotReady;
    }

    if (!userClient->ivars->actionLock) {
        return kIOReturnNotReady;
    }

    IOLockLock(userClient->ivars->actionLock);
    if (userClient->ivars->statusAction) {
        userClient->ivars->statusAction->release();
        userClient->ivars->statusAction = nullptr;
    }

    args->completion->retain();
    userClient->ivars->statusAction = args->completion;
    userClient->ivars->statusRegistered = true;
    userClient->ivars->stopping = false;
    IOLockUnlock(userClient->ivars->actionLock);

    userClient->ivars->driver->RegisterStatusListener(userClient);
    return kIOReturnSuccess;
}

kern_return_t StatusHandler::CopyStatusSnapshot(IOUserClientMethodArguments* args) {
    if (!args) {
        return kIOReturnBadArgument;
    }

    OSDictionary* statusDict = nullptr;
    uint64_t sequence = 0;
    uint64_t timestamp = 0;

    auto kr = driver_->CopyControllerSnapshot(&statusDict, &sequence, &timestamp);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    if (args->scalarOutput && args->scalarOutputCount >= 2) {
        args->scalarOutput[0] = sequence;
        args->scalarOutput[1] = timestamp;
        args->scalarOutputCount = 2;
    }

    if (statusDict) {
        statusDict->release();
    }

    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
