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

using namespace ASFW::Audio::Backends;

// The 10 progress flags ClearRestartProgress (and thus ApplyTerminalPhase) must reset.
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
    SetSessionState(s, DuplexRestartState::kRunning, "confirmed_running");
    EXPECT_EQ(s.state, DuplexRestartState::kRunning);
    EXPECT_EQ(s.phase, DuplexRestartPhase::kPreparingDevice);  // untouched
}

TEST(RestartJournalTests, SetSessionPhaseSetsPhaseOnly) {
    DuplexRestartSession s{};
    s.state = DuplexRestartState::kRunning;
    SetSessionPhase(s, DuplexRestartPhase::kPreparingDevice);
    EXPECT_EQ(s.phase, DuplexRestartPhase::kPreparingDevice);
    EXPECT_EQ(s.state, DuplexRestartState::kRunning);  // untouched
}

// ApplyTerminalPhase(kIdle): phase+state -> kIdle, terminalError cleared, progress flags reset.
TEST(RestartJournalTests, ApplyTerminalPhaseIdleResetsToIdle) {
    DuplexRestartSession s{};
    s.state = DuplexRestartState::kRunning;
    s.phase = DuplexRestartPhase::kRunning;
    s.terminalError = kIOReturnError;
    SetAllProgressFlags(s);
    ApplyTerminalPhase(s, DuplexRestartPhase::kIdle, "reset_before_start");
    EXPECT_EQ(s.phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(s.state, DuplexRestartState::kIdle);
    EXPECT_EQ(s.terminalError, kIOReturnSuccess);  // cleared for non-failure terminal
    ExpectAllProgressCleared(s);
}

// ApplyTerminalPhase(kFailed): phase+state -> kFailed, terminalError PRESERVED.
TEST(RestartJournalTests, ApplyTerminalPhaseFailedPreservesTerminalError) {
    DuplexRestartSession s{};
    s.state = DuplexRestartState::kRunning;
    s.phase = DuplexRestartPhase::kRunning;
    s.terminalError = kIOReturnError;
    SetAllProgressFlags(s);
    ApplyTerminalPhase(s, DuplexRestartPhase::kFailed, "stop_failed");
    EXPECT_EQ(s.phase, DuplexRestartPhase::kFailed);
    EXPECT_EQ(s.state, DuplexRestartState::kFailed);
    EXPECT_EQ(s.terminalError, kIOReturnError);  // preserved on failure
    ExpectAllProgressCleared(s);
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

}  // namespace
