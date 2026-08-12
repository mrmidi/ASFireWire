// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../Devices/AudioIdentity.hpp"

#include <DriverKit/IOLib.h>

#include <unordered_set>

namespace ASFW::Audio::Duplex {

class DuplexOperationGate final {
public:
    using EndpointId = Devices::AudioEndpointId;

    explicit DuplexOperationGate(IOLock** lock) noexcept : lockRef_(lock) {}

    [[nodiscard]] bool Acquire(EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return false;
        IOLockLock(lock);
        const auto [_, inserted] = active_.insert(endpointId);
        IOLockUnlock(lock);
        return inserted;
    }

    void Release(EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) return;
        IOLockLock(lock);
        active_.erase(endpointId);
        IOLockUnlock(lock);
    }

    void RequestStop(EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return;
        IOLockLock(lock);
        stopRequested_.insert(endpointId);
        IOLockUnlock(lock);
    }

    void ClearStop(EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return;
        IOLockLock(lock);
        stopRequested_.erase(endpointId);
        IOLockUnlock(lock);
    }

    void MarkRemoteDeviceLost(EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return;
        IOLockLock(lock);
        remoteLost_.insert(endpointId);
        IOLockUnlock(lock);
    }

    void AcknowledgeDevicePresent(EndpointId endpointId) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return;
        IOLockLock(lock);
        remoteLost_.erase(endpointId);
        IOLockUnlock(lock);
    }

    [[nodiscard]] bool IsStopRequested(EndpointId endpointId) const noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return false;
        IOLockLock(lock);
        const bool requested = stopRequested_.contains(endpointId) ||
                               remoteLost_.contains(endpointId);
        IOLockUnlock(lock);
        return requested;
    }

    [[nodiscard]] bool IsActiveLocked(EndpointId endpointId) const noexcept {
        return active_.contains(endpointId);
    }
    [[nodiscard]] bool IsStopRequestedLocked(EndpointId endpointId) const noexcept {
        return stopRequested_.contains(endpointId);
    }
    void AcquireLocked(EndpointId endpointId) noexcept {
        if (endpointId) active_.insert(endpointId);
    }
    void ReleaseLocked(EndpointId endpointId) noexcept { active_.erase(endpointId); }
    void ClearStopLocked(EndpointId endpointId) noexcept { stopRequested_.erase(endpointId); }

private:
    IOLock** lockRef_;
    std::unordered_set<EndpointId, Devices::AudioEndpointIdHash> active_;
    std::unordered_set<EndpointId, Devices::AudioEndpointIdHash> stopRequested_;
    std::unordered_set<EndpointId, Devices::AudioEndpointIdHash> remoteLost_;
};

} // namespace ASFW::Audio::Duplex
