// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOService.h>
#include <cstdint>

class ASFWAudioNub : public IOService {
public:
    virtual ~ASFWAudioNub() = default;
    void SetStreamMode(uint32_t) {}
    void SetGuid(uint64_t) {}
    [[nodiscard]] uint32_t GetCurrentSampleRateHz() const noexcept { return 48000; }
    void NotifyDeviceClockChanged(uint32_t) noexcept {}
    // No audio driver in the host suite: restarts stay in place.
    bool NotifyIoRestartRequired(uint32_t) noexcept { return false; }
    kern_return_t RequestTxPreparation(uint64_t generation) {
        ++txPreparationRequests;
        lastTxPreparationGeneration = generation;
        return kIOReturnSuccess;
    }
    void RequestTimingRecovery(uint64_t) {}

    uint64_t txPreparationRequests{0};
    uint64_t lastTxPreparationGeneration{0};
};
