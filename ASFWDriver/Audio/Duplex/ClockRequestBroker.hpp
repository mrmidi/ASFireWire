// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "RestartJournal.hpp"
#include "RestartSessionStore.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>

namespace ASFW::Audio::Duplex {

class ClockRequestBroker final {
public:
    using EndpointId = Devices::AudioEndpointId;

    struct PendingClockRequest final {
        AudioClockConfig desiredClock{};
        DuplexRestartReason reason{DuplexRestartReason::kManualReconfigure};
        uint64_t token{0};
    };

    ClockRequestBroker(IOLock** lock, RestartSessionStore& sessionStore) noexcept
        : lockRef_(lock), sessionStore_(sessionStore) {}

    [[nodiscard]] uint64_t AllocateTokenLocked() noexcept { return nextClockToken_++; }

    [[nodiscard]] std::optional<PendingClockRequest> QueuePendingLocked(
        EndpointId endpointId, const PendingClockRequest& request) noexcept {
        std::optional<PendingClockRequest> superseded;
        if (const auto it = pending_.find(endpointId); it != pending_.end()) {
            superseded = it->second;
        }
        pending_[endpointId] = request;
        if (auto* session = sessionStore_.FindSessionLocked(endpointId)) {
            session->pendingClock = request.desiredClock;
            session->pendingReason = request.reason;
            session->hasPendingClockRequest = true;
        }
        return superseded;
    }

    void ClearLocked(EndpointId endpointId) noexcept {
        pending_.erase(endpointId);
        completed_.erase(endpointId);
    }

    [[nodiscard]] bool HasPendingLocked(EndpointId endpointId) const noexcept {
        return pending_.contains(endpointId);
    }

    [[nodiscard]] bool TryConsumePending(
        EndpointId endpointId, PendingClockRequest& outRequest) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) return false;
        IOLockLock(lock);
        const bool result = TryConsumePendingLocked(endpointId, outRequest);
        IOLockUnlock(lock);
        return result;
    }

    [[nodiscard]] bool TryTakeCompleted(
        EndpointId endpointId, uint64_t token,
        DuplexClockRequestCompletion& outCompletion) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) return false;
        IOLockLock(lock);
        auto storeIt = completed_.find(endpointId);
        if (storeIt == completed_.end()) {
            IOLockUnlock(lock);
            return false;
        }
        auto completionIt = storeIt->second.byToken.find(token);
        if (completionIt == storeIt->second.byToken.end()) {
            IOLockUnlock(lock);
            return false;
        }
        outCompletion = completionIt->second;
        storeIt->second.byToken.erase(completionIt);
        auto& order = storeIt->second.insertionOrder;
        order.erase(std::remove(order.begin(), order.end(), token), order.end());
        if (storeIt->second.byToken.empty() && order.empty()) completed_.erase(storeIt);
        IOLockUnlock(lock);
        return true;
    }

    void Complete(const DuplexClockRequestCompletion& completion,
                  EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) return;
        IOLockLock(lock);
        auto& store = completed_[endpointId];
        if (!store.byToken.contains(completion.token)) {
            store.insertionOrder.push_back(completion.token);
        }
        store.byToken[completion.token] = completion;
        while (store.insertionOrder.size() > kMaxCompletedClockRequestsPerEndpoint) {
            const uint64_t evicted = store.insertionOrder.front();
            store.insertionOrder.pop_front();
            store.byToken.erase(evicted);
        }
        if (auto* session = sessionStore_.FindSessionLocked(endpointId)) {
            session->lastClockCompletion = completion;
        }
        IOLockUnlock(lock);
        ASFW_LOG_V2(Audio,
                    "[FSM] clock token=%llu outcome=%{public}s status=0x%08x endpoint=%llu restartId=%llu gen=%u",
                    completion.token, ToString(completion.outcome),
                    static_cast<unsigned>(completion.status), endpointId.value,
                    completion.restartId, GenerationValue(completion.generation));
    }

    void FailPending(EndpointId endpointId, DuplexClockRequestOutcome outcome,
                     IOReturn status) noexcept {
        PendingClockRequest request{};
        if (!TryConsumePending(endpointId, request)) return;
        const auto session = sessionStore_.LoadSession(endpointId);
        Complete(DuplexClockRequestCompletion{
                     .token = request.token,
                     .desiredClock = request.desiredClock,
                     .reason = request.reason,
                     .outcome = outcome,
                     .status = status,
                     .restartId = session.restartId,
                     .generation = session.topologyGeneration,
                 },
                 endpointId);
    }

private:
    struct ClockCompletionStore final {
        std::unordered_map<uint64_t, DuplexClockRequestCompletion> byToken;
        std::deque<uint64_t> insertionOrder;
    };

    [[nodiscard]] bool TryConsumePendingLocked(
        EndpointId endpointId, PendingClockRequest& outRequest) noexcept {
        const auto it = pending_.find(endpointId);
        if (it == pending_.end()) return false;
        outRequest = it->second;
        pending_.erase(it);
        if (auto* session = sessionStore_.FindSessionLocked(endpointId)) {
            session->pendingClock = {};
            session->pendingReason = DuplexRestartReason::kInitialStart;
            session->hasPendingClockRequest = false;
        }
        return true;
    }

    IOLock** lockRef_;
    RestartSessionStore& sessionStore_;
    std::map<EndpointId, PendingClockRequest> pending_;
    std::map<EndpointId, ClockCompletionStore> completed_;
    uint64_t nextClockToken_{1};
    static constexpr size_t kMaxCompletedClockRequestsPerEndpoint = 32;
};

} // namespace ASFW::Audio::Duplex
