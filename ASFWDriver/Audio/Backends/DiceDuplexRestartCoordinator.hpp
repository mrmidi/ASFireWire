// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceDuplexRestartCoordinator.hpp - Top-level DICE restart FSM owner

#pragma once

#include "DiceHostTransport.hpp"

#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Protocols/Audio/DICE/Core/IDICEDuplexProtocol.hpp"
#include "../../Protocols/Audio/DICE/Core/DICERestartSession.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ASFW::Audio {

class DiceDuplexRestartCoordinator final {
public:
    using QueueProviderFactory = std::function<std::unique_ptr<IDiceQueueMemoryProvider>(uint64_t guid)>;

    DiceDuplexRestartCoordinator(Discovery::DeviceRegistry& registry,
                                 IDiceHostTransport& hostTransport,
                                 Driver::HardwareInterface& hardware,
                                 QueueProviderFactory queueProviderFactory) noexcept;
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

private:
    struct PendingClockRequest {
        DICE::DiceDesiredClockConfig desiredClock{};
        DICE::DiceRestartReason reason{DICE::DiceRestartReason::kManualReconfigure};
        uint64_t token{0};
    };

    struct ClockCompletionStore {
        std::unordered_map<uint64_t, DICE::DiceClockRequestCompletion> byToken{};
        std::deque<uint64_t> insertionOrder{};
    };

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

    [[nodiscard]] Discovery::DeviceRecord* RequireDiceRecord(uint64_t guid,
                                                             DICE::IDICEDuplexProtocol*& outDiceProtocol) noexcept;
    [[nodiscard]] std::unique_ptr<IDiceQueueMemoryProvider> MakeQueueProvider(uint64_t guid) const noexcept;
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

    [[nodiscard]] DICE::DiceRestartSession LoadSession(uint64_t guid) const noexcept;
    void StoreSession(const DICE::DiceRestartSession& session) noexcept;

    Discovery::DeviceRegistry& registry_;
    IDiceHostTransport& hostTransport_;
    Driver::HardwareInterface& hardware_;
    QueueProviderFactory queueProviderFactory_;

    IOLock* lock_{nullptr};
    std::unordered_set<uint64_t> activeGuids_{};
    std::unordered_set<uint64_t> stopRequestedGuids_{};
    std::unordered_map<uint64_t, DICE::DiceRestartSession> sessions_{};
    std::unordered_map<uint64_t, PendingClockRequest> pendingClockRequests_{};
    std::unordered_map<uint64_t, ClockCompletionStore> completedClockRequests_{};
    uint64_t nextClockToken_{1};
    uint64_t nextRestartId_{1};

    static constexpr uint32_t kSyncBridgeTimeoutMs = 12000;
    static constexpr uint32_t kSyncBridgePollMs = 10;
    static constexpr size_t kMaxCompletedClockRequestsPerGuid = 32;
};

} // namespace ASFW::Audio
