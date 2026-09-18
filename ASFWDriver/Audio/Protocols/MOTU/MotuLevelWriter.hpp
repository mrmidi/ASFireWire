// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuLevelWriter.hpp - Latest-value-wins writer for one MOTU level register.
//
// A slider drag or a held volume key changes a level far faster than the device needs to
// hear about it (documentation/AUDIO_BACKENDS_CONTROLS.md §5.3). This writer keeps at most
// one write in flight; a value that arrives meanwhile replaces the pending one instead of
// queueing behind it, and after each write it rests kCooldownNs before sending again. The
// device therefore sees at most one write per (round trip + cooldown) -- under 40/s --
// however fast the host moves the control, and the last value of every burst is always sent.
//
// A failed write is not retried on its own: that would turn an unplugged device into a
// write every cooldown forever. The next change from the host tries again.
//
// Write completions and the cooldown timer arrive on other queues, so the state sits behind
// a lock in a core that each in-flight callback keeps alive. Cancel() stops the core from
// issuing anything further, which is what lets the owner be destroyed with a write still
// on the bus.

#pragma once

#include "../../../Scheduling/ITimerScheduler.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace ASFW::Audio::Motu {

class MotuLevelWriter final {
public:
    using DoneFn = std::function<void(bool ok)>;
    using SendFn = std::function<void(uint8_t value, DoneFn done)>;

    static constexpr uint64_t kCooldownNs = 25'000'000;

    /// With no scheduler there is no cooldown: the next value goes out as soon as the
    /// previous write completes.
    MotuLevelWriter(SendFn send, Scheduling::ITimerScheduler* scheduler)
        : core_(std::make_shared<Core>(std::move(send), scheduler)) {}

    ~MotuLevelWriter() { Cancel(); }

    MotuLevelWriter(const MotuLevelWriter&) = delete;
    MotuLevelWriter& operator=(const MotuLevelWriter&) = delete;

    void Set(uint8_t value) { Core::Set(core_, value); }

    /// Permanent: nothing is sent after this returns, including a pending value.
    void Cancel() { core_->Cancel(); }

private:
    class Core final {
    public:
        Core(SendFn send, Scheduling::ITimerScheduler* scheduler)
            : send_(std::move(send)), scheduler_(scheduler), lock_(IOLockAlloc()) {}

        ~Core() {
            if (lock_ != nullptr) {
                IOLockFree(lock_);
            }
        }

        Core(const Core&) = delete;
        Core& operator=(const Core&) = delete;

        static void Set(const std::shared_ptr<Core>& self, uint8_t value) {
            bool start = false;
            {
                Guard guard(self->lock_);
                if (self->cancelled_) {
                    return;
                }
                self->desired_ = value;
                self->hasDesired_ = true;
                if (self->state_ == State::kIdle) {
                    self->state_ = State::kSending;
                    start = true;
                }
            }
            if (start) {
                Send(self, value);
            }
        }

        void Cancel() {
            Scheduling::TimerToken token = Scheduling::kInvalidTimerToken;
            {
                Guard guard(lock_);
                cancelled_ = true;
                hasDesired_ = false;
                token = cooldownToken_;
                cooldownToken_ = Scheduling::kInvalidTimerToken;
            }
            if (token != Scheduling::kInvalidTimerToken && scheduler_ != nullptr) {
                scheduler_->Cancel(token);
            }
        }

    private:
        enum class State : uint8_t { kIdle, kSending, kCooling };

        class Guard final {
        public:
            explicit Guard(IOLock* lock) : lock_(lock) {
                if (lock_ != nullptr) {
                    IOLockLock(lock_);
                }
            }
            ~Guard() {
                if (lock_ != nullptr) {
                    IOLockUnlock(lock_);
                }
            }
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;

        private:
            IOLock* lock_;
        };

        static void Send(const std::shared_ptr<Core>& self, uint8_t value) {
            self->send_(value, [self, value](bool ok) { OnSent(self, value, ok); });
        }

        static void OnSent(const std::shared_ptr<Core>& self, uint8_t value, bool ok) {
            std::optional<uint8_t> next{};
            bool cool = false;
            {
                Guard guard(self->lock_);
                if (self->cancelled_) {
                    self->state_ = State::kIdle;
                    return;
                }
                if (ok) {
                    self->lastSent_ = value;
                    self->hasLastSent_ = true;
                } else {
                    self->hasLastSent_ = false;
                    // Drop the failed value; a newer one that arrived meanwhile still goes.
                    if (self->hasDesired_ && self->desired_ == value) {
                        self->hasDesired_ = false;
                    }
                }
                if (self->scheduler_ != nullptr) {
                    self->state_ = State::kCooling;
                    cool = true;
                } else if (self->NeedsSendLocked()) {
                    self->state_ = State::kSending;
                    next = self->desired_;
                } else {
                    self->state_ = State::kIdle;
                }
            }
            if (cool) {
                const Scheduling::TimerToken token = self->scheduler_->ScheduleAfter(
                    kCooldownNs, [self] { OnCooldown(self); });
                Guard guard(self->lock_);
                // The timer may already have fired on another queue; only a live cooldown
                // keeps its token.
                if (self->state_ == State::kCooling) {
                    self->cooldownToken_ = token;
                }
            } else if (next.has_value()) {
                Send(self, *next);
            }
        }

        static void OnCooldown(const std::shared_ptr<Core>& self) {
            std::optional<uint8_t> next{};
            {
                Guard guard(self->lock_);
                self->cooldownToken_ = Scheduling::kInvalidTimerToken;
                if (self->cancelled_) {
                    self->state_ = State::kIdle;
                    return;
                }
                if (self->NeedsSendLocked()) {
                    self->state_ = State::kSending;
                    next = self->desired_;
                } else {
                    self->state_ = State::kIdle;
                }
            }
            if (next.has_value()) {
                Send(self, *next);
            }
        }

        [[nodiscard]] bool NeedsSendLocked() const noexcept {
            return hasDesired_ && (!hasLastSent_ || desired_ != lastSent_);
        }

        SendFn send_;
        Scheduling::ITimerScheduler* scheduler_{nullptr};
        IOLock* lock_{nullptr};

        State state_{State::kIdle};
        bool cancelled_{false};
        bool hasDesired_{false};
        uint8_t desired_{0};
        bool hasLastSent_{false};
        uint8_t lastSent_{0};
        Scheduling::TimerToken cooldownToken_{Scheduling::kInvalidTimerToken};
    };

    std::shared_ptr<Core> core_;
};

} // namespace ASFW::Audio::Motu
