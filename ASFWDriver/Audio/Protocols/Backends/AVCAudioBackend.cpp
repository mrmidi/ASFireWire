// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AVCAudioBackend.hpp"

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>

namespace ASFW::Audio {

AVCAudioBackend::AVCAudioBackend(AudioNubPublisher& publisher,
                                 Discovery::DeviceRegistry& registry,
                                 AudioRuntimeRegistry& runtime,
                                 Driver::IsochService& isoch,
                                 Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , hostTransport_(isoch)
    , duplexCoordinator_(
          registry,
          runtime,
          hostTransport_,
          hardware,
          &stopping_,
          [this](uint64_t guid) -> ASFW::Audio::Runtime::IDirectAudioBindingSource* {
              auto endpoint = runtime_.FindEndpointRuntime(guid);
              return endpoint ? endpoint.get() : nullptr;
          }) {
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
        if (stopStatus != kIOReturnSuccess) {
            // StopStreaming only succeeds after every OHCI context reports
            // ACTIVE clear.  Removing the nub sooner can revoke the backing
            // mapping while DMA still owns it, which is fatal on Apple silicon.
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: retaining nub after unsafe device-removal stop GUID=0x%016llx kr=0x%08x",
                           guid, stopStatus);
            return;
        }
    } else {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: OnDeviceRemoved skipping stop for inactive GUID=0x%016llx",
                 guid);
    }
    publisher_.TerminateNub(guid, "AVC-Removed");
    duplexCoordinator_.ClearSession(guid);

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_.erase(guid);
        recoveringGuids_.erase(guid);
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
            if (status != kIOReturnSuccess) {
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
    (void)hostTransport_.StopAll();
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

    if (!registry_.FindByGuid(guid)) {
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
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio, "AVCAudioBackend: Streaming stopped GUID=0x%016llx", guid);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
