//
//  DiagnosticsHandler.cpp
//  ASFWDriver
//
//  Handler for bus state diagnostics UserClient methods
//

#include "DiagnosticsHandler.hpp"
#include "../../Bus/BusManager.hpp"
#include "../../Bus/BusResetCoordinator.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"
#include "../WireFormats/DiagnosticsWireFormats.hpp"
#include "ASFWDriver.h"
#include "ControllerCoreAccess.hpp"

#include <DriverKit/OSData.h>
#include <cstring>

using ASFW::UserClient::Wire::BusStateWire;

namespace ASFW::UserClient {

DiagnosticsHandler::DiagnosticsHandler(ASFWDriver* driver) : driver_(driver) {}

kern_return_t DiagnosticsHandler::GetBusStateDiagnostics(IOUserClientMethodArguments* args) {
    if (!args) {
        return kIOReturnBadArgument;
    }

    auto* controller = GetControllerCorePtr(driver_);
    if (!controller) {
        return kIOReturnNotReady;
    }

    BusStateWire wire{};
    std::memset(&wire, 0, sizeof(wire));

    // Hardware registers
    auto* hw = controller->GetHardware();
    if (hw) {
        wire.hcControl = hw->ReadHCControl();
        wire.linkControl = hw->ReadLinkControl();
        wire.nodeId = hw->ReadNodeID();
        wire.cycleTime = hw->ReadCycleTime();

        auto phy1 = hw->ReadPhyRegister(1);
        wire.phyReg1 = phy1.value_or(0xFF);

        auto phy4 = hw->ReadPhyRegister(4);
        wire.phyReg4 = phy4.value_or(0xFF);
    }

    // BusResetCoordinator FSM state and metrics
    auto* busReset = controller->GetBusResetCoordinator();
    if (busReset) {
        wire.busResetFsmState = static_cast<uint8_t>(busReset->GetState());
        wire.busResetCount = busReset->Metrics().resetCount;
    }

    // Topology
    if (auto topo = controller->LatestTopology()) {
        wire.generation = topo->generation;
        wire.localNodeId = topo->localNodeId.has_value()
                               ? static_cast<uint8_t>(*topo->localNodeId & 0x3F)
                               : 0xFF;
        wire.rootNodeId = topo->rootNodeId.has_value()
                              ? static_cast<uint8_t>(*topo->rootNodeId & 0x3F)
                              : 0xFF;
        wire.irmNodeId = topo->irmNodeId.has_value() ? static_cast<uint8_t>(*topo->irmNodeId & 0x3F)
                                                     : 0xFF;
        wire.gapCount = topo->gapCount;
    }

    // BusManager config
    auto* busMgr = controller->GetBusManager();
    if (busMgr) {
        const auto& cfg = busMgr->GetConfig();
        wire.rootPolicy = static_cast<uint8_t>(cfg.rootPolicy);
        wire.delegateCm = cfg.delegateCycleMaster ? 1 : 0;
    }

    // Return as OSData
    OSData* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) {
        return kIOReturnNoMemory;
    }
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;

    return kIOReturnSuccess;
}

kern_return_t DiagnosticsHandler::ReadPhyRegister(IOUserClientMethodArguments* args) {
    if (!args || args->scalarInputCount < 1) {
        return kIOReturnBadArgument;
    }

    const uint8_t address = static_cast<uint8_t>(args->scalarInput[0] & 0xFF);
    if (address > 7) {
        return kIOReturnBadArgument;
    }

    auto* controller = GetControllerCorePtr(driver_);
    if (!controller) {
        return kIOReturnNotReady;
    }

    auto* hw = controller->GetHardware();
    if (!hw) {
        return kIOReturnNotReady;
    }

    auto value = hw->ReadPhyRegister(address);
    if (!value.has_value()) {
        return kIOReturnIOError;
    }

    if (args->scalarOutput != nullptr && args->scalarOutputCount >= 1) {
        args->scalarOutput[0] = static_cast<uint64_t>(*value);
        args->scalarOutputCount = 1;
    }

    return kIOReturnSuccess;
}

kern_return_t DiagnosticsHandler::InitiateBusReset(IOUserClientMethodArguments* args) {
    bool shortReset = true;
    if (args && args->scalarInputCount >= 1) {
        shortReset = (args->scalarInput[0] == 0);
    }

    auto* controller = GetControllerCorePtr(driver_);
    if (!controller) {
        return kIOReturnNotReady;
    }

    auto* busReset = controller->GetBusResetCoordinator();
    if (!busReset) {
        return kIOReturnNotReady;
    }

    ASFW_LOG(Hardware, "UserClient: InitiateBusReset (%s)", shortReset ? "short" : "long");
    busReset->RequestUserReset(shortReset);
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
