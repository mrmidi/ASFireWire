// WireRetentionTests.cpp - the decisions that keep a device's wire across
// CoreAudio IO (ASFWDriver/Audio/Runtime/WireRetention.hpp).

#include "Audio/Runtime/WireRetention.hpp"

#include <gtest/gtest.h>

using ASFW::Audio::Runtime::WireReleaseReason;
using ASFW::Audio::Runtime::WireRetention;
using ASFW::Audio::Runtime::WireStartPlan;
using ASFW::Audio::Runtime::WireState;
using ASFW::Audio::Runtime::WireStopPlan;

namespace {

constexpr uint32_t k48k = 48000;
constexpr uint32_t k44k = 44100;

WireRetention LiveAt(uint32_t rate) {
    WireRetention w;
    w.OnStarted(rate);
    return w;
}

WireRetention RetainedAt(uint32_t rate) {
    WireRetention w = LiveAt(rate);
    w.OnRetained();
    return w;
}

} // namespace

TEST(WireRetention, NoWireStartsFresh) {
    WireRetention w;
    EXPECT_EQ(w.State(), WireState::kDown);
    EXPECT_EQ(w.PlanStart(k48k, true, nullptr), WireStartPlan::kFreshStart);
}

TEST(WireRetention, LiveWireStopIsRetainedWhenProfileOptsIn) {
    WireRetention w = LiveAt(k48k);
    EXPECT_EQ(w.PlanStop(true, true, nullptr), WireStopPlan::kRetain);
}

TEST(WireRetention, StopReleasesWhenProfileDoesNotOptIn) {
    WireRetention w = LiveAt(k48k);
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStop(false, true, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kNotRetainable);
}

TEST(WireRetention, StopReleasesAfterTransportFault) {
    WireRetention w = LiveAt(k48k);
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStop(true, false, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kTransportFault);
}

TEST(WireRetention, IoRestartForcesTheStopInsideTheWindowToRelease) {
    // The IO restart rebuilds the TX queue; keeping the wire would keep the
    // queue the restart exists to replace.
    WireRetention w = LiveAt(k48k);
    w.OnIoRestartRequested();
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStop(true, true, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kIoRestart);
}

TEST(WireRetention, RetainedWireAtSameRateIsRejoined) {
    WireRetention w = RetainedAt(k48k);
    EXPECT_EQ(w.PlanStart(k48k, true, nullptr), WireStartPlan::kRejoin);
}

TEST(WireRetention, RetainedWireAtOtherRateIsReleasedFirst) {
    WireRetention w = RetainedAt(k48k);
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStart(k44k, true, &reason), WireStartPlan::kReleaseThenFresh);
    EXPECT_EQ(reason, WireReleaseReason::kRateMismatch);
}

TEST(WireRetention, RetainedWireAfterFaultIsReleasedFirst) {
    WireRetention w = RetainedAt(k48k);
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStart(k48k, false, &reason), WireStartPlan::kReleaseThenFresh);
    EXPECT_EQ(reason, WireReleaseReason::kTransportFault);
}

TEST(WireRetention, RetainedWireWithPendingIoRestartIsReleasedFirst) {
    WireRetention w = RetainedAt(k48k);
    w.OnIoRestartRequested();
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStart(k48k, true, &reason), WireStartPlan::kReleaseThenFresh);
    EXPECT_EQ(reason, WireReleaseReason::kIoRestart);
}

TEST(WireRetention, StartTimeoutKeepsTheWarmingWire) {
    // The Phase 88 log: a fresh start saw only NO-DATA for 4 s; tearing the
    // wire down restarted the device's warm-up on every HAL retry.
    WireRetention w;
    EXPECT_EQ(w.PlanStartTimeout(true, true, nullptr), WireStopPlan::kRetain);
    w.OnStartTimedOutRetained(k48k);
    EXPECT_EQ(w.State(), WireState::kRetained);
    EXPECT_EQ(w.ConsecutiveStartTimeouts(), 1U);
    EXPECT_EQ(w.PlanStart(k48k, true, nullptr), WireStartPlan::kRejoin);
}

TEST(WireRetention, ConsecutiveStartTimeoutsEventuallyRelease) {
    WireRetention w;
    for (uint32_t i = 0; i + 1 < WireRetention::kMaxConsecutiveStartTimeouts; ++i) {
        ASSERT_EQ(w.PlanStartTimeout(true, true, nullptr), WireStopPlan::kRetain) << i;
        w.OnStartTimedOutRetained(k48k);
    }
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStartTimeout(true, true, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kTimeoutLimit);
}

TEST(WireRetention, SuccessfulStartClearsTheTimeoutCount) {
    WireRetention w;
    w.OnStartTimedOutRetained(k48k);
    w.OnStartTimedOutRetained(k48k);
    w.OnStarted(k48k);
    EXPECT_EQ(w.ConsecutiveStartTimeouts(), 0U);
    EXPECT_EQ(w.State(), WireState::kLive);
}

TEST(WireRetention, StartTimeoutReleasesWhenNotRetainable) {
    WireRetention w;
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStartTimeout(false, true, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kNotRetainable);
}

TEST(WireRetention, StartTimeoutReleasesAfterFault) {
    WireRetention w;
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStartTimeout(true, false, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kTransportFault);
}

TEST(WireRetention, OnlyARetainedWireNeedsReleaseOnInvalidation) {
    // While IO runs, CoreAudio's StopIO releases (or the window's StopIO does);
    // only an idle kept wire has nobody else to release it.
    WireRetention w;
    EXPECT_FALSE(w.MustReleaseOnInvalidation());
    w.OnStarted(k48k);
    EXPECT_FALSE(w.MustReleaseOnInvalidation());
    w.OnRetained();
    EXPECT_TRUE(w.MustReleaseOnInvalidation());
}

// An IO restart requested while a start is finishing must still release the
// wire at the window's StopIO: a start does not swallow it. (The follow-up
// restart after a DICE reconfiguration lands about when StartIO completes.)
TEST(WireRetention, IoRestartRequestedDuringAStartSurvivesIt) {
    WireRetention w;
    w.OnIoRestartRequested();
    w.OnStarted(k48k);
    EXPECT_TRUE(w.IoRestartPending());
    WireReleaseReason reason{};
    EXPECT_EQ(w.PlanStop(true, true, &reason), WireStopPlan::kRelease);
    EXPECT_EQ(reason, WireReleaseReason::kIoRestart);
}

TEST(WireRetention, ReleaseResetsEverything) {
    WireRetention w = RetainedAt(k48k);
    w.OnIoRestartRequested();
    w.OnReleased();
    EXPECT_EQ(w.State(), WireState::kDown);
    EXPECT_EQ(w.RateHz(), 0U);
    EXPECT_FALSE(w.IoRestartPending());
    EXPECT_EQ(w.PlanStart(k48k, true, nullptr), WireStartPlan::kFreshStart);
}
