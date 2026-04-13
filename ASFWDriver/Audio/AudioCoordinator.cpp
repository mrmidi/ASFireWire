// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "AudioCoordinator.hpp"

#include "../Discovery/FWDevice.hpp"

namespace ASFW::Audio {

AudioCoordinator::AudioCoordinator(IOService* driver,
                                   Discovery::IDeviceManager& deviceManager,
                                   Discovery::DeviceRegistry& registry,
                                   Driver::IsochService& isoch,
                                   Driver::HardwareInterface& hardware) noexcept
    : publisher_(driver)
    , dice_(publisher_, registry, isoch, hardware)
    , avc_(publisher_, registry, isoch, hardware)
    , deviceManager_(deviceManager)
    , registry_(registry) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioCoordinator: Failed to allocate lock");
    }

    deviceManager_.RegisterDeviceObserver(this);
    ASFW_LOG(Audio, "AudioCoordinator: Registered device observer");
}

AudioCoordinator::~AudioCoordinator() noexcept {
    deviceManager_.UnregisterDeviceObserver(this);

    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AudioCoordinator::SetCMPClient(ASFW::CMP::CMPClient* client) noexcept {
    avc_.SetCMPClient(client);
}

void AudioCoordinator::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    dice_.OnDeviceRecordUpdated(device->GetGUID());
}

void AudioCoordinator::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    dice_.OnDeviceRecordUpdated(device->GetGUID());
}

void AudioCoordinator::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    (void)device;
    // No-op for now: bus resets can suspend devices transiently and we don't yet have a robust
    // "stop+restart while CoreAudio is running" pipeline here.
}

void AudioCoordinator::OnDeviceRemoved(Discovery::Guid64 guid) {
    if (guid == 0) return;

    // Ensure isoch transport is stopped (best-effort) and nubs are terminated.
    dice_.OnDeviceRemoved(guid);
    avc_.OnDeviceRemoved(guid);

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
        IOLockUnlock(lock_);
    }
}

void AudioCoordinator::OnAVCAudioConfigurationReady(uint64_t guid,
                                                   const Model::ASFWAudioDevice& config) noexcept {
    avc_.OnAudioConfigurationReady(guid, config);
}

IAudioBackend* AudioCoordinator::BackendForGuid(uint64_t guid) noexcept {
    if (guid == 0) return nullptr;

    const auto* record = registry_.FindByGuid(guid);
    if (!record) {
        return &avc_;
    }

    const auto integration = DeviceProtocolFactory::LookupIntegrationMode(record->vendorId, record->modelId);
    if (integration == DeviceIntegrationMode::kHardcodedNub) {
        return &dice_;
    }

    return &avc_;
}

IOReturn AudioCoordinator::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    bool setActive = false;
    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == 0) {
            activeGuid_ = guid;
            setActive = true;
        } else if (activeGuid_ == guid) {
            IOLockUnlock(lock_);
            // Idempotent start: avoid reconfiguring already-running IR/IT contexts.
            return kIOReturnSuccess;
        } else {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);

            ASFW_LOG_WARNING(Audio,
                             "AudioCoordinator: StartStreaming busy requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            // TODO(ASFW-MULTIDEVICE): Multi-device streaming is not implemented.
            // We currently have a single global IR/IT transport and single external SYT clock bridge.
            // Supporting multiple devices requires per-GUID IR/IT contexts, per-device queue wiring,
            // and a GUID-keyed clock discipline pipeline.
            return kIOReturnBusy;
        }
        IOLockUnlock(lock_);
    }

    auto* backend = BackendForGuid(guid);
    if (!backend) {
        if (setActive && lock_) {
            IOLockLock(lock_);
            if (activeGuid_ == guid) activeGuid_ = 0;
            IOLockUnlock(lock_);
        }
        return kIOReturnNotReady;
    }

    const IOReturn kr = backend->StartStreaming(guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StartStreaming failed backend=%{public}s GUID=0x%016llx kr=0x%x",
                       backend->Name(),
                       guid,
                       kr);
        if (setActive && lock_) {
            IOLockLock(lock_);
            if (activeGuid_ == guid) activeGuid_ = 0;
            IOLockUnlock(lock_);
        }
        return kr;
    }

    ASFW_LOG(Audio,
             "AudioCoordinator: StartStreaming ok backend=%{public}s GUID=0x%016llx",
             backend->Name(),
             guid);
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ != 0 && activeGuid_ != guid) {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);
            ASFW_LOG_WARNING(Audio,
                             "AudioCoordinator: StopStreaming busy requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            return kIOReturnBusy;
        }
        IOLockUnlock(lock_);
    }

    auto* backend = BackendForGuid(guid);
    if (!backend) return kIOReturnNotReady;

    const IOReturn kr = backend->StopStreaming(guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StopStreaming failed backend=%{public}s GUID=0x%016llx kr=0x%x",
                       backend->Name(),
                       guid,
                       kr);
        return kr;
    }

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == guid) activeGuid_ = 0;
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio,
             "AudioCoordinator: StopStreaming ok backend=%{public}s GUID=0x%016llx",
             backend->Name(),
             guid);
    return kIOReturnSuccess;
}

std::optional<uint64_t> AudioCoordinator::GetSinglePublishedGuid() const noexcept {
    // AudioNubPublisher is the source of truth for published audio endpoints.
    // This is intentionally used only for debug paths that still lack GUID selection.
    return publisher_.GetSingleGuid();
}

} // namespace ASFW::Audio
