// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceAudioBackend.hpp"

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include "../DICE/Core/DICENotificationMailbox.hpp"
#include "../DICE/Core/IDICEDuplexProtocol.hpp"
#include "../IDeviceProtocol.hpp"
#include "../DeviceProtocolFactory.hpp"
#include "../../DriverKit/Config/DICE/DiceProfileRegistry.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <memory>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <string>
#include <vector>

namespace ASFW::Audio {

namespace {

[[nodiscard]] uint64_t UptimeMilliseconds() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
        timebase.denom == 0) {
        return 0;
    }
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_absolute_time()) *
        timebase.numer / timebase.denom;
    return static_cast<uint64_t>(nanos / 1'000'000U);
}

} // namespace

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
                          &stopping_,
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
}

DiceAudioBackend::~DiceAudioBackend() noexcept {
    DICE::NotificationMailbox::ClearObserver(this);
    hostTransport_.SetTimingLossCallback({});
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void DiceAudioBackend::BeginTeardown() noexcept {
    const bool wasStopping = stopping_.exchange(true, std::memory_order_acq_rel);
    DICE::NotificationMailbox::ClearObserver(this);
    hostTransport_.SetTimingLossCallback({});

    const uint64_t recoveryRejectBefore =
        recoveryRejectCount_.load(std::memory_order_acquire);
    const uint64_t probeRejectBefore =
        probeRejectCount_.load(std::memory_order_acquire);
    const uint64_t probeAbortBefore =
        probeAbortCount_.load(std::memory_order_acquire);
    const uint64_t coordinatorAbortBefore =
        restartCoordinator_.TeardownAbortCount();
    const uint64_t startMs = UptimeMilliseconds();

    ASFW_LOG(Audio,
             "DiceAudioBackend: BeginTeardown stopping=true draining dice queue already=%u",
             wasStopping ? 1 : 0);

    if (workQueue_) {
#ifdef ASFW_HOST_TEST
        workQueue_->DispatchSync([] {});
#else
        workQueue_->DispatchSync(^{});
#endif
    }

    const uint64_t endMs = UptimeMilliseconds();
    const uint64_t drainMs = endMs >= startMs ? endMs - startMs : 0;
    const uint64_t coordinatorAborted =
        restartCoordinator_.TeardownAbortCount() - coordinatorAbortBefore;
    const uint64_t probeAborted =
        probeAbortCount_.load(std::memory_order_acquire) - probeAbortBefore;
    const uint64_t recoveryRejected =
        recoveryRejectCount_.load(std::memory_order_acquire) - recoveryRejectBefore;
    const uint64_t probeRejected =
        probeRejectCount_.load(std::memory_order_acquire) - probeRejectBefore;

    ASFW_LOG(Audio,
             "DiceAudioBackend: dice queue drained aborted=%llu recoveryRejected=%llu probeRejected=%llu drain=%llums",
             coordinatorAborted + probeAborted,
             recoveryRejected,
             probeRejected,
             drainMs);
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

    if (stopping_.load(std::memory_order_acquire)) {
        recoveryRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: recovery event ignored by teardown GUID=%llx reason=%u",
                 guid,
                 static_cast<unsigned>(reason));
        return;
    }

    if (!TryBeginRecovery(guid)) {
        return;
    }

    auto recover = ^{
        // FW-61: a block enqueued just before BeginTeardown's drain bails here before any
        // MMIO, so it cannot run after ASFWDriver::Stop detaches hardware.
        if (stopping_.load(std::memory_order_acquire)) {
            recoveryRejectCount_.fetch_add(1, std::memory_order_acq_rel);
            ASFW_LOG(Audio,
                     "DiceAudioBackend: queued recovery aborted by teardown GUID=%llx reason=%u",
                     guid,
                     static_cast<unsigned>(reason));
            FinishRecovery(guid);
            return;
        }
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

    if (stopping_.load(std::memory_order_acquire)) {
        probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: device notification ignored by teardown bits=0x%08x",
                 bits);
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
            if (stopping_.load(std::memory_order_acquire)) {
                probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
                ASFW_LOG(Audio,
                         "DiceAudioBackend: queued health probe ignored by teardown GUID=%llx bits=0x%08x",
                         guid,
                         bits);
                return;
            }
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
    if (stopping_.load(std::memory_order_acquire)) {
        probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: health probe refused by teardown GUID=%llx bits=0x%08x",
                 guid,
                 notificationBits);
        return;
    }

    // Hold a shared_ptr for the duration of the (blocking) health probe so the
    // protocol cannot be torn down underneath us by a concurrent device removal.
    auto protocol = runtime_.FindShared(guid);
    auto* diceProtocol = protocol ? protocol->AsDiceDuplexProtocol() : nullptr;
    if (!diceProtocol) {
        return;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: health probe refused by teardown before read GUID=%llx bits=0x%08x",
                 guid,
                 notificationBits);
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
        if (stopping_.load(std::memory_order_acquire)) {
            probeAbortCount_.fetch_add(1, std::memory_order_acq_rel);
            ASFW_LOG(Audio,
                     "DiceAudioBackend: health probe aborted by teardown GUID=%llx bits=0x%08x kr=0x%x",
                     guid,
                     notificationBits,
                     kIOReturnAborted);
            return;
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
                     guid, notifyStr, clockStr, extStr, sourceLocked, extClockHealthy);

    (void)guid;
    (void)notificationBits;
}

bool DiceAudioBackend::TryBeginRecovery(uint64_t guid) noexcept {
    if (!lock_) return false;

    IOLockLock(lock_);
    if (recoveringGuids_.find(guid) != recoveringGuids_.end()) {
        IOLockUnlock(lock_);
        return false;
    }
    recoveringGuids_.insert(guid);
    IOLockUnlock(lock_);
    return true;
}

void DiceAudioBackend::FinishRecovery(uint64_t guid) noexcept {
    if (!lock_) return;

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
    if (!record) {
        ASFW_LOG(Audio, "DiceAudioBackend::EnsureNubForGuid: no registry record for GUID=0x%016llx", guid);
        return;
    }

    const auto integration = DeviceProtocolFactory::LookupIntegrationMode(record->vendorId, record->modelId);
    if (integration != DeviceIntegrationMode::kHardcodedNub) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: skipping GUID=0x%016llx vendor=0x%06x model=0x%06x integration=%u (not hardcodedNub)",
                 guid, record->vendorId, record->modelId, static_cast<unsigned>(integration));
        return;
    }

    // Check modelid/vendor id first to find a known profile.
    ASFW::Isoch::Audio::DICE::DiceDeviceIdentity identity{
        .guid = record->guid,
        .vendorId = record->vendorId,
        .modelId = record->modelId
    };
    static ASFW::Isoch::Audio::DICE::DiceProfileRegistry diceRegistry{};
    const auto* profile = diceRegistry.FindProfile(identity);
    if (!profile) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: no isoch profile for GUID=0x%016llx vendor=0x%06x model=0x%06x (profileCount=%u)",
                 guid, record->vendorId, record->modelId, diceRegistry.ProfileCount());
        return;
    }

    ASFW_LOG(Audio,
             "DiceAudioBackend::EnsureNubForGuid: matched profile=%{public}s for GUID=0x%016llx",
             profile->Name(), guid);

    auto protocol = runtime_.FindShared(guid);

    Model::ASFWAudioDevice dev{};
    dev.guid = record->guid;
    dev.vendorId = record->vendorId;
    dev.modelId = record->modelId;
    dev.deviceName = profile->Name();
    dev.inputChannelCount = profile->RxChannelCount();
    dev.outputChannelCount = profile->TxChannelCount();
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";

    // Enrich with the device's real per-channel labels (if the protocol has
    // loaded them), update the endpoint runtime, then publish the nub. Host
    // input == device TX, host output == device RX (see AudioTypes.hpp), which
    // is exactly how GetChannelLabels reports them.
    auto finish = [this, guid](Model::ASFWAudioDevice dev,
                               const std::shared_ptr<IDeviceProtocol>& protocol) {
        if (stopping_.load(std::memory_order_acquire)) {
            return;
        }
        if (protocol) {
            std::vector<std::string> inNames;
            std::vector<std::string> outNames;
            if (protocol->GetChannelLabels(inNames, outNames)) {
                if (!inNames.empty()) {
                    dev.inputChannelNames = std::move(inNames);
                }
                if (!outNames.empty()) {
                    dev.outputChannelNames = std::move(outNames);
                }
                ASFW_LOG(Audio,
                         "DiceAudioBackend::EnsureNubForGuid: applied device channel labels in=%zu out=%zu (GUID=0x%016llx)",
                         dev.inputChannelNames.size(), dev.outputChannelNames.size(), guid);
            }
        }
        if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
            endpoint->UpdateConfig(dev);
        }
        (void)publisher_.EnsureNub(guid, dev, "DICE");
    };

    // Channel labels live in the TCAT stream-format name sections, cached only
    // once runtime caps load (during the first stream discovery). Load them
    // once before the first publish so CoreAudio shows the real names from the
    // start. The load early-returns if caps are already cached; publish happens
    // regardless of outcome (names fall back to synthesized "<plug> N").
    if (auto* dice = protocol ? protocol->AsDiceDuplexProtocol() : nullptr) {
        dice->EnsureRuntimeStreamGeometry(
            [finish, dev, protocol](IOReturn /*status*/) mutable {
                finish(std::move(dev), protocol);
            });
        return;
    }
    finish(std::move(dev), protocol);
}

IOReturn DiceAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: StartStreaming refused by teardown GUID=0x%016llx",
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
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: StopStreaming refused by teardown GUID=0x%016llx",
                 guid);
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.erase(guid);
            recoveringGuids_.erase(guid);
            IOLockUnlock(lock_);
        }
        return kIOReturnAborted;
    }

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
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: RequestClockConfig refused by teardown GUID=0x%016llx",
                 guid);
        return kIOReturnAborted;
    }

    const IOReturn status = restartCoordinator_.RequestClockConfig(guid, desiredClock, reason);
    if (status == kIOReturnSuccess) {
        EnsureNubForGuid(guid);
    }
    return status;
}

} // namespace ASFW::Audio
