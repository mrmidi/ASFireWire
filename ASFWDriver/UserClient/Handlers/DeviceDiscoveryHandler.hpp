//
//  DeviceDiscoveryHandler.hpp
//  ASFWDriver
//
//  Handler for Device Discovery API
//

#pragma once

#include <DriverKit/IOLib.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <memory>

class ASFWDriver;
struct IOUserClientMethodArguments;

namespace ASFW::UserClient {

/**
 * @brief Handler for device discovery functionality
 *
 * Provides GUI access to discovered FireWire devices and their units.
 * Serializes device/unit information from DeviceManager into OSDictionary/OSArray.
 */
class DeviceDiscoveryHandler {
public:
    explicit DeviceDiscoveryHandler(ASFWDriver* driver);
    ~DeviceDiscoveryHandler() = default;

    /**
     * @brief Get array of all discovered devices
     *
     * Returns serialized device data through IOUserClientMethodArguments.
     *
     * @param args IOUserClientMethodArguments with structureOutput
     * @return kIOReturnSuccess on success, error code otherwise
     */
    kern_return_t GetDiscoveredDevices(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient
