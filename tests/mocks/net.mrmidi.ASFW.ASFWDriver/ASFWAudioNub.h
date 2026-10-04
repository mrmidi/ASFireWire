// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOService.h>
#include <DriverKit/OSAction.h>
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
    void TxPreparationReady(OSAction*, uint64_t generation) {
        ++txPreparationRequests;
        lastTxPreparationGeneration = generation;
    }
    void RequestTimingRecovery(uint64_t rxEpoch) { lastTimingRecoveryEpoch = rxEpoch; }

    kern_return_t StopAudioStreaming() {
        ++stopStreamingCalls;
        return kIOReturnSuccess;
    }
    // Each registration records what it was given; the driver's stop passes null.
    kern_return_t RegisterTxPreparationAction(OSAction* action) { txPreparationAction = action; return kIOReturnSuccess; }
    kern_return_t RegisterZtsAnchorAction(OSAction* action) { ztsAnchorAction = action; return kIOReturnSuccess; }
    kern_return_t RegisterDeviceClockChangedAction(OSAction* action) { deviceClockChangedAction = action; return kIOReturnSuccess; }
    kern_return_t RegisterIoRestartRequiredAction(OSAction* action) { ioRestartRequiredAction = action; return kIOReturnSuccess; }

    uint32_t stopStreamingCalls{0};
    OSAction* txPreparationAction{nullptr};
    OSAction* ztsAnchorAction{nullptr};
    OSAction* deviceClockChangedAction{nullptr};
    OSAction* ioRestartRequiredAction{nullptr};
    uint64_t txPreparationRequests{0};
    uint64_t lastTxPreparationGeneration{0};
    uint64_t lastTimingRecoveryEpoch{0};
};
