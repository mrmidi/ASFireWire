// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioCoordinator.hpp"

#include "AudioEndpointRuntime.hpp"
#include "AudioRuntimeRegistry.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include <cstdio>
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

namespace ASFW::Audio {

AudioCoordinator::AudioCoordinator(IOService* driver,
                                   Discovery::IDeviceManager& deviceManager,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                    Driver::IsochService& isoch,
                                    Driver::HardwareInterface& hardware,
                                    DICE::DiceNotificationRouter& diceNotifications) noexcept
    : publisher_(driver)
    , deviceManager_(deviceManager)
    , registry_(registry)
    , runtime_(runtime)
    , hostTransport_(isoch)
    , sessions_(registry_, runtime_, hostTransport_, hardware, &teardownRequested_,
                [this](uint64_t guid) -> Runtime::IDirectAudioBindingSource* {
                    auto endpoint = runtime_.FindEndpointRuntime(guid);
                    return endpoint ? endpoint.get() : nullptr;
                })
    , dice_(publisher_, registry_, runtime_, sessions_, hardware, diceNotifications)
    , motu_(publisher_, registry_, runtime_, sessions_, hardware)
    , avc_(publisher_, registry_, runtime_, hostTransport_, sessions_, hardware) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioCoordinator: Failed to allocate lock");
    }

    sessions_.SetStartGuard([this](uint64_t guid) {
        return !publisher_.IsGeometryChangeBlocked(guid);
    });
    deviceManager_.RegisterDeviceObserver(this);
    hostTransport_.SetTimingLossCallback([this](uint64_t guid) { HandleHostTimingLoss(guid); });
    ASFW_LOG(Audio, "AudioCoordinator: Registered device observer");
}

AudioCoordinator::~AudioCoordinator() noexcept {
    deviceManager_.UnregisterDeviceObserver(this);
    hostTransport_.SetTimingLossCallback({});

    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AudioCoordinator::SetCMPClient(ASFW::CMP::CMPClient* client) noexcept {
    runtime_.SetCMPClient(client);
}

void AudioCoordinator::OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    const uint64_t guid = device->GetGUID();
    sessions_.Present(guid);
    if (lock_) {
        IOLockLock(lock_);
        remoteLostGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
    if (auto* backend = BackendForGuid(guid)) {
        backend->OnDeviceRecordUpdated(guid);
    }
}

void AudioCoordinator::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    const uint64_t guid = device->GetGUID();
    sessions_.Present(guid);
    if (lock_) {
        IOLockLock(lock_);
        remoteLostGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
    auto* backend = BackendForGuid(guid);
    if (backend) {
        backend->OnDeviceRecordUpdated(guid);
    }

    const bool recoverActiveStream = sessions_.IsStreaming(guid);
    if (!recoverActiveStream || !backend) {
        return;
    }

    backend->OnDeviceResumed(guid);
}

void AudioCoordinator::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) {
        return;
    }

    const uint64_t guid = device->GetGUID();
    const bool suspendedActiveStream = sessions_.IsStreaming(guid);
    if (!suspendedActiveStream) {
        return;
    }

    ASFW_LOG_WARNING(Audio,
                     "AudioCoordinator: Active device suspended; waiting for resume to recover GUID=0x%016llx",
                     guid);
}

void AudioCoordinator::OnDeviceRemoved(Discovery::Guid64 guid) {
    if (guid == 0) {
        return;
    }

    bool wasActive = false;
    bool firstRemoval = true;
    if (lock_) {
        IOLockLock(lock_);
        firstRemoval = remoteLostGuids_.insert(guid).second;
        wasActive = (activeGuid_ == guid);
        if (wasActive) {
            activeGuid_ = 0;
        }
        IOLockUnlock(lock_);
    }
    if (!firstRemoval) {
        return;
    }

    // Discovery has completed a new-generation scan and confirmed this GUID is
    // absent. Latch before touching backend work: delayed recovery and StopIO
    // callbacks must not recreate a session for the old route.
    sessions_.Retire(guid);
    // The registry has already invalidated the route policy by the time
    // removal is reported. Cancel all backend work so cleanup does not depend
    // on resolving a policy for a device that is known to be gone.
    dice_.CancelRemoteDeviceWork(guid);
    motu_.CancelRemoteDeviceWork(guid);
    avc_.CancelRemoteDeviceWork(guid);

    kern_return_t hostStatus = kIOReturnSuccess;
    if (wasActive) {
        // Do not release IRM resources after the reset that proved the remote
        // device absent: that allocation is already invalid in this generation.
        hostStatus = StopHostTransport("remote-device-lost", true);
        if (hostStatus != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "AudioCoordinator: remote-device host teardown incomplete "
                           "GUID=0x%016llx kr=0x%08x; completing removal",
                           guid, hostStatus);
        }
    }

    // Drop cross-seam transport views before unpublishing CoreAudio. Runtime
    // shared_ptr copies keep an already executing control operation alive, but
    // the terminal latch prevents it from starting a new duplex session.
    runtime_.Remove(guid);
    publisher_.TerminateNub(guid, "remote-device-lost");
    sessions_.Erase(guid);
    ASFW_LOG(Audio,
             "[Lifecycle] AudioCoordinator remote-device-lost owner GUID=0x%016llx "
             "active=%u host=0x%08x",
             guid, wasActive ? 1U : 0U, hostStatus);
}

void AudioCoordinator::OnAVCAudioConfigurationReady(uint64_t guid,
                                                   const Model::ASFWAudioDevice& config) noexcept {
    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        endpoint->UpdateConfig(config);
    }
    avc_.OnAudioConfigurationReady(guid, config);
}

void AudioCoordinator::HandleCycleInconsistent() noexcept {
    uint64_t guid = 0;
    if (lock_) {
        IOLockLock(lock_);
        guid = activeGuid_;
        IOLockUnlock(lock_);
    }

    if (guid == 0) {
        if (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3) {
            ASFW_LOG(Audio, "AudioCoordinator: Ignoring cycleInconsistent with no active audio GUID");
        }
        return;
    }

    if (auto* backend = BackendForGuid(guid)) {
        backend->HandleCycleInconsistent(guid);
    }
}

IAudioBackend* AudioCoordinator::BackendForGuid(uint64_t guid) noexcept {
    if (guid == 0) return nullptr;

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record.has_value()) {
        return nullptr;
    }

    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy || !registry_.IsCurrent(policy->route)) return nullptr;
    const auto backendKind = ChooseAudioBackend(policy->plan);
    if (!backendKind.has_value()) {
        return nullptr;
    }
    switch (*backendKind) {
        case AudioBackendKind::MotuRegister:
            return &motu_;
        case AudioBackendKind::Dice:
            return &dice_;
        case AudioBackendKind::Avc:
            return &avc_;
    }
    return nullptr;
}

IOReturn AudioCoordinator::StartStreaming(uint64_t guid) noexcept {
    if (publisher_.IsGeometryChangeBlocked(guid)) return kIOReturnNotReady;
    if (guid == 0) return kIOReturnBadArgument;

    bool setActive = false;
    if (lock_) {
        IOLockLock(lock_);
        if (remoteLostGuids_.contains(guid)) {
            IOLockUnlock(lock_);
            return kIOReturnNoDevice;
        }
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
            // This is the explicit v1 multi-device boundary: multiple GUIDs may
            // publish nubs/runtimes, but only one GUID may own isoch transport.
            // Simultaneous streaming starts here and requires per-GUID IR/IT
            // contexts, timing bridge, IRM/channel allocation, and backend sessions.
            return kIOReturnBusy;
        }
        IOLockUnlock(lock_);
    }

    const IOReturn kr = sessions_.Attach(guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StartStreaming failed GUID=0x%016llx kr=0x%x",
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
             "AudioCoordinator: StartStreaming ok GUID=0x%016llx",
             guid);
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    if (lock_) {
        IOLockLock(lock_);
        if (remoteLostGuids_.contains(guid)) {
            IOLockUnlock(lock_);
            return kIOReturnSuccess;
        }
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

    const IOReturn kr = sessions_.Detach(guid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StopStreaming failed GUID=0x%016llx kr=0x%x",
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
             "AudioCoordinator: StopStreaming ok GUID=0x%016llx",
             guid);
    return kIOReturnSuccess;
}

IOReturn AudioCoordinator::RequestClockConfig(
    uint64_t guid,
    const AudioClockConfig& desiredClock,
    DuplexRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ != 0 && activeGuid_ != guid) {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);
            ASFW_LOG_WARNING(Audio,
                             "AudioCoordinator: RequestClockConfig busy requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            return kIOReturnBusy;
        }
        IOLockUnlock(lock_);
    }

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record.has_value()) {
        ASFW_LOG_WARNING(Audio,
                         "AudioCoordinator: RequestClockConfig no registry record for GUID=0x%016llx (device not registered yet?)",
                         guid);
        return kIOReturnNotReady;
    }

    const IOReturn kr = sessions_.ChangeClock(guid, desiredClock, reason);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: RequestClockConfig failed GUID=0x%016llx kr=0x%x",
                       guid,
                       kr);
        return kr;
    }

    // Keep the endpoint's clock in step with the new device rate so the next
    // StartIO seeds the direct-binding/ZTS clock at the live rate. Without this
    // the binding stays at the publish-time rate (48 kHz) while the device runs
    // 44.1 kHz, and CoreAudio churns StartIO/StopIO on the clock mismatch.
    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        endpoint->SetCurrentSampleRate(desiredClock.sampleRateHz);
    }

    ASFW_LOG(Audio,
             "AudioCoordinator: RequestClockConfig ok GUID=0x%016llx rate=%uHz reason=%u",
             guid,
             desiredClock.sampleRateHz,
             static_cast<unsigned>(reason));
    return kIOReturnSuccess;
}

void AudioCoordinator::BeginTeardown() noexcept {
    ASFW_LOG(Audio, "AudioCoordinator: BeginTeardown");
    teardownRequested_.store(true, std::memory_order_release);
    // Block new backend recovery callbacks before draining either backend
    // queue. The coordinator owns this one subscription for every family.
    hostTransport_.SetTimingLossCallback({});
    dice_.BeginTeardown();
    motu_.BeginTeardown();
    avc_.BeginTeardown();
    const kern_return_t hostStatus = StopHostTransport("service-teardown");
    if (hostStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: host isoch teardown incomplete kr=0x%08x",
                       hostStatus);
    }

    if (lock_) {
        IOLockLock(lock_);
        activeGuid_ = 0;
        IOLockUnlock(lock_);
    }
}

kern_return_t AudioCoordinator::StopHostTransport(const char* reason,
                                                   bool generationInvalidated) noexcept {
    const kern_return_t status = generationInvalidated
                                     ? hostTransport_.StopAllAfterBusReset()
                                     : hostTransport_.StopAll();
    ASFW_LOG(Audio,
             "[Lifecycle] AudioCoordinator host-isoch teardown owner reason=%{public}s "
             "generation-invalidated=%u kr=0x%08x",
             reason, generationInvalidated ? 1U : 0U, status);
    return status;
}

IOReturn AudioCoordinator::MotuCaptureCommand(uint64_t guid, uint32_t stream,
                                             uint32_t command, std::string& output) noexcept {
    if (captureCommandBusy_.test_and_set(std::memory_order_acquire)) return kIOReturnBusy;
    struct Release final {
        std::atomic_flag& gate;
        ~Release() { gate.clear(std::memory_order_release); }
    } release{captureCommandBusy_};
    auto* capture = hostTransport_.DiagnosticCapture(stream);
    if (!capture || guid == 0 || command > 2) return kIOReturnBadArgument;
    if (teardownRequested_.load(std::memory_order_acquire)) return kIOReturnNotReady;
    if (command == 0) {
        // Only arm a known MOTU endpoint; never label another family's packets as MOTU.
        if (BackendForGuid(guid) != &motu_) return kIOReturnUnsupported;
        const auto protocol = runtime_.FindShared(guid);
        if (!protocol) return kIOReturnNotReady;
        Wire::MotuRxStreamMetadata metadata{};
        metadata.guid = guid;
        metadata.streamIndex = stream;
        const auto record = registry_.SnapshotByGuid(guid);
        const char* model = record ? DeviceProfiles::Audio::AudioDeviceCatalog::
            MotuModelNameForSwVersion(record->unitSwVersion.value_or(0)) : nullptr;
        snprintf(metadata.model, sizeof(metadata.model), "%s", model ? model : protocol->GetName());
        const auto endpoint = runtime_.FindEndpointRuntime(guid);
        Runtime::DirectAudioBindingSnapshot binding{};
        if (endpoint && endpoint->CopyDirectAudioBinding(binding))
            metadata.sampleRateHz = binding.sampleRateHz;
        return capture->Arm(metadata) ? kIOReturnSuccess : kIOReturnBusy;
    }
    Wire::MotuRxDiagnosticCapture::Snapshot snapshot{};
    if (!capture->CopySnapshot(snapshot)) return kIOReturnBusy;
    if (snapshot.metadata.guid != guid) return kIOReturnNotFound;
    if (!capture->Disarm()) return kIOReturnBusy;
    if (command == 2) {
        output = capture->FormatCaptureSummary();
        if (output.empty()) return kIOReturnBusy;
    }
    return kIOReturnSuccess;
}

bool AudioCoordinator::RequestMotuTimingRecovery(uint64_t guid) noexcept {
    if (teardownRequested_.load(std::memory_order_acquire) || BackendForGuid(guid) != &motu_)
        return false;
    return motu_.QueueTimingRecovery(guid);
}

void AudioCoordinator::HandleHostTimingLoss(uint64_t guid) noexcept {
    if (lock_) {
        IOLockLock(lock_);
        const bool remoteLost = remoteLostGuids_.contains(guid);
        IOLockUnlock(lock_);
        if (remoteLost) {
            return;
        }
    }
    if (auto* backend = BackendForGuid(guid)) {
        backend->HandleHostTimingLoss(guid);
    }
}

std::optional<uint64_t> AudioCoordinator::GetSinglePublishedGuid() const noexcept {
    // AudioNubPublisher is the source of truth for published audio endpoints.
    // This is intentionally used only for debug paths that still lack GUID selection.
    return publisher_.GetSingleGuid();
}

} // namespace ASFW::Audio
