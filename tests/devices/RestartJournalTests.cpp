// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-69a characterization tests for RestartJournal — the FSM-journal state mutators extracted
// from AudioDuplexCoordinator into RestartJournal.hpp. These pin the state-mutating half
// by EFFECT on DuplexRestartSession. The Log* emitters are void and observable only via the field
// trace, which is a no-op in host tests (os_log stubbed), so their byte-identical strings are
// preserved by the verbatim whole-body move and exercised by the coordinator integration suite.
// Host tests, no hardware.

#include <gtest/gtest.h>

#include <optional>

#include "Audio/Protocols/Backends/RestartJournal.hpp"

namespace {

using namespace ASFW::Audio;
using namespace ASFW::Audio::Backends;

// The 10 progress flags ClearRestartProgress (and thus EnterIdle/EnterFailed) must reset.
void SetAllProgressFlags(DuplexRestartSession& s) {
    s.ownerClaimed = true;
    s.devicePrepared = true;
    s.deviceRxProgrammed = true;
    s.deviceTxArmed = true;
    s.deviceRunning = true;
    s.hostDuplexClaimed = true;
    s.hostPlaybackReserved = true;
    s.hostCaptureReserved = true;
    s.hostReceiveStarted = true;
    s.hostTransmitStarted = true;
}

void ExpectAllProgressCleared(const DuplexRestartSession& s) {
    EXPECT_FALSE(s.ownerClaimed);
    EXPECT_FALSE(s.devicePrepared);
    EXPECT_FALSE(s.deviceRxProgrammed);
    EXPECT_FALSE(s.deviceTxArmed);
    EXPECT_FALSE(s.deviceRunning);
    EXPECT_FALSE(s.hostDuplexClaimed);
    EXPECT_FALSE(s.hostPlaybackReserved);
    EXPECT_FALSE(s.hostCaptureReserved);
    EXPECT_FALSE(s.hostReceiveStarted);
    EXPECT_FALSE(s.hostTransmitStarted);
}

TEST(RestartJournalTests, SetSessionStateSetsStateOnly) {
    DuplexRestartSession s{};
    s.phase = DuplexRestartPhase::kPreparingDevice;
    SetSessionState(s, LifecycleStarting{}, "start_requested");
    EXPECT_EQ(KindOf(s.lifecycle), DuplexLifecycleKind::Starting);
    EXPECT_EQ(s.phase, DuplexRestartPhase::kPreparingDevice);  // untouched
}

TEST(RestartJournalTests, SetSessionPhaseSetsPhaseOnly) {
    DuplexRestartSession s{};
    s.lifecycle = LifecycleRunning{};
    SetSessionPhase(s, DuplexRestartPhase::kPreparingDevice);
    EXPECT_EQ(s.phase, DuplexRestartPhase::kPreparingDevice);
    EXPECT_EQ(KindOf(s.lifecycle), DuplexLifecycleKind::Running);  // untouched
}

// EnterIdle: lifecycle -> Idle, ledger cleared. No error concept: an Idle
// terminal carries no terminal error by construction.
TEST(RestartJournalTests, EnterIdleResetsToIdle) {
    DuplexRestartSession s{};
    s.lifecycle = LifecycleRunning{};
    s.phase = DuplexRestartPhase::kRunning;
    SetAllProgressFlags(s);
    EnterIdle(s, "reset_before_start");
    EXPECT_EQ(s.phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(s.lifecycle), DuplexLifecycleKind::Idle);
    ExpectAllProgressCleared(s);
}

// EnterFailed: lifecycle -> Failed ARMED with the terminal error. The error is
// the alternative's payload — the single authority, no session-level field.
TEST(RestartJournalTests, EnterFailedArmsTerminalErrorOnTheAlternative) {
    DuplexRestartSession s{};
    s.lifecycle = LifecycleRunning{};
    s.phase = DuplexRestartPhase::kRunning;
    SetAllProgressFlags(s);
    EnterFailed(s, kIOReturnError, "stop_failed");
    EXPECT_EQ(s.phase, DuplexRestartPhase::kFailed);
    ASSERT_TRUE(std::holds_alternative<LifecycleFailed>(s.lifecycle));
    EXPECT_EQ(std::get<LifecycleFailed>(s.lifecycle).terminalError, kIOReturnError);
    ExpectAllProgressCleared(s);
}

// EnterFailed over an already-armed Failed re-arms the payload (last failure wins).
TEST(RestartJournalTests, EnterFailedReArmsPayloadOnExistingFailure) {
    DuplexRestartSession s{};
    s.lifecycle = LifecycleFailed{.terminalError = kIOReturnError};
    s.phase = DuplexRestartPhase::kFailed;
    SetAllProgressFlags(s);
    EnterFailed(s, kIOReturnTimeout, "re-failed");
    ASSERT_TRUE(std::holds_alternative<LifecycleFailed>(s.lifecycle));
    EXPECT_EQ(std::get<LifecycleFailed>(s.lifecycle).terminalError, kIOReturnTimeout);
}

TEST(RestartJournalTests, ClearFailureSnapshotResetsLastFailure) {
    DuplexRestartSession s{};
    s.lastFailure = DuplexRestartIssueInfo{};
    ASSERT_TRUE(s.lastFailure.has_value());
    ClearFailureSnapshot(s);
    EXPECT_FALSE(s.lastFailure.has_value());
}

// RecordIssue fills the destination optional with the passed facts and copies restartId +
// generation from the session (the epoch stamp), leaving the session's own fields untouched.
TEST(RestartJournalTests, RecordIssuePopulatesAndStampsSessionEpoch) {
    DuplexRestartSession s{};
    s.restartId = 7;
    std::optional<DuplexRestartIssueInfo> dest;
    RecordIssue(s, dest,
                DuplexRestartPhase::kPreparingDevice,
                DuplexRestartErrorClass::kStageFailure,
                DuplexRestartFailureCause::kTimingLoss,
                kIOReturnError,
                /*retryable=*/true,
                /*rollbackAttempted=*/false,
                /*rollbackStatus=*/kIOReturnSuccess,
                /*hostStateKnown=*/true,
                /*deviceStateKnown=*/false);
    ASSERT_TRUE(dest.has_value());
    EXPECT_EQ(dest->failedPhase, DuplexRestartPhase::kPreparingDevice);
    EXPECT_EQ(dest->errorClass, DuplexRestartErrorClass::kStageFailure);
    EXPECT_EQ(dest->cause, DuplexRestartFailureCause::kTimingLoss);
    EXPECT_EQ(dest->status, kIOReturnError);
    EXPECT_TRUE(dest->retryable);
    EXPECT_FALSE(dest->rollbackAttempted);
    EXPECT_TRUE(dest->hostStateKnown);
    EXPECT_FALSE(dest->deviceStateKnown);
    EXPECT_EQ(dest->restartId, 7u);                                  // stamped from session
    EXPECT_EQ(dest->generation.value, s.topologyGeneration.value);   // stamped from session
}

struct ScopedDisableLifecycleAssert {
    ScopedDisableLifecycleAssert() { gDisableLifecycleAssertForTesting.store(true); }
    ~ScopedDisableLifecycleAssert() { gDisableLifecycleAssertForTesting.store(false); }
};

// Illegal transition (Removed -> Running) fails through SetLifecycle with assertions disabled.
// The session lifecycle, phase, and progress flags must remain strictly unchanged.
TEST(RestartJournalTests, IllegalTransitionFailsAndLeavesStateAndLedgerUnchanged) {
    ScopedDisableLifecycleAssert disableAssert;
    DuplexRestartSession s{};
    s.lifecycle = LifecycleRemoved{};
    s.phase = DuplexRestartPhase::kFailed;
    SetAllProgressFlags(s);

    const bool ok = SetLifecycle(s, LifecycleRunning{}, "illegal_test");
    EXPECT_FALSE(ok);
    EXPECT_EQ(KindOf(s.lifecycle), DuplexLifecycleKind::Removed);
    EXPECT_EQ(s.phase, DuplexRestartPhase::kFailed);
    // Cleanup ledger remains untouched
    EXPECT_TRUE(s.ownerClaimed);
    EXPECT_TRUE(s.devicePrepared);
    EXPECT_TRUE(s.deviceRxProgrammed);
    EXPECT_TRUE(s.deviceTxArmed);
    EXPECT_TRUE(s.deviceRunning);
    EXPECT_TRUE(s.hostDuplexClaimed);
    EXPECT_TRUE(s.hostPlaybackReserved);
    EXPECT_TRUE(s.hostCaptureReserved);
    EXPECT_TRUE(s.hostReceiveStarted);
    EXPECT_TRUE(s.hostTransmitStarted);
}

// EnterFailed called from an illegal state (Removed -> Failed) fails and does not clear ledger.
TEST(RestartJournalTests, EnterFailedRejectsIllegalTransitionAndPreservesLedger) {
    ScopedDisableLifecycleAssert disableAssert;
    DuplexRestartSession s{};
    s.lifecycle = LifecycleRemoved{};
    s.phase = DuplexRestartPhase::kFailed;
    SetAllProgressFlags(s);

    const bool ok = EnterFailed(s, kIOReturnError, "illegal_test");
    EXPECT_FALSE(ok);
    EXPECT_EQ(KindOf(s.lifecycle), DuplexLifecycleKind::Removed);
    EXPECT_EQ(s.phase, DuplexRestartPhase::kFailed);
    // Ledger must NOT have been cleared by ClearRestartProgress
    EXPECT_TRUE(s.ownerClaimed);
    EXPECT_TRUE(s.devicePrepared);
    EXPECT_TRUE(s.hostDuplexClaimed);
}

}  // namespace
