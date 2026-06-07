// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceAudioBackend.hpp"

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include "../DICE/Core/DICENotificationMailbox.hpp"
#include "../DICE/TCAT/DICEKnownProfiles.hpp"
#include "../DeviceProtocolFactory.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <memory>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <string>
#include <vector>

namespace ASFW::Audio {

DiceAudioBackend::DiceAudioBackend(AudioNubPublisher& publisher,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                   Driver::IsochService& isoch,
                                   Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , hostTransport_(isoch)
    , restartCoordinator_(registry,
                          runtime,
                          hostTransport_,
                          hardware,
                          [this](uint64_t guid) -> ASFW::Audio::Runtime::IDirectAudioBindingSource* {
                              auto endpoint = runtime_.FindEndpointRuntime(guid);
                              return endpoint ? endpoint.get() : nullptr;
                          }) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: Failed to allocate lock");
    }

    IODispatchQueue* queue = nullptr;
    const kern_return_t kr = IODispatchQueue::Create("com.asfw.audio.dice", 0, 0, &queue);
    if (kr == kIOReturnSuccess && queue) {
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: Failed to create work queue (0x%x)", kr);
    }

    DICE::NotificationMailbox::SetObserver(this, &DiceAudioBackend::NotificationObserverThunk);
    hostTransport_.SetTimingLossCallback([this](uint64_t guid) {
        HandleRecoveryEvent(guid, DICE::DiceRestartReason::kRecoverAfterTimingLoss);
    });
    hostTransport_.SetTxRecoveryCallback([this](uint64_t guid, uint32_t reasonBits) {
        ASFW_LOG_WARNING(Audio,
                         "DiceAudioBackend: TX recovery requested GUID=%llx reasons=0x%08x",
                         guid,
                         reasonBits);
        HandleRecoveryEvent(guid, DICE::DiceRestartReason::kRecoverAfterTxFault);
        return true;
    });
}

DiceAudioBackend::~DiceAudioBackend() noexcept {
    DICE::NotificationMailbox::ClearObserver(this);
    hostTransport_.SetTimingLossCallback({});
    hostTransport_.SetTxRecoveryCallback({});
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void DiceAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    EnsureNubForGuid(guid);
}

void DiceAudioBackend::OnDeviceRemoved(uint64_t guid) noexcept {
    if (guid == 0) return;

    (void)StopStreaming(guid);
    publisher_.TerminateNub(guid, "DICE-Removed");
    restartCoordinator_.ClearSession(guid);

    if (lock_) {
        IOLockLock(lock_);
        attemptsByGuid_.erase(guid);
        retryOutstanding_.erase(guid);
        activeStreamingGuids_.erase(guid);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
}

void DiceAudioBackend::HandleRecoveryEvent(uint64_t guid, DICE::DiceRestartReason reason) noexcept {
    if (guid == 0) {
        return;
    }

    if (!TryBeginRecovery(guid)) {
        return;
    }

    auto recover = ^{
        const IOReturn status = restartCoordinator_.RecoverStreaming(guid, reason);
        if (status == kIOReturnSuccess) {
            EnsureNubForGuid(guid);
            ASFW_LOG(Audio,
                     "DiceAudioBackend: Recovery succeeded GUID=%llx reason=%u",
                     guid,
                     static_cast<unsigned>(reason));
            FinishRecovery(guid);
            return;
        }

        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: Recovery failed GUID=%llx reason=%u kr=0x%x",
                       guid,
                       static_cast<unsigned>(reason),
                       status);
        FinishRecovery(guid);
    };

    if (workQueue_) {
        workQueue_->DispatchAsync(recover);
        return;
    }

    recover();
}

void DiceAudioBackend::HandleDeviceNotification(uint32_t bits) noexcept {
    if ((bits & (DICE::Notify::kLockChange | DICE::Notify::kExtStatus)) == 0) {
        return;
    }

    std::vector<uint64_t> guids;
    if (lock_) {
        IOLockLock(lock_);
        guids.assign(activeStreamingGuids_.begin(), activeStreamingGuids_.end());
        IOLockUnlock(lock_);
    }

    for (const uint64_t guid : guids) {
        auto probe = ^{
            ProbeDuplexHealth(guid, bits);
        };

        if (workQueue_) {
            workQueue_->DispatchAsync(probe);
        } else {
            probe();
        }
    }
}

void DiceAudioBackend::ProbeDuplexHealth(uint64_t guid, uint32_t notificationBits) noexcept {
    // Hold a shared_ptr for the duration of the (blocking) health probe so the
    // protocol cannot be torn down underneath us by a concurrent device removal.
    auto protocol = runtime_.FindShared(guid);
    auto* diceProtocol = protocol ? protocol->AsDiceDuplexProtocol() : nullptr;
    if (!diceProtocol) {
        return;
    }

    struct WaitState {
        std::atomic<bool> done{false};
        IOReturn status{kIOReturnTimeout};
        DICE::DiceDuplexHealthResult result{};
    };

    auto waitState = std::make_shared<WaitState>();
    diceProtocol->ReadDuplexHealth([waitState](IOReturn status, DICE::DiceDuplexHealthResult result) {
        waitState->status = status;
        waitState->result = std::move(result);
        waitState->done.store(true, std::memory_order_release);
    });

    for (uint32_t waited = 0; waited < kHealthBridgeTimeoutMs; waited += kHealthBridgePollMs) {
        if (waitState->done.load(std::memory_order_acquire)) {
            break;
        }
        IOSleep(kHealthBridgePollMs);
    }

    if (!waitState->done.load(std::memory_order_acquire)) {
        ASFW_LOG_WARNING(Audio,
                         "DiceAudioBackend: health probe timed out GUID=%llx bits=0x%08x",
                         guid,
                         notificationBits);
        return;
    }

    if (waitState->status != kIOReturnSuccess) {
        ASFW_LOG_WARNING(Audio,
                         "DiceAudioBackend: health probe failed GUID=%llx bits=0x%08x kr=0x%x",
                         guid,
                         notificationBits,
                         waitState->status);
        return;
    }

    const bool sourceLocked = DICE::IsSourceLocked(waitState->result.status);
    bool extClockHealthy = true;
    const uint32_t clockSource = waitState->result.appliedClock.clockSelect & DICE::ClockSelect::kSourceMask;
    if (clockSource == static_cast<uint32_t>(DICE::ClockSource::ARX1)) {
        extClockHealthy =
            DICE::IsArx1Locked(waitState->result.extStatus) &&
            !DICE::HasArx1Slip(waitState->result.extStatus);
    }

    char notifyStr[96];
    char clockStr[40];
    char extStr[128];
    DICE::FormatNotification(notificationBits, notifyStr, sizeof(notifyStr));
    DICE::FormatGlobalStatus(waitState->result.status, clockStr, sizeof(clockStr));
    DICE::FormatExtStatus(waitState->result.extStatus, extStr, sizeof(extStr));

    if (sourceLocked && extClockHealthy) {
        // Healthy: the device is just narrating its clock/ext status. Surface
        // what it actually reports (rate-limited) instead of staying silent.
        ASFW_LOG_RL(Audio, "dice/notify-confirm", 1000, OS_LOG_TYPE_DEFAULT,
                    "DiceAudioBackend: notify confirm GUID=%llx notify=%{public}s clock=%{public}s ext=%{public}s healthy",
                    guid, notifyStr, clockStr, extStr);
        return;
    }

    ASFW_LOG_WARNING(Audio,
                     "DiceAudioBackend: lock health DEGRADED GUID=%llx notify=%{public}s clock=%{public}s ext=%{public}s sourceLocked=%u extHealthy=%u -> recover",
                     guid,
                     notifyStr,
                     clockStr,
                     extStr,
                     sourceLocked ? 1U : 0U,
                     extClockHealthy ? 1U : 0U);
    HandleRecoveryEvent(guid, DICE::DiceRestartReason::kRecoverAfterLockLoss);
}

bool DiceAudioBackend::TryBeginRecovery(uint64_t guid) noexcept {
    if (!lock_) {
        return true;
    }

    IOLockLock(lock_);
    const auto [_, inserted] = recoveringGuids_.insert(guid);
    IOLockUnlock(lock_);
    return inserted;
}

void DiceAudioBackend::FinishRecovery(uint64_t guid) noexcept {
    if (!lock_) {
        return;
    }

    IOLockLock(lock_);
    recoveringGuids_.erase(guid);
    IOLockUnlock(lock_);
}

void DiceAudioBackend::NotificationObserverThunk(void* context, uint32_t bits) noexcept {
    auto* self = static_cast<DiceAudioBackend*>(context);
    if (!self) {
        return;
    }
    self->HandleDeviceNotification(bits);
}

void DiceAudioBackend::EnsureNubForGuid(uint64_t guid) noexcept {
    if (guid == 0) return;

    const auto* record = registry_.FindByGuid(guid);
    if (!record) return;

    const auto integration = DeviceProtocolFactory::LookupIntegrationMode(record->vendorId, record->modelId);
    if (integration != DeviceIntegrationMode::kHardcodedNub) {
        return;
    }

    auto protocol = runtime_.FindShared(guid);
    if (!protocol) {
        return;
    }

    AudioStreamRuntimeCaps caps{};
    const bool ready = protocol->GetRuntimeAudioStreamCaps(caps);

    if (!ready || caps.sampleRateHz == 0 || caps.hostInputPcmChannels == 0 || caps.hostOutputPcmChannels == 0) {
        if (DICE::TCAT::TryGetKnownDICEProfile(record->vendorId, record->modelId, caps)) {
            ASFW_LOG_WARNING(Audio,
                             "DiceAudioBackend: runtime caps not ready for GUID=%llx; using known DICE profile %u/%u",
                             guid,
                             caps.hostInputPcmChannels,
                             caps.hostOutputPcmChannels);
        } else {
            bool shouldRetry = false;
            uint8_t attempt = 0;
            bool outstanding = false;

            if (lock_) {
                IOLockLock(lock_);
                attempt = attemptsByGuid_[guid];
                outstanding = (retryOutstanding_.find(guid) != retryOutstanding_.end());
                if (attempt < kCapsRetryMaxAttempts && !outstanding) {
                    attemptsByGuid_[guid] = static_cast<uint8_t>(attempt + 1u);
                    retryOutstanding_.insert(guid);
                    shouldRetry = true;
                    attempt = static_cast<uint8_t>(attempt + 1u);
                }
                IOLockUnlock(lock_);
            }

            if (outstanding) {
                return;
            }

            if (shouldRetry && workQueue_) {
                ASFW_LOG(Audio,
                         "DiceAudioBackend: runtime caps not ready for GUID=%llx; retry %u/%u in %u ms",
                         guid,
                         attempt,
                         kCapsRetryMaxAttempts,
                         kCapsRetryDelayMs);
                workQueue_->DispatchAsync(^{
                    IOSleep(kCapsRetryDelayMs);
                    if (lock_) {
                        IOLockLock(lock_);
                        retryOutstanding_.erase(guid);
                        IOLockUnlock(lock_);
                    }
                    EnsureNubForGuid(guid);
                });
                return;
            }

            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: runtime caps not ready for GUID=%llx; refusing to publish a lying nub",
                           guid);
            return;
        }
    }

    Model::ASFWAudioDevice dev{};
    dev.guid = record->guid;
    dev.vendorId = record->vendorId;
    dev.modelId = record->modelId;
    dev.deviceName = !record->vendorName.empty() && !record->modelName.empty()
        ? (record->vendorName + " " + record->modelName)
        : std::string(protocol ? protocol->GetName() : "DICE Audio");
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";

    dev.currentSampleRate = caps.sampleRateHz ? caps.sampleRateHz : 48000;
    dev.sampleRates = {dev.currentSampleRate};

    dev.inputChannelCount = caps.hostInputPcmChannels;
    dev.outputChannelCount = caps.hostOutputPcmChannels;
    dev.channelCount = (dev.inputChannelCount > dev.outputChannelCount)
        ? dev.inputChannelCount
        : dev.outputChannelCount;

    // DICE family policy: 48k uses blocking cadence (NDDD).
    dev.streamMode = Model::StreamMode::kBlocking;

    if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
        endpoint->UpdateConfig(dev);
    }

    (void)publisher_.EnsureNub(guid, dev, "DICE");

    if (lock_) {
        IOLockLock(lock_);
        attemptsByGuid_.erase(guid);
        retryOutstanding_.erase(guid);
        IOLockUnlock(lock_);
    }
}



IOReturn DiceAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
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
                       "DiceAudioBackend: StartStreaming refused missing direct runtime/memory GUID=0x%016llx endpoint=%p",
                       guid,
                       endpoint.get());
        return kIOReturnNotReady;
    }

    const IOReturn status = restartCoordinator_.StartStreaming(guid);
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

IOReturn DiceAudioBackend::StopStreaming(uint64_t guid) noexcept {
    const IOReturn status = restartCoordinator_.StopStreaming(guid);
    if (status == kIOReturnSuccess && lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.erase(guid);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
    return status;
}

IOReturn DiceAudioBackend::RequestClockConfig(uint64_t guid,
                                              const DICE::DiceDesiredClockConfig& desiredClock,
                                              DICE::DiceRestartReason reason) noexcept {
    const IOReturn status = restartCoordinator_.RequestClockConfig(guid, desiredClock, reason);
    if (status == kIOReturnSuccess) {
        EnsureNubForGuid(guid);
    }
    return status;
}

} // namespace ASFW::Audio
