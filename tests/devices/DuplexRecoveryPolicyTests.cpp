// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-66 characterization tests for the recovery-policy decision kernel extracted from
// AudioDuplexCoordinator.cpp into DiceRecoveryPolicy.hpp.
//
// The kernel was previously trapped in an anonymous namespace with no direct coverage (only
// transitive coverage via the coordinator's RecoverStreaming integration tests). These tests
// lock the exact current decision table, so the extraction is provably behaviour-preserving
// and the policy is guarded going forward. The functions are constexpr, so the core
// invariants are also asserted at compile time.

#include <gtest/gtest.h>

#include "Audio/Protocols/Backends/DuplexRecoveryPolicy.hpp"

namespace {

using namespace ASFW::Audio;
using namespace ASFW::Audio::Backends;

// A context with dependencies satisfied (record + protocol present) and neither stop nor
// idle-apply, so the state/footprint branches are reached. Individual tests tweak fields.
constexpr DuplexRecoveryContext WithDeps(DuplexLifecycleKind state) noexcept {
    DuplexRecoveryContext c{};
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
static_assert(FailureCauseForReason(DuplexRestartReason::kBusResetRebind) ==
              DuplexRestartFailureCause::kBusResetRebind);
static_assert(FailureCauseForReason(DuplexRestartReason::kRecoverAfterTimingLoss) ==
              DuplexRestartFailureCause::kTimingLoss);
static_assert(FailureCauseForReason(DuplexRestartReason::kRecoverAfterCycleInconsistent) ==
              DuplexRestartFailureCause::kCycleInconsistent);
static_assert(FailureCauseForReason(DuplexRestartReason::kRecoverAfterLockLoss) ==
              DuplexRestartFailureCause::kLockLoss);
static_assert(FailureCauseForReason(DuplexRestartReason::kRecoverAfterTxFault) ==
              DuplexRestartFailureCause::kTxFault);
static_assert(FailureCauseForReason(DuplexRestartReason::kInitialStart) ==
              DuplexRestartFailureCause::kNone);
static_assert(FailureCauseForReason(DuplexRestartReason::kManualReconfigure) ==
              DuplexRestartFailureCause::kNone);
static_assert(FailureCauseForReason(DuplexRestartReason::kSampleRateChange) ==
              DuplexRestartFailureCause::kNone);
static_assert(FailureCauseForReason(DuplexRestartReason::kClockSourceChange) ==
              DuplexRestartFailureCause::kNone);
static_assert(IsRecoveryReason(DuplexRestartReason::kBusResetRebind));
static_assert(!IsRecoveryReason(DuplexRestartReason::kInitialStart));
static_assert(!IsRecoveryReason(DuplexRestartReason::kSampleRateChange));
static_assert(!IsRecoveryReason(DuplexRestartReason::kClockSourceChange));
static_assert(!IsRecoveryReason(DuplexRestartReason::kManualReconfigure));

// ---- RestartStateForStartReason: reason -> restart state ----
static_assert(RestartStateForStartReason(DuplexRestartReason::kBusResetRebind) ==
              DuplexLifecycleKind::Recovering);
static_assert(RestartStateForStartReason(DuplexRestartReason::kRecoverAfterTimingLoss) ==
              DuplexLifecycleKind::Recovering);
static_assert(RestartStateForStartReason(DuplexRestartReason::kRecoverAfterCycleInconsistent) ==
              DuplexLifecycleKind::Recovering);
static_assert(RestartStateForStartReason(DuplexRestartReason::kRecoverAfterLockLoss) ==
              DuplexLifecycleKind::Recovering);
static_assert(RestartStateForStartReason(DuplexRestartReason::kRecoverAfterTxFault) ==
              DuplexLifecycleKind::Recovering);
static_assert(RestartStateForStartReason(DuplexRestartReason::kInitialStart) ==
              DuplexLifecycleKind::Starting);
static_assert(RestartStateForStartReason(DuplexRestartReason::kSampleRateChange) ==
              DuplexLifecycleKind::Starting);
static_assert(RestartStateForStartReason(DuplexRestartReason::kClockSourceChange) ==
              DuplexLifecycleKind::Starting);
static_assert(RestartStateForStartReason(DuplexRestartReason::kManualReconfigure) ==
              DuplexLifecycleKind::Starting);
// Cross-invariant: a reason enters kRecovering exactly when it has a failure cause.
static_assert(RestartStateForStartReason(DuplexRestartReason::kRecoverAfterLockLoss) ==
                  DuplexLifecycleKind::Recovering &&
              IsRecoveryReason(DuplexRestartReason::kRecoverAfterLockLoss));
static_assert(RestartStateForStartReason(DuplexRestartReason::kManualReconfigure) ==
                  DuplexLifecycleKind::Starting &&
              !IsRecoveryReason(DuplexRestartReason::kManualReconfigure));

// ---- EvaluateRecoveryPolicy: compile-time locks on the branch precedence ----
static_assert(EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::Idle)).disposition ==
              DuplexRecoveryDisposition::kIgnore);
static_assert(EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::Running)).disposition ==
              DuplexRecoveryDisposition::kRestart);
static_assert(EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::Stopping)).reason ==
              DuplexRecoveryPolicyReason::kSuppressedByStop);

TEST(DiceRecoveryPolicyTests, RetryableStatusSet) {
    EXPECT_TRUE(IsRetryableStatus(kIOReturnTimeout));
    EXPECT_TRUE(IsRetryableStatus(kIOReturnNoDevice));
    EXPECT_FALSE(IsRetryableStatus(kIOReturnSuccess));
    EXPECT_FALSE(IsRetryableStatus(kIOReturnError));
}

TEST(DiceRecoveryPolicyTests, RecoveryReasonMapping) {
    EXPECT_TRUE(IsRecoveryReason(DuplexRestartReason::kRecoverAfterLockLoss));
    EXPECT_FALSE(IsRecoveryReason(DuplexRestartReason::kSampleRateChange));
    EXPECT_EQ(FailureCauseForReason(DuplexRestartReason::kRecoverAfterTxFault),
              DuplexRestartFailureCause::kTxFault);
}

TEST(DiceRecoveryPolicyTests, RestartStateForStartReasonMapping) {
    // Recovery reasons enter kRecovering; deliberate (re)starts enter kStarting.
    EXPECT_EQ(RestartStateForStartReason(DuplexRestartReason::kBusResetRebind),
              DuplexLifecycleKind::Recovering);
    EXPECT_EQ(RestartStateForStartReason(DuplexRestartReason::kRecoverAfterTxFault),
              DuplexLifecycleKind::Recovering);
    EXPECT_EQ(RestartStateForStartReason(DuplexRestartReason::kInitialStart),
              DuplexLifecycleKind::Starting);
    EXPECT_EQ(RestartStateForStartReason(DuplexRestartReason::kManualReconfigure),
              DuplexLifecycleKind::Starting);
}

// stop wins over everything, including an otherwise-restartable running session.
TEST(DiceRecoveryPolicyTests, StopRequestedIsSuppressed) {
    DuplexRecoveryContext c = WithDeps(DuplexLifecycleKind::Running);
    c.stopRequested = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kSuppressedByStop);
}

TEST(DiceRecoveryPolicyTests, StoppingStateIsSuppressed) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::Stopping));
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kSuppressedByStop);
}

TEST(DiceRecoveryPolicyTests, ApplyingIdleClockIsInvalidated) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::ApplyingIdleClock));
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kIdleApplyInvalidated);
}

// Missing record/protocol + an active session (or footprint) -> fail the session.
TEST(DiceRecoveryPolicyTests, MissingDependencyWithActiveSessionFails) {
    DuplexRecoveryContext c{};
    c.state = DuplexLifecycleKind::Running;
    c.hasDiceRecord = false;
    c.hasProtocol = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kFailSession);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kMissingDependency);
}

// Missing dependency but idle with no footprint -> nothing to recover, ignore.
TEST(DiceRecoveryPolicyTests, MissingDependencyWhileIdleIsIgnored) {
    DuplexRecoveryContext c{};
    c.state = DuplexLifecycleKind::Idle;
    c.hasDiceRecord = true;
    c.hasProtocol = false;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kIdleWithoutFootprint);
}

TEST(DiceRecoveryPolicyTests, FailedRetryableRestarts) {
    DuplexRecoveryContext c = WithDeps(DuplexLifecycleKind::Failed);
    c.lastFailureRetryable = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kRetryableFailure);
}

TEST(DiceRecoveryPolicyTests, FailedNonRetryableFailsSession) {
    DuplexRecoveryContext c = WithDeps(DuplexLifecycleKind::Failed);
    c.lastFailureRetryable = false;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kFailSession);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kNonRetryableFailure);
}

TEST(DiceRecoveryPolicyTests, RunningRestarts) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::Running));
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kRunningWithFootprint);
}

// An idle state but with a footprint still restarts (footprint outlives the state). Each of
// the three OR-branches of hasRestartFootprint independently forces the restart.
TEST(DiceRecoveryPolicyTests, IdleWithHostFootprintRestarts) {
    DuplexRecoveryContext c = WithDeps(DuplexLifecycleKind::Idle);
    c.hasHostFootprint = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kRunningWithFootprint);
}

TEST(DiceRecoveryPolicyTests, IdleWithRestartIntentRestarts) {
    DuplexRecoveryContext c = WithDeps(DuplexLifecycleKind::Idle);
    c.hasRestartIntent = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kRunningWithFootprint);
}

TEST(DiceRecoveryPolicyTests, IdleWithDeviceFootprintRestarts) {
    DuplexRecoveryContext c = WithDeps(DuplexLifecycleKind::Idle);
    c.hasDeviceFootprint = true;
    const auto d = EvaluateRecoveryPolicy(c);
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kRestart);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kRunningWithFootprint);
}

TEST(DiceRecoveryPolicyTests, IdleWithoutFootprintIsIgnored) {
    const auto d = EvaluateRecoveryPolicy(WithDeps(DuplexLifecycleKind::Idle));
    EXPECT_EQ(d.disposition, DuplexRecoveryDisposition::kIgnore);
    EXPECT_EQ(d.reason, DuplexRecoveryPolicyReason::kIdleWithoutFootprint);
}

TEST(DiceRecoveryPolicyTests, ToStringCoversAllValues) {
    EXPECT_STREQ(ToString(DuplexRecoveryDisposition::kIgnore), "Ignore");
    EXPECT_STREQ(ToString(DuplexRecoveryDisposition::kRestart), "Restart");
    EXPECT_STREQ(ToString(DuplexRecoveryDisposition::kFailSession), "FailSession");

    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kRunningWithFootprint),
                 "running_with_footprint");
    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kRetryableFailure), "retryable_failure");
    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kIdleWithoutFootprint),
                 "idle_without_footprint");
    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kSuppressedByStop), "suppressed_by_stop");
    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kIdleApplyInvalidated),
                 "idle_apply_invalidated");
    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kMissingDependency), "missing_dependency");
    EXPECT_STREQ(ToString(DuplexRecoveryPolicyReason::kNonRetryableFailure),
                 "non_retryable_failure");
}

} // namespace
