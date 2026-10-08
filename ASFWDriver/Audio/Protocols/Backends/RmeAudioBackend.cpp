// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#include "RmeAudioBackend.hpp"
#include "../../Core/AudioNubPublisher.hpp"
#include "../../Core/AudioRuntimeRegistry.hpp"
#include "../../Core/AudioEndpointRuntime.hpp"
#include "../IDeviceProtocol.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>

namespace ASFW::Audio {
RmeAudioBackend::RmeAudioBackend(AudioNubPublisher& publisher, Discovery::DeviceRegistry& registry,
                                 AudioRuntimeRegistry& runtime, Session::AudioSessions& sessions) noexcept
    : publisher_(publisher), registry_(registry), runtime_(runtime), sessions_(sessions) {
    lock_ = IOLockAlloc();
    IODispatchQueue* queue = nullptr;
    if (IODispatchQueue::Create("com.asfw.audio.rme", 0, 0, &queue) == kIOReturnSuccess && queue)
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
}

RmeAudioBackend::~RmeAudioBackend() noexcept {
    BeginTeardown();
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
}

void RmeAudioBackend::BeginTeardown() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (teardownStarted_.exchange(true, std::memory_order_acq_rel)) {
        while (!teardownComplete_.load(std::memory_order_acquire)) IOSleep(1);
        return;
    }
    recoveryAdmission_.CloseAndWait();
    if (workQueue_) {
#ifdef ASFW_HOST_TEST
        workQueue_->DispatchSync([] {});
#else
        workQueue_->DispatchSync(^{ });
#endif
    }
    if (lock_) {
        IOLockLock(lock_);
        recoveringGuids_.clear();
        IOLockUnlock(lock_);
    }
    teardownComplete_.store(true, std::memory_order_release);
}

void RmeAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    if (!stopping_.load(std::memory_order_acquire)) EnsureNubForGuid(guid);
}

void RmeAudioBackend::OnDeviceResumed(uint64_t guid) noexcept {
    PublicationGate::AdmissionScope admission(recoveryAdmission_);
    if (!admission.IsAdmitted() || guid == 0 || stopping_.load(std::memory_order_acquire) ||
        !workQueue_ || !sessions_.IsStreaming(guid) || sessions_.IsCancelled(guid)) {
        return;
    }

    const auto record = registry_.SnapshotByGuid(guid);
    const auto* policy = record ? DeviceProfiles::Audio::CurrentAudioPolicy(*record) : nullptr;
    if (!policy || policy->plan.family != DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister ||
        !registry_.IsCurrent(policy->route)) {
        return;
    }

    if (lock_) {
        IOLockLock(lock_);
        const bool inserted = recoveringGuids_.insert(guid).second;
        IOLockUnlock(lock_);
        if (!inserted) return;
    }

    const auto route = policy->route;
    const uint64_t run = sessions_.RunningRun(guid);
    workQueue_->DispatchAsync(^{
        // The resumed route has fresh node/channel state. Re-run the full RME
        // configure/reserve/assign/start sequence while CoreAudio still owns IO.
        if (!stopping_.load(std::memory_order_acquire) && registry_.IsCurrent(route) &&
            sessions_.IsStreaming(guid) && !sessions_.IsCancelled(guid)) {
            const IOReturn status =
                sessions_.RequestRestart(guid, DuplexRestartReason::kBusResetRebind, run);
            if (status != kIOReturnSuccess && status != kIOReturnUnsupported &&
                status != kIOReturnAborted) {
                ASFW_LOG_ERROR(Audio,
                    "RmeAudioBackend: post-reset recovery failed GUID=0x%016llx kr=0x%x",
                    guid, status);
            }
        }
        if (lock_) {
            IOLockLock(lock_);
            recoveringGuids_.erase(guid);
            IOLockUnlock(lock_);
        }
    });
}

void RmeAudioBackend::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    if (lock_) {
        IOLockLock(lock_);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
}

void RmeAudioBackend::EnsureNubForGuid(uint64_t guid) noexcept {
    if (guid == 0 || stopping_.load(std::memory_order_acquire)) return;
    const auto record = registry_.SnapshotByGuid(guid);
    auto protocol = runtime_.FindShared(guid);
    if (!record || !protocol || !registry_.CurrentRoute(guid)) return;
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy || policy->plan.family != DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister) return;
    auto config = BuildNubConfig(*record, policy->plan.profileBuilder, protocol->GetName());
    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) endpoint->UpdateConfig(config);
    (void)publisher_.EnsureNub(guid, config, "RME");
}

void RmeAudioBackend::HandleHostTimingLoss(uint64_t guid) noexcept {
    PublicationGate::AdmissionScope admission(recoveryAdmission_);
    if (!admission.IsAdmitted() || guid == 0 || stopping_.load(std::memory_order_acquire) || !workQueue_) return;
    const auto record = registry_.SnapshotByGuid(guid);
    const auto* policy = record ? DeviceProfiles::Audio::CurrentAudioPolicy(*record) : nullptr;
    if (!policy || !registry_.IsCurrent(policy->route) ||
        sessions_.RunningRun(guid) == Session::SessionScheduler::kNotRunning ||
        recoveryInFlight_.exchange(true, std::memory_order_acq_rel)) return;
    const auto route = policy->route;
    const uint64_t run = sessions_.RunningRun(guid);
    workQueue_->DispatchAsync(^{
        if (!stopping_.load(std::memory_order_acquire) && registry_.IsCurrent(route) &&
            !sessions_.IsCancelled(guid)) {
            (void)sessions_.RequestRestart(guid, DuplexRestartReason::kRecoverAfterTimingLoss, run);
        }
        recoveryInFlight_.store(false, std::memory_order_release);
    });
}
} // namespace ASFW::Audio
