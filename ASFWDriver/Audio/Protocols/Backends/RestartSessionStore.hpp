// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RestartSessionStore.hpp
//
// FW-69b (Step 5 of FW-64, store half): the per-GUID restart-session persistence + restart-id
// allocator extracted from AudioDuplexCoordinator. Owns the sessions_ map and
// nextRestartId_.
//
// Behaviour-preserving extraction, same shape as the FW-68 DuplexOperationGate: the store
// BORROWS the coordinator's existing IOLock* (via a pointer to it) rather than owning one,
// because sessions_ is read/mutated inside multi-domain single-lock critical sections that also
// touch the clock-request maps and the operation gate (RequestClockConfig, ClearSession,
// TryConsumePendingClockRequest, CompleteClockRequest). Owning a separate lock would split those
// atomic sections. So the store exposes two tiers:
//   * self-locking methods (LoadSession/StoreSession/GetSession/AllocateRestartId) that reproduce
//     the former coordinator members byte-for-byte, for callers that do not already hold the lock;
//   * *Locked accessors (FindSessionLocked/EraseSessionLocked) that assume the caller already
//     holds the lock, for the multi-domain sections that must stay one uninterrupted hold.
//
// Names keep their Dice* prefix for now (neutral rename is FW-73b). The FSM journal is the
// separate FW-69a component (RestartJournal.hpp).

#pragma once

#include "../DICE/Core/DICERestartSession.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace ASFW::Audio::Backends {

using ASFW::Audio::DICE::DiceRestartSession;

class RestartSessionStore {
public:
    // `lock` points at the coordinator's IOLock* member (allocated in its constructor body after
    // this store is constructed, then stable for its lifetime); the store reads it fresh each call.
    explicit RestartSessionStore(IOLock** lock) noexcept : lockRef_(lock) {}

    // --- self-locking API: byte-for-byte reproductions of the former coordinator members
    //     LoadSession / StoreSession / GetSession / AllocateRestartId.

    [[nodiscard]] DiceRestartSession LoadSession(uint64_t guid) const noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || guid == 0) {
            return DiceRestartSession{.guid = guid};
        }
        IOLockLock(lock);
        const auto it = sessions_.find(guid);
        DiceRestartSession session = (it != sessions_.end())
            ? it->second
            : DiceRestartSession{.guid = guid};
        IOLockUnlock(lock);
        return session;
    }

    void StoreSession(const DiceRestartSession& session) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || session.guid == 0) {
            return;
        }
        IOLockLock(lock);
        sessions_[session.guid] = session;
        IOLockUnlock(lock);
    }

    [[nodiscard]] std::optional<DiceRestartSession> GetSession(uint64_t guid) const noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || guid == 0) {
            return std::nullopt;
        }
        IOLockLock(lock);
        const auto it = sessions_.find(guid);
        const auto session = (it != sessions_.end())
            ? std::optional<DiceRestartSession>(it->second)
            : std::nullopt;
        IOLockUnlock(lock);
        return session;
    }

    [[nodiscard]] uint64_t AllocateRestartId() noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) {
            return 0;
        }
        IOLockLock(lock);
        const uint64_t restartId = nextRestartId_++;
        IOLockUnlock(lock);
        return restartId;
    }

    // --- lock-held accessors: the caller already holds *lockRef_. Do only the map op, so the
    //     coordinator's multi-domain critical sections stay one uninterrupted hold. These match
    //     the former inline `sessions_.find(guid)` / `sessions_.erase(guid)` uses exactly.

    [[nodiscard]] DiceRestartSession* FindSessionLocked(uint64_t guid) noexcept {
        const auto it = sessions_.find(guid);
        return it != sessions_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] const DiceRestartSession* FindSessionLocked(uint64_t guid) const noexcept {
        const auto it = sessions_.find(guid);
        return it != sessions_.end() ? &it->second : nullptr;
    }

    void EraseSessionLocked(uint64_t guid) noexcept {
        sessions_.erase(guid);
    }

private:
    IOLock** lockRef_;  // borrowed: &coordinator.lock_ (not owned; never allocated/freed here)
    std::unordered_map<uint64_t, DiceRestartSession> sessions_{};
    uint64_t nextRestartId_{1};
};

} // namespace ASFW::Audio::Backends
