// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceNotificationRouter.hpp - Attribute each DICE notification to its device.
//
// Every DICE device we own writes its notifications to the same host address
// (kNotificationHandlerOffset). The write's source node, looked up in the
// device registry for the generation it arrived in, names the device; the
// bits go to that device's mailbox and to the observer. Linux gives each
// device its own address instead (dice-transaction.c:309-321); attributing by
// source keeps the owner value, and so the wire, unchanged.
//
// Threading: Deliver runs on the Default queue (local request dispatch).
// Register/Unregister run wherever DICE protocols are created and destroyed.

#pragma once

#include "DiceNotificationMailbox.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>
#include <unordered_map>

namespace ASFW::Discovery {
class DeviceRegistry;
}

namespace ASFW::Audio::DICE {

class DiceNotificationRouter final {
public:
    using Observer = void (*)(void* context, uint64_t guid, uint32_t bits);

    explicit DiceNotificationRouter(Discovery::DeviceRegistry& registry) noexcept;
    ~DiceNotificationRouter() noexcept;

    DiceNotificationRouter(const DiceNotificationRouter&) = delete;
    DiceNotificationRouter& operator=(const DiceNotificationRouter&) = delete;

    // The mailbox of the protocol driving `guid`. Unregister before the
    // mailbox is destroyed.
    void Register(uint64_t guid, DiceNotificationMailbox& mailbox) noexcept;
    void Unregister(uint64_t guid, const DiceNotificationMailbox& mailbox) noexcept;

    // Called with each attributed notification, under the router's lock:
    // after ClearObserver returns, no call is running or will start.
    void SetObserver(void* context, Observer observer) noexcept;
    void ClearObserver(void* context) noexcept;

    // A notification quadlet written by node `sourceId` in `generation`.
    // Returns the device's GUID, or 0 when no known device has that node.
    uint64_t Deliver(uint32_t generation, uint16_t sourceId, uint32_t bits) noexcept;

private:
    Discovery::DeviceRegistry& registry_;
    IOLock* lock_{nullptr};
    std::unordered_map<uint64_t, DiceNotificationMailbox*> mailboxes_;
    void* observerContext_{nullptr};
    Observer observer_{nullptr};
};

} // namespace ASFW::Audio::DICE
