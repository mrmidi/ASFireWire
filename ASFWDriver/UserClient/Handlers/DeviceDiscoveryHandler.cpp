//
//  DeviceDiscoveryHandler.cpp
//  ASFWDriver
//
//  Handler for Device Discovery API
//

#include "DeviceDiscoveryHandler.hpp"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Logging/Logging.hpp"
#include "../WireFormats/DeviceDiscoveryWireFormats.hpp"

#include <DriverKit/OSData.h>

namespace ASFW::UserClient {

namespace {

using namespace Wire;

/**
 * @brief Convert FWDevice::State to wire format enum
 */
uint8_t StateToWire(Discovery::FWDevice::State state) {
    using State = Discovery::FWDevice::State;
    switch (state) {
        case State::Created:    return 0;
        case State::Ready:      return 1;
        case State::Suspended:  return 2;
        case State::Terminated: return 3;
        default:                return 0;
    }
}

/**
 * @brief Convert FWUnit::State to wire format enum
 */
uint8_t UnitStateToWire(Discovery::FWUnit::State state) {
    using State = Discovery::FWUnit::State;
    switch (state) {
        case State::Created:    return 0;
        case State::Ready:      return 1;
        case State::Suspended:  return 2;
        case State::Terminated: return 3;
        default:                return 0;
    }
}

/**
 * @brief Helper to safely copy string to fixed-size buffer
 */
void CopyStringToBuffer(char* dest, size_t destSize, std::string_view src) {
    size_t copyLen = std::min(src.length(), destSize - 1);
    memcpy(dest, src.data(), copyLen);
    dest[copyLen] = '\0';
}

} // anonymous namespace

DeviceDiscoveryHandler::DeviceDiscoveryHandler(ASFWDriver* driver)
    : driver_(driver)
{
}

kern_return_t DeviceDiscoveryHandler::GetDiscoveredDevices(IOUserClientMethodArguments* args) {
    if (!args) {
        ASFW_LOG(UserClient, "GetDiscoveredDevices: null arguments");
        return kIOReturnBadArgument;
    }

    if (!driver_) {
        ASFW_LOG(UserClient, "GetDiscoveredDevices: driver not available");
        return kIOReturnNotReady;
    }

    // Get ControllerCore from driver
    auto* controllerCore = static_cast<Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) {
        ASFW_LOG(UserClient, "GetDiscoveredDevices: controller not available");
        return kIOReturnNotReady;
    }

    // Get DeviceManager
    auto* deviceManager = controllerCore->GetDeviceManager();
    if (!deviceManager) {
        ASFW_LOG(UserClient, "GetDiscoveredDevices: device manager not available");
        return kIOReturnNotReady;
    }

    // Get all devices from DeviceManager
    auto allDevices = deviceManager->GetAllDevices();

    ASFW_LOG(UserClient, "GetDiscoveredDevices: found %zu devices", allDevices.size());

    // Calculate total size needed
    size_t totalSize = sizeof(DeviceDiscoveryWire);
    for (const auto& device : allDevices) {
        totalSize += sizeof(FWDeviceWire);
        totalSize += device->GetUnits().size() * sizeof(FWUnitWire);
    }

    ASFW_LOG(UserClient, "GetDiscoveredDevices: total wire format size=%zu bytes", totalSize);

    // Create OSData buffer
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) {
        ASFW_LOG(UserClient, "GetDiscoveredDevices: failed to allocate OSData");
        return kIOReturnNoMemory;
    }

    // Write header
    DeviceDiscoveryWire header{};
    header.deviceCount = static_cast<uint32_t>(allDevices.size());
    header._padding = 0;
    if (!data->appendBytes(&header, sizeof(header))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write each device
    for (const auto& device : allDevices) {
        FWDeviceWire deviceWire{};
        deviceWire.guid = device->GetGUID();
        deviceWire.vendorId = device->GetVendorID();
        deviceWire.modelId = device->GetModelID();
        deviceWire.generation = device->GetGeneration();
        deviceWire.nodeId = device->GetNodeID();
        deviceWire.state = StateToWire(device->GetState());
        deviceWire.unitCount = static_cast<uint8_t>(device->GetUnits().size());
        deviceWire._padding = 0;

        // Copy vendor and model names
        CopyStringToBuffer(deviceWire.vendorName, sizeof(deviceWire.vendorName), device->GetVendorName());
        CopyStringToBuffer(deviceWire.modelName, sizeof(deviceWire.modelName), device->GetModelName());

        if (!data->appendBytes(&deviceWire, sizeof(deviceWire))) {
            data->release();
            return kIOReturnNoMemory;
        }

        // Write units for this device
        for (const auto& unit : device->GetUnits()) {
            FWUnitWire unitWire{};
            unitWire.specId = unit->GetUnitSpecID();
            unitWire.swVersion = unit->GetUnitSwVersion();
            unitWire.romOffset = unit->GetDirectoryOffset();
            unitWire.state = UnitStateToWire(unit->GetState());
            memset(unitWire._padding, 0, sizeof(unitWire._padding));

            // Copy vendor and product names
            CopyStringToBuffer(unitWire.vendorName, sizeof(unitWire.vendorName), unit->GetVendorName());
            CopyStringToBuffer(unitWire.productName, sizeof(unitWire.productName), unit->GetProductName());

            if (!data->appendBytes(&unitWire, sizeof(unitWire))) {
                data->release();
                return kIOReturnNoMemory;
            }
        }
    }

    // Return data through structureOutput (like other working methods)
    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;

    ASFW_LOG(UserClient, "GetDiscoveredDevices: returning %zu devices in %zu bytes",
             allDevices.size(), data->getLength());
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
