//
//  DeviceDiscoveryWireFormats.hpp
//  ASFWDriver
//
//  Wire format structures for Device Discovery
//

#ifndef ASFW_USERCLIENT_DEVICE_DISCOVERY_WIRE_FORMATS_HPP
#define ASFW_USERCLIENT_DEVICE_DISCOVERY_WIRE_FORMATS_HPP

#include "WireFormatsCommon.hpp"

namespace ASFW::UserClient::Wire {

/// Wire format for a FireWire unit
struct __attribute__((packed)) FWUnitWire {
    uint32_t specId;
    uint32_t swVersion;
    uint32_t romOffset;
    uint8_t state;              // 0=Created, 1=Ready, 2=Suspended, 3=Terminated
    uint8_t _padding[3];
    char vendorName[64];        // null-terminated
    char productName[64];       // null-terminated
};

/// Wire format for a FireWire device
struct __attribute__((packed)) FWDeviceWire {
    uint64_t guid;
    uint32_t vendorId;
    uint32_t modelId;
    uint32_t generation;
    uint8_t nodeId;
    uint8_t state;              // 0=Created, 1=Ready, 2=Suspended, 3=Terminated
    uint8_t unitCount;          // Number of units following this device
    uint8_t _padding;
    char vendorName[64];        // null-terminated
    char modelName[64];         // null-terminated
    // Followed by: FWUnitWire array (unitCount elements)
};

/// Wire format for device discovery response
struct __attribute__((packed)) DeviceDiscoveryWire {
    uint32_t deviceCount;       // Number of devices
    uint32_t _padding;
    // Followed by: FWDeviceWire array (with embedded units)
};

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_DEVICE_DISCOVERY_WIRE_FORMATS_HPP
