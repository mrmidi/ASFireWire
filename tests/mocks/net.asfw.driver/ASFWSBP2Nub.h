// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>

#include <cstdint>
#include <map>
#include <string>

class ASFWSBP2Nub : public IOService {
public:
    kern_return_t CopyProperties(OSDictionary** out) override {
        *out = OSDictionary::withCapacity(4);
        return kIOReturnSuccess;
    }

    // Record the numeric properties a publisher sets, for assertions.
    kern_return_t SetProperties(OSDictionary* properties) override {
        ++setPropertiesCalls;
        lastPropertyCount = properties->getCount();
        for (const char* key : kRecordedKeys) {
            if (auto* number = OSDynamicCast(OSNumber, properties->getObject(key))) {
                numbers[key] = number->unsigned64BitValue();
            }
        }
        return kIOReturnSuccess;
    }

    kern_return_t Terminate(uint64_t) override {
        ++terminateCalls;
        return kIOReturnSuccess;
    }

    int terminateCalls{0};
    int setPropertiesCalls{0};
    unsigned lastPropertyCount{0};
    std::map<std::string, uint64_t> numbers;

    static constexpr const char* kRecordedKeys[] = {
        "Vendor_ID", "GUID", "Unit_Spec_ID", "Unit_SW_Version", "Command_Set_Spec_ID",
        "Command_Set", "Command_Set_Revision", "Firmware_Revision", "Unit_Characteristics",
        "Management_Agent_Offset", "Device_Type", "ASFWBusGeneration",
    };
};
