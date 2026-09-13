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
// Callback context matches the rest of the AV/C+CMP stack: completions run on
// the async engine's completion path (bus callbacks, AR dispatch, timers), so
// no locking is done here — same assumption the BeBoB base makes.

#pragma once

#include "EfcProtocol.hpp"

#include "../../../Async/AsyncTypes.hpp"
#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"

#include <DriverKit/IOReturn.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ASFW::Audio::Fireworks {

class EfcTransport final {
public:
    // status: kIOReturnSuccess with a validated response; kIOReturnError when the
    // device answered with a non-OK EFC status (response.header.status says
    // which); kIOReturnTimeout after all attempts; kIOReturnAborted on cancel.
    using Completion = std::function<void(IOReturn status, const Efc::Response& response)>;

    EfcTransport(Async::IFireWireBusOps& busOps,
                 Async::IFireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler* timerScheduler) noexcept;
    ~EfcTransport();

    EfcTransport(const EfcTransport&) = delete;
    EfcTransport& operator=(const EfcTransport&) = delete;

    void SetRoute(const Discovery::DeviceRouteToken& route) noexcept;
    [[nodiscard]] const Discovery::DeviceRouteToken& Route() const noexcept { return route_; }

    void Submit(Efc::Category category,
                uint32_t command,
                std::span<const uint32_t> params,
                Completion completion);

    // Fail everything queued or in flight with `status` (kIOReturnAborted on teardown).
    void CancelAll(IOReturn status) noexcept;

    // Mailbox entry point: true when the frame belonged to this transport.
    [[nodiscard]] bool OnResponse(uint16_t sourceID, std::span<const uint8_t> payload);

    [[nodiscard]] bool HasInflight() const noexcept { return inflight_ != nullptr; }
    [[nodiscard]] size_t QueuedCount() const noexcept { return queue_.size(); }
    [[nodiscard]] uint32_t InflightSeqnum() const noexcept;
    [[nodiscard]] uint32_t InflightAttempts() const noexcept;
    [[nodiscard]] bool IsRegistered() const noexcept { return registered_; }

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
    };

    static bool ObserverThunk(void* context, uint16_t sourceID, std::span<const uint8_t> payload);

    void StartNext();
    void Send();
    void OnWriteCompleted(uint64_t serial, Async::AsyncStatus status);
    void OnTimeout(uint64_t serial);
    void FinishInflight(IOReturn status, const Efc::Response* response);
    [[nodiscard]] bool SourceMatches(uint16_t sourceID) const noexcept;
    void CancelTimeout(Pending& pending) noexcept;

    Async::IFireWireBusOps& busOps_;
    Async::IFireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    Discovery::DeviceRouteToken route_{};
    uint32_t nextSeqnum_{Efc::kSeqnumFirst};
    uint64_t nextSerial_{0};
    std::vector<std::unique_ptr<Pending>> queue_{};
    std::unique_ptr<Pending> inflight_{};
    bool registered_{false};
};

} // namespace ASFW::Audio::Fireworks
