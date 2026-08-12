// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDuplexCoordinator.hpp - Top-level audio duplex lifecycle owner

#pragma once

#include "ClockRequestBroker.hpp"
#include "IsochDuplexHostTransport.hpp"
#include "DuplexOperationGate.hpp"
#include "RestartSessionStore.hpp"
#include "../Protocols/Duplex/IDuplexDeviceControl.hpp"
#include "../Devices/AudioIdentity.hpp"

#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Hardware/HardwareInterface.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace ASFW::Audio {

namespace Runtime {
class IDirectAudioBindingSource;
}

class AudioRuntimeRegistry;
class IDeviceProtocol;

class AudioDuplexCoordinator final {
public:
    using EndpointId = Devices::AudioEndpointId;
    using DirectAudioBindingSourceProvider =
        std::function<ASFW::Audio::Runtime::IDirectAudioBindingSource*(EndpointId)>;

    AudioDuplexCoordinator(Discovery::DeviceRegistry& registry,
                           AudioRuntimeRegistry& runtime,
                           IIsochDuplexHostTransport& hostTransport,
                           Driver::HardwareInterface& hardware,
                           const std::atomic<bool>* cancel,
                           DirectAudioBindingSourceProvider bindingSourceProvider) noexcept;
    ~AudioDuplexCoordinator() noexcept;

    AudioDuplexCoordinator(const AudioDuplexCoordinator&) = delete;
    AudioDuplexCoordinator& operator=(const AudioDuplexCoordinator&) = delete;

    [[nodiscard]] IOReturn StartStreaming(EndpointId endpointId) noexcept;
    [[nodiscard]] IOReturn StopStreaming(EndpointId endpointId) noexcept;
    [[nodiscard]] IOReturn RequestClockConfig(
        EndpointId endpointId,
        const AudioClockConfig& desiredClock,
        DuplexRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RecoverStreaming(EndpointId endpointId,
                                            DuplexRestartReason reason) noexcept;

    // Called once discovery has conclusively retired a endpoint after a bus reset.
    // It cancels this endpoint's queued work without treating the whole controller
    // as gone; AcknowledgeDevicePresent reopens it after rediscovery.
    void CancelRemoteDevice(EndpointId endpointId) noexcept;
    void AcknowledgeDevicePresent(EndpointId endpointId) noexcept;
    // Backend work queues use this before any device-side probe or recovery.
    // It covers both service teardown and a endpoint retired by discovery.
    [[nodiscard]] bool IsDeviceOperationCancelled(EndpointId endpointId) const noexcept;
    void ClearSession(EndpointId endpointId) noexcept;
    [[nodiscard]] std::optional<DuplexRestartSession>
    GetSession(EndpointId endpointId) const noexcept;
    // True while a host-initiated duplex operation (start/stop/clock change/recovery) holds the
    // per-endpoint gate or a clock request is queued behind it. Health probes use this to tell a
    // genuine device-initiated clock move from the echo of the host's own in-flight change.
    [[nodiscard]] bool IsOperationInFlight(EndpointId endpointId) const noexcept;

    [[nodiscard]] uint64_t TeardownAbortCount() const noexcept {
        return teardownAbortCount_.load(std::memory_order_acquire);
    }

private:
    using PendingClockRequest = Duplex::ClockRequestBroker::PendingClockRequest;

    [[nodiscard]] IOReturn RunStartStreaming(EndpointId endpointId) noexcept;
    [[nodiscard]] IOReturn RunStopStreaming(EndpointId endpointId) noexcept;
    [[nodiscard]] IOReturn RunRecoveryStreaming(EndpointId endpointId,
                                                DuplexRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RunClockRequestLoop(EndpointId endpointId, PendingClockRequest initialRequest) noexcept;
    [[nodiscard]] IOReturn ApplyClockRequest(EndpointId endpointId, const PendingClockRequest& request) noexcept;

    [[nodiscard]] IOReturn RunDuplexStart(EndpointId endpointId,
                                          Discovery::DeviceRecord& record,
                                          IDuplexDeviceControl& deviceControl,
                                          DuplexRestartSession& session,
                                          const AudioClockConfig& desiredClock,
                                          DuplexRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RunDuplexStop(EndpointId endpointId,
                                         Discovery::DeviceRecord& record,
                                         IDuplexDeviceControl& deviceControl,
                                         DuplexRestartSession& session) noexcept;
    [[nodiscard]] IOReturn RunIdleClockApply(EndpointId endpointId,
                                             IDuplexDeviceControl& deviceControl,
                                             DuplexRestartSession& session,
                                             FW::Generation topologyGeneration,
                                             const AudioClockConfig& desiredClock,
                                             DuplexRestartReason reason) noexcept;
    // Resolves the record + its protocol-neutral duplex surface for `endpointId`. `outHold` receives a
    // shared_ptr to the owning IDeviceProtocol; callers must keep it alive for as long as
    // they use `outDeviceControl` (it is a view into the held protocol).
    [[nodiscard]] std::optional<Discovery::DeviceRecord> RequireDuplexRecord(
        EndpointId endpointId,
        IDuplexDeviceControl*& outDeviceControl,
        std::shared_ptr<IDeviceProtocol>& outHold) noexcept;
    [[nodiscard]] ASFW::Audio::Runtime::IDirectAudioBindingSource* GetDirectAudioBindingSource(EndpointId endpointId) const noexcept;
    [[nodiscard]] bool TryAcquireEndpoint(EndpointId endpointId) noexcept;
    void ReleaseEndpoint(EndpointId endpointId) noexcept;
    void RequestStopIntent(EndpointId endpointId) noexcept;
    void ClearStopIntent(EndpointId endpointId) noexcept;
    [[nodiscard]] bool IsStopRequested(EndpointId endpointId) const noexcept;
    [[nodiscard]] uint64_t AllocateRestartId() noexcept;
    [[nodiscard]] bool IsRestartEpochCurrent(EndpointId endpointId,
                                             uint64_t restartId,
                                             const Discovery::DeviceRouteToken& route) const noexcept;
    [[nodiscard]] bool TryConsumePendingClockRequest(EndpointId endpointId, PendingClockRequest& outRequest) noexcept;
    [[nodiscard]] bool TryTakeCompletedClockRequest(
        EndpointId endpointId,
        uint64_t token,
        DuplexClockRequestCompletion& outCompletion) noexcept;
    void CompleteClockRequest(const DuplexClockRequestCompletion& completion, EndpointId endpointId) noexcept;
    void FailPendingClockRequest(EndpointId endpointId,
                                 DuplexClockRequestOutcome outcome,
                                 IOReturn status) noexcept;
    void RecordTeardownAbort(const char* stage, EndpointId endpointId) noexcept;

    [[nodiscard]] DuplexRestartSession LoadSession(EndpointId endpointId) const noexcept;
    void StoreSession(const DuplexRestartSession& session) noexcept;

    // FW-61: global teardown cancel token. Distinct from the per-endpoint stop intent
    // (gate_, see DuplexOperationGate) - that aborts one stream; this aborts everything for
    // service teardown. Owned by the audio composition root and read on the
    // coordinator queue.
    [[nodiscard]] bool TeardownRequested() const noexcept {
        return cancel_ != nullptr && cancel_->load(std::memory_order_acquire);
    }

    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    IIsochDuplexHostTransport& hostTransport_;
    Driver::HardwareInterface& hardware_;
    const std::atomic<bool>* cancel_{nullptr};
    DirectAudioBindingSourceProvider bindingSourceProvider_;

    IOLock* lock_{nullptr};
    // FW-68: per-endpoint active-session + stop-intent gating. Borrows &lock_ (see
    // DuplexOperationGate) so its critical sections share the coordinator's single lock.
    Duplex::DuplexOperationGate gate_{&lock_};
    // FW-69b: per-endpoint session persistence + restart-id allocator. Borrows &lock_ (see
    // RestartSessionStore) so its critical sections share the coordinator's single lock.
    Duplex::RestartSessionStore store_{&lock_};
    // FW-67: clock token + pending/completion delivery. Borrows &lock_ and shares the store so
    // pending/completion state remains atomic with the restart-session snapshot.
    Duplex::ClockRequestBroker clockRequests_{&lock_, store_};
    std::atomic<uint64_t> teardownAbortCount_{0};

    static constexpr uint32_t kSyncBridgeTimeoutMs = 12000;
    static constexpr uint32_t kSyncBridgePollMs = 10;
    static constexpr uint32_t kGlobalClockLockTimeoutMs = 1000;
    static constexpr uint32_t kGlobalClockLockPollMs = 10;
    static constexpr uint32_t kGlobalClockStableReads = 3;
};

} // namespace ASFW::Audio
