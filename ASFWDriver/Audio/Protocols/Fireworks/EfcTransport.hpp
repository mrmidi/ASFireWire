// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// EfcTransport.hpp — Asynchronous EFC transaction runner for one Fireworks unit.
//
// One command in flight at a time (Fireworks firmware serialises anyway);
// further Submit() calls queue in order. A command is a block write to the
// device's command register; the answer arrives through EfcResponseMailbox as
// a block write from the device into the host's response window and is matched
// by sequence number (device echoes seqnum + 1). Timeout/retry policy follows
// Linux fireworks_transaction.c: 125 ms per attempt, three attempts.
//
// Concurrency: callers reach this object from several queues — the Default
// queue (discovery / Initialize), the audio backend's dice queue (PrepareDuplex,
// ApplyClockConfig, health), the nub queue (UpdateRuntimeContext) — while
// responses land on the AR dispatch path and timeouts on the scheduler. All
// transaction state is therefore guarded by an IOLock, exactly like
// FCPTransport and CMPClient; completions are always invoked with the lock
// released. Bus and timer callbacks capture a weak lifetime token so a late
// completion after teardown is a no-op instead of a use-after-free.

#pragma once

#include "EfcProtocol.hpp"

#include "../../../Async/AsyncTypes.hpp"
#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "EfcResponseMailbox.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Audio::Fireworks {

// Owned through a shared_ptr so the response mailbox can hold a weak reference
// to it. Constructing one on the stack compiles and then silently misbehaves:
// weak_from_this() returns an EMPTY weak_ptr for an object no shared_ptr owns,
// so every bus and timer callback fails its upgrade and quietly does nothing. That is what makes teardown safe without waiting: a response already
// being delivered holds the transport alive for the duration of the callback,
// and destruction happens when that reference drops. See EfcResponseMailbox.hpp.
class EfcTransport final : public IEfcResponseObserver,
                           public std::enable_shared_from_this<EfcTransport> {
public:
    // status: kIOReturnSuccess with a validated response; kIOReturnError when the
    // device answered with a non-OK EFC status (response.header.status says
    // which); kIOReturnTimeout after all attempts; kIOReturnAborted on cancel;
    // kIOReturnNotResponding when the bus generation moved underneath the write.
    using Completion = std::function<void(IOReturn status, const Efc::Response& response)>;

    EfcTransport(Async::IFireWireBusOps& busOps,
                 Async::IFireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler* timerScheduler) noexcept;
    ~EfcTransport();

    EfcTransport(const EfcTransport&) = delete;
    EfcTransport& operator=(const EfcTransport&) = delete;

    void SetRoute(const Discovery::DeviceRouteToken& route) noexcept;
    [[nodiscard]] Discovery::DeviceRouteToken Route() const noexcept;

    void Submit(Efc::Category category,
                uint32_t command,
                std::span<const uint32_t> params,
                Completion completion);

    // Fail everything queued or in flight with `status` (kIOReturnAborted on
    // teardown). Cancels the outstanding bus write and response timeout.
    void CancelAll(IOReturn status) noexcept;

    // Mailbox entry point: true when the frame belonged to this transport.
    [[nodiscard]] bool OnResponse(uint16_t sourceID, std::span<const uint8_t> payload);

    [[nodiscard]] bool HasInflight() const noexcept;
    [[nodiscard]] size_t QueuedCount() const noexcept;
    [[nodiscard]] uint32_t InflightSeqnum() const noexcept;
    [[nodiscard]] uint32_t InflightAttempts() const noexcept;
    [[nodiscard]] bool IsRegistered() const noexcept { return registered_; }

    /// Register with the response mailbox. Must be called by the owner AFTER the
    /// shared_ptr exists -- shared_from_this() is unusable during construction,
    /// and registering there would compile and then never deliver anything.
    [[nodiscard]] bool RegisterForResponses() noexcept;

    /// Mailbox delivery. Held alive by the caller for the duration of this call.
    [[nodiscard]] bool OnEfcResponse(uint16_t sourceID,
                                     std::span<const uint8_t> payload) override;

private:
    struct Pending {
        Efc::Category category{Efc::Category::kHwInfo};
        uint32_t command{0};
        uint32_t seqnum{0};
        uint32_t attempts{0};
        uint64_t serial{0};
        std::vector<uint8_t> frame{};
        Completion completion{};
        Scheduling::TimerToken timeout{Scheduling::kInvalidTimerToken};
        Async::AsyncHandle writeHandle{};
        bool writeOutstanding{false};
    };

    // Everything Send() needs, copied out under the lock so the bus call runs
    // with the lock released.
    struct SendSnapshot {
        uint64_t serial{0};
        uint32_t seqnum{0};
        uint32_t attempt{0};
        Efc::Category category{Efc::Category::kHwInfo};
        uint32_t command{0};
        uint8_t node{0};
        std::vector<uint8_t> frame{};
    };

    static bool ObserverThunk(void* context, uint16_t sourceID, std::span<const uint8_t> payload);

    void StartNext();
    [[nodiscard]] SendSnapshot PrepareSendLocked() noexcept;
    void Send(const SendSnapshot& snapshot);
    void ArmTimeout(uint64_t serial);
    void OnWriteCompleted(uint64_t serial, Async::AsyncStatus status);
    void OnTimeout(uint64_t serial);
    void Complete(std::unique_ptr<Pending> done, IOReturn status, const Efc::Response* response);

    Async::IFireWireBusOps& busOps_;
    Async::IFireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    IOLock* lock_{nullptr};

    // ---- guarded by lock_ ----
    Discovery::DeviceRouteToken route_{};
    uint32_t nextSeqnum_{Efc::kSeqnumFirst};
    uint64_t nextSerial_{0};
    std::vector<std::unique_ptr<Pending>> queue_{};
    std::unique_ptr<Pending> inflight_{};

    bool registered_{false};
};

} // namespace ASFW::Audio::Fireworks
