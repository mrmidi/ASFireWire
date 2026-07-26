// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AVCAudioBackend.hpp"

#include <vector>

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>

namespace ASFW::Audio {

AVCAudioBackend::AVCAudioBackend(AudioNubPublisher& publisher,
                                 Discovery::DeviceRegistry& registry,
                                 AudioRuntimeRegistry& runtime,
                                 IIsochDuplexHostTransport& hostTransport,
                                 AudioDuplexCoordinator& duplexCoordinator,
                                 Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , hostTransport_(hostTransport)
    , duplexCoordinator_(duplexCoordinator) {
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
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AVCAudioBackend::OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept {
    if (guid == 0) return;

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_[guid] = config;
        IOLockUnlock(lock_);
    }

    (void)publisher_.EnsureNub(guid, config, "AVC");
}

void AVCAudioBackend::OnDeviceRemoved(uint64_t guid) noexcept {
    if (guid == 0) return;

    bool shouldStop = false;
    if (lock_) {
        IOLockLock(lock_);
        shouldStop = (activeGuid_ == guid);
        IOLockUnlock(lock_);
    }

    if (shouldStop) {
        const IOReturn stopStatus = StopStreaming(guid);
        if (!IsRemovalStopSettled(guid, stopStatus)) {
            // StopStreaming only succeeds after every OHCI context reports
            // ACTIVE clear.  Removing the nub sooner can revoke the backing
            // mapping while DMA still owns it, which is fatal on Apple silicon.
            //
            // FW-144: deferring here used to mean abandoning. The nub — and so
            // the CoreAudio device — survived the device it represents, until a
            // full driver teardown happened to free it. Record the removal as
            // owed and re-attempt it instead.
            DeferNubRemoval(guid, stopStatus);
            return;
        }
    } else {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: OnDeviceRemoved skipping stop for inactive GUID=0x%016llx",
                 guid);
    }
    FinishDeviceRemoval(guid);
}

bool AVCAudioBackend::IsActiveDevice(uint64_t guid) noexcept {
    if (!lock_) {
        return true;
    }
    IOLockLock(lock_);
    const bool active = (activeGuid_ == guid);
    IOLockUnlock(lock_);
    return active;
}

bool AVCAudioBackend::IsRemovalStopSettled(uint64_t guid, IOReturn stopStatus) noexcept {
    if (stopStatus == kIOReturnSuccess) {
        return true;
    }
    if (stopStatus != kIOReturnNoDevice) {
        return false;
    }

    // The device record is already gone, so every device-side stop stage is
    // moot — there is no plug to break and no node to address, and no amount of
    // waiting brings the record back. Deferring on this status is what kept the
    // CoreAudio device alive after an unplug (FW-144): the retry asked again,
    // got the same permanent answer, and never terminated the nub.
    //
    // What still has to happen is the host-side cleanup, which needs no device:
    // release the reserved profile and clear the active GUID. Then the removal
    // is safe to complete.
    const kern_return_t hostStatus = hostTeardownRequest_ ? hostTeardownRequest_()
                                                          : kIOReturnNotReady;
    if (hostStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "AVCAudioBackend: host cleanup incomplete for removed device "
                       "GUID=0x%016llx kr=0x%08x; completing removal anyway",
                       guid, hostStatus);
    } else {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: device record already gone; host cleanup done GUID=0x%016llx",
                 guid);
    }
    return true;
}

void AVCAudioBackend::DeferNubRemoval(uint64_t guid, IOReturn stopStatus) noexcept {
    bool alreadyPending = false;
    if (lock_) {
        IOLockLock(lock_);
        alreadyPending = !pendingNubRemoval_.insert(guid).second;
        IOLockUnlock(lock_);
    }

    ASFW_LOG_ERROR(Audio,
                   "AVCAudioBackend: deferring nub removal after unsafe device-removal stop "
                   "GUID=0x%016llx kr=0x%08x alreadyPending=%u",
                   guid, stopStatus, alreadyPending ? 1U : 0U);

    // The blocking condition is local isoch quiescence, and on the observed
    // trace it cleared a few microseconds later — the ADK side frees the TX
    // isoch resources from its own stop path, after this one has already given
    // up. Re-attempting once the current teardown chain unwinds is therefore
    // the cheapest trigger that actually catches it. Anything still blocked
    // after that is retried from the next backend event.
    if (workQueue_) {
        workQueue_->DispatchAsync(^{
            RetryPendingNubRemovals();
        });
    }
}

void AVCAudioBackend::RetryPendingNubRemovals() noexcept {
    std::vector<uint64_t> pending;
    if (lock_) {
        IOLockLock(lock_);
        pending.assign(pendingNubRemoval_.begin(), pendingNubRemoval_.end());
        IOLockUnlock(lock_);
    }
    if (pending.empty()) {
        return;
    }

    for (const uint64_t guid : pending) {
        const IOReturn stopStatus = StopStreaming(guid);
        if (!IsRemovalStopSettled(guid, stopStatus)) {
            // Still not safe. Stay pending rather than forcing it — the whole
            // point of the guard is that revoking a live DMA mapping is fatal.
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: nub removal still blocked GUID=0x%016llx kr=0x%08x",
                           guid, stopStatus);
            continue;
        }
        ASFW_LOG(Audio, "AVCAudioBackend: completing deferred nub removal GUID=0x%016llx", guid);
        FinishDeviceRemoval(guid);
    }
}

void AVCAudioBackend::FinishDeviceRemoval(uint64_t guid) noexcept {
    publisher_.TerminateNub(guid, "AVC-Removed");
    duplexCoordinator_.ClearSession(guid);

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_.erase(guid);
        recoveringGuids_.erase(guid);
        timingLossAttempts_.erase(guid);
        pendingNubRemoval_.erase(guid);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
        IOLockUnlock(lock_);
    }
}

void AVCAudioBackend::OnDeviceResumed(uint64_t guid) noexcept {
    if (guid == 0 || stopping_.load(std::memory_order_acquire)) {
        return;
    }

    bool queueRecovery = false;
    if (lock_) {
        IOLockLock(lock_);
        queueRecovery = (activeGuid_ == guid) && recoveringGuids_.insert(guid).second;
        IOLockUnlock(lock_);
    }
    if (!queueRecovery) {
        return;
    }

    // DeviceManager emits resume only after it refreshed the stable GUID's
    // node/generation mapping. The coordinator stops stale host state, makes
    // fresh IRM reservations, and lets the AV/C adapter establish fresh PCRs.
    // This is the same reset-then-reconnect ordering as Linux cmp.c:294-334.
    ASFW_LOG(Audio,
             "AVCAudioBackend: device resumed; recovering active CMP stream GUID=0x%016llx",
             guid);
    auto recover = ^{
        if (!stopping_.load(std::memory_order_acquire)) {
            const IOReturn status = duplexCoordinator_.RecoverStreaming(
                guid, DuplexRestartReason::kBusResetRebind);
            // Unsupported means the policy declined to act — a decision, not a
            // fault. Reporting it as a failure would put an error line on a
            // benign path (FW-146).
            if (status != kIOReturnSuccess && status != kIOReturnUnsupported) {
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
    bool armed = false;
    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == guid) {
            armed = recoveringGuids_.insert(guid).second;
        }
        IOLockUnlock(lock_);
    }
    if (!armed) {
        return;
    }

    // A duplex operation already running (start/stop/recovery) is itself the
    // transition that tripped the replay-discontinuity detector; let it settle.
    if (duplexCoordinator_.IsOperationInFlight(guid)) {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: timing-loss dropped (duplex op in flight) GUID=0x%016llx",
                 guid);
        FinishRecovery(guid);
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
        // nub and CoreAudio presence are already gone (FW-146). FinishDeviceRemoval
        // clears activeGuid_, which is what makes the device observable as gone.
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
        // (StartIO/StopIO churn, a brief RX gap) and already self-healed;
        // suppress and reset the escalation budget.
        if (hostTransport_.IsReceiveReplayEstablished()) {
            if (lock_) {
                IOLockLock(lock_);
                timingLossAttempts_.erase(guid);
                IOLockUnlock(lock_);
            }
            ASFW_LOG(Audio,
                     "AVCAudioBackend: timing-loss self-healed (RX replay re-established) "
                     "GUID=0x%016llx",
                     guid);
            FinishRecovery(guid);
            return;
        }

        // Still stalled: a genuine device outage. Bound the escalations so a
        // device that only partially returns cannot restart-loop forever.
        uint8_t attempt = 0;
        if (lock_) {
            IOLockLock(lock_);
            attempt = ++timingLossAttempts_[guid];
            IOLockUnlock(lock_);
        }
        if (attempt > kTimingLossMaxAttempts) {
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: timing-loss escalation budget exhausted "
                           "(attempt=%u); leaving stream stopped GUID=0x%016llx",
                           attempt, guid);
            FinishRecovery(guid);
            return;
        }

        // Escalate: coordinator restart re-establishes CMP/PCR (the wire-observable
        // recovery — bebob break_both_connections + cmp_connection_establish; doc §2/§7).
        ASFW_LOG_WARNING(Audio,
                         "AVCAudioBackend: RX replay stalled past settle; restarting duplex "
                         "attempt=%u GUID=0x%016llx",
                         attempt, guid);
        const IOReturn status = duplexCoordinator_.RecoverStreaming(
            guid, DICE::DiceRestartReason::kRecoverAfterTimingLoss);
        if (status == kIOReturnSuccess) {
            if (lock_) {
                IOLockLock(lock_);
                timingLossAttempts_.erase(guid); // fresh session; reset budget
                IOLockUnlock(lock_);
            }
            ASFW_LOG(Audio,
                     "AVCAudioBackend: timing-loss recovery succeeded GUID=0x%016llx",
                     guid);
        } else if (status == kIOReturnUnsupported) {
            // The policy declined to recover. Nothing was restarted, so the
            // budget must keep accumulating — resetting it here would mean a
            // device that always declines can never exhaust its escalations.
            ASFW_LOG(Audio,
                     "AVCAudioBackend: timing-loss recovery not applicable "
                     "(attempt=%u retained) GUID=0x%016llx",
                     attempt, guid);
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
    if (stopping_.load(std::memory_order_acquire)) {
        return;
    }

    const bool wasStopping = stopping_.exchange(true, std::memory_order_acq_rel);
    // Drain queued recovery before hardware detaches. Each queued block checks
    // stopping_ before it can re-establish PCRs in the teardown window.
    if (workQueue_) {
#ifdef ASFW_HOST_TEST
        workQueue_->DispatchSync([] {});
#else
        workQueue_->DispatchSync(^{ });
#endif
    }
    ASFW_LOG(Audio,
             "AVCAudioBackend: BeginTeardown stopping=true already=%u",
             wasStopping ? 1U : 0U);
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
        // Claim the backend before leaving the lock. The coordinator performs
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

    const IOReturn startStatus = duplexCoordinator_.StartStreaming(guid);
    if (startStatus != kIOReturnSuccess) {
        return failStart(startStatus, "AudioDuplexCoordinator");
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

    const IOReturn stopStatus = duplexCoordinator_.StopStreaming(guid);
    if (stopStatus != kIOReturnSuccess) return stopStatus;

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
        timingLossAttempts_.erase(guid);
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio, "AVCAudioBackend: Streaming stopped GUID=0x%016llx", guid);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
