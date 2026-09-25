// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AVCAudioBackend.hpp"

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Audio {

AVCAudioBackend::AVCAudioBackend(AudioNubPublisher& publisher,
                                 Discovery::DeviceRegistry& registry,
                                 AudioRuntimeRegistry& runtime,
                                 IIsochDuplexHostTransport& hostTransport,
                                 Session::AudioSessions& sessions,
                                 Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , hostTransport_(hostTransport)
    , sessions_(sessions) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AVCAudioBackend: Failed to allocate lock");
    }

    IODispatchQueue* queue = nullptr;
    const kern_return_t queueStatus = IODispatchQueue::Create("com.asfw.audio.avc", 0, 0, &queue);
    if (queueStatus == kIOReturnSuccess && queue) {
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else {
        ASFW_LOG_ERROR(Audio,
                       "AVCAudioBackend: Failed to create recovery queue (0x%x)",
                       queueStatus);
    }

}

AVCAudioBackend::~AVCAudioBackend() noexcept {
    // Stage 2b D1: defensive teardown in the destructor, matching MotuAudioBackend's
    // shape (BeginTeardown is idempotent; this only covers a destructor-only path).
    BeginTeardown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AVCAudioBackend::OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept {
    if (guid == 0) return;

    PublicationGate::AdmissionScope admission(publicationGate_);
    if (!admission.IsAdmitted()) {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: publication refused by teardown GUID=0x%016llx",
                 guid);
        return;
    }

#ifdef ASFW_HOST_TEST
    if (beforePublishHookForTesting_) {
        beforePublishHookForTesting_();
    }
#endif

    if (admission.AbortIfStopping()) {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: publication cancelled by concurrent teardown GUID=0x%016llx",
                 guid);
        return;
    }

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_[guid] = config;
        IOLockUnlock(lock_);
    }

    (void)publisher_.EnsureNub(guid, config, "AVC");
}

void AVCAudioBackend::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    if (guid == 0) return;

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_.erase(guid);
        recoveringGuids_.erase(guid);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio,
             "AVCAudioBackend: remote-device work cancelled GUID=0x%016llx",
             guid);
}

bool AVCAudioBackend::IsActiveDevice(uint64_t guid) noexcept {
    return sessions_.IsStreaming(guid);
}

void AVCAudioBackend::OnDeviceResumed(uint64_t guid) noexcept {
    if (guid == 0 || stopping_.load(std::memory_order_acquire)) {
        return;
    }

    bool queueRecovery = false;
    if (lock_) {
        IOLockLock(lock_);
        queueRecovery = sessions_.IsStreaming(guid) && recoveringGuids_.insert(guid).second;
        IOLockUnlock(lock_);
    }
    if (!queueRecovery) {
        return;
    }

    // DeviceManager emits resume only after it refreshed the stable GUID's
    // node/generation mapping. The session stops stale host state, makes
    // fresh IRM reservations, and lets the AV/C adapter establish fresh PCRs.
    // This is the same reset-then-reconnect ordering as Linux cmp.c:294-334.
    ASFW_LOG(Audio,
             "AVCAudioBackend: device resumed; recovering active CMP stream GUID=0x%016llx",
             guid);
    auto recover = ^{
        if (!stopping_.load(std::memory_order_acquire)) {
            const IOReturn status =
                sessions_.RequestRestart(guid, DuplexRestartReason::kBusResetRebind);
            // Unsupported/Aborted mean the session declined to act — a decision,
            // not a fault. Reporting it as a failure would put an error line on a
            // benign path (FW-146).
            if (status != kIOReturnSuccess && status != kIOReturnUnsupported &&
                status != kIOReturnAborted) {
                ASFW_LOG_ERROR(Audio,
                               "AVCAudioBackend: post-reset recovery failed GUID=0x%016llx kr=0x%x",
                               guid,
                               status);
            }
        }
        if (lock_) {
            IOLockLock(lock_);
            recoveringGuids_.erase(guid);
            IOLockUnlock(lock_);
        }
    };

    if (workQueue_) {
        workQueue_->DispatchAsync(recover);
        return;
    }

    // Queue creation failure must not block DeviceManager's resume observer.
    // A later explicit stream start will make a fresh connection instead.
    ASFW_LOG_ERROR(Audio,
                   "AVCAudioBackend: recovery queue unavailable; leaving stream stopped GUID=0x%016llx",
                   guid);
    if (lock_) {
        IOLockLock(lock_);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
}

void AVCAudioBackend::FinishRecovery(uint64_t guid) noexcept {
    if (lock_) {
        IOLockLock(lock_);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
}

void AVCAudioBackend::HandleTimingLoss(uint64_t guid) noexcept {
    if (guid == 0 || stopping_.load(std::memory_order_acquire)) {
        return;
    }

    // Only the active CMP stream is recoverable. IsochService reports the duplex
    // GUID it claimed; a mismatch means the loss belongs to no stream we own.
    // recoveringGuids_ dedups against a bus-reset recovery already in flight
    // (OnDeviceResumed) and against a second timing-loss for the same GUID.
    // The fault belongs to the run streaming now. If a reconcile is under way
    // the loss is that transition's own, and no run is confirmed: the session
    // drops it as stale when the block below asks for the restart.
    const uint64_t observedRun = sessions_.RunningRun(guid);
    bool armed = false;
    if (lock_) {
        IOLockLock(lock_);
        if (sessions_.IsStreaming(guid)) {
            armed = recoveringGuids_.insert(guid).second;
        }
        IOLockUnlock(lock_);
    }
    if (!armed) {
        return;
    }

    auto recover = ^{
        // FW-61: a block enqueued just before BeginTeardown's drain bails here
        // before any PCR/MMIO work, so it cannot run after hardware detaches.
        // Debounce off the RX queue: give the [TxAlign] self-heal its transient
        // window. Poll stopping_ so teardown aborts within one tick.
        // stopping_ only covers driver teardown. The device itself can be
        // unplugged during the settle window — that is the ordinary case, not
        // an edge one — and nothing else cancels this block, so without the
        // liveness check the escalation runs against a device whose record,
        // nub and CoreAudio presence are already gone (FW-146). The generic
        // remote-device owner clears activeGuid_ before it unpublishes the nub.
        for (uint32_t waited = 0; waited < kTimingLossSettleMs;
             waited += kTimingLossPollMs) {
            if (stopping_.load(std::memory_order_acquire)) {
                FinishRecovery(guid);
                return;
            }
            if (!IsActiveDevice(guid)) {
                ASFW_LOG(Audio,
                         "AVCAudioBackend: timing-loss abandoned; device left during settle "
                         "GUID=0x%016llx",
                         guid);
                FinishRecovery(guid);
                return;
            }
            IOSleep(kTimingLossPollMs);
        }
        if (stopping_.load(std::memory_order_acquire) || !IsActiveDevice(guid)) {
            FinishRecovery(guid);
            return;
        }

        // AV/C health verdict = RX cadence (no register probe, doc §5/§6). If
        // replay re-established during the settle window the gap was host-side
        // (StartIO/StopIO churn, a brief RX gap) and already self-healed.
        if (hostTransport_.IsReceiveReplayEstablished()) {
            ASFW_LOG(Audio,
                     "AVCAudioBackend: timing-loss self-healed (RX replay re-established) "
                     "GUID=0x%016llx",
                     guid);
            FinishRecovery(guid);
            return;
        }

        // Still stalled: a genuine device outage. A session restart
        // re-establishes CMP/PCR (the wire-observable recovery — bebob
        // break_both_connections + cmp_connection_establish; doc §2/§7).
        ASFW_LOG_WARNING(Audio,
                         "AVCAudioBackend: RX replay stalled past settle; restarting duplex "
                         "GUID=0x%016llx",
                         guid);
        const IOReturn status = sessions_.RequestRestart(
            guid, DuplexRestartReason::kRecoverAfterTimingLoss, observedRun);
        if (status == kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "AVCAudioBackend: timing-loss recovery succeeded GUID=0x%016llx",
                     guid);
        } else if (status == kIOReturnUnsupported || status == kIOReturnAborted) {
            // The session declined: the fault is stale, or repeated failures
            // already left the streams stopped.
            ASFW_LOG(Audio,
                     "AVCAudioBackend: timing-loss recovery not applicable "
                     "GUID=0x%016llx kr=0x%x",
                     guid, status);
        } else {
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: timing-loss recovery failed GUID=0x%016llx kr=0x%x",
                           guid, status);
        }
        FinishRecovery(guid);
    };

    if (workQueue_) {
        workQueue_->DispatchAsync(recover);
        return;
    }
    recover();
}

void AVCAudioBackend::BeginTeardown() noexcept {
    stopping_.store(true, std::memory_order_release);

    if (teardownComplete_.load(std::memory_order_acquire)) {
        return;
    }

    if (!teardownStarted_.exchange(true, std::memory_order_acq_rel)) {
        // Atomically close admission and wait for any admitted in-flight publication to finish.
        publicationGate_.CloseAndWait();

        // Drain queued recovery before hardware detaches. Each queued block checks
        // stopping_ before it can re-establish PCRs in the teardown window.
        if (workQueue_) {
#ifdef ASFW_HOST_TEST
            if (onTeardownDrainStartedHookForTesting_) {
                onTeardownDrainStartedHookForTesting_();
            }
            workQueue_->DispatchSync([] {});
#else
            workQueue_->DispatchSync(^{ });
#endif
        }

        teardownComplete_.store(true, std::memory_order_release);
        ASFW_LOG(Audio,
                 "AVCAudioBackend: BeginTeardown draining publicationRejected=%llu",
                 publicationGate_.RejectCount());
        return;
    }

    // Secondary / concurrent callers wait safely until the primary drain is finished.
#ifdef ASFW_HOST_TEST
    if (onSecondaryTeardownWaitingHookForTesting_) {
        onSecondaryTeardownWaitingHookForTesting_();
    }
#endif
    while (!teardownComplete_.load(std::memory_order_acquire)) {
        IOSleep(1);
    }
}

IOReturn AVCAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    if (stopping_.load(std::memory_order_acquire)) return kIOReturnAborted;

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ != 0 && activeGuid_ != guid) {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);
            ASFW_LOG_WARNING(Audio,
                             "AVCAudioBackend: StartStreaming busy requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            return kIOReturnBusy;
        }
        // Claim the backend before leaving the lock. The session performs
        // blocking setup, so delaying this assignment until it returns would
        // allow a second GUID to begin concurrently.
        activeGuid_ = guid;
        IOLockUnlock(lock_);
    }

    auto failStart = [&](IOReturn status, const char* stage) -> IOReturn {
        if (lock_) {
            IOLockLock(lock_);
            if (activeGuid_ == guid) {
                activeGuid_ = 0;
            }
            IOLockUnlock(lock_);
        }
        ASFW_LOG_ERROR(Audio,
                       "AVCAudioBackend: StartStreaming failed stage=%{public}s GUID=0x%016llx kr=0x%x",
                       stage ? stage : "unknown",
                       guid,
                       status);
        return status;
    };

    Model::ASFWAudioDevice config{};
    bool hasConfig = false;
    if (lock_) {
        IOLockLock(lock_);
        auto it = configByGuid_.find(guid);
        if (it != configByGuid_.end()) {
            config = it->second;
            hasConfig = true;
        }
        IOLockUnlock(lock_);
    }
    if (!hasConfig) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (no config) GUID=0x%016llx", guid);
        return failStart(kIOReturnNotReady, "config");
    }

    if (!registry_.SnapshotByGuid(guid).has_value()) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (no device record) GUID=0x%016llx", guid);
        return failStart(kIOReturnNotReady, "device record");
    }

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        (void)publisher_.EnsureNub(guid, config, "AVC-Start");
        nub = publisher_.GetNub(guid);
        if (!nub) return failStart(kIOReturnNotReady, "nub");
    }

    const auto endpoint = runtime_.FindEndpointRuntime(guid);
    if (!endpoint) {
        return failStart(kIOReturnNotReady, "direct binding source");
    }
    if (!endpoint->HasCompleteDirectAudioMemory()) {
        return failStart(kIOReturnNotReady, "direct memory");
    }

    const IOReturn startStatus = sessions_.Attach(guid);
    if (startStatus != kIOReturnSuccess) {
        return failStart(startStatus, "session");
    }

    ASFW_LOG(Audio,
             "AVCAudioBackend: Streaming started GUID=0x%016llx (in=%u out=%u mode=%{public}s)",
             guid,
             config.inputChannelCount,
             config.outputChannelCount,
             config.streamMode == Model::StreamMode::kBlocking ? "blocking" : "non-blocking");

    return kIOReturnSuccess;
}

IOReturn AVCAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    if (stopping_.load(std::memory_order_acquire)) return kIOReturnAborted;

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ != 0 && activeGuid_ != guid) {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);
            ASFW_LOG_WARNING(Audio,
                             "AVCAudioBackend: StopStreaming refused requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            return kIOReturnBusy;
        }
        if (activeGuid_ == 0) {
            IOLockUnlock(lock_);
            ASFW_LOG(Audio,
                     "AVCAudioBackend: StopStreaming idempotent inactive GUID=0x%016llx",
                     guid);
            return kIOReturnSuccess;
        }
        IOLockUnlock(lock_);
    }

    const IOReturn stopStatus = sessions_.Detach(guid);
    if (stopStatus != kIOReturnSuccess) return stopStatus;

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio, "AVCAudioBackend: Streaming stopped GUID=0x%016llx", guid);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
