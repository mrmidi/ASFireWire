// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeviceNotificationDispatch.hpp - Hand each notification to its device's backend.
//
// DiceNotificationRouter names the device a notification came from, but not
// what its bits mean. Every device that writes to the host's notification
// address arrives there, DICE or not: the MOTU 828 Mk3 writes its status word
// to the same address, and in that word 0x02 is "clock locked" where DICE has
// TX_CFG_CHG. Read as DICE, a healthy Mk3 asks for a configuration-change
// restart on every report. So the router's one observer is this dispatcher,
// which picks the backend with the same catalog decision that picks it for
// streaming (ChooseAudioBackend), and that backend decodes the bits.
//
// Threading: the observer runs on the Default queue under the router's lock.
// After Close() returns no call is running or will start.

#pragma once

#include "../Protocols/Backends/IAudioBackend.hpp"
#include "../Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../Discovery/DeviceRegistry.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace ASFW::Audio {

/// The backend family that drives `guid` on its current route, or nullopt when
/// the device has no current audio policy.
[[nodiscard]] inline std::optional<AudioBackendKind>
CurrentAudioBackendKind(const Discovery::DeviceRegistry& registry, uint64_t guid) noexcept {
    if (guid == 0) {
        return std::nullopt;
    }
    const auto record = registry.SnapshotByGuid(guid);
    if (!record.has_value()) {
        return std::nullopt;
    }
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy || !registry.IsCurrent(policy->route)) {
        return std::nullopt;
    }
    return ChooseAudioBackend(policy->plan);
}

class DeviceNotificationDispatch final {
public:
    using BackendResolver = std::function<IAudioBackend*(uint64_t guid)>;

    DeviceNotificationDispatch(DICE::DiceNotificationRouter& router,
                               BackendResolver resolve) noexcept
        : router_(router), resolve_(std::move(resolve)) {
        router_.SetObserver(this, &DeviceNotificationDispatch::Deliver);
    }
    ~DeviceNotificationDispatch() noexcept { Close(); }

    DeviceNotificationDispatch(const DeviceNotificationDispatch&) = delete;
    DeviceNotificationDispatch& operator=(const DeviceNotificationDispatch&) = delete;

    /// Stop dispatching. Idempotent; call before the backends tear down.
    void Close() noexcept { router_.ClearObserver(this); }

private:
    static void Deliver(void* context, uint64_t guid, uint32_t bits) noexcept {
        auto* self = static_cast<DeviceNotificationDispatch*>(context);
        if (self == nullptr || !self->resolve_) {
            return;
        }
        if (auto* backend = self->resolve_(guid)) {
            backend->HandleDeviceNotification(guid, bits);
        }
    }

    DICE::DiceNotificationRouter& router_;
    BackendResolver resolve_;
};

} // namespace ASFW::Audio
