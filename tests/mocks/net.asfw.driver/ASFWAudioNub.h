// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include <DriverKit/IOService.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <cstdint>
#include "Audio/Model/AudioPropertyKeys.hpp"
#include "Audio/Runtime/RemoteDeviceStopResult.hpp"

class ASFWAudioNub : public IOService {
public:
    std::atomic<uint64_t> remoteStopResult{0};
    void RecordRemoteDeviceStopResult(kern_return_t status) {
        ASFW::Audio::Runtime::RemoteDeviceStopResult::Publish(remoteStopResult, status);
    }
    virtual ~ASFWAudioNub() = default;
    void SetStreamMode(uint32_t) {}
    void SetGuid(uint64_t) {}
    // Mirrors the real nub: 48 kHz until RefreshFromProperties finds the
    // published rate in the properties CopyProperties returns.
    [[nodiscard]] uint32_t GetCurrentSampleRateHz() const noexcept { return currentRateHz; }
    void RefreshFromProperties() {
        ++refreshes;
        OSDictionary* raw = nullptr;
        if (CopyProperties(&raw) != kIOReturnSuccess || raw == nullptr) return;
        OSSharedPtr<OSDictionary> props(raw, OSNoRetain);
        if (auto* rate = OSDynamicCast(OSNumber,
                                       props->getObject(ASFW::Audio::Model::PropertyKeys::kCurrentSampleRate));
            rate != nullptr && rate->unsigned32BitValue() != 0) {
            currentRateHz = rate->unsigned32BitValue();
        }
    }
    uint32_t currentRateHz{48000};
    uint32_t refreshes{0};
    void NotifyDeviceClockChanged(uint32_t rate) noexcept {
        ++clockChangeNotifications;
        lastObservedRate = rate;
    }
    uint32_t clockChangeNotifications{0};
    uint32_t lastObservedRate{0};
    // No audio driver in the host suite: restarts stay in place.
    bool NotifyIoRestartRequired(uint32_t) noexcept { return false; }
    void TxPreparationReady(OSAction*, uint64_t generation) {
        ++txPreparationRequests;
        lastTxPreparationGeneration = generation;
    }
    void RequestTimingRecovery(uint64_t rxEpoch) { lastTimingRecoveryEpoch = rxEpoch; }

    kern_return_t rpcStopResult{kIOReturnSuccess};
    kern_return_t StopAudioStreamingOrRemoteResult() {
        if (const auto terminal = ASFW::Audio::Runtime::RemoteDeviceStopResult::Read(remoteStopResult))
            return *terminal;
        return StopAudioStreaming();
    }
    kern_return_t StopAudioStreaming() {
        ++stopStreamingCalls;
        return rpcStopResult;
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
