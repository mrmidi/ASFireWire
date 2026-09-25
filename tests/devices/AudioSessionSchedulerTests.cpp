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

#include "Bus/IRM/IRMTypes.hpp"

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
// The Apogee recipe: pre-stream clock gate, interleaved host starts, staged stop.
const SessionShape kApogeeDevice{"apogee-duet", Ids::kApogeeVendorId, Ids::kApogeeDuetModelId,
                                 kAvcUnitSpecifier, kAvcUnitVersion};
const SessionShape kDiceDevice{"pro24dsp", Ids::kFocusriteVendorId, Ids::kSPro24DspModelId,
                               Ids::kFocusriteVendorId, kDiceUnitVersion,
                               &ASFW::Testing::DICE::DiceDeviceImages::kSaffirePro24Dsp, true};

template <const SessionShape& Shape>
struct ShapedSchedulerTest : ::testing::Test {
    SessionRig rig{Shape};

    ScriptedDeviceControl& Device() { return static_cast<ScriptedDeviceControl&>(*rig.protocol); }

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

using SchedulerTest = ShapedSchedulerTest<kCmpDevice>;
using ApogeeTest = ShapedSchedulerTest<kApogeeDevice>;
using DiceQuietPeriodTest = ShapedSchedulerTest<kDiceDevice>;

TEST_F(DiceQuietPeriodTest, EachEventRearmsTheQuietPeriod) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    const uint64_t firstRun = Snapshot().run;
    ASSERT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          firstRun), kIOReturnSuccess);
    EXPECT_EQ(rig.sessionTimer.PendingCount(), 1U);
    rig.Wait(300);
    ASSERT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          firstRun), kIOReturnSuccess);
    rig.Wait(399);
    EXPECT_EQ(Snapshot().run, firstRun);
    rig.Wait(1);
    EXPECT_EQ(Snapshot().run, firstRun + 1);
    EXPECT_EQ(rig.sessionTimer.PendingCount(), 0U);
}

TEST_F(DiceQuietPeriodTest, DetachCancelsPendingRestart) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    ASSERT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          Snapshot().run), kIOReturnSuccess);
    ASSERT_EQ(rig.sessionTimer.PendingCount(), 1U);
    ASSERT_EQ(rig.sessions.Detach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(rig.sessionTimer.PendingCount(), 0U);
    rig.Wait(1000);
    EXPECT_EQ(Count("D prepare"), 0);
    EXPECT_EQ(Snapshot().state, SessionState::Idle);
}

TEST_F(DiceQuietPeriodTest, ClockChangeCancelsPendingRestart) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    ASSERT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          Snapshot().run), kIOReturnSuccess);
    ASSERT_EQ(rig.sessionTimer.PendingCount(), 1U);
    ASSERT_EQ(rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 44100},
                                       DuplexRestartReason::kSampleRateChange), kIOReturnSuccess);
    const uint64_t runAfterClock = Snapshot().run;
    EXPECT_EQ(rig.sessionTimer.PendingCount(), 0U);
    rig.Wait(1000);
    EXPECT_EQ(Snapshot().run, runAfterClock);
}

TEST_F(DiceQuietPeriodTest, TeardownCancelsPendingRestart) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    const uint64_t run = Snapshot().run;
    ASSERT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss,
                                          run), kIOReturnSuccess);
    rig.cancel.store(true, std::memory_order_release);
    rig.sessions.BeginTeardown();
    EXPECT_EQ(rig.sessionTimer.PendingCount(), 0U);
    rig.Wait(1000);
    EXPECT_EQ(Snapshot().run, run);
}

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

TEST_F(SchedulerTest, RestartObserverWaitsForQueuedReconcile) {
    std::promise<void> held;
    std::promise<void> release;
    auto releaseFuture = release.get_future();
    int prepareCalls = 0;
    rig.hooks["device.prepare"] = [&] {
        if (++prepareCalls == 1) {
            held.set_value();
            releaseFuture.wait();
        }
    };
    std::atomic<int> observed{0};
    rig.sessions.SetRestartObserver([&](uint64_t) { observed.fetch_add(1); });
    auto start = std::async(std::launch::async, [&] { return rig.sessions.Attach(rig.guid); });
    held.get_future().wait();
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kBusResetRebind),
              kIOReturnSuccess);
    EXPECT_EQ(observed.load(), 0);
    release.set_value();
    EXPECT_EQ(start.get(), kIOReturnSuccess);
    EXPECT_EQ(observed.load(), 1);
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


// ---------------------------------------------------------------------------
// Carried over from the deleted AudioDuplexCoordinatorTests: behaviour the
// session goldens do not pin.
// ---------------------------------------------------------------------------

// 9 host->device AM824 slots x 8 events x 4 bytes + an 8-byte CIP header.
constexpr uint32_t kPlaybackPayloadBytes = 8 + 8 * 9 * 4;

[[nodiscard]] std::string ReservePlaybackLine(ASFW::FW::FwSpeed speed) {
    return "H reserve playback allowed=ffffffffffffffff bw=" +
           std::to_string(ASFW::IRM::PacketBandwidthUnits(kPlaybackPayloadBytes,
                                                           static_cast<uint8_t>(speed)));
}

// The IRM is charged at the speed the transmit context runs at. Charging for
// one speed and transmitting at another is how a reservation that fits turns
// into packets the bus was not paid for.
TEST_F(SchedulerTest, TransmitSpeedIsTheSpeedTheReservationWasChargedAt) {
    rig.link = {.localToNode = ASFW::FW::FwSpeed::S200, .isochToNode = ASFW::FW::FwSpeed::S200};
    rig.Install(ASFW::FW::Generation{1});
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(Count(ReservePlaybackLine(ASFW::FW::FwSpeed::S200)), 1);
    EXPECT_EQ(Count("H prepare tx ch=0 sid=0 @s200"), 1);
}

// Isochronous speed comes from Self-ID, never from async outcomes: SpeedPolicy
// demotes the async speed when a device times out a request, and isoch used to
// inherit it, doubling the bandwidth charge. Apple keeps them apart
// (IOFWIsochChannel.cpp:653 vs IOFireWireController.cpp:2755-2759).
TEST_F(SchedulerTest, AsyncSpeedDemotionDoesNotLowerIsochronousSpeed) {
    rig.link = {.localToNode = ASFW::FW::FwSpeed::S200, .isochToNode = ASFW::FW::FwSpeed::S400};
    rig.Install(ASFW::FW::Generation{1});
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(Count(ReservePlaybackLine(ASFW::FW::FwSpeed::S400)), 1);
    EXPECT_EQ(Count("H prepare tx ch=0 sid=0 @s400"), 1);
}

TEST_F(SchedulerTest, S100IsochronousPathIsNotAnUnsetSpeed) {
    rig.link = {.localToNode = ASFW::FW::FwSpeed::S400, .isochToNode = ASFW::FW::FwSpeed::S100};
    rig.Install(ASFW::FW::Generation{1});
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(Count("H reserve playback allowed=ffffffffffffffff bw=1232"), 1);
    EXPECT_EQ(Count("H prepare tx ch=0 sid=0 @s100"), 1);
}

// A confirmation the device takes its time over keeps the start going: nothing
// is stopped while it is pending, and the start completes when it arrives.
TEST_F(SchedulerTest, SlowConfirmationKeepsTheHostRunningUntilItArrives) {
    Hold("device.confirm");
    auto start = std::async(std::launch::async, [&] { return rig.sessions.Attach(rig.guid); });
    WaitHeld();
    EXPECT_EQ(Count("H stop"), 0);
    EXPECT_EQ(Count("D stop"), 0);
    Release();
    EXPECT_EQ(start.get(), kIOReturnSuccess);
    EXPECT_EQ(Count("H stop"), 0);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
}

TEST_F(SchedulerTest, TeardownDuringPrepareAbortsWithoutTouchingTheDevice) {
    rig.hooks["device.prepare"] = [&] { rig.cancel.store(true, std::memory_order_release); };
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnAborted);
    EXPECT_EQ(Count("D program"), 0);
    EXPECT_EQ(Count("H stop"), 0);
    EXPECT_EQ(Count("D break"), 0);
    EXPECT_EQ(rig.sessions.TeardownAbortCount(), 1U);
}

TEST_F(SchedulerTest, RetryableFailureRestartsOnRecovery) {
    rig.failures["device.program_rx"] = kIOReturnTimeout;
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnTimeout);
    EXPECT_EQ(Snapshot().state, SessionState::Failed);

    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnSuccess);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
    EXPECT_EQ(Snapshot().lastStatus, kIOReturnSuccess);
}

// While CoreAudio runs the streams, a recovery restart goes to the host, whose
// StopIO -> StartIO rebuilds the audio-owned TX queue. Restarting in place kept
// the old queue and the transmit prime refused it (hardware, 2026-09-25: a
// bus-reset burst while playing left the Pro 24 DSP silent, session failed).
TEST_F(SchedulerTest, RestartWhileCoreAudioRunsIsHandedToTheHost) {
    std::atomic<int> routed{0};
    DuplexRestartReason routedReason{};
    rig.sessions.SetHostRestartRouter([&](uint64_t guid, DuplexRestartReason reason) {
        EXPECT_EQ(guid, rig.guid);
        routedReason = reason;
        routed.fetch_add(1);
        return true;
    });
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    const size_t linesBefore = rig.bus.Trace().Lines().size();

    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kBusResetRebind),
              kIOReturnSuccess);
    EXPECT_EQ(routed.load(), 1);
    EXPECT_EQ(routedReason, DuplexRestartReason::kBusResetRebind);
    // Nothing was rebuilt here: the host's StopIO/StartIO does it.
    EXPECT_EQ(rig.bus.Trace().Lines().size(), linesBefore);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
}

// No audio driver listening (the router declines): restart in place, as before.
TEST_F(SchedulerTest, RestartRunsInPlaceWhenNoHostTakesIt) {
    std::atomic<int> routed{0};
    rig.sessions.SetHostRestartRouter([&](uint64_t, DuplexRestartReason) {
        routed.fetch_add(1);
        return false;
    });
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    const size_t linesBefore = rig.bus.Trace().Lines().size();

    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kBusResetRebind),
              kIOReturnSuccess);
    EXPECT_EQ(routed.load(), 1);
    EXPECT_GT(rig.bus.Trace().Lines().size(), linesBefore);
    EXPECT_TRUE(rig.sessions.IsStreaming(rig.guid));
}

// With CoreAudio detached there is no IO to bounce: the host is never asked.
TEST_F(SchedulerTest, NoHostHandoverWhileCoreAudioIsDetached) {
    std::atomic<int> routed{0};
    rig.sessions.SetHostRestartRouter([&](uint64_t, DuplexRestartReason) {
        routed.fetch_add(1);
        return true;
    });
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);
    EXPECT_EQ(routed.load(), 0);
}

// Unsupported, not success (FW-146): an ignored recovery ran nothing, and a
// caller that reads success resets budgets it should keep.
TEST_F(SchedulerTest, RecoveryIsIgnoredWhileNothingShouldRun) {
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);
    ASSERT_EQ(rig.sessions.Detach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnUnsupported);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());
}

// 2x/4x rates are not validated end to end and are refused before any host
// allocation.
TEST_F(SchedulerTest, UnsupportedClockIsRefusedBeforeAnyWork) {
    EXPECT_EQ(rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 96000},
                                       DuplexRestartReason::kSampleRateChange),
              kIOReturnUnsupported);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());
    EXPECT_EQ(Snapshot().desiredClock.sampleRateHz, 0U);
}

// A published endpoint whose geometry changed must be recreated before any
// start, recovery or clock change; rediscovery does not reopen it.
TEST_F(SchedulerTest, ChangedEndpointGeometryRefusesStartRecoveryAndClock) {
    rig.sessions.SetStartGuard([](uint64_t) { return false; });
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnNotReady);
    EXPECT_EQ(rig.sessions.RequestRestart(rig.guid, DuplexRestartReason::kRecoverAfterTimingLoss),
              kIOReturnNotReady);
    EXPECT_EQ(rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 44100},
                                       DuplexRestartReason::kSampleRateChange),
              kIOReturnNotReady);
    rig.sessions.Present(rig.guid);
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnNotReady);
    EXPECT_TRUE(rig.bus.Trace().Lines().empty());
}

TEST_F(ApogeeTest, ClockGateNeedsConsecutiveStableReads) {
    Device().healthLocked = {true, false, true, true, true};
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(Count("D health"), 5);
}

TEST_F(ApogeeTest, ClockGateFailureRollsBackBeforeAnyHostStart) {
    rig.failures["device.health"] = kIOReturnNoDevice;
    EXPECT_EQ(rig.sessions.Attach(rig.guid), kIOReturnNoDevice);
    EXPECT_EQ(Count("H start"), 0);
    EXPECT_EQ(Count("D break connections"), 1);
    EXPECT_EQ(Count("H stop all"), 1);
}

// The Duet runs at 48 kHz until dynamic rate changes exist: a start after a
// manual 44.1 kHz request still prepares the device at 48 kHz.
TEST_F(ApogeeTest, StartIsPinnedTo48kEvenAfterAnotherRateWasApplied) {
    ASSERT_EQ(rig.sessions.ChangeClock(rig.guid, AudioClockConfig{.sampleRateHz = 44100},
                                       DuplexRestartReason::kManualReconfigure),
              kIOReturnSuccess);
    EXPECT_EQ(Count("D apply clock rate=44100"), 1);
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    EXPECT_EQ(Count("D prepare rate=48000"), 1);
    EXPECT_EQ(Snapshot().appliedClock.sampleRateHz, 48000U);
}

// FW-61: the staged stop reaches both directions even when steps fail, and
// reports the failure, so a context that did not quiesce is not treated as
// safely releasable.
TEST_F(ApogeeTest, StagedStopFinishesAndReportsTheFirstHostFailure) {
    ASSERT_EQ(rig.sessions.Attach(rig.guid), kIOReturnSuccess);
    rig.bus.Trace().Clear();
    rig.failures["device.disconnect_playback"] = kIOReturnTimeout;
    rig.failures["device.disconnect_capture"] = kIOReturnError;
    rig.failures["host.stop_transmit"] = kIOReturnError;
    rig.failures["host.stop_receive"] = kIOReturnTimeout;
    EXPECT_EQ(rig.sessions.Detach(rig.guid), kIOReturnError);
    EXPECT_EQ(Count("D disconnect playback"), 1);
    EXPECT_EQ(Count("H stop tx"), 1);
    EXPECT_EQ(Count("D disconnect capture"), 1);
    EXPECT_EQ(Count("H stop rx"), 1);
    EXPECT_EQ(Count("H stop all"), 1);
    EXPECT_EQ(Count("D stop"), 0);
}

} // namespace
