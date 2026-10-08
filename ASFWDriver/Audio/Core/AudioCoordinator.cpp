// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioCoordinator.hpp"

#include "AudioEndpointRuntime.hpp"
#include "AudioRuntimeRegistry.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include <net.asfw.driver/ASFWAudioNub.h>
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
    , dice_(publisher_, registry_, runtime_, sessions_, diceNotifications)
    , motu_(publisher_, registry_, runtime_, sessions_)
    , avc_(publisher_, registry_, runtime_, hostTransport_, sessions_)
    , host_(publisher_, registry_, runtime_, sessions_, hostTransport_) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioCoordinator: Failed to allocate lock");
    }

    host_.Install(AudioBackendKind::RmeRegister, rmeAdapter_);

    sessions_.SetStartGuard([this](uint64_t guid) {
        return !publisher_.IsGeometryChangeBlocked(guid);
    });
    // A restart while CoreAudio runs the streams goes through the host
    // (StopIO -> StartIO) so the audio-owned TX queue is rebuilt too.
    sessions_.SetHostRestartRouter([this](uint64_t guid, DuplexRestartReason reason) {
        ASFWAudioNub* nub = publisher_.GetNub(guid);
        return nub != nullptr && nub->NotifyIoRestartRequired(static_cast<uint32_t>(reason));
    });
    sessions_.SetRestartObserver([this](uint64_t guid) {
        if (auto* backend = BackendForGuid(guid)) {
            backend->OnStreamsRestarted(guid);
        } else if (ServedByHost(guid)) {
            host_.RefreshPublication(guid);
        }
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
        remoteLostStopResults_.erase(guid);
        IOLockUnlock(lock_);
    }
    if (auto* backend = BackendForGuid(guid)) {
        backend->OnDeviceRecordUpdated(guid);
    } else if (ServedByHost(guid)) {
        host_.RefreshPublication(guid);
    }
}

void AudioCoordinator::OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) return;
    const uint64_t guid = device->GetGUID();
    sessions_.Present(guid);
    if (lock_) {
        IOLockLock(lock_);
        remoteLostStopResults_.erase(guid);
        IOLockUnlock(lock_);
    }
    auto* backend = BackendForGuid(guid);
    const bool hostServed = backend == nullptr && ServedByHost(guid);
    if (backend) {
        backend->OnDeviceRecordUpdated(guid);
    } else if (hostServed) {
        host_.RefreshPublication(guid);
    }

    const bool recoverActiveStream = sessions_.IsStreaming(guid);
    if (!recoverActiveStream) {
        return;
    }

    if (backend) {
        backend->OnDeviceResumed(guid);
    } else if (hostServed) {
        host_.OnDeviceResumed(guid);
    }
}

void AudioCoordinator::HandleBusReset() noexcept {
    uint64_t guid = 0;
    if (lock_) {
        IOLockLock(lock_);
        guid = activeGuid_;
        IOLockUnlock(lock_);
    }
    if (!guid || teardownRequested_.load(std::memory_order_acquire)) return;
    sessions_.CancelPendingRestart(guid);
    // Linux dice-stream.c:587-605 stops the domain on reset because firmware
    // loses stream synchronization. Never spend the ROM-scan interval running
    // the old finite IT mapping. The generation invalidated its IRM leases;
    // local cleanup must not issue remote release/device-stop transactions.
    // Do not destroy consumers or mutate the session's reservation ledger on
    // this interrupt queue while reconciliation may still own them.
    const auto status = hostTransport_.QuiesceForBusReset();
    ASFW_LOG(Audio, "[StopTrace] owner=reset guid=%016llx local=0x%x (%{public}s) action=await-rebind", guid, status, ASFW::Logging::IOReturnName(status));
}

void AudioCoordinator::OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) {
    if (!device) {
        return;
    }

    const uint64_t guid = device->GetGUID();
    // A restart still waiting out its quiet period targets the generation that
    // just ended; the resume requests a fresh one. Firing it now would restart
    // a device with no operational node (hardware, 2026-09-25: a reset landing
    // as the quiet period expired sent CoreAudio a restart it could not start).
    sessions_.CancelPendingRestart(guid);
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
        firstRemoval = remoteLostStopResults_.try_emplace(guid, kIOReturnNotReady).second;
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
    host_.CancelRemoteDeviceWork(guid);
    avc_.CancelRemoteDeviceWork(guid);

    kern_return_t hostStatus = kIOReturnSuccess;
    if (wasActive) {
        // Do not release IRM resources after the reset that proved the remote
        // device absent: that allocation is already invalid in this generation.
        hostStatus = StopHostTransport("remote-device-lost", true);
        if (hostStatus != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "AudioCoordinator: remote-device host teardown incomplete "
                           "GUID=0x%016llx kr=0x%08x (%{public}s); completing removal",
                           guid, hostStatus, ASFW::Logging::IOReturnName(hostStatus));
        }
    }

    // Drop cross-seam transport views before unpublishing CoreAudio. Runtime
    // shared_ptr copies keep an already executing control operation alive, but
    // the terminal latch prevents it from starting a new duplex session.
    if (lock_) {
        IOLockLock(lock_);
        if (auto it = remoteLostStopResults_.find(guid); it != remoteLostStopResults_.end())
            it->second = hostStatus;
        IOLockUnlock(lock_);
    }
    runtime_.Remove(guid);
    publisher_.TerminateNub(guid, "remote-device-lost", hostStatus);
    sessions_.Erase(guid);
    ASFW_LOG(Audio,
             "[Lifecycle] AudioCoordinator remote-device-lost owner GUID=0x%016llx "
             "active=%u host=0x%08x (%{public}s)",
             guid, wasActive ? 1U : 0U, hostStatus, ASFW::Logging::IOReturnName(hostStatus));
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

    // Only backends forward it. RME never acted on cycle inconsistent; whether
    // the host forwards it for every family is decided when DICE, the one
    // family that acts on it, moves (AUDIO_DEVICE_HOST.md §6 E5).
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
        case AudioBackendKind::RmeRegister:
            return nullptr;  // served by the host
        case AudioBackendKind::Dice:
            return &dice_;
        case AudioBackendKind::Avc:
            return &avc_;
    }
    return nullptr;
}

bool AudioCoordinator::ServedByHost(uint64_t guid) const noexcept {
    const auto kind = host_.KindForGuid(guid);
    return kind.has_value() && *kind == AudioBackendKind::RmeRegister;
}

IOReturn AudioCoordinator::StartStreaming(uint64_t guid, AudioClockConfig clock) noexcept {
    if (publisher_.IsGeometryChangeBlocked(guid)) return kIOReturnNotReady;
    if (guid == 0) return kIOReturnBadArgument;

    bool setActive = false;
    if (lock_) {
        IOLockLock(lock_);
        if (remoteLostStopResults_.contains(guid)) {
            IOLockUnlock(lock_);
            return kIOReturnNoDevice;
        }
        if (activeGuid_ == 0) {
            activeGuid_ = guid;
            setActive = true;
        } else if (activeGuid_ != guid) {
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

    // Ownership can survive a failed stop after host transport has quiesced.
    // Only the session knows whether this is an idempotent running start or
    // whether retained device connections must be cleaned up before restarting.
    const IOReturn kr = sessions_.Attach(guid, clock);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: StartStreaming failed GUID=0x%016llx kr=0x%x (%{public}s)",
                       guid,
                       kr, ASFW::Logging::IOReturnName(kr));
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
        if (auto it = remoteLostStopResults_.find(guid); it != remoteLostStopResults_.end()) {
            const auto status = it->second;
            IOLockUnlock(lock_);
            return status;
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
                       "AudioCoordinator: StopStreaming failed GUID=0x%016llx kr=0x%x (%{public}s)",
                       guid,
                       kr, ASFW::Logging::IOReturnName(kr));
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

    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (policy && policy->plan.family == DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister &&
        desiredClock.sampleRateHz != 48000U) {
        // Fireface control does not expose a software rate switch. Refuse at
        // the coordinator boundary before any protocol read/write is queued.
        return kIOReturnUnsupported;
    }

    const IOReturn kr = sessions_.ChangeClock(guid, desiredClock, reason);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: RequestClockConfig failed GUID=0x%016llx kr=0x%x (%{public}s)",
                       guid,
                       kr, ASFW::Logging::IOReturnName(kr));
        return kr;
    }

    // Keep the endpoint's clock in step with the new device rate so the next
    // StartIO seeds the direct-binding/ZTS clock at the live rate. Without this
    // the binding stays at the publish-time rate (48 kHz) while the device runs
    // 44.1 kHz, and CoreAudio churns StartIO/StopIO on the clock mismatch.
    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        Model::ASFWAudioDevice config;
        // An AV/C CONTROL result does not commit an audio configuration. Its
        // host-window transaction installs rate, formations and epoch after
        // STATUS/readback confirmation for every catalog endpoint.
        if (!endpoint->CopyConfig(config) || config.rateFormationCandidates.empty())
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
    // Pending restarts go first: once the backends drain, nothing can raise one.
    sessions_.BeginTeardown();
    // Block new backend recovery callbacks before draining either backend
    // queue. The coordinator owns this one subscription for every family.
    hostTransport_.SetTimingLossCallback({});
    dice_.BeginTeardown();
    motu_.BeginTeardown();
    host_.BeginTeardown();
    avc_.BeginTeardown();
    const kern_return_t hostStatus = StopHostTransport("service-teardown");
    if (hostStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AudioCoordinator: host isoch teardown incomplete kr=0x%08x (%{public}s)",
                       hostStatus, ASFW::Logging::IOReturnName(hostStatus));
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
             "generation-invalidated=%u kr=0x%08x (%{public}s)",
             reason, generationInvalidated ? 1U : 0U, status, ASFW::Logging::IOReturnName(status));
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
        const bool remoteLost = remoteLostStopResults_.contains(guid);
        IOLockUnlock(lock_);
        if (remoteLost) {
            return;
        }
    }
    if (auto* backend = BackendForGuid(guid)) {
        backend->HandleHostTimingLoss(guid);
    } else if (ServedByHost(guid)) {
        host_.OnRuntimeFault(guid, DuplexRestartReason::kRecoverAfterTimingLoss);
    }
}

std::optional<uint64_t> AudioCoordinator::GetSinglePublishedGuid() const noexcept {
    // AudioNubPublisher is the source of truth for published audio endpoints.
    // This is intentionally used only for debug paths that still lack GUID selection.
    return publisher_.GetSingleGuid();
}

} // namespace ASFW::Audio
