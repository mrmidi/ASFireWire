//
//  AVCHandler.cpp
//  ASFWDriver
//
//  Handler for AV/C Protocol API
//

#include "AVCHandler.hpp"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../../Protocols/AVC/AVCDiscovery.hpp"
#include "../../Protocols/AVC/AVCUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Logging/Logging.hpp"
#include "../WireFormats/AVCWireFormats.hpp"

#include <DriverKit/OSData.h>

namespace ASFW::UserClient {

namespace {

using namespace Wire;

} // anonymous namespace

AVCHandler::AVCHandler(ASFWDriver* driver)
    : driver_(driver)
{
}

kern_return_t AVCHandler::GetAVCUnits(IOUserClientMethodArguments* args) {
    if (!args) {
        ASFW_LOG(UserClient, "GetAVCUnits: null arguments");
        return kIOReturnBadArgument;
    }

    if (!driver_) {
        ASFW_LOG(UserClient, "GetAVCUnits: driver not available");
        return kIOReturnNotReady;
    }

    // Get ControllerCore from driver
    auto* controllerCore = static_cast<Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) {
        ASFW_LOG(UserClient, "GetAVCUnits: controller not available");
        return kIOReturnNotReady;
    }

    // Get AVCDiscovery from ControllerCore
    auto* avcDiscovery = controllerCore->GetAVCDiscovery();
    if (!avcDiscovery) {
        ASFW_LOG(UserClient, "GetAVCUnits: AV/C discovery not available");
        // Return success with 0 units (AV/C might not be initialized yet)
        AVCQueryWire header{};
        header.unitCount = 0;
        header._padding = 0;
        
        OSData* data = OSData::withBytes(&header, sizeof(header));
        if (!data) {
            return kIOReturnNoMemory;
        }
        
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    // Get all AV/C units
    auto allUnits = avcDiscovery->GetAllAVCUnits();

    ASFW_LOG(UserClient, "GetAVCUnits: found %zu AV/C units", allUnits.size());

    // Calculate total size needed
    size_t totalSize = sizeof(AVCQueryWire) + (allUnits.size() * sizeof(AVCUnitWire));

    ASFW_LOG(UserClient, "GetAVCUnits: total wire format size=%zu bytes", totalSize);

    // Create OSData buffer
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) {
        ASFW_LOG(UserClient, "GetAVCUnits: failed to allocate OSData");
        return kIOReturnNoMemory;
    }

    // Write header
    AVCQueryWire header{};
    header.unitCount = static_cast<uint32_t>(allUnits.size());
    header._padding = 0;
    if (!data->appendBytes(&header, sizeof(header))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write each AV/C unit
    for (auto* avcUnit : allUnits) {
        if (!avcUnit) {
            continue;
        }

        AVCUnitWire unitWire{};
        
        // Get device from AVCUnit
        auto device = avcUnit->GetDevice();
        if (device) {
            unitWire.guid = device->GetGUID();
            unitWire.nodeId = device->GetNodeID();
        } else {
            unitWire.guid = 0;
            unitWire.nodeId = 0xFFFF;
        }

        unitWire.isInitialized = avcUnit->IsInitialized() ? 1 : 0;
        unitWire.subunitCount = static_cast<uint8_t>(avcUnit->GetSubunits().size());
        unitWire._padding = 0;

        if (!data->appendBytes(&unitWire, sizeof(unitWire))) {
            data->release();
            return kIOReturnNoMemory;
        }
    }

    // Return data through structureOutput
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;

    ASFW_LOG(UserClient, "GetAVCUnits: returning %zu units in %zu bytes",
             allUnits.size(), data->getLength());
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
