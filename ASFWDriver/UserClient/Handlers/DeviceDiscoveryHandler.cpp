//
//  DeviceDiscoveryHandler.cpp
//  ASFWDriver
//
//  Handler for Device Discovery API
//

#include "DeviceDiscoveryHandler.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../../Logging/Logging.hpp"
#include "../WireFormats/DeviceDiscoveryWireFormats.hpp"
#include "ASFWDriver.h"
#include "ControllerCoreAccess.hpp"

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
    case State::Created:
        return 0;
    case State::Ready:
        return 1;
    case State::Quarantined:
        return 2;
    case State::Suspended:
        return 3;
    case State::Terminated:
        return 4;
    default:
        return 0;
    }
}

/**
 * @brief Convert FWUnit::State to wire format enum
 */
uint8_t UnitStateToWire(Discovery::FWUnit::State state) {
    using State = Discovery::FWUnit::State;
    switch (state) {
    case State::Created:
        return 0;
    case State::Ready:
        return 1;
    case State::Suspended:
        return 2;
    case State::Terminated:
        return 3;
    default:
        return 0;
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

DeviceDiscoveryHandler::DeviceDiscoveryHandler(ASFWDriver* driver) : driver_(driver) {}

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
    auto* controllerCore = GetControllerCorePtr(driver_);
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
    size_t totalSize = sizeof(DeviceDiscoveryWireV2);
    uint32_t deviceCount = 0;
    for (const auto& device : allDevices) {
        if (!device) {
            continue;
        }
        ++deviceCount;
        totalSize += sizeof(FWDeviceWireV2);
        totalSize += device->GetIdentityEvidence().rawBusInfoQuadlets.size() * sizeof(uint32_t);
        totalSize += device->GetUnits().size() * sizeof(FWUnitWireV2);
    }
    if (totalSize > UINT32_MAX) {
        return kIOReturnMessageTooLarge;
    }

    ASFW_LOG(UserClient, "GetDiscoveredDevices: total wire format size=%zu bytes", totalSize);

    // Create OSData buffer
    OSData* data = OSData::withCapacity(static_cast<uint32_t>(totalSize));
    if (!data) {
        ASFW_LOG(UserClient, "GetDiscoveredDevices: failed to allocate OSData");
        return kIOReturnNoMemory;
    }

    // Write header
    DeviceDiscoveryWireV2 header{};
    header.version = kDeviceDiscoveryWireVersion;
    header.headerSize = sizeof(header);
    header.byteSize = static_cast<uint32_t>(totalSize);
    header.deviceCount = deviceCount;
    if (!data->appendBytes(&header, sizeof(header))) {
        data->release();
        return kIOReturnNoMemory;
    }

    // Write each device
    for (const auto& device : allDevices) {
        if (!device) {
            continue;
        }
        const auto& evidence = device->GetIdentityEvidence();
        if (device->GetUnits().size() > UINT16_MAX ||
            evidence.rawBusInfoQuadlets.size() > UINT16_MAX) {
            data->release();
            return kIOReturnMessageTooLarge;
        }
        const size_t deviceByteSize = sizeof(FWDeviceWireV2) +
            evidence.rawBusInfoQuadlets.size() * sizeof(uint32_t) +
            device->GetUnits().size() * sizeof(FWUnitWireV2);
        if (deviceByteSize > UINT16_MAX) {
            data->release();
            return kIOReturnMessageTooLarge;
        }

        FWDeviceWireV2 deviceWire{};
        deviceWire.version = kDeviceDiscoveryWireVersion;
        deviceWire.byteSize = static_cast<uint16_t>(deviceByteSize);
        deviceWire.deviceInstanceId = device->GetInstanceId().value;
        deviceWire.observedGuid = device->GetObservedGuid();
        deviceWire.generation = device->GetGeneration().value;
        deviceWire.nodeId = device->GetNodeID();
        deviceWire.state = StateToWire(device->GetState());
        deviceWire.quarantineReason = static_cast<uint8_t>(device->GetQuarantineReason());
        deviceWire.deviceKind = static_cast<uint8_t>(device->GetKind());
        deviceWire.unitCount = static_cast<uint16_t>(device->GetUnits().size());
        deviceWire.busInfoWordCount =
            static_cast<uint16_t>(evidence.rawBusInfoQuadlets.size());
        deviceWire.nodeVendorOui = evidence.nodeVendorOui;
        if (evidence.rootVendorId) {
            deviceWire.evidenceFlags |= kHasRootVendorId;
            deviceWire.rootVendorId = *evidence.rootVendorId;
        }
        if (evidence.rootModelId) {
            deviceWire.evidenceFlags |= kHasRootModelId;
            deviceWire.rootModelId = *evidence.rootModelId;
        }

        // Copy vendor and model names
        CopyStringToBuffer(deviceWire.vendorName, sizeof(deviceWire.vendorName),
                           device->GetVendorName());
        CopyStringToBuffer(deviceWire.modelName, sizeof(deviceWire.modelName),
                           device->GetModelName());

        if (!data->appendBytes(&deviceWire, sizeof(deviceWire))) {
            data->release();
            return kIOReturnNoMemory;
        }

        if (!evidence.rawBusInfoQuadlets.empty() &&
            !data->appendBytes(evidence.rawBusInfoQuadlets.data(),
                               evidence.rawBusInfoQuadlets.size() * sizeof(uint32_t))) {
            data->release();
            return kIOReturnNoMemory;
        }

        // Write units for this device
        for (const auto& unit : device->GetUnits()) {
            if (!unit) {
                continue;
            }
            FWUnitWireV2 unitWire{};
            unitWire.version = kDeviceDiscoveryWireVersion;
            unitWire.byteSize = sizeof(unitWire);
            unitWire.deviceInstanceId = device->GetInstanceId().value;
            unitWire.specId = unit->GetUnitSpecID();
            unitWire.swVersion = unit->GetUnitSwVersion();
            unitWire.romOffset = unit->GetDirectoryOffset();
            unitWire.state = UnitStateToWire(unit->GetState());
            if (const auto value = unit->GetVendorID()) {
                unitWire.evidenceFlags |= kHasUnitVendorId;
                unitWire.vendorId = *value;
            }
            if (const auto value = unit->GetModelID()) {
                unitWire.evidenceFlags |= kHasUnitModelId;
                unitWire.modelId = *value;
            }
            if (const auto value = unit->GetLUN()) {
                unitWire.evidenceFlags |= kHasLun;
                unitWire.lun = *value;
            }
            if (const auto value = unit->GetManagementAgentOffset()) {
                unitWire.evidenceFlags |= kHasManagementAgentOffset;
                unitWire.managementAgentOffset = *value;
            }
            if (const auto value = unit->GetUnitCharacteristics()) {
                unitWire.evidenceFlags |= kHasUnitCharacteristics;
                unitWire.unitCharacteristics = *value;
            }
            if (const auto value = unit->GetFastStart()) {
                unitWire.evidenceFlags |= kHasFastStart;
                unitWire.fastStart = *value;
            }

            // Copy vendor and product names
            CopyStringToBuffer(unitWire.vendorName, sizeof(unitWire.vendorName),
                               unit->GetVendorName());
            CopyStringToBuffer(unitWire.productName, sizeof(unitWire.productName),
                               unit->GetProductName());

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
