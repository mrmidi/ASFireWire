// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDeviceHost.cpp - see AudioDeviceHost.hpp and
// documentation/AUDIO_DEVICE_HOST.md §4.

#include "AudioDeviceHost.hpp"

#include "../Core/AudioEndpointRuntime.hpp"
#include "../Core/AudioNubPublisher.hpp"
#include "../Core/AudioRuntimeRegistry.hpp"
#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "../Protocols/Duplex/FamilyDriver.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../Session/AudioSessions.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <net.asfw.driver/ASFWAudioNub.h>

#include <cstdio>
#include <type_traits>
#include <variant>

namespace ASFW::Audio::Host {

namespace {

constexpr const char* kNoFamily = "-";

[[nodiscard]] uint64_t UptimeMilliseconds() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
        return 0;
    }
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_absolute_time()) * timebase.numer / timebase.denom;
    return static_cast<uint64_t>(nanos / 1'000'000U);
}

// A runtime fault, or a device config change, belongs to the run streaming
// when it fired; the session drops it once that run has ended. Bus-reset
// rebinds are topology events and always restart (DiceAudioBackend
// HandleRecoveryEvent, the reference behaviour; §5 Δ4).
[[nodiscard]] constexpr bool IsRunTied(DuplexRestartReason reason) noexcept {
    switch (reason) {
        case DuplexRestartReason::kRecoverAfterTimingLoss:
        case DuplexRestartReason::kRecoverAfterCycleInconsistent:
        case DuplexRestartReason::kRecoverAfterLockLoss:
        case DuplexRestartReason::kRecoverAfterTxFault:
        case DuplexRestartReason::kDeviceConfigChange:
            return true;
        default:
            return false;
    }
}

// Unsupported/Aborted mean the session declined to act (stale fault, nothing
// running, repeated failures left it stopped): a decision, not a fault (FW-146).
[[nodiscard]] constexpr HostOutcome RestartOutcome(IOReturn status) noexcept {
    if (status == kIOReturnSuccess) return HostOutcome::RestartRequested;
    if (status == kIOReturnUnsupported || status == kIOReturnAborted) return HostOutcome::Declined;
    return HostOutcome::Failed;
}

} // namespace

const char* ToString(HostEvent event) noexcept {
    switch (event) {
        case HostEvent::Publish: return "Publish";
        case HostEvent::Rebind: return "Rebind";
        case HostEvent::RuntimeFault: return "RuntimeFault";
        case HostEvent::DeviceEvent: return "DeviceEvent";
        case HostEvent::Teardown: return "Teardown";
        case HostEvent::kCount: break;
    }
    return "?";
}

const char* ToString(HostOutcome outcome) noexcept {
    switch (outcome) {
        case HostOutcome::Published: return "Published";
        case HostOutcome::Refreshed: return "Refreshed";
        case HostOutcome::PublishFailed: return "PublishFailed";
        case HostOutcome::RefusedTeardown: return "RefusedTeardown";
        case HostOutcome::RefusedStaleRoute: return "RefusedStaleRoute";
        case HostOutcome::RefusedNoPolicy: return "RefusedNoPolicy";
        case HostOutcome::RefusedNoAdapter: return "RefusedNoAdapter";
        case HostOutcome::RefusedDescribe: return "RefusedDescribe";
        case HostOutcome::RefusedGeometryChanged: return "RefusedGeometryChanged";
        case HostOutcome::KeptCommittedFormation: return "KeptCommittedFormation";
        case HostOutcome::Queued: return "Queued";
        case HostOutcome::Deduped: return "Deduped";
        case HostOutcome::NotStreaming: return "NotStreaming";
        case HostOutcome::Cancelled: return "Cancelled";
        case HostOutcome::SelfHealed: return "SelfHealed";
        case HostOutcome::DeviceLeft: return "DeviceLeft";
        case HostOutcome::RestartRequested: return "RestartRequested";
        case HostOutcome::Declined: return "Declined";
        case HostOutcome::Failed: return "Failed";
        case HostOutcome::ConfigChange: return "ConfigChange";
        case HostOutcome::ClockProbe: return "ClockProbe";
        case HostOutcome::ClockHealthy: return "ClockHealthy";
        case HostOutcome::ClockDegraded: return "ClockDegraded";
        case HostOutcome::DeviceRateChange: return "DeviceRateChange";
        case HostOutcome::ClockEcho: return "ClockEcho";
        case HostOutcome::ProbeFailed: return "ProbeFailed";
        case HostOutcome::Drained: return "Drained";
        case HostOutcome::kCount: break;
    }
    return "?";
}

// FaultContext for one device and one fault, valid during JudgeRuntimeFault.
class AudioDeviceHost::HostFaultContext final : public FaultContext {
public:
    HostFaultContext(AudioDeviceHost& host, uint64_t guid) noexcept : host_(host), guid_(guid) {}

    [[nodiscard]] bool Cancelled() const noexcept override {
        return host_.Stopping() || host_.sessions_.IsCancelled(guid_);
    }
    [[nodiscard]] bool StillStreaming() const noexcept override {
        return host_.sessions_.IsStreaming(guid_);
    }
    [[nodiscard]] bool ReceiveReplayEstablished() const noexcept override {
        return host_.hostTransport_.IsReceiveReplayEstablished();
    }
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override {
        if (Cancelled()) return std::unexpected(kIOReturnAborted);
        // Hold the protocol for the blocking read so a concurrent removal
        // cannot free it underneath (DiceAudioBackend ProbeDuplexHealth).
        const auto protocol = host_.runtime_.FindShared(guid_);
        auto* family = protocol ? protocol->AsFamilyDriver() : nullptr;
        if (family == nullptr) return std::unexpected(kIOReturnNotReady);
        return family->ReadHealth(timeoutMs);
    }
    void Sleep(uint32_t milliseconds) noexcept override {
#ifdef ASFW_HOST_TEST
        if (host_.sleepForTesting_) {
            host_.sleepForTesting_(milliseconds);
            return;
        }
#endif
        IOSleep(milliseconds);
    }

private:
    AudioDeviceHost& host_;
    uint64_t guid_;
};

AudioDeviceHost::AudioDeviceHost(AudioNubPublisher& publisher,
                                 Discovery::DeviceRegistry& registry,
                                 AudioRuntimeRegistry& runtime,
                                 Session::AudioSessions& sessions,
                                 IIsochDuplexHostTransport& hostTransport) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , sessions_(sessions)
    , hostTransport_(hostTransport) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "[AudioHost] failed to allocate lock");
    }
    IODispatchQueue* queue = nullptr;
    const kern_return_t kr = IODispatchQueue::Create("com.asfw.audio.host", 0, 0, &queue);
    if (kr == kIOReturnSuccess && queue) {
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else {
        ASFW_LOG_ERROR(Audio, "[AudioHost] failed to create work queue kr=0x%08x (%{public}s)",
                       kr, ASFW::Logging::IOReturnName(kr));
    }
}

AudioDeviceHost::~AudioDeviceHost() noexcept {
    BeginTeardown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AudioDeviceHost::Install(AudioBackendKind kind, FamilyAdapter& adapter) noexcept {
    const auto index = static_cast<size_t>(kind);
    if (index >= adapters_.size()) return;
    adapters_[index] = &adapter;
    adapter.SetEventSink(this);
}

FamilyAdapter* AudioDeviceHost::AdapterFor(AudioBackendKind kind) const noexcept {
    const auto index = static_cast<size_t>(kind);
    return index < adapters_.size() ? adapters_[index] : nullptr;
}

std::optional<AudioBackendKind> AudioDeviceHost::KindForGuid(uint64_t guid) const noexcept {
    if (guid == 0) return std::nullopt;
    const auto record = registry_.SnapshotByGuid(guid);
    if (!record) return std::nullopt;
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy || !registry_.IsCurrent(policy->route)) return std::nullopt;
    return ChooseAudioBackend(policy->plan);
}

void AudioDeviceHost::Record(uint64_t guid, const char* family, HostEvent event, HostOutcome outcome,
                             IOReturn status, uint64_t run, const char* detail) noexcept {
    const auto e = static_cast<size_t>(event);
    const auto o = static_cast<size_t>(outcome);
    if (e < counters_.size() && o < counters_[e].size()) {
        counters_[e][o].fetch_add(1, std::memory_order_relaxed);
    }
    ASFW_LOG(Audio,
             "[AudioHost] guid=%016llx family=%{public}s event=%{public}s outcome=%{public}s "
             "kr=0x%08x (%{public}s) run=%llu%{public}s%{public}s",
             guid, family ? family : kNoFamily, ToString(event), ToString(outcome),
             status, ASFW::Logging::IOReturnName(status), run,
             detail ? " " : "", detail ? detail : "");
}

uint64_t AudioDeviceHost::OutcomeCount(HostEvent event, HostOutcome outcome) const noexcept {
    const auto e = static_cast<size_t>(event);
    const auto o = static_cast<size_t>(outcome);
    if (e >= counters_.size() || o >= counters_[e].size()) return 0;
    return counters_[e][o].load(std::memory_order_relaxed);
}

void AudioDeviceHost::Dispatch(std::function<void()> work) noexcept {
    if (!workQueue_) {
        // No queue: run inline rather than drop the work (the backends did the
        // same). Callers are never on the Default queue.
        work();
        return;
    }
#ifdef ASFW_HOST_TEST
    workQueue_->DispatchAsync(work);
#else
    workQueue_->DispatchAsync(^{ work(); });
#endif
}

bool AudioDeviceHost::TryBeginRecovery(uint64_t guid) noexcept {
    if (!lock_) return false;
    IOLockLock(lock_);
    const bool inserted = recoveringGuids_.insert(guid).second;
    IOLockUnlock(lock_);
    return inserted;
}

void AudioDeviceHost::FinishRecovery(uint64_t guid) noexcept {
    if (!lock_) return;
    IOLockLock(lock_);
    recoveringGuids_.erase(guid);
    IOLockUnlock(lock_);
}

// --- Publication (§4.3) --------------------------------------------------

void AudioDeviceHost::OfferDiscoveredDescription(uint64_t guid,
                                                 const Model::ASFWAudioDevice& description) noexcept {
    if (guid == 0) return;
    if (lock_) {
        IOLockLock(lock_);
        discoveredByGuid_.insert_or_assign(guid, description);
        IOLockUnlock(lock_);
    }
    RefreshPublication(guid);
}

void AudioDeviceHost::RefreshPublication(uint64_t guid) noexcept {
    if (guid == 0) return;

    // Shared: the admission must survive an asynchronous Describe.
    auto admission = std::make_shared<PublicationGate::AdmissionScope>(gate_);
    if (!admission->IsAdmitted()) {
        Record(guid, kNoFamily, HostEvent::Publish, HostOutcome::RefusedTeardown, kIOReturnAborted);
        return;
    }

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record) {
        Record(guid, kNoFamily, HostEvent::Publish, HostOutcome::RefusedNoPolicy, kIOReturnNotFound,
               0, "reason=no-registry-record");
        return;
    }
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy) {
        Record(guid, kNoFamily, HostEvent::Publish, HostOutcome::RefusedNoPolicy, kIOReturnNotFound,
               0, "reason=no-current-policy");
        return;
    }
    if (!registry_.IsCurrent(policy->route)) {
        Record(guid, kNoFamily, HostEvent::Publish, HostOutcome::RefusedStaleRoute, kIOReturnNotReady);
        return;
    }
    const auto kind = ChooseAudioBackend(policy->plan);
    FamilyAdapter* adapter = kind ? AdapterFor(*kind) : nullptr;
    if (!adapter) {
        Record(guid, kNoFamily, HostEvent::Publish, HostOutcome::RefusedNoAdapter, kIOReturnUnsupported);
        return;
    }

    DescribeInput input{
        .record = *record,
        .policy = record->audioPolicy,
        .protocol = runtime_.FindShared(guid),
    };
    if (publisher_.GetNub(guid) != nullptr) {
        if (const auto endpoint = runtime_.FindEndpointRuntime(guid)) {
            Model::ASFWAudioDevice committed;
            if (endpoint->CopyConfig(committed)) input.committed = std::move(committed);
        }
    }
    if (lock_) {
        IOLockLock(lock_);
        if (const auto it = discoveredByGuid_.find(guid); it != discoveredByGuid_.end()) {
            input.discovered = it->second;
        }
        IOLockUnlock(lock_);
    }

    const auto route = policy->route;
    const char* family = adapter->Name();
    const auto committed = input.committed;
    adapter->Describe(input, [this, guid, route, family, admission, committed](DescribeResult result) {
        if (admission->AbortIfStopping()) {
            Record(guid, family, HostEvent::Publish, HostOutcome::RefusedTeardown, kIOReturnAborted);
            return;
        }
#ifdef ASFW_HOST_TEST
        if (beforePublishHookForTesting_) beforePublishHookForTesting_();
#endif
        if (admission->AbortIfStopping()) {
            Record(guid, family, HostEvent::Publish, HostOutcome::RefusedTeardown, kIOReturnAborted);
            return;
        }
        if (!registry_.IsCurrent(route)) {
            Record(guid, family, HostEvent::Publish, HostOutcome::RefusedStaleRoute, kIOReturnNotReady);
            return;
        }

        // Publish a description, or check it against the live nub. `note` (may be
        // null) is the family's account of a fallback it made; the host prints it
        // beside the outcome it produced.
        const auto publish = [&](const Model::ASFWAudioDevice& value, const char* note) {
            char detail[96];
            const char* detailText = nullptr;
            if (note != nullptr) {
                snprintf(detail, sizeof(detail), "note=%s", note);
                detailText = detail;
            }
            // A live nub's graph was read once; never replace the runtime
            // config under it. Check, and latch a mismatch (§4.3). The check is
            // against the committed configuration, which a rate transaction
            // moves after publication (§4.7 E7b); without one, against the
            // first publication as before.
            if (publisher_.GetNub(guid) != nullptr) {
                if (committed.has_value()) {
                    const bool unchanged = Model::ClassifyAgainstCommitted(*committed, value) ==
                                           Model::GeometryRefreshDecision::kMayRefresh;
                    if (!unchanged) publisher_.BlockGeometryChange(guid, family);
                    Record(guid, family, HostEvent::Publish,
                           unchanged ? HostOutcome::Refreshed : HostOutcome::RefusedGeometryChanged,
                           unchanged ? kIOReturnSuccess : kIOReturnNotPermitted, 0, detailText);
                    return;
                }
                const bool unchanged = publisher_.RefreshNubProperties(guid, value, family);
                Record(guid, family, HostEvent::Publish,
                       unchanged ? HostOutcome::Refreshed : HostOutcome::RefusedGeometryChanged,
                       unchanged ? kIOReturnSuccess : kIOReturnNotPermitted, 0, detailText);
                return;
            }
            if (const auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
                endpoint->UpdateConfig(value);
            }
            const bool published = publisher_.EnsureNub(guid, value, family);
            Record(guid, family, HostEvent::Publish,
                   published ? HostOutcome::Published : HostOutcome::PublishFailed,
                   published ? kIOReturnSuccess : kIOReturnError, 0, detailText);
        };

        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DescribeRefusal>) {
                    char detail[96];
                    snprintf(detail, sizeof(detail), "reason=%s", value.reason ? value.reason : "?");
                    Record(guid, family, HostEvent::Publish, HostOutcome::RefusedDescribe, value.status,
                           0, detail);
                } else if constexpr (std::is_same_v<T, KeepCommitted>) {
                    Record(guid, family, HostEvent::Publish, HostOutcome::KeptCommittedFormation);
                } else if constexpr (std::is_same_v<T, DescribedWithNote>) {
                    publish(value.device, value.note);
                } else {
                    publish(value, nullptr);
                }
            },
            result);
    });
}

// --- Rebind and runtime faults ------------------------------------------

void AudioDeviceHost::OnDeviceResumed(uint64_t guid) noexcept {
    if (guid == 0) return;
    PublicationGate::AdmissionScope admission(gate_);
    const auto kind = KindForGuid(guid);
    FamilyAdapter* adapter = kind ? AdapterFor(*kind) : nullptr;
    const char* family = adapter ? adapter->Name() : kNoFamily;

    if (!admission.IsAdmitted() || Stopping() || sessions_.IsCancelled(guid)) {
        Record(guid, family, HostEvent::Rebind, HostOutcome::Cancelled, kIOReturnAborted);
        return;
    }
    if (!sessions_.IsStreaming(guid)) {
        Record(guid, family, HostEvent::Rebind, HostOutcome::NotStreaming);
        return;
    }
    if (!TryBeginRecovery(guid)) {
        Record(guid, family, HostEvent::Rebind, HostOutcome::Deduped);
        return;
    }
    Record(guid, family, HostEvent::Rebind, HostOutcome::Queued);

    Dispatch([this, guid, family] {
        // Teardown drains this queue; a block queued just before the drain
        // must not touch the device (FW-61).
        if (Stopping() || sessions_.IsCancelled(guid)) {
            Record(guid, family, HostEvent::Rebind, HostOutcome::Cancelled, kIOReturnAborted);
            FinishRecovery(guid);
            return;
        }
        const IOReturn status = sessions_.RequestRestart(guid, DuplexRestartReason::kBusResetRebind);
        Record(guid, family, HostEvent::Rebind, RestartOutcome(status), status);
        FinishRecovery(guid);
    });
}

void AudioDeviceHost::OnRuntimeFault(uint64_t guid, DuplexRestartReason reason) noexcept {
    if (guid == 0) return;
    PublicationGate::AdmissionScope admission(gate_);
    const auto kind = KindForGuid(guid);
    FamilyAdapter* adapter = kind ? AdapterFor(*kind) : nullptr;
    const char* family = adapter ? adapter->Name() : kNoFamily;

    char detail[48];
    snprintf(detail, sizeof(detail), "reason=%u", static_cast<unsigned>(reason));

    if (!admission.IsAdmitted() || Stopping() || sessions_.IsCancelled(guid)) {
        Record(guid, family, HostEvent::RuntimeFault, HostOutcome::Cancelled, kIOReturnAborted, 0, detail);
        return;
    }
    if (!adapter) {
        Record(guid, family, HostEvent::RuntimeFault, HostOutcome::Declined, kIOReturnUnsupported, 0, detail);
        return;
    }
    // A fault the family ignores (only DICE acts on cycle inconsistent) is
    // declined before it can take the recovery slot from one it acts on.
    if (!adapter->ActsOn(reason)) {
        snprintf(detail, sizeof(detail), "reason=%u not-acted-on", static_cast<unsigned>(reason));
        Record(guid, family, HostEvent::RuntimeFault, HostOutcome::Declined, kIOReturnUnsupported, 0, detail);
        return;
    }
    const uint64_t observedRun = IsRunTied(reason) ? sessions_.RunningRun(guid) : 0;
    if (!TryBeginRecovery(guid)) {
        Record(guid, family, HostEvent::RuntimeFault, HostOutcome::Deduped, kIOReturnSuccess, observedRun, detail);
        return;
    }
    Record(guid, family, HostEvent::RuntimeFault, HostOutcome::Queued, kIOReturnSuccess, observedRun, detail);

    Dispatch([this, guid, reason, observedRun, adapter, family] {
        char blockDetail[48];
        snprintf(blockDetail, sizeof(blockDetail), "reason=%u", static_cast<unsigned>(reason));
        if (Stopping() || sessions_.IsCancelled(guid)) {
            Record(guid, family, HostEvent::RuntimeFault, HostOutcome::Cancelled, kIOReturnAborted,
                   observedRun, blockDetail);
            FinishRecovery(guid);
            return;
        }
        HostFaultContext context(*this, guid);
        const FaultVerdict verdict = adapter->JudgeRuntimeFault(guid, reason, context);
        if (verdict == FaultVerdict::kSelfHealed) {
            Record(guid, family, HostEvent::RuntimeFault, HostOutcome::SelfHealed, kIOReturnSuccess,
                   observedRun, blockDetail);
            FinishRecovery(guid);
            return;
        }
        if (verdict == FaultVerdict::kDeviceLeft || context.Cancelled()) {
            Record(guid, family, HostEvent::RuntimeFault,
                   verdict == FaultVerdict::kDeviceLeft ? HostOutcome::DeviceLeft : HostOutcome::Cancelled,
                   kIOReturnAborted, observedRun, blockDetail);
            FinishRecovery(guid);
            return;
        }
        const IOReturn status = sessions_.RequestRestart(guid, reason, observedRun);
        Record(guid, family, HostEvent::RuntimeFault, RestartOutcome(status), status, observedRun, blockDetail);
        FinishRecovery(guid);
    });
}

// --- Device events (§4.4) -------------------------------------------------

void AudioDeviceHost::OnDeviceEvent(uint64_t guid, DeviceEvent event, uint32_t detail) noexcept {
    if (guid == 0) return;
    const auto kind = KindForGuid(guid);
    FamilyAdapter* adapter = kind ? AdapterFor(*kind) : nullptr;
    const char* family = adapter ? adapter->Name() : kNoFamily;
    char text[32];
    snprintf(text, sizeof(text), "detail=0x%08x", detail);

    if (event == DeviceEvent::kStreamConfigChanged) {
        Record(guid, family, HostEvent::DeviceEvent, HostOutcome::ConfigChange, kIOReturnSuccess, 0, text);
        OnRuntimeFault(guid, DuplexRestartReason::kDeviceConfigChange);
        return;
    }

    PublicationGate::AdmissionScope admission(gate_);
    if (!admission.IsAdmitted() || Stopping()) {
        Record(guid, family, HostEvent::DeviceEvent, HostOutcome::Cancelled, kIOReturnAborted, 0, text);
        return;
    }
    // Only a streaming device's clock health matters; an idle one re-reads its
    // clock at the next start (DiceAudioBackend HandleDeviceNotification).
    if (!sessions_.IsStreaming(guid)) {
        Record(guid, family, HostEvent::DeviceEvent, HostOutcome::NotStreaming, kIOReturnSuccess, 0, text);
        return;
    }
    Record(guid, family, HostEvent::DeviceEvent, HostOutcome::ClockProbe, kIOReturnSuccess, 0, text);
    Dispatch([this, guid, family, detail] { ProbeClock(guid, family, detail); });
}

void AudioDeviceHost::ProbeClock(uint64_t guid, const char* family, uint32_t detail) noexcept {
    char text[96];
    snprintf(text, sizeof(text), "detail=0x%08x", detail);
    HostFaultContext context(*this, guid);
    if (context.Cancelled()) {
        Record(guid, family, HostEvent::DeviceEvent, HostOutcome::Cancelled, kIOReturnAborted, 0, text);
        return;
    }
    const auto health = context.ReadHealth(kHealthReadTimeoutMs);
    if (!health) {
        const IOReturn status = health.error();
        Record(guid, family, HostEvent::DeviceEvent,
               (status == kIOReturnAborted && context.Cancelled()) ? HostOutcome::Cancelled
                                                                   : HostOutcome::ProbeFailed,
               status, 0, text);
        return;
    }

    if (!health->sourceLocked || !health->clockReferenceHealthy) {
        // Logged only: the TCAT kexts also only log lock loss
        // (AUDIO_SESSION_REDESIGN.md §2.4). A real config change restarts.
        snprintf(text, sizeof(text), "detail=0x%08x locked=%u refHealthy=%u status=0x%08x ext=0x%08x",
                 detail, health->sourceLocked ? 1U : 0U, health->clockReferenceHealthy ? 1U : 0U,
                 health->status, health->extStatus);
        Record(guid, family, HostEvent::DeviceEvent, HostOutcome::ClockDegraded, kIOReturnSuccess, 0, text);
        return;
    }

    // Healthy, but the device may have moved to another rate on its own
    // (front panel, external sync). Tell the audio driver to resync the HAL,
    // unless the host itself is moving the clock: during a host rate change
    // the device relocks before the nub's belief updates, and a second,
    // competing config change wedges the HAL (DiceAudioBackend ProbeDuplexHealth).
    auto* nub = publisher_.GetNub(guid);
    const uint32_t deviceRateHz = health->nominalRateHz;
    const uint32_t hostRateHz = nub ? nub->GetCurrentSampleRateHz() : 0;
    if (nub && deviceRateHz != 0 && hostRateHz != 0 && deviceRateHz != hostRateHz) {
        snprintf(text, sizeof(text), "detail=0x%08x device=%u host=%u", detail, deviceRateHz, hostRateHz);
        const auto session = sessions_.Snapshot(guid);
        const bool echoesHostClock = session.has_value() &&
                                     (session->clockChangePending ||
                                      session->desiredClock.sampleRateHz == deviceRateHz);
        if (echoesHostClock || sessions_.IsReconciling(guid)) {
            Record(guid, family, HostEvent::DeviceEvent, HostOutcome::ClockEcho, kIOReturnSuccess, 0, text);
            return;
        }
        Record(guid, family, HostEvent::DeviceEvent, HostOutcome::DeviceRateChange, kIOReturnSuccess, 0, text);
        nub->NotifyDeviceClockChanged(deviceRateHz);
        return;
    }
    snprintf(text, sizeof(text), "detail=0x%08x status=0x%08x ext=0x%08x", detail, health->status,
             health->extStatus);
    Record(guid, family, HostEvent::DeviceEvent, HostOutcome::ClockHealthy, kIOReturnSuccess, 0, text);
}

// --- Removal and teardown -------------------------------------------------

void AudioDeviceHost::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    if (guid == 0 || !lock_) return;
    IOLockLock(lock_);
    recoveringGuids_.erase(guid);
    discoveredByGuid_.erase(guid);
    IOLockUnlock(lock_);
}

void AudioDeviceHost::BeginTeardown() noexcept {
    // Latch first: queued blocks and in-flight entry points read it.
    stopping_.store(true, std::memory_order_release);
    for (FamilyAdapter* adapter : adapters_) {
        if (adapter) adapter->SetEventSink(nullptr);
    }
    if (teardownComplete_.load(std::memory_order_acquire)) return;

    if (!teardownStarted_.exchange(true, std::memory_order_acq_rel)) {
        // Close admission and wait for admitted entry points and publications.
        gate_.CloseAndWait();
        const uint64_t startMs = UptimeMilliseconds();
        // Drain queued work before hardware detaches; each block checks
        // stopping_ before it can touch the device (FW-61).
        if (workQueue_) {
#ifdef ASFW_HOST_TEST
            if (onTeardownDrainStartedHookForTesting_) onTeardownDrainStartedHookForTesting_();
            workQueue_->DispatchSync([] {});
#else
            workQueue_->DispatchSync(^{});
#endif
        }
        const uint64_t endMs = UptimeMilliseconds();
        teardownComplete_.store(true, std::memory_order_release);

        char detail[160];
        snprintf(detail, sizeof(detail),
                 "drain=%llums publishRefused=%llu rebindCancelled=%llu faultCancelled=%llu "
                 "eventCancelled=%llu gateRejects=%llu",
                 endMs >= startMs ? endMs - startMs : 0ULL,
                 OutcomeCount(HostEvent::Publish, HostOutcome::RefusedTeardown),
                 OutcomeCount(HostEvent::Rebind, HostOutcome::Cancelled),
                 OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Cancelled),
                 OutcomeCount(HostEvent::DeviceEvent, HostOutcome::Cancelled),
                 gate_.RejectCount());
        Record(0, kNoFamily, HostEvent::Teardown, HostOutcome::Drained, kIOReturnSuccess, 0, detail);
        return;
    }

    // A second, concurrent caller waits until the first has drained.
#ifdef ASFW_HOST_TEST
    if (onSecondaryTeardownWaitingHookForTesting_) onSecondaryTeardownWaitingHookForTesting_();
#endif
    while (!teardownComplete_.load(std::memory_order_acquire)) {
        IOSleep(1);
    }
}

} // namespace ASFW::Audio::Host
