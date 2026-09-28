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
        IOLockLock(lock_); active_.clear(); IOLockUnlock(lock_);
    }
    teardownComplete_.store(true, std::memory_order_release);
}

void RmeAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    if (!stopping_.load(std::memory_order_acquire)) EnsureNubForGuid(guid);
}

void RmeAudioBackend::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    if (lock_) { IOLockLock(lock_); active_.erase(guid); IOLockUnlock(lock_); }
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

IOReturn RmeAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    if (stopping_.load(std::memory_order_acquire)) return kIOReturnAborted;
    const auto record = registry_.SnapshotByGuid(guid);
    const auto* policy = record ? DeviceProfiles::Audio::CurrentAudioPolicy(*record) : nullptr;
    if (!policy || policy->plan.family != DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister ||
        policy->plan.streamTraits.start.startRatePinHz != 48000U) return kIOReturnUnsupported;
    if (!publisher_.GetNub(guid)) EnsureNubForGuid(guid);
    auto endpoint = runtime_.FindEndpointRuntime(guid);
    if (!publisher_.GetNub(guid) || !endpoint || !endpoint->HasCompleteDirectAudioMemory())
        return kIOReturnNotReady;
    const IOReturn status = sessions_.Attach(guid);
    if (status == kIOReturnSuccess && lock_) {
        IOLockLock(lock_); active_.insert(guid); IOLockUnlock(lock_);
    }
    return status;
}

IOReturn RmeAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire)) return kIOReturnAborted;
    const IOReturn status = sessions_.Detach(guid);
    if (status == kIOReturnSuccess) CancelRemoteDeviceWork(guid);
    return status;
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
