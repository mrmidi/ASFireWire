// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBBootloaderClient.hpp — the only code in this driver that transacts with a
// BeBoB bootloader.
//
// It exposes exactly three operations and no parameterised write. Every write
// goes through IsPermittedBootloaderWrite, so a caller cannot reach the
// destructive neighbours of the cue command even by constructing bytes by hand:
// in that byte field 0x0a rewrites the device GUID and 0x10 the hardware ID,
// both permanently in flash.
//
// The client owns transport only. All sequencing lives in
// BeBoBBootloaderPreparation, which is a pure state machine, so the ordering
// rules stay testable without hardware that a mistake would cost a power cycle.

#pragma once

#include "BeBoBBootloaderCue.hpp"
#include "BeBoBBootloaderPreparation.hpp"

#include "../../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../../Discovery/DeviceRouteToken.hpp"
#include "../../../../Scheduling/ITimerScheduler.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace ASFW::Audio::Families::BeBoB::Bootloader {

/// Reported to the state machine as one of its events.
using PreparationEventCallback = std::function<void(PreparationEvent)>;

class BeBoBBootloaderClient final
    : public std::enable_shared_from_this<BeBoBBootloaderClient> {
public:
    BeBoBBootloaderClient(ASFW::Async::IFireWireBusOps& busOps,
                          ASFW::Scheduling::ITimerScheduler& scheduler,
                          Discovery::DeviceRouteToken route) noexcept;

    /// Read the BootROM info block. Success carries the parsed block, which is
    /// the only way to reach a cue.
    void ReadInfo(PreparationEventCallback completion);

    /// Write the cue, once. Refuses at the choke point if the bytes or the
    /// address are not exactly right, and reports that as a write failure
    /// rather than putting anything on the bus.
    void SendCue(const BeBoBBootloaderCue& cue, PreparationEventCallback completion);

    /// Read the bootloader response after the configured delay. The delay is a
    /// timer, never IOSleep: this runs on the single dispatch queue.
    void ReadResponse(uint32_t delayMs, PreparationEventCallback completion);

    /// Drop pending work. Callbacks already queued may still arrive and are
    /// safe: the state machine ignores events that cannot occur in its state.
    void Cancel() noexcept;

    [[nodiscard]] const Discovery::DeviceRouteToken& Route() const noexcept {
        return route_;
    }

private:
    [[nodiscard]] ASFW::Async::FWAddress AddressFor(uint32_t addressLo) const noexcept;

    /// The route's node id narrowed to the operational form, or nullopt when the
    /// route is not usable. Refusing here keeps an invalid route from being cast
    /// into a plausible-looking node.
    [[nodiscard]] std::optional<ASFW::FW::NodeId> OperationalNode() const noexcept;

    ASFW::Async::IFireWireBusOps& busOps_;
    ASFW::Scheduling::ITimerScheduler& scheduler_;
    Discovery::DeviceRouteToken route_{};
    ASFW::Scheduling::TimerToken pendingTimer_{
        ASFW::Scheduling::kInvalidTimerToken};
    bool cancelled_{false};
};

} // namespace ASFW::Audio::Families::BeBoB::Bootloader
