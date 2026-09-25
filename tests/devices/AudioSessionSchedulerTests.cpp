// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioSessionSchedulerTests.cpp - What the session golden traces cannot show:
// requests that overlap a running reconcile, stale and in-flight faults, the
// fault budget, and teardown or retirement in the middle of a start.
//
// A device stage can be held open (a hook that blocks) while another thread
// makes a request, which is how CoreAudio and the backend queues overlap in
// the driver.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"

#include <future>
#include <string>
#include <thread>

namespace {

using namespace ASFW::Testing::Session;
using ASFW::Audio::Session::SessionState;
namespace Ids = ASFW::DeviceProfiles::Audio;

// A CMP family: no pre-stream clock gate, one stream per direction.
const SessionShape kCmpDevice{"phase88", Ids::kTerraTecVendorId, Ids::kPhase88RackFwModelId,
                              kAvcUnitSpecifier, kAvcUnitVersion};

struct SchedulerTest : ::testing::Test {
    SessionRig rig{kCmpDevice, Impl::Scheduler};

    [[nodiscard]] ASFW::Audio::Session::SessionSnapshot Snapshot() const {
        return rig.sessions.Snapshot(rig.guid).value_or(ASFW::Audio::Session::SessionSnapshot{});
    }

    // Hold the next `stage` open until Release(); Started() reports it is held.
    void Hold(const std::string& stage) {
        rig.hooks[stage] = [this] {
            held_.set_value();
            release_.get_future().wait();
        };
    }
    void WaitHeld() { held_.get_future().wait(); }
    void Release() { release_.set_value(); }

    template <typename Pred>
    static bool WaitFor(Pred&& pred) {
        for (int i = 0; i < 20000; ++i) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return false;
    }

    [[nodiscard]] int Count(const std::string& prefix) {
        int n = 0;
        for (const auto& line : rig.bus.Trace().Lines()) {
            n += line.rfind(prefix, 0) == 0 ? 1 : 0;
        }
        return n;
    }

private:
    std::promise<void> held_;
    std::promise<void> release_;
};

TEST_F(SchedulerTest, DetachWithNothingRunningDoesNothing) {
    EXPECT_EQ(rig.sessions.Detach(rig.guid), kIOReturnSuccess);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());
    EXPECT_EQ(Snapshot().state, SessionState::Idle);
}

TEST_F(SchedulerTest, ClockChangeDuringStartIsAppliedByOneFollowUpRestart) {
    Hold("device.prepare");
    auto start = std::async(std::launch::async, [&] { return rig.sessions.Attach(rig.guid); });
    WaitHeld();
    auto clock = std::async(std::launch::async, [&] {
        return rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 44100},
                                        DuplexRestartReason::kSampleRateChange);
    });
    ASSERT_TRUE(WaitFor([&] { return Snapshot().clockChangePending; }));
    Release();

    EXPECT_EQ(start.get(), kIOReturnSuccess);
    EXPECT_EQ(clock.get(), kIOReturnSuccess);
    EXPECT_EQ(Count("D prepare rate=48000"), 1);
    EXPECT_EQ(Count("D prepare rate=44100"), 1);
    EXPECT_EQ(Count("D stop"), 1) << "the follow-up restarts from a stopped device";
    EXPECT_EQ(Snapshot().appliedClock.sampleRateHz, 44100U);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
}

TEST_F(SchedulerTest, LatestClockWinsAndTheSupersededChangeIsAborted) {
    Hold("device.prepare");
    auto start = std::async(std::launch::async, [&] { return rig.sessions.Attach(rig.guid); });
    WaitHeld();
    auto first = std::async(std::launch::async, [&] {
        return rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 44100},
                                        DuplexRestartReason::kSampleRateChange);
    });
    ASSERT_TRUE(WaitFor([&] { return Snapshot().desiredClock.sampleRateHz == 44100; }));
    auto second = std::async(std::launch::async, [&] {
        return rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 32000},
                                        DuplexRestartReason::kSampleRateChange);
    });
    ASSERT_TRUE(WaitFor([&] { return Snapshot().desiredClock.sampleRateHz == 32000; }));
    Release();

    EXPECT_EQ(start.get(), kIOReturnSuccess);
    EXPECT_EQ(first.get(), kIOReturnAborted);
    EXPECT_EQ(second.get(), kIOReturnSuccess);
    EXPECT_EQ(Count("D prepare rate=44100"), 0) << "a superseded rate never reaches the device";
    EXPECT_EQ(Count("D prepare rate=32000"), 1);
}

TEST_F(SchedulerTest, StopDuringStartAbortsTheStartAndRollsBack) {
    Hold("device.prepare");
    auto start = std::async(std::launch::async, [&] { return rig.sessions.Attach(rig.guid); });
    WaitHeld();
    auto stop = std::async(std::launch::async, [&] { return rig.sessions.Detach(rig.guid); });
    ASSERT_TRUE(WaitFor([&] { return !Snapshot().halAttached; }));
    Release();

    EXPECT_EQ(start.get(), kIOReturnAborted);
    EXPECT_EQ(stop.get(), kIOReturnSuccess);
    EXPECT_EQ(Count("D break connections"), 1);
    EXPECT_EQ(Count("D confirm"), 0);
    EXPECT_FALSE(rig.sessions.IsStreaming(rig.guid));
}

TEST_F(SchedulerTest, FaultFromAnEndedRunIsDropped) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    const uint64_t firstRun = Snapshot().run;
    ASSERT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          firstRun),
              kIOReturnSuccess);
    const uint64_t secondRun = Snapshot().run;
    ASSERT_GT(secondRun, firstRun);

    rig.bus.Trace().Clear();
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          firstRun),
              kIOReturnAborted);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());
    EXPECT_EQ(Snapshot().run, secondRun);
}

TEST_F(SchedulerTest, FaultRaisedDuringAReconcileIsDropped) {
    IOReturn fault = kIOReturnSuccess;
    rig.hooks["device.program_rx"] = [&] {
        // A replay detector firing while the streams are being brought up.
        fault = rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss);
    };
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(fault, kIOReturnAborted);
    EXPECT_EQ(Count("D prepare"), 1) << "no follow-up restart";
}

TEST_F(SchedulerTest, BusResetDuringAReconcileQueuesOneFollowUpRestart) {
    IOReturn rebind = kIOReturnError;
    rig.hooks["device.program_rx"] = [&] {
        // Raised on the reconciling thread itself: it must not wait on its own reconcile.
        rebind = rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kBusResetRebind);
    };
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(rebind, kIOReturnSuccess);
    EXPECT_EQ(Count("D prepare"), 2);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
}

TEST_F(SchedulerTest, RepeatedFailedFaultRecoveriesStopUntilTheNextAttach) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    for (uint32_t i = 0; i < ASFW::Audio::Session::SessionScheduler::kMaxFaultRestartFailures; ++i) {
        rig.failures["device.prepare"] = kIOReturnTimeout;
        EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
                  kIOReturnTimeout);
    }
    EXPECT_EQ(Snapshot().state, SessionState::Faulted);

    rig.bus.Trace().Clear();
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());

    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
}

TEST_F(SchedulerTest, NonRetryableFailureIsReportedInsteadOfRetried) {
    rig.failures["device.confirm"] = kIOReturnError;
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnError);
    rig.bus.Trace().Clear();
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnError);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());
}

TEST_F(SchedulerTest, TeardownReleasesAWaitingCaller) {
    Hold("device.prepare");
    auto start = std::async(std::launch::async, [&] { return rig.sessions.Attach(rig.guid); });
    WaitHeld();
    auto stop = std::async(std::launch::async, [&] { return rig.sessions.Detach(rig.guid); });
    ASSERT_TRUE(WaitFor([&] { return !Snapshot().halAttached; }));
    rig.cancel.store(true, std::memory_order_release);
    EXPECT_EQ(stop.get(), kIOReturnAborted) << "the waiter leaves before the reconcile ends";
    Release();
    EXPECT_EQ(start.get(), kIOReturnAborted);
    EXPECT_EQ(Count("D break connections"), 0) << "no device traffic after teardown";
}

TEST_F(SchedulerTest, RetiredDeviceAbortsTheStartAndRefusesNewOnes) {
    rig.hooks["device.prepare"] = [&] { rig.sessions.Retire(rig.guid); };
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnAborted);
    EXPECT_EQ(Count("D break connections"), 1);
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnNoDevice);
    EXPECT_TRUE(rig.sessions.IsCancelled(rig.guid));

    rig.sessions.Present(rig.guid);
    EXPECT_FALSE(rig.sessions.IsCancelled(rig.guid));
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
}

} // namespace
