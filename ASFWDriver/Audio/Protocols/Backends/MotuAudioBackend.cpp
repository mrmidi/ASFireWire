// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioBackend.cpp - see MotuAudioBackend.hpp.

#include "MotuAudioBackend.hpp"

#include "AudioDuplexCoordinator.hpp"
#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioNubPublisher.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include "../DeviceProtocolFactory.hpp"
#include "../IDeviceProtocol.hpp"
#include "../MOTU/MotuV2Protocol.hpp"

#include <algorithm>
#include <memory>
#include <string>

namespace ASFW::Audio {

MotuAudioBackend::MotuAudioBackend(AudioNubPublisher& publisher,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                   AudioDuplexCoordinator& duplexCoordinator,
                                   Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , coordinator_(duplexCoordinator) {
    lock_ = IOLockAlloc();
}

MotuAudioBackend::~MotuAudioBackend() noexcept {
    BeginTeardown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void MotuAudioBackend::BeginTeardown() noexcept {
    // Latch first so an in-flight StartStreaming refuses rather than handing the
    // coordinator a device whose bus is going away.
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.clear();
        IOLockUnlock(lock_);
    }
}

void MotuAudioBackend::EnsureNubForGuid(uint64_t guid) noexcept {
    if (guid == 0) {
        return;
    }

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record.has_value()) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: no registry record for GUID=0x%016llx",
                 guid);
        return;
    }

    auto protocol = runtime_.FindShared(guid);
    if (!protocol) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: no protocol for GUID=0x%016llx",
                 guid);
        return;
    }

    // MOTU has no profile registry to consult. The device's geometry comes from its own
    // registers via PrepareDuplex, which MotuV2Protocol reports through runtime caps --
    // so the nub is built from the hardware's answer rather than a table keyed on
    // model_id (which MOTU publishes as 0 anyway).
    Model::ASFWAudioDevice dev{};
    dev.guid = record->guid;
    dev.vendorId = record->vendorId;
    dev.modelId = record->modelId;
    dev.deviceName = protocol->GetName();
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    dev.sampleRates = {44100u, 48000u};
    dev.currentSampleRate = 48000u;

    AudioStreamRuntimeCaps caps{};
    if (protocol->GetRuntimeAudioStreamCaps(caps) && caps.sampleRateHz != 0) {
        dev.inputChannelCount = caps.hostInputPcmChannels;
        dev.outputChannelCount = caps.hostOutputPcmChannels;
        dev.currentSampleRate = caps.sampleRateHz;
    } else {
        // Not yet prepared: publish nothing rather than a nub with zero channels, which
        // CoreAudio would surface as a broken device the user has to remove by hand.
        ASFW_LOG(Audio,
                 "MotuAudioBackend::EnsureNubForGuid: runtime caps unavailable for GUID=0x%016llx; deferring publish",
                 guid);
        return;
    }
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);

    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        endpoint->UpdateConfig(dev);
    }
    (void)publisher_.EnsureNub(guid, dev, "MOTU");
}

IOReturn MotuAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "MotuAudioBackend: StartStreaming refused by teardown GUID=0x%016llx",
                 guid);
        return kIOReturnAborted;
    }

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        EnsureNubForGuid(guid);
        nub = publisher_.GetNub(guid);
        if (!nub) {
            return kIOReturnNotReady;
        }
    }

    auto endpoint = runtime_.FindEndpointRuntime(guid);
    if (!endpoint || !endpoint->HasCompleteDirectAudioMemory()) {
        ASFW_LOG_ERROR(Audio,
                       "MotuAudioBackend: StartStreaming refused missing direct runtime/memory GUID=0x%016llx",
                       guid);
        return kIOReturnNotReady;
    }

    const IOReturn status = coordinator_.StartStreaming(guid);
    if (status == kIOReturnSuccess) {
        EnsureNubForGuid(guid);
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.insert(guid);
            IOLockUnlock(lock_);
        }
    }
    return status;
}

IOReturn MotuAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.erase(guid);
            IOLockUnlock(lock_);
        }
        return kIOReturnAborted;
    }

    const IOReturn status = coordinator_.StopStreaming(guid);
    if (status == kIOReturnSuccess && lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
    return status;
}

} // namespace ASFW::Audio
