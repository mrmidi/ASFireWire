// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDuplexRestartCoordinator.hpp - Top-level DICE restart FSM owner

#pragma once

#include "ClockRequestBroker.hpp"
#include "DiceHostTransport.hpp"
#include "DuplexOperationGate.hpp"
#include "RestartSessionStore.hpp"

#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Hardware/HardwareInterface.hpp"
#include "../DICE/Core/IDICEDuplexProtocol.hpp"
#include "../DICE/Core/DICERestartSession.hpp"

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

class DiceDuplexRestartCoordinator final {
public:
    using DirectAudioBindingSourceProvider = std::function<ASFW::Audio::Runtime::IDirectAudioBindingSource*(uint64_t guid)>;

    DiceDuplexRestartCoordinator(Discovery::DeviceRegistry& registry,
                                 AudioRuntimeRegistry& runtime,
                                 IDiceHostTransport& hostTransport,
                                 Driver::HardwareInterface& hardware,
                                 const std::atomic<bool>* cancel,
                                 DirectAudioBindingSourceProvider bindingSourceProvider) noexcept;
    ~DiceDuplexRestartCoordinator() noexcept;

    DiceDuplexRestartCoordinator(const DiceDuplexRestartCoordinator&) = delete;
    DiceDuplexRestartCoordinator& operator=(const DiceDuplexRestartCoordinator&) = delete;

    [[nodiscard]] IOReturn StartStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn StopStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn RequestClockConfig(
        uint64_t guid,
        const DICE::DiceDesiredClockConfig& desiredClock,
        DICE::DiceRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RecoverStreaming(uint64_t guid,
                                            DICE::DiceRestartReason reason) noexcept;

    void ClearSession(uint64_t guid) noexcept;
    [[nodiscard]] std::optional<DICE::DiceRestartSession> GetSession(uint64_t guid) const noexcept;

    [[nodiscard]] uint64_t TeardownAbortCount() const noexcept {
        return teardownAbortCount_.load(std::memory_order_acquire);
    }

private:
    using PendingClockRequest = Backends::ClockRequestBroker::PendingClockRequest;

    [[nodiscard]] IOReturn RunStartStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn RunStopStreaming(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn RunRecoveryStreaming(uint64_t guid,
                                                DICE::DiceRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RunClockRequestLoop(uint64_t guid, PendingClockRequest initialRequest) noexcept;
    [[nodiscard]] IOReturn ApplyClockRequest(uint64_t guid, const PendingClockRequest& request) noexcept;

    [[nodiscard]] IOReturn RunDuplexStart(uint64_t guid,
                                          Discovery::DeviceRecord& record,
                                          DICE::IDICEDuplexProtocol& diceProtocol,
                                          DICE::DiceRestartSession& session,
                                          const DICE::DiceDesiredClockConfig& desiredClock,
                                          DICE::DiceRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RunDuplexStop(uint64_t guid,
                                         Discovery::DeviceRecord& record,
                                         DICE::IDICEDuplexProtocol& diceProtocol,
                                         DICE::DiceRestartSession& session) noexcept;
    [[nodiscard]] IOReturn RunIdleClockApply(uint64_t guid,
                                             DICE::IDICEDuplexProtocol& diceProtocol,
                                             DICE::DiceRestartSession& session,
                                             FW::Generation topologyGeneration,
                                             const DICE::DiceDesiredClockConfig& desiredClock,
                                             DICE::DiceRestartReason reason) noexcept;
    // Resolves the record + its DICE duplex surface for `guid`. `outHold` receives a
    // shared_ptr to the owning IDeviceProtocol; callers must keep it alive for as long as
    // they use `outDiceProtocol` (it is a view into the held protocol).
    [[nodiscard]] Discovery::DeviceRecord* RequireDiceRecord(
        uint64_t guid,
        DICE::IDICEDuplexProtocol*& outDiceProtocol,
        std::shared_ptr<IDeviceProtocol>& outHold) noexcept;
    [[nodiscard]] ASFW::Audio::Runtime::IDirectAudioBindingSource* GetDirectAudioBindingSource(uint64_t guid) const noexcept;
    [[nodiscard]] bool TryAcquireGuid(uint64_t guid) noexcept;
    void ReleaseGuid(uint64_t guid) noexcept;
    void RequestStopIntent(uint64_t guid) noexcept;
    void ClearStopIntent(uint64_t guid) noexcept;
    [[nodiscard]] bool IsStopRequested(uint64_t guid) const noexcept;
    [[nodiscard]] uint64_t AllocateRestartId() noexcept;
    [[nodiscard]] bool IsRestartEpochCurrent(uint64_t guid,
                                             uint64_t restartId,
                                             FW::Generation topologyGeneration) const noexcept;
    [[nodiscard]] bool TryConsumePendingClockRequest(uint64_t guid, PendingClockRequest& outRequest) noexcept;
    [[nodiscard]] bool TryTakeCompletedClockRequest(
        uint64_t guid,
        uint64_t token,
        DICE::DiceClockRequestCompletion& outCompletion) noexcept;
    void CompleteClockRequest(const DICE::DiceClockRequestCompletion& completion, uint64_t guid) noexcept;
    void FailPendingClockRequest(uint64_t guid,
                                 DICE::DiceClockRequestOutcome outcome,
                                 IOReturn status) noexcept;
    void RecordTeardownAbort(const char* stage, uint64_t guid) noexcept;

    [[nodiscard]] DICE::DiceRestartSession LoadSession(uint64_t guid) const noexcept;
    void StoreSession(const DICE::DiceRestartSession& session) noexcept;

    // FW-61: global teardown cancel token. Distinct from the per-guid stop intent
    // (gate_, see DuplexOperationGate) - that aborts one stream; this aborts everything for
    // service teardown. Owned by DiceAudioBackend and read on the dice queue.
    [[nodiscard]] bool TeardownRequested() const noexcept {
        return cancel_ != nullptr && cancel_->load(std::memory_order_acquire);
    }

    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    IDiceHostTransport& hostTransport_;
    Driver::HardwareInterface& hardware_;
    const std::atomic<bool>* cancel_{nullptr};
    DirectAudioBindingSourceProvider bindingSourceProvider_;

    IOLock* lock_{nullptr};
    // FW-68: per-GUID active-session + stop-intent gating. Borrows &lock_ (see
    // DuplexOperationGate) so its critical sections share the coordinator's single lock.
    Backends::DuplexOperationGate gate_{&lock_};
    // FW-69b: per-GUID session persistence + restart-id allocator. Borrows &lock_ (see
    // RestartSessionStore) so its critical sections share the coordinator's single lock.
    Backends::RestartSessionStore store_{&lock_};
    // FW-67: clock token + pending/completion delivery. Borrows &lock_ and shares the store so
    // pending/completion state remains atomic with the restart-session snapshot.
    Backends::ClockRequestBroker clockRequests_{&lock_, store_};
    std::atomic<uint64_t> teardownAbortCount_{0};

    static constexpr uint32_t kSyncBridgeTimeoutMs = 12000;
    static constexpr uint32_t kSyncBridgePollMs = 10;
    static constexpr uint32_t kGlobalClockLockTimeoutMs = 1000;
    static constexpr uint32_t kGlobalClockLockPollMs = 10;
    static constexpr uint32_t kGlobalClockStableReads = 3;
};

} // namespace ASFW::Audio
