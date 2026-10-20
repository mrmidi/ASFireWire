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
#include <functional>
#include <map>
#include <utility>
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

    // The endpoint's MIDI seam, as the real nub forwards it from the endpoint
    // runtime. No MIDI (kIOReturnNotReady) unless a test installs a provider.
    std::function<kern_return_t(IOMemoryDescriptor**, uint32_t*, uint32_t*, uint32_t*,
                                uint32_t*, uint32_t*)> midiTransportProvider{};
    kern_return_t CopyMidiTransportMemory(IOMemoryDescriptor** outMemory, uint32_t* outPorts,
                                          uint32_t* outSlotIndex, uint32_t* outDbs,
                                          uint32_t* outPcmChannels, uint32_t* outDbcAligned) {
        if (!midiTransportProvider) return kIOReturnNotReady;
        return midiTransportProvider(outMemory, outPorts, outSlotIndex, outDbs, outPcmChannels,
                                     outDbcAligned);
    }

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

    kern_return_t RegisterControlChangeAction(OSAction* action) { controlChangeAction = action; return kIOReturnSuccess; }
    OSAction* controlChangeAction{nullptr};

    // Generic parameter seam. A push must never cause a write; reads answer
    // from `readValues` (parameter -> {kind, bits}), as the controller's state.
    kern_return_t SetControlParameter(uint32_t, uint64_t, uint64_t, uint64_t*, uint64_t*) {
        ++parameterWrites; return kIOReturnUnsupported;
    }
    kern_return_t ReadControlParameter(uint32_t parameterId, bool normalized, uint64_t* kind, uint64_t* bits) {
        ++parameterReads;
        const auto found = readValues.find({parameterId, normalized});
        if (found == readValues.end()) return kIOReturnNotFound;
        *kind = found->second.first; *bits = found->second.second;
        return kIOReturnSuccess;
    }
    std::map<std::pair<uint32_t, bool>, std::pair<uint64_t, uint64_t>> readValues;
    uint32_t parameterWrites{0};
    uint32_t parameterReads{0};
    uint32_t stopStreamingCalls{0};
    OSAction* txPreparationAction{nullptr};
    OSAction* ztsAnchorAction{nullptr};
    OSAction* deviceClockChangedAction{nullptr};
    OSAction* ioRestartRequiredAction{nullptr};
    uint64_t txPreparationRequests{0};
    uint64_t lastTxPreparationGeneration{0};
    uint64_t lastTimingRecoveryEpoch{0};
};
