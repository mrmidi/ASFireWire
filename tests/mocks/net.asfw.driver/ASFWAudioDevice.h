// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host stand-in for the IIG-generated ASFWAudioDevice.h. The TX producer only
// publishes zero timestamps to the HAL, so this records each publication for
// the tests to inspect.

#pragma once

#include <DriverKit/OSObject.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include <cstdint>
#include <vector>

struct ASFWAudioDriver_IVars;

class ASFWAudioDevice : public OSObject, public IOUserAudioDevice {
public:
    void SetDriverIvars(ASFWAudioDriver_IVars* ivars) {
        driverIvars = ivars;
        ++driverIvarsWrites;
    }
    ASFWAudioDriver_IVars* driverIvars{nullptr};
    uint32_t driverIvarsWrites{0};

    struct ZeroTimestamp final {
        uint64_t sampleTime{0};
        uint64_t hostTime{0};
    };

    kern_return_t ReleaseKeptWireForDriverStop() {
        ++keptWireReleaseRequests;
        return kIOReturnSuccess;
    }
    uint32_t keptWireReleaseRequests{0};

    void UpdateCurrentZeroTimestamp(uint64_t sampleTime, uint64_t hostTime) override {
        published.push_back({sampleTime, hostTime});
    }

    std::vector<ZeroTimestamp> published;
};
