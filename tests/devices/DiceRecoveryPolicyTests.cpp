// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// FW-66 characterization tests for the recovery-policy decision kernel extracted from
// DiceDuplexRestartCoordinator.cpp into DiceRecoveryPolicy.hpp.
//
// The kernel was previously trapped in an anonymous namespace with no direct coverage (only
// transitive coverage via the coordinator's RecoverStreaming integration tests). These tests
// lock the exact current decision table, so the extraction is provably behaviour-preserving
// and the policy is guarded going forward. The functions are constexpr, so the core
// invariants are also asserted at compile time.

#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DiceRecoveryPolicy.hpp"

namespace {

using namespace ASFW::Audio::Backends;

// A context with dependencies satisfied (record + protocol present) and neither stop nor
// idle-apply, so the state/footprint branches are reached. Individual tests tweak fields.
constexpr DiceRecoveryContext WithDeps(DiceRestartState state) noexcept {
    DiceRecoveryContext c{};
    c.state = state;
    c.hasDiceRecord = true;
    c.hasProtocol = true;
    return c;
}

// ---- IsRetryableStatus: exactly four codes retry ----
static_assert(IsRetryableStatus(kIOReturnTimeout));
static_assert(IsRetryableStatus(kIOReturnAborted));
static_assert(IsRetryableStatus(kIOReturnNotReady));
static_assert(IsRetryableStatus(kIOReturnNoDevice));
static_assert(!IsRetryableStatus(kIOReturnSuccess));
static_assert(!IsRetryableStatus(kIOReturnError));

// ---- FailureCauseForReason / IsRecoveryReason ----
static_assert(FailureCauseForReason(DiceRestartReason::kBusResetRebind) ==
              DiceRestartFailureCause::kBusResetRebind);
static_assert(FailureCauseForReason(DiceRestartReason::kRecoverAfterTimingLoss) ==
              DiceRestartFailureCause::kTimingLoss);
static_assert(FailureCauseForReason(DiceRestartReason::kRecoverAfterCycleInconsistent) ==
              DiceRestartFailureCause::kCycleInconsistent);
static_assert(FailureCauseForReason(DiceRestartReason::kRecoverAfterLockLoss) ==
              DiceRestartFailureCause::kLockLoss);
static_assert(FailureCauseForReason(DiceRestartReason::kRecoverAfterTxFault) ==
              DiceRestartFailureCause::kTxFault);
static_assert(FailureCauseForReason(DiceRestartReason::kInitialStart) ==
              DiceRestartFailureCause::kNone);
static_assert(FailureCauseForReason(DiceRestartReason::kManualReconfigure) ==
              DiceRestartFailureCause::kNone);
static_assert(FailureCauseForReason(DiceRestartReason::kSampleRateChange) ==
              DiceRestartFailureCause::kNone);
static_assert(FailureCauseForReason(DiceRestartReason::kClockSourceChange) ==
              DiceRestartFailureCause::kNone);
static_assert(IsRecoveryReason(DiceRestartReason::kBusResetRebind));
static_assert(!IsRecoveryReason(DiceRestartReason::kInitialStart));
static_assert(!IsRecoveryReason(DiceRestartReason::kSampleRateChange));
static_assert(!IsRecoveryReason(DiceRestartReason::kClockSourceChange));
static_assert(!IsRecoveryReason(DiceRestartReason::kManualReconfigure));

// ---- RestartStateForStartReason: reason -> restart state ----
static_assert(RestartStateForStartReason(DiceRestartReason::kBusResetRebind) ==
              DiceRestartState::kRecovering);
static_assert(RestartStateForStartReason(DiceRestartReason::kRecoverAfterTimingLoss) ==
              DiceRestartState::kRecovering);
static_assert(RestartStateForStartReason(DiceRestartReason::kRecoverAfterCycleInconsistent) ==
              DiceRestartState::kRecovering);
static_assert(RestartStateForStartReason(DiceRestartReason::kRecoverAfterLockLoss) ==
              DiceRestartState::kRecovering);
static_assert(RestartStateForStartReason(DiceRestartReason::kRecoverAfterTxFault) ==
              DiceRestartState::kRecovering);
static_assert(RestartStateForStartReason(DiceRestartReason::kInitialStart) ==
              DiceRestartState::kStarting);
static_assert(RestartStateForStartReason(DiceRestartReason::kSampleRateChange) ==
              DiceRestartState::kStarting);
static_assert(RestartStateForStartReason(DiceRestartReason::kClockSourceChange) ==
              DiceRestartState::kStarting);
static_assert(RestartStateForStartReason(DiceRestartReason::kManualReconfigure) ==
              DiceRestartState::kStarting);
// Cross-invariant: a reason enters kRecovering exactly when it has a failure cause.
static_assert(RestartStateForStartReason(DiceRestartReason::kRecoverAfterLockLoss) ==
                  DiceRestartState::kRecovering &&
              IsRecoveryReason(DiceRestartReason::kRecoverAfterLockLoss));
static_assert(RestartStateForStartReason(DiceRestartReason::kManualReconfigure) ==
                  DiceRestartState::kStarting &&
              !IsRecoveryReason(DiceRestartReason::kManualReconfigure));

// ---- EvaluateRecoveryPolicy: compile-time locks on the branch precedence ----
static_assert(EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kIdle)).disposition ==
              DiceRecoveryDisposition::kIgnore);
static_assert(EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kRunning)).disposition ==
              DiceRecoveryDisposition::kRestart);
static_assert(EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kStopping)).reason ==
              DiceRecoveryPolicyReason::kSuppressedByStop);

TEST(DiceRecoveryPolicyTests, RetryableStatusSet) {
    EXPECT_TRUE(IsRetryableStatus(kIOReturnTimeout));
    EXPECT_TRUE(IsRetryableStatus(kIOReturnNoDevice));
    EXPECT_FALSE(IsRetryableStatus(kIOReturnSuccess));
    EXPECT_FALSE(IsRetryableStatus(kIOReturnError));
}

TEST(DiceRecoveryPolicyTests, RecoveryReasonMapping) {
    EXPECT_TRUE(IsRecoveryReason(DiceRestartReason::kRecoverAfterLockLoss));
    EXPECT_FALSE(IsRecoveryReason(DiceRestartReason::kSampleRateChange));
    EXPECT_EQ(FailureCauseForReason(DiceRestartReason::kRecoverAfterTxFault),
              DiceRestartFailureCause::kTxFault);
}

TEST(DiceRecoveryPolicyTests, RestartStateForStartReasonMapping) {
    // Recovery reasons enter kRecovering; deliberate (re)starts enter kStarting.
    EXPECT_EQ(RestartStateForStartReason(DiceRestartReason::kBusResetRebind),
              DiceRestartState::kRecovering);
    EXPECT_EQ(RestartStateForStartReason(DiceRestartReason::kRecoverAfterTxFault),
              DiceRestartState::kRecovering);
    EXPECT_EQ(RestartStateForStartReason(DiceRestartReason::kInitialStart),
              DiceRestartState::kStarting);
    EXPECT_EQ(RestartStateForStartReason(DiceRestartReason::kManualReconfigure),
              DiceRestartState::kStarting);
}

// stop wins over everything, including an otherwise-restartable running session.
TEST(DiceRecoveryPolicyTests, StopRequestedIsSuppressed) {
    DiceRecoveryContext c = WithDeps(DiceRestartState::kRunning);
    c.stopRequested = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kSuppressedByStop);
}

TEST(DiceRecoveryPolicyTests, StoppingStateIsSuppressed) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kStopping));
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kSuppressedByStop);
}

TEST(DiceRecoveryPolicyTests, ApplyingIdleClockIsInvalidated) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kApplyingIdleClock));
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kIdleApplyInvalidated);
}

// Missing record/protocol + an active session (or footprint) -> fail the session.
TEST(DiceRecoveryPolicyTests, MissingDependencyWithActiveSessionFails) {
    DiceRecoveryContext c{};
    c.state = DiceRestartState::kRunning;
    c.hasDiceRecord = false;
    c.hasProtocol = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kFailSession);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kMissingDependency);
}

// Missing dependency but idle with no footprint -> nothing to recover, ignore.
TEST(DiceRecoveryPolicyTests, MissingDependencyWhileIdleIsIgnored) {
    DiceRecoveryContext c{};
    c.state = DiceRestartState::kIdle;
    c.hasDiceRecord = true;
    c.hasProtocol = false;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kIdleWithoutFootprint);
}

TEST(DiceRecoveryPolicyTests, FailedRetryableRestarts) {
    DiceRecoveryContext c = WithDeps(DiceRestartState::kFailed);
    c.lastFailureRetryable = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kRetryableFailure);
}

TEST(DiceRecoveryPolicyTests, FailedNonRetryableFailsSession) {
    DiceRecoveryContext c = WithDeps(DiceRestartState::kFailed);
    c.lastFailureRetryable = false;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kFailSession);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kNonRetryableFailure);
}

TEST(DiceRecoveryPolicyTests, RunningRestarts) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kRunning));
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kRunningWithFootprint);
}

// An idle state but with a footprint still restarts (footprint outlives the state). Each of
// the three OR-branches of hasRestartFootprint independently forces the restart.
TEST(DiceRecoveryPolicyTests, IdleWithHostFootprintRestarts) {
    DiceRecoveryContext c = WithDeps(DiceRestartState::kIdle);
    c.hasHostFootprint = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kRunningWithFootprint);
}

TEST(DiceRecoveryPolicyTests, IdleWithRestartIntentRestarts) {
    DiceRecoveryContext c = WithDeps(DiceRestartState::kIdle);
    c.hasRestartIntent = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kRunningWithFootprint);
}

TEST(DiceRecoveryPolicyTests, IdleWithDeviceFootprintRestarts) {
    DiceRecoveryContext c = WithDeps(DiceRestartState::kIdle);
    c.hasDeviceFootprint = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kRunningWithFootprint);
}

TEST(DiceRecoveryPolicyTests, IdleWithoutFootprintIsIgnored) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DiceRestartState::kIdle));
    EXPECT_EQ(d.disposition, DiceRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DiceRecoveryPolicyReason::kIdleWithoutFootprint);
}

TEST(DiceRecoveryPolicyTests, ToStringCoversAllValues) {
    EXPECT_STREQ(ToString(DiceRecoveryDisposition::kIgnore), "Ignore");
    EXPECT_STREQ(ToString(DiceRecoveryDisposition::kRestart), "Restart");
    EXPECT_STREQ(ToString(DiceRecoveryDisposition::kFailSession), "FailSession");

    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kRunningWithFootprint),
                 "running_with_footprint");
    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kRetryableFailure), "retryable_failure");
    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kIdleWithoutFootprint),
                 "idle_without_footprint");
    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kSuppressedByStop), "suppressed_by_stop");
    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kIdleApplyInvalidated),
                 "idle_apply_invalidated");
    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kMissingDependency), "missing_dependency");
    EXPECT_STREQ(ToString(DiceRecoveryPolicyReason::kNonRetryableFailure),
                 "non_retryable_failure");
}

} // namespace
