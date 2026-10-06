// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>

class ASFWSBP2Nub : public IOService {
public:
    kern_return_t CopyProperties(OSDictionary** out) override {
        *out = OSDictionary::withCapacity(4);
        return kIOReturnSuccess;
    }

    kern_return_t Terminate(uint64_t) override {
        ++terminateCalls;
        return kIOReturnSuccess;
    }

    int terminateCalls{0};
};
