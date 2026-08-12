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

inline constexpr uint16_t kDeviceDiscoveryWireVersion = 2;

enum DeviceEvidenceFlags : uint8_t {
    kHasRootVendorId = 1U << 0U,
    kHasRootModelId = 1U << 1U,
};

enum UnitEvidenceFlags : uint32_t {
    kHasUnitVendorId = 1U << 0U,
    kHasUnitModelId = 1U << 1U,
    kHasLun = 1U << 2U,
    kHasManagementAgentOffset = 1U << 3U,
    kHasUnitCharacteristics = 1U << 4U,
    kHasFastStart = 1U << 5U,
};

/// Complete unit-directory evidence for one runtime unit.
struct __attribute__((packed)) FWUnitWireV2 {
    uint16_t version;
    uint16_t byteSize;
    uint64_t deviceInstanceId;
    uint32_t romOffset;
    uint32_t specId;
    uint32_t swVersion;
    uint32_t evidenceFlags;
    uint32_t vendorId;
    uint32_t modelId;
    uint32_t lun;
    uint32_t managementAgentOffset;
    uint32_t unitCharacteristics;
    uint32_t fastStart;
    uint8_t state;
    uint8_t _padding[3];
    char vendorName[64];        // null-terminated
    char productName[64];       // null-terminated
};

static_assert(sizeof(FWUnitWireV2) == 184, "FWUnitWireV2 layout changed");

/// Runtime device plus independent bus/root identity evidence. Raw BIB words
/// follow this fixed record, then FWUnitWireV2 records.
struct __attribute__((packed)) FWDeviceWireV2 {
    uint16_t version;
    uint16_t byteSize;
    uint64_t deviceInstanceId;
    uint64_t observedGuid;
    uint32_t generation;
    uint16_t nodeId;
    uint8_t state;
    uint8_t quarantineReason;
    uint8_t deviceKind;
    uint8_t evidenceFlags;
    uint16_t unitCount;
    uint16_t busInfoWordCount;
    uint32_t nodeVendorOui;
    uint32_t rootVendorId;
    uint32_t rootModelId;
    char vendorName[64];        // null-terminated
    char modelName[64];         // null-terminated
    // Followed by raw bus-info quadlets, then FWUnitWireV2[unitCount].
};

static_assert(sizeof(FWDeviceWireV2) == 174, "FWDeviceWireV2 layout changed");

struct __attribute__((packed)) DeviceDiscoveryWireV2 {
    uint16_t version;
    uint16_t headerSize;
    uint32_t byteSize;
    uint32_t deviceCount;
    uint32_t _reserved;
    // Followed by FWDeviceWireV2 records and their bounded sections.
};

static_assert(sizeof(DeviceDiscoveryWireV2) == 16,
              "DeviceDiscoveryWireV2 layout changed");

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_DEVICE_DISCOVERY_WIRE_FORMATS_HPP
