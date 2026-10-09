// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDeviceHost.hpp - The one lifecycle shell around every audio family.
//
// documentation/AUDIO_DEVICE_HOST.md. Replaces the shell the four backends
// each wrote for themselves: publication admission, route checks, one work
// queue, per-device recovery dedupe, and teardown. A family supplies only a
// FamilyAdapter (how to describe the endpoint, whether a fault is real, which
// device events it raises). The session (Audio/Session) is used unchanged.
//
// Every decision goes through Record() (§6.1): one [AudioHost] log line and
// one per-outcome counter. Tests assert on the counters, so a path that skips
// Record fails a test.
//
// Threading: entry points may be called from any queue. Publication runs on
// the caller's thread (or the adapter's completion thread); rebinds, runtime
// faults and clock probes run on the host's own queue, com.asfw.audio.host,
// where they may block. BeginTeardown must run before the core detaches
// hardware: it closes admission, drains the queue and returns once no host
// work can touch the device again.

#pragma once

#include "FamilyAdapter.hpp"
#include "PublicationGate.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ASFW::Discovery {
class DeviceRegistry;
}

namespace ASFW::Audio {
class AudioNubPublisher;
class AudioRuntimeRegistry;
class IIsochDuplexHostTransport;
namespace Session {
class AudioSessions;
}
}

namespace ASFW::Audio::Host {

enum class HostEvent : uint8_t {
    Publish,
    Rebind,        // bus reset while streaming
    RuntimeFault,  // timing loss, cycle inconsistent, device config change, ...
    DeviceEvent,   // raised by an adapter
    Teardown,
    kCount,
};

enum class HostOutcome : uint8_t {
    // Publish
    Published,
    Refreshed,
    PublishFailed,
    RefusedTeardown,
    RefusedStaleRoute,
    RefusedNoPolicy,
    RefusedNoAdapter,
    RefusedDescribe,
    RefusedGeometryChanged,
    KeptCommittedFormation,
    // Rebind / RuntimeFault
    Queued,
    Deduped,
    NotStreaming,
    Cancelled,
    SelfHealed,
    DeviceLeft,
    RestartRequested,
    Declined,  // the session declined: stale run, faulted, nothing to do
    Failed,
    // DeviceEvent
    ConfigChange,
    ClockProbe,
    ClockHealthy,
    ClockDegraded,
    DeviceRateChange,
    ClockEcho,  // a rate mismatch the host itself caused; no resync
    ProbeFailed,
    // Teardown
    Drained,
    kCount,
};

[[nodiscard]] const char* ToString(HostEvent event) noexcept;
[[nodiscard]] const char* ToString(HostOutcome outcome) noexcept;

class AudioDeviceHost final : public DeviceEventSink {
public:
    AudioDeviceHost(AudioNubPublisher& publisher,
                    Discovery::DeviceRegistry& registry,
                    AudioRuntimeRegistry& runtime,
                    Session::AudioSessions& sessions,
                    IIsochDuplexHostTransport& hostTransport) noexcept;
    ~AudioDeviceHost() noexcept override;

    AudioDeviceHost(const AudioDeviceHost&) = delete;
    AudioDeviceHost& operator=(const AudioDeviceHost&) = delete;

    /// Route devices of `kind` to `adapter`. Composition time only, before any
    /// device callback. The adapter must outlive the host's teardown.
    void Install(AudioBackendKind kind, FamilyAdapter& adapter) noexcept;

    /// Which family serves `guid` now, if its current policy names one.
    [[nodiscard]] std::optional<AudioBackendKind> KindForGuid(uint64_t guid) const noexcept;

    // --- Entry points (§4.2) ---------------------------------------------

    /// Describe the endpoint and publish it, or check it against the live nub.
    void RefreshPublication(uint64_t guid) noexcept;
    /// Discovery pushed a description (AV/C). Stored for Describe, then published.
    void OfferDiscoveredDescription(uint64_t guid, const Model::ASFWAudioDevice& description) noexcept;
    /// The device came back after a bus reset. Restarts only a streaming device.
    void OnDeviceResumed(uint64_t guid) noexcept;
    /// A runtime fault on `guid`'s streams; the family judges whether it is real.
    void OnRuntimeFault(uint64_t guid, DuplexRestartReason reason) noexcept;
    /// DeviceEventSink: an adapter's neutral event.
    void OnDeviceEvent(uint64_t guid, DeviceEvent event, uint32_t detail) noexcept override;
    /// The device is gone: drop its queued work and stored state.
    void CancelRemoteDeviceWork(uint64_t guid) noexcept;
    /// Close admission, drain the queue. Idempotent; concurrent callers wait.
    void BeginTeardown() noexcept;

    // --- Instrumentation (§6.1) ------------------------------------------

    [[nodiscard]] uint64_t OutcomeCount(HostEvent event, HostOutcome outcome) const noexcept;

    /// Timeout for the clock-status health read (DiceAudioBackend's
    /// kHealthBridgeTimeoutMs).
    static constexpr uint32_t kHealthReadTimeoutMs = 1000;

#ifdef ASFW_HOST_TEST
    void SetBeforePublishHookForTesting(std::function<void()> hook) noexcept {
        beforePublishHookForTesting_ = std::move(hook);
    }
    void SetOnTeardownDrainStartedHookForTesting(std::function<void()> hook) noexcept {
        onTeardownDrainStartedHookForTesting_ = std::move(hook);
    }
    void SetOnTeardownGateClosedHookForTesting(std::function<void()> hook) noexcept {
        gate_.SetOnGateClosedForTesting(std::move(hook));
    }
    void SetOnSecondaryTeardownWaitingHookForTesting(std::function<void()> hook) noexcept {
        onSecondaryTeardownWaitingHookForTesting_ = std::move(hook);
    }
    /// Replace the sleep used by fault judgements (FaultContext::Sleep).
    void SetSleepForTesting(std::function<void(uint32_t)> sleep) noexcept {
        sleepForTesting_ = std::move(sleep);
    }
    [[nodiscard]] IODispatchQueue* WorkQueueForTesting() const noexcept { return workQueue_.get(); }
    [[nodiscard]] bool IsTeardownCompleteForTesting() const noexcept {
        return teardownComplete_.load(std::memory_order_acquire);
    }
#endif

private:
    class HostFaultContext;

    void Record(uint64_t guid, const char* family, HostEvent event, HostOutcome outcome,
                IOReturn status = kIOReturnSuccess, uint64_t run = 0,
                const char* detail = nullptr) noexcept;

    [[nodiscard]] FamilyAdapter* AdapterFor(AudioBackendKind kind) const noexcept;
    [[nodiscard]] bool TryBeginRecovery(uint64_t guid) noexcept;
    void FinishRecovery(uint64_t guid) noexcept;
    void Dispatch(std::function<void()> work) noexcept;
    void ProbeClock(uint64_t guid, const char* family, uint32_t detail) noexcept;
    [[nodiscard]] bool Stopping() const noexcept { return stopping_.load(std::memory_order_acquire); }

    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Session::AudioSessions& sessions_;
    IIsochDuplexHostTransport& hostTransport_;

    std::array<FamilyAdapter*, 4> adapters_{};

    OSSharedPtr<IODispatchQueue> workQueue_{};
    PublicationGate gate_{};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> teardownStarted_{false};
    std::atomic<bool> teardownComplete_{false};

    IOLock* lock_{nullptr};
    std::unordered_set<uint64_t> recoveringGuids_{};                               // lock_
    std::unordered_map<uint64_t, Model::ASFWAudioDevice> discoveredByGuid_{};      // lock_

    std::array<std::array<std::atomic<uint64_t>, static_cast<size_t>(HostOutcome::kCount)>,
               static_cast<size_t>(HostEvent::kCount)> counters_{};

#ifdef ASFW_HOST_TEST
    std::function<void()> beforePublishHookForTesting_{};
    std::function<void()> onTeardownDrainStartedHookForTesting_{};
    std::function<void()> onSecondaryTeardownWaitingHookForTesting_{};
    std::function<void(uint32_t)> sleepForTesting_{};
#endif
};

} // namespace ASFW::Audio::Host
