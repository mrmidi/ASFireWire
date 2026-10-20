// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "FirefaceClockStatus.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"
#include "../../../Logging/Logging.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ASFW::Audio::RME {

// Cold control-path trace, always persisted in the exported driver ring. Each
// line holds at most 16 actual payload bytes; offset/length reconstruct larger
// writes without relying on HexDumps verbosity. No audio-loop instrumentation.
inline void TraceRmeBytes(const Protocols::Ports::ProtocolRegisterIO& io,
                          const char* operation, uint64_t address,
                          std::span<const uint32_t> words) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t offset = 0; offset < words.size() * 4U; offset += 16U) {
        std::array<char, 49> bytes{};
        const size_t count = std::min<size_t>(16U, words.size() * 4U - offset);
        for (size_t i = 0; i < count; ++i) {
            const size_t index = offset + i;
            const auto value = static_cast<uint8_t>(words[index / 4U] >> (8U * (index % 4U)));
            bytes[3U * i] = hex[value >> 4U];
            bytes[3U * i + 1U] = hex[value & 0xfU];
            bytes[3U * i + 2U] = i + 1U == count ? '\0' : ' ';
        }
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u op=%{public}s addr=0x%012llx "
                 "offset=%u length=%u bytesLE=[%{public}s]",
                 io.Route().guid, io.Generation().value, operation, address,
                 static_cast<unsigned>(offset), static_cast<unsigned>(words.size() * 4U), bytes.data());
    }
}
// One FF800 fetch/assignment/enable stage, owning its fixed route and words. All state
// transitions run on the scheduler's queue; no transition sleeps or awaits IO.
// Cancellation may come from another queue. The lock protects completion,
// timer/handle ownership only, and is never held over IO or scheduler calls.
class FF800TransmitStart final : public std::enable_shared_from_this<FF800TransmitStart> {
public:
    enum class Phase { kFetch, kAssign, kEnable };
    using Completion = std::function<void(IOReturn, uint8_t)>;
    using Task = std::function<void(FF800TransmitStart&)>;

    [[nodiscard]] static std::shared_ptr<FF800TransmitStart> Create(
        const Protocols::Ports::ProtocolRegisterIO& io, Scheduling::ITimerScheduler& scheduler,
        std::array<uint32_t, 3> words, const std::atomic<bool>* cancel = nullptr,
        Phase phase = Phase::kAssign) {
        return std::shared_ptr<FF800TransmitStart>(new FF800TransmitStart(io, scheduler, words, cancel, phase));
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
        Schedule(0, [](auto& self) {
            self.CheckCancellation();
            if (self.phase_ == Phase::kFetch) {
                ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u recipe=vendor341-full-init-v1 "
                         "order=fetch,reserve,init,poll,prepare,enable,timer5ms,hostRX,hostTX,confirm",
                         self.io_.Route().guid, self.io_.Generation().value);
                // RME 3.41 hwStart 0x3f05: fetch PCM before reservation/init.
                const std::array<uint32_t, 28> fetch{};
                self.Write("enable PCM fetching", Register::kFetchMask, fetch,
                           [](auto& op) { op.Finish(kIOReturnSuccess); });
            } else if (self.phase_ == Phase::kEnable) {
                self.Enable();
            } else {
                self.Initialize();
            }
        });
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
                       const std::atomic<bool>* cancel, Phase phase)
        : io_(io), scheduler_(scheduler), words_(words), cancel_(cancel), phase_(phase),
          startedAt_(scheduler.NowNs()) {}

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
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u phase=%{public}s complete kr=0x%08x (%{public}s) elapsedUs=%llu",
                 io_.Route().guid, io_.Generation().value,
                 phase_ == Phase::kFetch ? "enable PCM fetching" :
                 phase_ == Phase::kAssign ? "initialize and allocate device transmit" : "enable and settle",
                 status, Logging::IOReturnName(status), (scheduler_.NowNs() - startedAt_) / 1000U);
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

    void Write(const char* operation, uint64_t address, std::span<const uint32_t> words, Task next) {
        if (!Current()) return;
        const uint64_t epoch = ++request_;
        TraceRmeBytes(io_, operation, address, words);
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u op=%{public}s submit write bytes=%u",
                 io_.Route().guid, io_.Generation().value, operation, static_cast<unsigned>(words.size() * 4U));
        const uint64_t submittedAt = scheduler_.NowNs();
        const uint64_t deadline = submittedAt + kRegisterTimeout;
        Schedule(kRegisterTimeout, [epoch, operation](auto& self) {
            if (self.request_ == epoch) {
                ++self.request_;
                ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u op=%{public}s write completion missing (1500 ms)",
                         self.io_.Route().guid, self.io_.Generation().value, operation);
                self.Finish(kIOReturnTimeout);
            }
        });
        if (!Current()) return; // Timer shutdown/refusal must prevent unbounded IO.
        const auto self = shared_from_this();
        const auto handle = io_.WriteBlockLEQuadlets(Address(address), words,
            [self, epoch, deadline, submittedAt, operation, next = std::move(next)](Async::AsyncStatus status) mutable {
                // The completion timestamp, not the later continuation dispatch,
                // is the earliest safe origin for the hardware settle interval.
                if (self->done_.load(std::memory_order_acquire)) return;
                const uint64_t completedAt = self->scheduler_.NowNs();
                self->Schedule(0, [epoch, deadline, submittedAt, operation, completedAt, status, next = std::move(next)](auto& op) {
                    if (op.request_ != epoch) return;
                    ++op.request_; op.ClearHandle();
                    ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u op=%{public}s write complete kr=0x%08x (%{public}s) elapsedUs=%llu",
                             op.io_.Route().guid, op.io_.Generation().value, operation,
                             Protocols::Ports::MapAsyncStatusToIOReturn(status),
                             Logging::IOReturnName(Protocols::Ports::MapAsyncStatusToIOReturn(status)),
                             (completedAt - submittedAt) / 1000U);
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
    void Initialize() {
        // Vendor RME 3.41 x86_64 hwInitDevice 0x7850 / FWWrite call 0x7919:
        // all three words form ONE 12-byte command; no format-tail write.
        // See documentation/ff800-vendor-status.md and FFADO fireface_hw.cpp:841-874.
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u init rate=%u channels=%u hostTxChannel=%u speed=S%u",
                 io_.Route().guid, io_.Generation().value, words_[0], words_[2] & 0x7ffU,
                 words_[1] & 0x3fU, (words_[2] & 0x800U) ? 800U : 400U);
        Write("initialize stream (complete command)", Register::kFF800Init, words_, [](auto& self) {
            self.channelDeadline_ = self.writeCompletedAt_ + 500 * kMillisecond;
            ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u wait=device transmit channel "
                     "addr=0x0000801c0000 bytes=16 pollMs=5 deadlineMs=500 pending=0xffffffff",
                     self.io_.Route().guid, self.io_.Generation().value);
            self.PollChannel();
        });
    }
    void PollChannel() {
        if (!Current()) return;
        const uint64_t now = scheduler_.NowNs();
        if (now >= channelDeadline_) { ChannelTimeout(); return; }
        const uint64_t epoch = ++request_;
        ++polls_;
        Schedule(channelDeadline_ - now, [epoch](auto& self) {
            if (self.request_ != epoch) return;
            ++self.request_;
            self.CancelAttempt();
            self.ChannelTimeout();
        });
        if (!Current()) return;
        const auto self = shared_from_this();
        const auto handle = io_.ReadBlock(Address(Register::kStatus), 16,
            [self, epoch](Async::AsyncStatus status, std::span<const uint8_t> bytes) {
                // The bus owns the callback span. Copy before dispatching and
                // reject a truncated tuple rather than accepting zero-filled q2.
                std::array<uint32_t, 4> words{};
                const auto length = bytes.size();
                if (status == Async::AsyncStatus::kSuccess && length == 16) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        const size_t offset = i * 4U;
                        words[i] = static_cast<uint32_t>(bytes[offset]) |
                            (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
                            (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
                            (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
                    }
                }
                self->Schedule(0, [epoch, status, words, length](auto& op) {
                    if (op.request_ != epoch) return;
                    ++op.request_; op.ClearHandle();
                    if (op.scheduler_.NowNs() >= op.channelDeadline_) { op.ChannelTimeout(); return; }
                    if (status != Async::AsyncStatus::kSuccess || length != 16) {
                        const IOReturn kr = status == Async::AsyncStatus::kSuccess ? kIOReturnUnderrun :
                            Protocols::Ports::MapAsyncStatusToIOReturn(status);
                        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u op=read allocation status "
                                 "addr=0x0000801c0000 expectedBytes=16 actualBytes=%u kr=0x%08x",
                                 op.io_.Route().guid, op.io_.Generation().value, static_cast<unsigned>(length), kr);
                        op.Finish(kr); return;
                    }
                    op.lastStatus_ = words;
                    op.hasStatus_ = true;
                    const auto clock = DecodeClockStatus(FirefaceModel::kFF800, words[0], words[1]);
                    const bool allocated = words[2] != 0xffffffffU;
                    const bool ready = allocated && IsClockReady(clock);
                    // Snapshot first, every 100 ms, and success. Never log each
                    // 5 ms poll; q0's low rate bits can change continuously.
                    if (op.polls_ == 1 || ready || op.scheduler_.NowNs() >= op.nextStatusLogAt_) {
                        op.LogStatus(ready ? "channel and clock ready" : allocated ? "channel ready; waiting for clock" : "waiting for channel");
                        op.nextStatusLogAt_ = op.scheduler_.NowNs() + 100 * kMillisecond;
                    }
                    if (ready) {
                        op.Finish(kIOReturnSuccess, static_cast<uint8_t>(words[2] & 0x3fU));
                    } else {
                        op.Schedule(5 * kMillisecond, [](auto& next) { next.PollChannel(); });
                    }
                });
            });
        PublishHandle(handle);
    }
    void LogStatus(const char* reason) {
        const auto decoded = DecodeStatus(lastStatus_[0], lastStatus_[1]);
        // The exported ring has a 232-byte message limit. Keep raw/decoded
        // snapshots separate so neither the tuple nor the explanation truncates.
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u status=%{public}s polls=%u "
                 "elapsedMs=%llu q0=%08x q1=%08x q2=%08x q3=%08x",
                 io_.Route().guid, io_.Generation().value, reason, polls_,
                 (scheduler_.NowNs() - writeCompletedAt_) / kMillisecond,
                 lastStatus_[0], lastStatus_[1], lastStatus_[2], lastStatus_[3]);
        ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u clock=%{public}s rate=%u "
                 "word=%{public}s spdif=%{public}s adat1=%{public}s adat2=%{public}s deviceTx=%{public}s channel=%d",
                 io_.Route().guid, io_.Generation().value, Name(decoded.configured), decoded.configuredRateHz,
                 Name(decoded.wordClock), Name(decoded.spdif), Name(decoded.adat1), Name(decoded.adat2),
                 lastStatus_[2] == 0xffffffffU ? "pending" : "allocated",
                 lastStatus_[2] == 0xffffffffU ? -1 : static_cast<int>(lastStatus_[2] & 0x3fU));
        TraceRmeBytes(io_, "read allocation status", Register::kStatus, lastStatus_);
    }
    void ChannelTimeout() {
        if (hasStatus_) LogStatus("allocation deadline expired (last received snapshot)");
        else ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u allocation deadline expired; no status response polls=%u",
                      io_.Route().guid, io_.Generation().value, polls_);
        Finish(hasStatus_ && lastStatus_[2] != 0xffffffffU ? kIOReturnNotReady : kIOReturnTimeout);
    }
    void Enable() {
        // RME 3.41 hwStart 0x4000/0x401a: enable, wait 5 ms, then start host RX/TX.
        Write("enable device transmit", Register::kFF800Start, std::span{words_}.first(1), [](auto& self) {
            const uint64_t due = self.writeCompletedAt_ + 5 * kMillisecond;
            const uint64_t now = self.scheduler_.NowNs();
            ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u wait=post-enable settle delayMs=5 source=driver-timer",
                     self.io_.Route().guid, self.io_.Generation().value);
            self.Schedule(due > now ? due - now : 0, [](auto& op) {
                ASFW_LOG(Audio, "[RME][Choreo] guid=%016llx gen=%u post-enable settle complete; host receive then transmit may start",
                         op.io_.Route().guid, op.io_.Generation().value);
                op.Finish(kIOReturnSuccess);
            });
        });
    }

    Protocols::Ports::ProtocolRegisterIO io_;
    Scheduling::ITimerScheduler& scheduler_;
    std::array<uint32_t, 3> words_;
    const std::atomic<bool>* cancel_;
    const Phase phase_;
    const uint64_t startedAt_;
    IOLock* lock_{IOLockAlloc()};
    std::atomic<bool> done_{false};
    Completion completion_;
    IOReturn status_{kIOReturnAborted};
    uint8_t channel_{0};
    std::vector<std::shared_ptr<std::atomic<Scheduling::TimerToken>>> timers_;
    Async::AsyncHandle handle_{};
    // Scheduler-queue confined below; late callbacks carry a request epoch.
    uint64_t request_{0}, writeCompletedAt_{0}, channelDeadline_{0}, nextStatusLogAt_{0};
    std::array<uint32_t, 4> lastStatus_{};
    uint32_t polls_{0};
    bool hasStatus_{false};
};

} // namespace ASFW::Audio::RME
