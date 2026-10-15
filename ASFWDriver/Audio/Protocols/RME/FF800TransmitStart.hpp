// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "FirefaceClockStatus.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"
#include "../../../Logging/Logging.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ASFW::Audio::RME {

// One FF800 assignment, owning its fixed route and command words. All state
// transitions run on the scheduler's queue; no transition sleeps or awaits IO.
// Cancellation may come from another queue. The lock protects completion,
// timer/handle ownership only, and is never held over IO or scheduler calls.
class FF800TransmitStart final : public std::enable_shared_from_this<FF800TransmitStart> {
public:
    using Completion = std::function<void(IOReturn, uint8_t)>;
    using Task = std::function<void(FF800TransmitStart&)>;

    [[nodiscard]] static std::shared_ptr<FF800TransmitStart> Create(
        const Protocols::Ports::ProtocolRegisterIO& io, Scheduling::ITimerScheduler& scheduler,
        std::array<uint32_t, 3> words, const std::atomic<bool>* cancel = nullptr) {
        return std::shared_ptr<FF800TransmitStart>(new FF800TransmitStart(io, scheduler, words, cancel));
    }
    ~FF800TransmitStart() { Finish(kIOReturnAborted); if (lock_) IOLockFree(lock_); }

    void Begin(Completion completion) {
        bool finished;
        IOReturn status;
        uint8_t channel;
        {
            Guard guard(lock_);
            finished = done_.load(std::memory_order_acquire);
            status = status_; channel = channel_;
            if (!finished) completion_ = std::move(completion);
        }
        if (finished) { if (completion) completion(status, channel); return; }
        if (!lock_) { Finish(kIOReturnNoResources); return; }
        Schedule(0, [](auto& self) { self.CheckCancellation(); self.WriteFrequency(); });
    }
    void Cancel() { Finish(kIOReturnAborted); }

private:
    static constexpr uint64_t kMillisecond = 1'000'000;
    static constexpr uint64_t kRegisterTimeout = 1500 * kMillisecond;
    struct Guard {
        explicit Guard(IOLock* lock) : lock_(lock) { if (lock_) IOLockLock(lock_); }
        ~Guard() { if (lock_) IOLockUnlock(lock_); }
        IOLock* lock_;
    };
    FF800TransmitStart(const Protocols::Ports::ProtocolRegisterIO& io,
                       Scheduling::ITimerScheduler& scheduler, std::array<uint32_t, 3> words,
                       const std::atomic<bool>* cancel)
        : io_(io), scheduler_(scheduler), words_(words), cancel_(cancel) {}

    [[nodiscard]] bool Current() {
        if (done_.load(std::memory_order_acquire)) return false;
        if (cancel_ && cancel_->load(std::memory_order_acquire)) { Finish(kIOReturnAborted); return false; }
        if (!io_.IsRouteCurrent()) { Finish(kIOReturnOffline); return false; }
        return true;
    }
    void CheckCancellation() {
        if (Current()) Schedule(5 * kMillisecond, [](auto& self) { self.CheckCancellation(); });
    }
    void Schedule(uint64_t delay, Task task) {
        if (done_.load(std::memory_order_acquire)) return;
        auto ticket = std::make_shared<std::atomic<Scheduling::TimerToken>>(0);
        const auto weak = weak_from_this();
        const auto token = scheduler_.ScheduleAfter(delay, [weak, ticket, task = std::move(task)] {
            if (const auto self = weak.lock()) {
                {
                    Guard guard(self->lock_);
                    std::erase(self->timers_, ticket);
                }
                if (self->Current()) task(*self);
            }
        });
        ticket->store(token, std::memory_order_release);
        bool cancel;
        {
            Guard guard(lock_);
            cancel = done_.load(std::memory_order_acquire);
            if (!cancel) timers_.push_back(ticket);
        }
        if (cancel && token) scheduler_.Cancel(token);
        if (!token && !cancel) Finish(kIOReturnNotReady);
    }
    void Finish(IOReturn status, uint8_t channel = 0) {
        Completion completion;
        std::vector<std::shared_ptr<std::atomic<Scheduling::TimerToken>>> timers;
        Async::AsyncHandle handle;
        {
            Guard guard(lock_);
            if (done_.exchange(true, std::memory_order_acq_rel)) return;
            status_ = status; channel_ = channel;
            completion = std::move(completion_);
            timers.swap(timers_);
            handle = std::exchange(handle_, {});
        }
        for (const auto& ticket : timers) {
            if (const auto token = ticket->load(std::memory_order_acquire)) scheduler_.Cancel(token);
        }
        if (handle) (void)io_.CancelTransaction(handle);
        if (completion) completion(status, channel);
    }
    void PublishHandle(Async::AsyncHandle handle) {
        bool cancel;
        {
            Guard guard(lock_);
            cancel = done_.load(std::memory_order_acquire);
            if (!cancel) handle_ = handle;
        }
        if (cancel && handle) (void)io_.CancelTransaction(handle);
    }
    void ClearHandle() { Guard guard(lock_); handle_ = {}; }
    void CancelAttempt() {
        Async::AsyncHandle handle;
        { Guard guard(lock_); handle = std::exchange(handle_, {}); }
        if (handle) (void)io_.CancelTransaction(handle);
    }
    [[nodiscard]] static Async::FWAddress Address(uint64_t value) {
        return Async::FWAddress{Async::FWAddress::AddressParts{
            .addressHi = static_cast<uint16_t>(value >> 32), .addressLo = static_cast<uint32_t>(value)}};
    }

    void Write(uint64_t address, std::span<const uint32_t> words, Task next) {
        if (!Current()) return;
        const uint64_t epoch = ++request_;
        const uint64_t deadline = scheduler_.NowNs() + kRegisterTimeout;
        Schedule(kRegisterTimeout, [epoch](auto& self) {
            if (self.request_ == epoch) { ++self.request_; self.Finish(kIOReturnTimeout); }
        });
        const auto self = shared_from_this();
        const auto handle = io_.WriteBlockLEQuadlets(Address(address), words,
            [self, epoch, deadline, next = std::move(next)](Async::AsyncStatus status) mutable {
                // The completion timestamp, not the later continuation dispatch,
                // is the earliest safe origin for the hardware settle interval.
                if (self->done_.load(std::memory_order_acquire)) return;
                const uint64_t completedAt = self->scheduler_.NowNs();
                self->Schedule(0, [epoch, deadline, completedAt, status, next = std::move(next)](auto& op) {
                    if (op.request_ != epoch) return;
                    ++op.request_; op.ClearHandle();
                    if (completedAt >= deadline) { op.Finish(kIOReturnTimeout); return; }
                    if (status != Async::AsyncStatus::kSuccess) {
                        op.Finish(Protocols::Ports::MapAsyncStatusToIOReturn(status)); return;
                    }
                    op.writeCompletedAt_ = completedAt;
                    next(op);
                });
            });
        PublishHandle(handle);
    }
    void Read(uint64_t address, uint64_t deadline, std::function<void(uint32_t)> next,
              Task timedOut = [](auto& self) { self.Finish(kIOReturnTimeout); }) {
        if (!Current()) return;
        const uint64_t now = scheduler_.NowNs();
        if (now >= deadline) { timedOut(*this); return; }
        const uint64_t epoch = ++request_;
        Schedule(deadline - now, [epoch, timedOut](auto& self) {
            if (self.request_ == epoch) { ++self.request_; self.CancelAttempt(); timedOut(self); }
        });
        const auto self = shared_from_this();
        const auto handle = io_.ReadQuadLE(Address(address),
            [self, epoch, deadline, next = std::move(next), timedOut](Async::AsyncStatus status, uint32_t value) mutable {
                self->Schedule(0, [epoch, deadline, status, value, next = std::move(next), timedOut](auto& op) {
                    if (op.request_ != epoch) return;
                    ++op.request_; op.ClearHandle();
                    if (op.scheduler_.NowNs() >= deadline || status == Async::AsyncStatus::kTimeout) {
                        timedOut(op); return;
                    }
                    if (status != Async::AsyncStatus::kSuccess) {
                        op.Finish(Protocols::Ports::MapAsyncStatusToIOReturn(status)); return;
                    }
                    next(value);
                });
            });
        PublishHandle(handle);
    }
    void ReadClock(uint64_t deadline, std::function<void(ClockStatus)> next) {
        Read(Register::kStatus, deadline, [this, deadline, next = std::move(next)](uint32_t q0) mutable {
            Read(Register::kStatus + 4, deadline, [q0, next = std::move(next)](uint32_t q1) {
                next(DecodeClockStatus(FirefaceModel::kFF800, q0, q1));
            });
        });
    }
    void WriteFrequency() {
        // Linux ff-protocol-former.c:341-350 completes STF then sleeps 100 ms.
        Write(Register::kFF800Init, std::span{words_}.first(1), [](auto& self) {
            self.rateDeadline_ = self.writeCompletedAt_ + Register::kFF800RateSettleTimeoutMs * kMillisecond;
            const auto settleAt = self.writeCompletedAt_ + Register::kFF800RateSettleMs * kMillisecond;
            const auto now = self.scheduler_.NowNs();
            self.Schedule(settleAt > now ? settleAt - now : 0, [](auto& op) { op.PollRate(); });
        });
    }
    void PollRate() {
        if (scheduler_.NowNs() >= rateDeadline_) { WriteFormat(); return; }
        ReadClock(rateDeadline_, [this](ClockStatus status) {
            if (status.configured48k) WriteFormat();
            else Schedule(Register::kFF800RateSettlePollMs * kMillisecond, [](auto& self) { self.PollRate(); });
        });
    }
    void WriteFormat() {
        Write(Register::kFF800PacketFormat, std::span{words_}.subspan(1), [](auto& self) {
            // Every attempt revalidates the achieved clock; an advisory settle
            // timeout is never permission to run transport at the wrong rate.
            self.ReadClock(self.scheduler_.NowNs() + kRegisterTimeout, [&self](ClockStatus status) {
                if (!IsClockReady(status)) {
                    ASFW_LOG(Audio, "[RME] FF800 clock not ready after init status0=0x%08x status1=0x%08x", status.q0, status.q1);
                    self.Finish(kIOReturnNotReady); return;
                }
                self.channelDeadline_ = self.scheduler_.NowNs() + 500 * kMillisecond;
                self.PollChannel();
            });
        });
    }
    void PollChannel() {
        Read(Register::kStatus + 8, channelDeadline_, [this](uint32_t word) {
            if (word == 0xffffffffU) Schedule(5 * kMillisecond, [](auto& self) { self.PollChannel(); });
            else Finish(kIOReturnSuccess, static_cast<uint8_t>(word & 0x3fU));
        }, [](auto& self) { self.ChannelTimeout(); });
    }
    void ChannelTimeout() {
        if (retried_) {
            ASFW_LOG(Audio, "[RME] FF800 no transmit channel after two bounded allocation attempts");
            Finish(kIOReturnTimeout); return;
        }
        retried_ = true;
        ASFW_LOG(Audio, "[RME] FF800 no transmit channel; resetting stream state and retrying once");
        // FFADO fireface_hw.cpp:923-949: end block before another init.
        const std::array<uint32_t, 3> end{};
        Write(Register::kFF800Stop, end, [](auto& self) { self.WriteFrequency(); });
    }

    Protocols::Ports::ProtocolRegisterIO io_;
    Scheduling::ITimerScheduler& scheduler_;
    std::array<uint32_t, 3> words_;
    const std::atomic<bool>* cancel_;
    IOLock* lock_{IOLockAlloc()};
    std::atomic<bool> done_{false};
    Completion completion_;
    IOReturn status_{kIOReturnAborted};
    uint8_t channel_{0};
    std::vector<std::shared_ptr<std::atomic<Scheduling::TimerToken>>> timers_;
    Async::AsyncHandle handle_{};
    // Scheduler-queue confined below; late callbacks carry a request epoch.
    uint64_t request_{0}, writeCompletedAt_{0}, rateDeadline_{0}, channelDeadline_{0};
    bool retried_{false};
};

} // namespace ASFW::Audio::RME
