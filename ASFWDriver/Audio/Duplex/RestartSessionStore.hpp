// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DuplexControlTypes.hpp"

#include <DriverKit/IOLib.h>

#include <map>
#include <optional>

namespace ASFW::Audio::Duplex {

class RestartSessionStore final {
public:
    using EndpointId = Devices::AudioEndpointId;

    explicit RestartSessionStore(IOLock** lock) noexcept : lockRef_(lock) {}

    [[nodiscard]] DuplexRestartSession LoadSession(EndpointId endpointId) const noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return DuplexRestartSession{.endpointId = endpointId};
        IOLockLock(lock);
        const auto it = sessions_.find(endpointId);
        auto session = it != sessions_.end()
                           ? it->second
                           : DuplexRestartSession{.endpointId = endpointId};
        IOLockUnlock(lock);
        return session;
    }

    void StoreSession(const DuplexRestartSession& session) noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !session.endpointId) return;
        IOLockLock(lock);
        sessions_[session.endpointId] = session;
        IOLockUnlock(lock);
    }

    [[nodiscard]] std::optional<DuplexRestartSession>
    GetSession(EndpointId endpointId) const noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock || !endpointId) return std::nullopt;
        IOLockLock(lock);
        const auto it = sessions_.find(endpointId);
        auto result = it != sessions_.end() ? std::optional{it->second} : std::nullopt;
        IOLockUnlock(lock);
        return result;
    }

    [[nodiscard]] uint64_t AllocateRestartId() noexcept {
        IOLock* const lock = *lockRef_;
        if (!lock) return 0;
        IOLockLock(lock);
        const uint64_t value = nextRestartId_++;
        IOLockUnlock(lock);
        return value;
    }

    [[nodiscard]] DuplexRestartSession* FindSessionLocked(EndpointId endpointId) noexcept {
        const auto it = sessions_.find(endpointId);
        return it != sessions_.end() ? &it->second : nullptr;
    }
    [[nodiscard]] const DuplexRestartSession*
    FindSessionLocked(EndpointId endpointId) const noexcept {
        const auto it = sessions_.find(endpointId);
        return it != sessions_.end() ? &it->second : nullptr;
    }
    void EraseSessionLocked(EndpointId endpointId) noexcept { sessions_.erase(endpointId); }

private:
    IOLock** lockRef_;
    std::map<EndpointId, DuplexRestartSession> sessions_;
    uint64_t nextRestartId_{1};
};

} // namespace ASFW::Audio::Duplex
