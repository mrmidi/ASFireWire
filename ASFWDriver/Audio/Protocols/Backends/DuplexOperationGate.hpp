// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexOperationGate.hpp
//
// FW-68 (Step 4 of FW-64): per-GUID duplex operation gating extracted from
// DiceDuplexRestartCoordinator. Owns the two per-GUID sets that gate concurrent duplex
// operations:
//   * activeGuids_        - a GUID currently owns the duplex run body (claim/release).
//   * stopRequestedGuids_ - a GUID has a pending per-stream stop intent.
//
// Behaviour-preserving extraction: the gate BORROWS the coordinator's existing IOLock* rather
// than owning its own. That is deliberate — those sets are read/mutated inside multi-domain
// single-lock critical sections in the coordinator (RequestClockConfig and ClearSession, which
// also touch the clock-request maps and the session store under one hold). Owning a separate
// lock would split those atomic sections; borrowing the coordinator's lock keeps every
// IOLockLock/IOLockUnlock pairing exactly as it was. The gate therefore exposes two tiers:
//   * self-locking methods (Acquire/Release/RequestStop/ClearStop/IsStopRequested) that
//     reproduce the former coordinator members byte-for-byte, for callers that do not already
//     hold the lock (StartStreaming/StopStreaming/RecoverStreaming and their reads); and
//   * *Locked variants that assume the caller already holds the lock, for the two multi-domain
//     critical sections that must stay a single uninterrupted hold.
//
// This per-GUID stop intent is distinct from the global FW-60/61 teardown cancel token
// (cancel_/TeardownRequested()), which aborts everything for service teardown and stays on the
// coordinator.

#pragma once

#include <DriverKit/IOLib.h>

#include <cstdint>
#include <unordered_set>

namespace ASFW::Audio::Backends {

class DuplexOperationGate {
public:
    // `lock` points at the coordinator's IOLock* member. The pointee is allocated in the
    // coordinator's constructor body (after this gate is constructed) and stays stable for the
    // coordinator's lifetime, so the gate reads it fresh on every call.
    explicit DuplexOperationGate(IOLock** lock) noexcept : lockRef_(lock) {}

    // --- self-locking API: byte-for-byte reproductions of the former coordinator members
    //     TryAcquireGuid / ReleaseGuid / RequestStopIntent / ClearStopIntent / IsStopRequested.

    [[nodiscard]] bool Acquire(uint64_t guid) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) {
            return false;
        }
        IOLockLock(lock);
        const auto [_, inserted] = activeGuids_.insert(guid);
        IOLockUnlock(lock);
        return inserted;
    }

    void Release(uint64_t guid) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) {
            return;
        }
        IOLockLock(lock);
        activeGuids_.erase(guid);
        IOLockUnlock(lock);
    }

    void RequestStop(uint64_t guid) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || guid == 0) {
            return;
        }
        IOLockLock(lock);
        stopRequestedGuids_.insert(guid);
        IOLockUnlock(lock);
    }

    void ClearStop(uint64_t guid) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || guid == 0) {
            return;
        }
        IOLockLock(lock);
        stopRequestedGuids_.erase(guid);
        IOLockUnlock(lock);
    }

    [[nodiscard]] bool IsStopRequested(uint64_t guid) const noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || guid == 0) {
            return false;
        }
        IOLockLock(lock);
        const bool requested = stopRequestedGuids_.find(guid) != stopRequestedGuids_.end();
        IOLockUnlock(lock);
        return requested;
    }

    // --- lock-held variants: the caller already holds *lockRef_. Do only the set operation, so
    //     the coordinator's multi-domain critical sections stay one uninterrupted hold. These
    //     match the former inline accesses in RequestClockConfig / ClearSession exactly.

    [[nodiscard]] bool IsActiveLocked(uint64_t guid) const noexcept {
        return activeGuids_.find(guid) != activeGuids_.end();
    }

    [[nodiscard]] bool IsStopRequestedLocked(uint64_t guid) const noexcept {
        return stopRequestedGuids_.find(guid) != stopRequestedGuids_.end();
    }

    void AcquireLocked(uint64_t guid) noexcept {
        activeGuids_.insert(guid);
    }

    void ReleaseLocked(uint64_t guid) noexcept {
        activeGuids_.erase(guid);
    }

    void ClearStopLocked(uint64_t guid) noexcept {
        stopRequestedGuids_.erase(guid);
    }

private:
    IOLock** lockRef_;  // borrowed: &coordinator.lock_ (not owned; never allocated/freed here)
    std::unordered_set<uint64_t> activeGuids_{};
    std::unordered_set<uint64_t> stopRequestedGuids_{};
};

} // namespace ASFW::Audio::Backends
