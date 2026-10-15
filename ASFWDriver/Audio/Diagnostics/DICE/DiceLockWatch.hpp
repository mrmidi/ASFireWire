// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../Protocols/DICE/Core/DICETypes.hpp"
#include "DiceLockSnapshot.hpp"
#include "../../Protocols/AudioTypes.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"
#include "../../../Logging/Logging.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace ASFW::Audio::DICE::Diagnostics {

// Owned by DiceFamilyDriver. Timer and bus callbacks retain only a session,
// with a fixed route and no pointer back into the family/protocol object.
class DiceLockWatch final {
    struct Session final : std::enable_shared_from_this<Session> {
        Protocols::Ports::ProtocolRegisterIO io;
        // AudioRuntimeRegistry's scheduler (SetTimerScheduler): it creates and
        // outlives every protocol, so a session never outlives it in use.
        Scheduling::ITimerScheduler* scheduler;
        const uint32_t globalBase;
        IOLock* const dataLock{IOLockAlloc()};
        IOLock* const submitLock{IOLockAlloc()};
        DiceLockSnapshot snapshot{};
        uint64_t startNs{0};
        std::array<Scheduling::TimerToken, 5> timers{};
        std::array<Async::AsyncHandle, 2> handles{};
        uint32_t pending{0};
        uint32_t statusValue{0};
        uint32_t extValue{0};
        IOReturn sampleError{kIOReturnSuccess};
        bool finalSample{false};
        uint32_t validReads{0};
        // Called once, outside every lock, when the watch completes. Never on
        // Stop or Reset: a cancelled watch has no verdict worth acting on.
        std::function<void(const DiceLockSnapshot&)> onVerdict;

        Session(Protocols::Ports::ProtocolRegisterIO source,
                Scheduling::ITimerScheduler* timer, uint32_t base)
            : io(source), scheduler(timer), globalBase(base) {}
        ~Session() {
            if (dataLock) IOLockFree(dataLock);
            if (submitLock) IOLockFree(submitLock);
        }
        struct Guard {
            IOLock* lock;
            explicit Guard(IOLock* value) : lock(value) { IOLockLock(lock); }
            ~Guard() { IOLockUnlock(lock); }
        };
        uint32_t Elapsed() const {
            return scheduler ? static_cast<uint32_t>(std::min<uint64_t>(
                (scheduler->NowNs() - startNs) / 1'000'000, 0xFFFFFFFFULL)) : 0;
        }
        DiceLockSnapshot Copy() const {
            if (!dataLock) return snapshot;
            Guard guard(dataLock);
            return snapshot;
        }
        void VerdictLocked() const {
            for (uint32_t i = 0; i < snapshot.receivers.size(); ++i) {
                const auto& rx = snapshot.receivers[i];
                if (rx.channel > 63) continue;
                if (rx.firstLockMs != 0xFFFFFFFFU) {
                    ASFW_LOG(DICE, "[DiceLock] guid=%016llx gen=%u verdict: rx%u (ch%u) first observed locked at +%ums lost=%u phase=%u samples=%u errors=%u",
                             snapshot.guid, snapshot.generation, i, rx.channel, rx.firstLockMs,
                             (snapshot.lostMask >> i) & 1U, static_cast<uint32_t>(snapshot.phase),
                             snapshot.samples, snapshot.readErrors);
                } else {
                    ASFW_LOG(DICE, "[DiceLock] guid=%016llx gen=%u verdict: rx%u (ch%u) %{public}s in %ums phase=%u samples=%u errors=%u lastError=0x%x lastExt=0x%08x",
                             snapshot.guid, snapshot.generation, i, rx.channel,
                             snapshot.phase == DiceLockPhase::Complete && snapshot.samples != 0
                                 ? "NEVER observed locked" : "lock not observed (watch incomplete)", snapshot.elapsedMs,
                             static_cast<uint32_t>(snapshot.phase), snapshot.samples,
                             snapshot.readErrors, snapshot.lastError, snapshot.extStatus);
                }
            }
            if (snapshot.expectedMask == 0) {
                ASFW_LOG(DICE, "[DiceLock] guid=%016llx gen=%u verdict: no assigned RX streams phase=%u",
                         snapshot.guid, snapshot.generation, static_cast<uint32_t>(snapshot.phase));
            }
        }
        // dataLock held. Sparse timer reads and notification health reads enter
        // the same accumulator. A sample time is observation time, not HW latency.
        void RecordLocked(uint32_t status, uint32_t ext, IOReturn error, uint32_t valid = 3) {
            snapshot.elapsedMs = Elapsed();
            if (error != kIOReturnSuccess) {
                ++snapshot.readErrors;
                snapshot.lastError = error;
                ASFW_LOG(DICE, "[DiceLock] guid=%016llx gen=%u +%ums read failed kr=0x%x",
                         snapshot.guid, snapshot.generation, snapshot.elapsedMs, error);
            }
            // EXT_STATUS can succeed (and clear its slip latches) even when
            // STATUS fails. Preserve that evidence instead of throwing it away.
            if (!(valid & 2U)) return;
            ++snapshot.samples;
            const uint32_t before = (snapshot.extStatus >> 6) & snapshot.expectedMask;
            const uint32_t locked = (ext >> 6) & snapshot.expectedMask;
            snapshot.lostMask |= before & ~locked;
            snapshot.observedMask |= locked;
            if (valid & 1U) snapshot.status = status;
            snapshot.extStatus = ext;
            snapshot.slipHistory |= ext & 0xFFFF0000U;
            for (uint32_t i = 0; i < snapshot.receivers.size(); ++i) {
                if ((locked & (1U << i)) && snapshot.receivers[i].firstLockMs == 0xFFFFFFFFU)
                    snapshot.receivers[i].firstLockMs = snapshot.elapsedMs;
            }
            char clock[48], extended[160];
            ASFW_LOG(DICE, "[DiceLock] guid=%016llx gen=%u +%ums status=%{public}s ext=%{public}s raw=0x%08x",
                     snapshot.guid, snapshot.generation, snapshot.elapsedMs,
                     FormatGlobalStatus(snapshot.status, clock, sizeof(clock)),
                     FormatExtStatus(ext, extended, sizeof(extended)), ext);
        }
        void Observe(uint32_t status, uint32_t ext, IOReturn error) {
            if (!dataLock) return;
            Guard guard(dataLock);
            if (snapshot.phase == DiceLockPhase::Watching) RecordLocked(status, ext, error, error == kIOReturnSuccess ? 3U : 0U);
        }
        void CancelTimers() {
            if (scheduler) for (auto token : timers) if (token) scheduler->Cancel(token);
        }
        void Completed(unsigned index, Async::AsyncStatus result, uint32_t value) {
            bool finished = false;
            {
                Guard guard(dataLock);
                if (snapshot.phase != DiceLockPhase::Watching || !(pending & (1U << index))) return;
                handles[index] = {};
                pending &= ~(1U << index);
                if (result != Async::AsyncStatus::kSuccess)
                    sampleError = Protocols::Ports::MapAsyncStatusToIOReturn(result);
                else validReads |= 1U << index;
                if (index == 0) statusValue = value;
                else extValue = value;
                if (pending) return;
                RecordLocked(statusValue, extValue, sampleError, validReads);
                if (finalSample) {
                    snapshot.phase = DiceLockPhase::Complete;
                    VerdictLocked();
                    finished = true;
                }
            }
            if (finished) {
                CancelTimers();
                if (onVerdict) onVerdict(Copy());
            }
        }
        void Sample(bool final) {
            // No blocking register I/O on the scheduler's Default queue. This
            // lock covers only submission, fencing Stop against a late submit.
            Guard submission(submitLock);
            std::array<Async::AsyncHandle, 2> staleHandles{};
            bool stale = false;
            {
                Guard guard(dataLock);
                if (snapshot.phase != DiceLockPhase::Watching) return;
                if (!io.IsRouteCurrent()) {
                    snapshot.elapsedMs = Elapsed();
                    snapshot.phase = DiceLockPhase::Reset;
                    staleHandles = std::exchange(handles, {});
                    pending = 0;
                    VerdictLocked();
                    stale = true;
                } else {
                    finalSample |= final;
                    if (pending) return; // coalesce with the outstanding sample
                    pending = 3;
                    sampleError = kIOReturnSuccess;
                    validReads = 0;
                }
            }
            if (stale) {
                CancelTimers();
                for (auto handle : staleHandles) if (handle) (void)io.CancelTransaction(handle);
                return;
            }
            const auto self = shared_from_this();
            for (unsigned i = 0; i < 2; ++i) {
                const auto handle = io.ReadQuadBE(MakeDICEAddress(globalBase +
                    (i == 0 ? GlobalOffset::kStatus : GlobalOffset::kExtStatus)),
                    [self, i](Async::AsyncStatus status, uint32_t value) {
                        self->Completed(i, status, value);
                    });
                Guard guard(dataLock);
                // A fake bus may complete inline; never retain that retired handle.
                if (pending & (1U << i)) handles[i] = handle;
            }
        }
        void Cancel(DiceLockPhase phase, IOReturn reason = kIOReturnSuccess) {
            if (!dataLock || !submitLock) return;
            std::array<Async::AsyncHandle, 2> cancelHandles;
            bool completed = false;
            {
                Guard submission(submitLock);
                Guard guard(dataLock);
                cancelHandles = std::exchange(handles, {});
                pending = 0;
                if (snapshot.phase == DiceLockPhase::Watching) {
                    snapshot.elapsedMs = Elapsed();
                    if (reason != kIOReturnSuccess) RecordLocked(0, 0, reason, 0);
                    snapshot.phase = phase;
                    VerdictLocked();
                    completed = phase == DiceLockPhase::Complete;
                }
            }
            CancelTimers();
            for (auto handle : cancelHandles) if (handle) (void)io.CancelTransaction(handle);
            if (completed && onVerdict) onVerdict(Copy());
        }
        void Begin() {
            if (!dataLock || !submitLock || !scheduler) {
                snapshot.phase = DiceLockPhase::Unavailable;
                snapshot.lastError = kIOReturnNotReady;
                snapshot.readErrors = 1;
                VerdictLocked();
                return;
            }
            bool failed = false;
            {
                Guard submission(submitLock);
                startNs = scheduler->NowNs();
                snapshot.phase = DiceLockPhase::Watching;
                const auto weak = weak_from_this();
                // The first sample lands after the start's NO-DATA prefill
                // (~190 ms), when a receiver can have seen data. The last entry
                // bounds a final sample whose reads never complete.
                constexpr uint32_t times[] = {500, 1000, 2000, 5000, 7000};
                for (unsigned i = 0; i < timers.size(); ++i) {
                    timers[i] = scheduler->ScheduleAfter(uint64_t{times[i]} * 1'000'000,
                        [weak, i] {
                            if (auto self = weak.lock()) {
                                if (i == 4) self->Cancel(DiceLockPhase::Complete, kIOReturnTimeout);
                                else self->Sample(i == 3);
                            }
                        });
                    if (!timers[i]) { failed = true; break; }
                }
            }
            if (failed) Cancel(DiceLockPhase::Unavailable, kIOReturnNoResources);
        }
    };
    IOLock* const lock_{IOLockAlloc()};
    std::shared_ptr<Session> session_;
    bool closed_{false};
    std::shared_ptr<Session> Current() const {
        if (!lock_) return {};
        Session::Guard guard(lock_);
        return session_;
    }
public:
    DiceLockWatch() = default;
    DiceLockWatch(const DiceLockWatch&) = delete;
    DiceLockWatch& operator=(const DiceLockWatch&) = delete;
    ~DiceLockWatch() { Close(); if (lock_) IOLockFree(lock_); }
    void Start(Protocols::Ports::ProtocolRegisterIO io,
               Scheduling::ITimerScheduler* scheduler, const GeneralSections& sections,
               const AudioStreamRuntimeCaps& caps,
               std::function<void(const DiceLockSnapshot&)> onVerdict = {}) {
        if (!lock_) {
            ASFW_LOG_ERROR(DICE, "[DiceLock] unavailable: failed to allocate watch lock");
            return;
        }
        Session::Guard owner(lock_);
        if (closed_) return;
        if (session_) session_->Cancel(DiceLockPhase::Stopped);
        auto next = std::make_shared<Session>(io, scheduler, sections.global.offset);
        next->onVerdict = std::move(onVerdict);
        next->snapshot.guid = io.Route().guid;
        next->snapshot.generation = io.Generation().value;
        for (uint32_t i = 0; i < std::min(caps.hostToDeviceStreamCount, 4U); ++i) {
            const uint32_t channel = caps.hostToDeviceStreams[i].isoChannel;
            if (channel > 63) continue;
            next->snapshot.receivers[i].channel = channel;
            next->snapshot.expectedMask |= 1U << i;
        }
        session_ = next;
        next->Begin();
    }
    void Close() {
        if (!lock_) return;
        Session::Guard owner(lock_);
        closed_ = true;
        if (session_) session_->Cancel(DiceLockPhase::Stopped);
    }
    void Cancel(DiceLockPhase phase = DiceLockPhase::Stopped) {
        if (auto state = Current()) state->Cancel(phase);
    }
    [[nodiscard]] std::function<void(IOReturn, uint32_t, uint32_t)> Observer() const {
        std::weak_ptr<Session> weak = Current();
        return [weak](IOReturn error, uint32_t status, uint32_t ext) {
            if (auto state = weak.lock()) state->Observe(status, ext, error);
        };
    }
    [[nodiscard]] DiceLockSnapshot Copy() const {
        if (auto state = Current()) return state->Copy();
        return {};
    }
};


} // namespace ASFW::Audio::DICE::Diagnostics
