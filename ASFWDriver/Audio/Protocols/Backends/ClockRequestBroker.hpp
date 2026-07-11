// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ClockRequestBroker.hpp
//
// FW-67: clock-request token allocation, pending-request coalescing, and
// completion delivery extracted from AudioDuplexCoordinator. The broker owns the pending
// and completed per-GUID maps plus the monotonic token allocator; it deliberately BORROWS the
// coordinator's existing IOLock* rather than creating a lock or queue of its own.
//
// That borrowed lock is load-bearing. RequestClockConfig and ClearSession each make one atomic
// update across this broker, RestartSessionStore, and DuplexOperationGate. Splitting those
// updates across independently locked components would permit an observer to see a pending
// request without the corresponding session flag (or vice versa). Like the other extracted
// helpers, the broker therefore provides both:
//   * self-locking operations for ordinary producer/consumer/completion paths; and
//   * *Locked operations for the coordinator's existing multi-domain critical sections.
//
// The restart/session vocabulary carries its neutral Duplex* spelling (FW-73b); this broker
// consumes it via ../Duplex/DuplexRestartSession.hpp.

#pragma once

#include "RestartJournal.hpp"
#include "RestartSessionStore.hpp"
#include "../Duplex/DuplexRestartSession.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace ASFW::Audio::Backends {

using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::DuplexClockRequestCompletion;
using ASFW::Audio::DuplexClockRequestOutcome;
using ASFW::Audio::DuplexRestartReason;

class ClockRequestBroker {
public:
    struct PendingClockRequest {
        AudioClockConfig desiredClock{};
        DuplexRestartReason reason{DuplexRestartReason::kManualReconfigure};
        uint64_t token{0};
    };

    // `lock` points at the coordinator's IOLock* member. `sessionStore` is the coordinator's
    // lock-borrowing session store; both remain valid for the broker's lifetime.
    ClockRequestBroker(IOLock** lock, RestartSessionStore& sessionStore) noexcept
        : lockRef_(lock), sessionStore_(sessionStore) {}

    // --- lock-held API: caller already holds *lockRef_. These keep RequestClockConfig and
    // ClearSession's cross-component state mutations atomic on the coordinator lock.

    [[nodiscard]] uint64_t AllocateTokenLocked() noexcept {
        return nextClockToken_++;
    }

    // Replaces the queued request for guid, returns the displaced request when one existed, and
    // mirrors the new pending state into its restart session. This is the former contiguous
    // pendingClockRequests_ + sessions_ mutation in RequestClockConfig.
    [[nodiscard]] std::optional<PendingClockRequest> QueuePendingLocked(
        uint64_t guid,
        const PendingClockRequest& request) noexcept {
        std::optional<PendingClockRequest> superseded{};
        const auto existingIt = pendingClockRequests_.find(guid);
        if (existingIt != pendingClockRequests_.end()) {
            superseded = existingIt->second;
        }
        pendingClockRequests_[guid] = request;

        auto* session = sessionStore_.FindSessionLocked(guid);
        if (session != nullptr) {
            session->pendingClock = request.desiredClock;
            session->pendingReason = request.reason;
            session->hasPendingClockRequest = true;
        }
        return superseded;
    }

    void ClearLocked(uint64_t guid) noexcept {
        pendingClockRequests_.erase(guid);
        completedClockRequests_.erase(guid);
    }

    // --- self-locking API: exact former coordinator helper behaviour.

    [[nodiscard]] bool TryConsumePending(
        uint64_t guid,
        PendingClockRequest& outRequest) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) {
            return false;
        }

        IOLockLock(lock);
        const bool consumed = TryConsumePendingLocked(guid, outRequest);
        IOLockUnlock(lock);
        return consumed;
    }

    [[nodiscard]] bool TryTakeCompleted(
        uint64_t guid,
        uint64_t token,
        DuplexClockRequestCompletion& outCompletion) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) {
            return false;
        }

        IOLockLock(lock);
        auto storeIt = completedClockRequests_.find(guid);
        if (storeIt == completedClockRequests_.end()) {
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
        if (storeIt->second.byToken.empty() && order.empty()) {
            completedClockRequests_.erase(storeIt);
        }
        IOLockUnlock(lock);
        return true;
    }

    void Complete(const DuplexClockRequestCompletion& completion, uint64_t guid) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) {
            return;
        }

        IOLockLock(lock);
        auto& store = completedClockRequests_[guid];
        if (store.byToken.find(completion.token) == store.byToken.end()) {
            store.insertionOrder.push_back(completion.token);
        }
        store.byToken[completion.token] = completion;
        while (store.insertionOrder.size() > kMaxCompletedClockRequestsPerGuid) {
            const uint64_t evictedToken = store.insertionOrder.front();
            store.insertionOrder.pop_front();
            store.byToken.erase(evictedToken);
        }

        auto* session = sessionStore_.FindSessionLocked(guid);
        if (session != nullptr) {
            session->lastClockCompletion = completion;
        }
        IOLockUnlock(lock);

        // Preserve the established [FSM] clock-completion trace verbatim; runtime consumers use
        // these fields to correlate the synchronous clock request with restart progress.
        ASFW_LOG_V2(DICE,
                    "[FSM] clock token=%llu outcome=%{public}s status=0x%08x guid=0x%llx restartId=%llu gen=%u",
                    completion.token,
                    ToString(completion.outcome),
                    static_cast<unsigned>(completion.status),
                    guid,
                    completion.restartId,
                    GenerationValue(completion.generation));
    }

    void FailPending(
        uint64_t guid,
        DuplexClockRequestOutcome outcome,
        IOReturn status) noexcept {
        PendingClockRequest request{};
        if (!TryConsumePending(guid, request)) {
            return;
        }

        // Keep the original lock boundaries: consuming the pending entry, reading the restart
        // session, and publishing the completion are three separate operations. In particular,
        // do not fold this into one new critical section; stop/recovery races intentionally see
        // the same interleavings as before the extraction.
        const auto session = sessionStore_.LoadSession(guid);
        Complete(DuplexClockRequestCompletion{
                     .token = request.token,
                     .desiredClock = request.desiredClock,
                     .reason = request.reason,
                     .outcome = outcome,
                     .status = status,
                     .restartId = session.restartId,
                     .generation = session.topologyGeneration,
                 },
                 guid);
    }

private:
    struct ClockCompletionStore {
        std::unordered_map<uint64_t, DuplexClockRequestCompletion> byToken{};
        std::deque<uint64_t> insertionOrder{};
    };

    [[nodiscard]] bool TryConsumePendingLocked(
        uint64_t guid,
        PendingClockRequest& outRequest) noexcept {
        const auto it = pendingClockRequests_.find(guid);
        if (it == pendingClockRequests_.end()) {
            return false;
        }

        outRequest = it->second;
        pendingClockRequests_.erase(it);
        auto* session = sessionStore_.FindSessionLocked(guid);
        if (session != nullptr) {
            session->pendingClock = {};
            session->pendingReason = DuplexRestartReason::kInitialStart;
            session->hasPendingClockRequest = false;
        }
        return true;
    }

    IOLock** lockRef_;  // borrowed: &coordinator.lock_ (not owned; never allocated/freed here)
    RestartSessionStore& sessionStore_;
    std::unordered_map<uint64_t, PendingClockRequest> pendingClockRequests_{};
    std::unordered_map<uint64_t, ClockCompletionStore> completedClockRequests_{};
    uint64_t nextClockToken_{1};

    static constexpr size_t kMaxCompletedClockRequestsPerGuid = 32;
};

} // namespace ASFW::Audio::Backends
