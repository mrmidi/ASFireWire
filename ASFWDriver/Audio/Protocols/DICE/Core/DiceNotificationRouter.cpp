// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceNotificationRouter.cpp - Attribute each DICE notification to its device.

#include "DiceNotificationRouter.hpp"

#include "../../../../Discovery/DeviceRegistry.hpp"

namespace ASFW::Audio::DICE {

namespace {
// IEEE 1394 node ID: bus in bits 15:6, physical node in bits 5:0.
constexpr uint16_t kPhysicalNodeMask = 0x003F;
} // namespace

DiceNotificationRouter::DiceNotificationRouter(Discovery::DeviceRegistry& registry) noexcept
    : registry_(registry), lock_(IOLockAlloc()) {}

DiceNotificationRouter::~DiceNotificationRouter() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void DiceNotificationRouter::Register(uint64_t guid, DiceNotificationMailbox& mailbox) noexcept {
    if (!lock_ || guid == 0) {
        return;
    }
    IOLockLock(lock_);
    mailboxes_[guid] = &mailbox;
    IOLockUnlock(lock_);
}

void DiceNotificationRouter::Unregister(uint64_t guid, const DiceNotificationMailbox& mailbox) noexcept {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    // A replacement protocol may already have registered for the same device.
    if (const auto it = mailboxes_.find(guid); it != mailboxes_.end() && it->second == &mailbox) {
        mailboxes_.erase(it);
    }
    IOLockUnlock(lock_);
}

void DiceNotificationRouter::SetObserver(void* context, Observer observer) noexcept {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    observerContext_ = context;
    observer_ = observer;
    IOLockUnlock(lock_);
}

void DiceNotificationRouter::ClearObserver(void* context) noexcept {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    if (observerContext_ == context) {
        observerContext_ = nullptr;
        observer_ = nullptr;
    }
    IOLockUnlock(lock_);
}

uint64_t DiceNotificationRouter::Deliver(uint32_t generation, uint16_t sourceId, uint32_t bits) noexcept {
    const auto record = registry_.SnapshotByNode(Discovery::Generation{generation},
                                                 static_cast<uint8_t>(sourceId & kPhysicalNodeMask));
    if (!record.has_value() || !lock_) {
        return 0;
    }
    const uint64_t guid = record->guid;
    IOLockLock(lock_);
    if (const auto it = mailboxes_.find(guid); it != mailboxes_.end()) {
        it->second->Publish(bits);
    }
    if (observer_) {
        observer_(observerContext_, guid, bits);
    }
    IOLockUnlock(lock_);
    return guid;
}

} // namespace ASFW::Audio::DICE
