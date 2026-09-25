// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SessionScheduler.hpp - One device's audio session: what should run, and making it so.
//
// documentation/AUDIO_SESSION_REDESIGN.md §4.2, stage S2. Callers never start
// or stop streams themselves. They edit what the session wants (StartIO and
// StopIO attach and detach the HAL, a rate change edits the clock, a fault or
// bus reset asks for a restart), and the scheduler reconciles the device with
// that: stop what runs, then start what is wanted, one reconcile at a time.
//
// Serialization. The caller whose request finds no reconcile running runs it,
// on its own thread; requests that arrive meanwhile only edit what is wanted
// and wait, and the running caller loops until one reconcile has seen every
// edit (coalescing). Every caller is off the driver's Default queue (the nub's
// queue or a backend queue), which is where the bus completions the
// reconcile waits for arrive.
//
// Staleness. A runtime fault names the run it saw; if that run has ended, the
// fault is dropped. A runtime fault raised while a reconcile runs is that
// reconcile's own transition and is dropped too. Bus-reset rebinds are exempt:
// they are topology events and must always restart.
//
// Quiet period (stage S4a). A family can ask for device and transport events
// to settle first: a restart request then only records a pending restart and
// (re)arms a timer, and one restart runs once the device has been quiet for
// the period (TCAT's debounce, §2.2). The timer fires on the Default queue and
// hands the restart to the sessions' queue. A pending restart is covered, and
// dropped, when the run it was raised on is no longer the current one: a
// start, clock change or other restart has already rebuilt the streams. HAL
// requests (attach, detach, clock) are never delayed.

#pragma once

#include "RestartRoutine.hpp"
#include "StopRoutine.hpp"

#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ASFW::Audio {
class AudioRuntimeRegistry;
class IDeviceProtocol;
}

namespace ASFW::Audio::Session {

enum class SessionState : uint8_t {
    Idle,        // nothing runs
    Restarting,  // a reconcile is in progress
    Running,     // the device confirmed its streams
    Failed,      // the last start or stop failed
    Faulted,     // repeated fault recoveries failed; faults are ignored until the next attach
};

[[nodiscard]] const char* ToString(SessionState state) noexcept;

// A consistent copy of the session, for queries.
struct SessionSnapshot {
    SessionState state{SessionState::Idle};
    bool halAttached{false};
    bool clockChangePending{false};
    uint64_t run{0};
    AudioClockConfig desiredClock{};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
    AudioDuplexChannels channels{};
    IOReturn lastStatus{kIOReturnSuccess};
};

class SessionScheduler final : public std::enable_shared_from_this<SessionScheduler> {
public:
    using BindingSourceProvider = std::function<Runtime::IDirectAudioBindingSource*(uint64_t)>;
    using StartGuard = std::function<bool(uint64_t)>;
    // Called after a restart request rebuilt the streams, on the thread that ran it.
    using RestartObserver = std::function<void(uint64_t)>;

    struct Dependencies {
        Discovery::DeviceRegistry& registry;
        AudioRuntimeRegistry& runtime;
        IIsochDuplexHostTransport& host;
        Driver::HardwareInterface& hardware;
        const std::atomic<bool>* teardown;
        std::atomic<uint64_t>& teardownAborts;
        BindingSourceProvider bindingSource;
        // Owned by AudioSessions, so a guard installed later reaches every
        // session. Null or empty: every start is allowed.
        const StartGuard* startGuard;
        // Owned by AudioSessions and installed before device callbacks begin.
        // Null: no quiet period can be kept; restarts run at once.
        Scheduling::ITimerScheduler* const* timer;
        // Where a restart that waited out its quiet period runs. Never the
        // Default queue.
        IODispatchQueue* queue;
        const RestartObserver* restartObserver;
        // Owned by AudioSessions, installed where the IRM client is created.
        ::ASFW::IRM::IRMClient* const* irm;
    };

    // Failed fault recoveries in a row before the session stops trying.
    static constexpr uint32_t kMaxFaultRestartFailures = 3;
    // How long a caller waits for the reconcile that covers its request.
    static constexpr uint32_t kWaitTimeoutMs = 15000;

    SessionScheduler(uint64_t guid, Dependencies dependencies) noexcept;
    ~SessionScheduler() noexcept;

    SessionScheduler(const SessionScheduler&) = delete;
    SessionScheduler& operator=(const SessionScheduler&) = delete;

    // CoreAudio wants streams (StartIO) or no longer does (StopIO).
    [[nodiscard]] IOReturn Attach() noexcept;
    [[nodiscard]] IOReturn Detach() noexcept;
    // A new clock: applied now while idle, or by a restart while running.
    // Returns kIOReturnAborted when a later change superseded this one.
    [[nodiscard]] IOReturn ChangeClock(const AudioClockConfig& clock, DuplexRestartReason reason) noexcept;
    // Restart the running streams. `observedRun` is the run the fault was seen
    // on (0: not tied to a run). While a reconcile runs, the request is queued
    // behind it and this returns at once with kIOReturnSuccess.
    [[nodiscard]] IOReturn RequestRestart(DuplexRestartReason reason, uint64_t observedRun = 0) noexcept;

    // Discovery retired the device after a bus reset, or found it again.
    void Retire() noexcept;
    void Present() noexcept;
    // Forget a restart waiting out its quiet period (teardown, retirement).
    void CancelPendingRestart() noexcept;
    [[nodiscard]] bool HasPendingRestart() const noexcept;

    [[nodiscard]] bool IsStreaming() const noexcept;
    [[nodiscard]] bool IsReconciling() const noexcept;
    [[nodiscard]] bool IsRetired() const noexcept;
    [[nodiscard]] uint64_t CurrentRun() const noexcept;
    // The run a fault seen now belongs to: the current run while the streams
    // are confirmed running, otherwise kNotRunning, which no run matches. Pass
    // it back as RequestRestart's `observedRun`.
    [[nodiscard]] uint64_t RunningRun() const noexcept;
    static constexpr uint64_t kNotRunning = ~uint64_t{0};
    [[nodiscard]] SessionSnapshot Snapshot() const noexcept;

private:
    // What callers want. Edited under lock_, read by the reconcile as a copy.
    struct Wanted {
        bool halAttached{false};
        AudioClockConfig clock{};
        bool clockDirty{false};
        DuplexRestartReason clockReason{DuplexRestartReason::kSampleRateChange};
        bool restart{false};
        bool restartIsFault{false};
        DuplexRestartReason restartReason{DuplexRestartReason::kManualReconfigure};
    };

    // What the device runs, as far as the session knows.
    struct Actual {
        SessionState state{SessionState::Idle};
        bool needsStop{false};  // host or device may still run something
        uint64_t run{0};
        AudioClockConfig appliedClock{};
        AudioStreamRuntimeCaps runtimeCaps{};
        AudioDuplexChannels channels{};
        IOReturn lastStatus{kIOReturnSuccess};
        uint32_t faultFailures{0};
    };

    // Results of recent reconciles, newest last: the tickets each covered, what
    // it returned, and the clock it aimed for.
    struct Result {
        uint64_t upTo{0};
        IOReturn status{kIOReturnSuccess};
        AudioClockConfig target{};
    };
    static constexpr uint32_t kResultHistory = 8;

    enum class WaitMode : uint8_t {
        UntilCovered,        // wait for the reconcile that covers the edit
        QueueBehindRunning,  // if one is running, leave the edit to it and return
    };

    // Apply `edit` to what is wanted, then return the status of the first
    // reconcile that saw the edit, running it here if none is in progress.
    // `targetOut` receives the clock that reconcile aimed for.
    template <typename Edit>
    [[nodiscard]] IOReturn Submit(Edit&& edit, AudioClockConfig* targetOut = nullptr,
                                  WaitMode mode = WaitMode::UntilCovered) noexcept;
    [[nodiscard]] IOReturn Reconcile(const Wanted& wanted) noexcept;
    [[nodiscard]] IOReturn StartStreams(const Wanted& wanted, const Discovery::DeviceRecord& record,
                                        const std::shared_ptr<IDeviceProtocol>& protocol) noexcept;
    [[nodiscard]] IOReturn StopStreams(const Discovery::DeviceRecord& record,
                                       const std::shared_ptr<IDeviceProtocol>& protocol) noexcept;
    [[nodiscard]] IOReturn ApplyClockIdle(const AudioClockConfig& clock,
                                          const std::shared_ptr<IDeviceProtocol>& protocol) noexcept;
    // Why a restart request cannot run now, or nullopt when it can.
    [[nodiscard]] std::optional<IOReturn> RestartRefusal(DuplexRestartReason reason,
                                                         uint64_t observedRun) const noexcept;
    [[nodiscard]] IOReturn RunRestart(DuplexRestartReason reason, uint64_t observedRun) noexcept;
    [[nodiscard]] IOReturn DeferRestart(DuplexRestartReason reason, uint64_t observedRun,
                                        uint32_t quietMs) noexcept;
    void ArmPendingTimer(uint32_t quietMs, uint64_t generation) noexcept;
    void FirePendingRestart(uint64_t generation) noexcept;
    [[nodiscard]] uint32_t RestartQuietPeriodMs() const noexcept;
    [[nodiscard]] bool TeardownRequested() const noexcept;
    [[nodiscard]] bool StartAllowed() const noexcept;
    [[nodiscard]] Actual LoadActual() const noexcept;
    void StoreActual(const Actual& actual) noexcept;
    void RecordResultLocked(uint64_t coveredTicket, IOReturn status, AudioClockConfig target) noexcept;
    [[nodiscard]] const Result* FindResultLocked(uint64_t ticket) const noexcept;

    const uint64_t guid_;
    Dependencies deps_;
    StopRoutine stop_;
    RestartRoutine restart_;

    IOLock* lock_{nullptr};
    Wanted wanted_{};
    Actual actual_{};
    bool reconciling_{false};
    uint64_t requested_{0};   // last ticket handed out
    uint64_t completed_{0};   // last ticket a finished reconcile covered
    Result results_[kResultHistory]{};
    uint32_t resultCount_{0};

    // A restart waiting out its quiet period. Merged requests keep the
    // strongest claim: a request not tied to a run (a bus reset) makes the
    // whole restart untied.
    struct PendingRestart {
        bool active{false};
        DuplexRestartReason reason{DuplexRestartReason::kManualReconfigure};
        uint64_t observedRun{0};
        uint64_t runAtRequest{0};   // covered once another run has started
        uint64_t generation{0};     // bumped per request; a stale timer is a no-op
        Scheduling::TimerToken timer{Scheduling::kInvalidTimerToken};
    };
    PendingRestart pending_{};

    std::atomic<bool> halAttached_{false};  // mirror of wanted_.halAttached for the routine
    std::atomic<bool> retired_{false};
};

} // namespace ASFW::Audio::Session
