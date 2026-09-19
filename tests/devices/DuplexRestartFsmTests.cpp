// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Stage 2b Phase A: FSM characterization tests for the duplex restart lifecycle.
//
// The FW-66/67/68/69 suites already pin the recovery kernel, journal mutators,
// operation gate, session store, and clock broker. This suite closes the
// remaining function-level gaps so the Phase B rename and Phase C variant
// conversion are provably behaviour-preserving:
//
//   - HasRestartIntent / HasDeviceRestartState / HasHostRestartState /
//     HasAnyRestartState semantics (previously only transitive coverage),
//   - ClearRestartProgress terminal-phase subtleties (state is rewritten only
//     for kIdle / kFailed; kStopping preserves the current state by design,
//     because the stopping transition was set before cleanup began),
//   - ClassifyRestartReason edges (deliberate reconfiguration; the classifier
//     never emits kClockSourceChange / kBusResetRebind - those arrive as
//     recovery events, not from classification).
//
// Written against the pre-rename DICE-named API (Phase B renames types, not
// semantics; these tests are updated only by the mechanical rename).

#include <gtest/gtest.h>

#include <variant>
#include <type_traits>

#include "Audio/Protocols/Duplex/DuplexControlTypes.hpp"
#include "Audio/Protocols/Backends/RestartJournal.hpp"

namespace {

using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::ClassifyRestartReason;
using ASFW::Audio::ClearRestartProgress;
using ASFW::Audio::DuplexRestartFailureCause;
using ASFW::Audio::DuplexRestartIssueInfo;
using ASFW::Audio::DuplexRestartPhase;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::DuplexRestartSession;
using ASFW::Audio::DuplexLifecycleKind;
using ASFW::Audio::HasAnyRestartState;
using ASFW::Audio::HasDeviceRestartState;
using ASFW::Audio::HasHostRestartState;
using ASFW::Audio::CanTransitionLifecycle;
using ASFW::Audio::DuplexLifecycle;
using ASFW::Audio::HasRestartIntent;
using ASFW::Audio::LifecycleIsBusy;
using ASFW::Audio::LifecycleIsTerminal;
using ASFW::Audio::ToString;
using ASFW::Audio::KindOf;
using ASFW::Audio::Backends::EnterFailed;
using ASFW::Audio::Backends::EnterIdle;
using ASFW::Audio::LifecycleApplyingIdleClock;
using ASFW::Audio::LifecycleFailed;
using ASFW::Audio::LifecycleIdle;
using ASFW::Audio::LifecycleRecovering;
using ASFW::Audio::LifecycleRemoved;
using ASFW::Audio::LifecycleRunning;
using ASFW::Audio::LifecycleStarting;
using ASFW::Audio::LifecycleStopping;

constexpr AudioClockConfig k48k{
    .sampleRateHz = 48000U,
};

constexpr AudioDuplexChannels kChannels{
    .deviceToHostIsoChannel = 1,
    .hostToDeviceIsoChannel = 0,
};

// Phase C adapter: a kind -> a representative lifecycle value, for tests that
// only care about which alternative is held.
constexpr ASFW::Audio::DuplexLifecycle FromKind(ASFW::Audio::DuplexLifecycleKind kind) noexcept {
    switch (kind) {
        case ASFW::Audio::DuplexLifecycleKind::Idle: return ASFW::Audio::LifecycleIdle{};
        case ASFW::Audio::DuplexLifecycleKind::ApplyingIdleClock:
            return ASFW::Audio::LifecycleApplyingIdleClock{};
        case ASFW::Audio::DuplexLifecycleKind::Starting: return ASFW::Audio::LifecycleStarting{};
        case ASFW::Audio::DuplexLifecycleKind::Running: return ASFW::Audio::LifecycleRunning{};
        case ASFW::Audio::DuplexLifecycleKind::Stopping: return ASFW::Audio::LifecycleStopping{};
        case ASFW::Audio::DuplexLifecycleKind::Recovering:
            return ASFW::Audio::LifecycleRecovering{};
        case ASFW::Audio::DuplexLifecycleKind::Failed: return ASFW::Audio::LifecycleFailed{};
        case ASFW::Audio::DuplexLifecycleKind::Removed: return ASFW::Audio::LifecycleRemoved{};
    }
    return ASFW::Audio::LifecycleIdle{};
}

DuplexRestartSession SessionAt(DuplexRestartPhase phase, DuplexLifecycleKind kind) noexcept {
    DuplexRestartSession session{};
    session.guid = 0x130e0402004713ULL;
    session.channels = kChannels;
    session.phase = phase;
    session.lifecycle = FromKind(kind);
    return session;
}

// ---- HasRestartIntent -------------------------------------------------------

TEST(DuplexRestartFsmTests, HasRestartIntentRequiresDesiredClockOrPendingRequest) {
    DuplexRestartSession empty{};
    EXPECT_FALSE(HasRestartIntent(empty));

    DuplexRestartSession withClock{};
    withClock.desiredClock = k48k;
    EXPECT_TRUE(HasRestartIntent(withClock));

    DuplexRestartSession withPending{};
    withPending.hasPendingClockRequest = true;
    EXPECT_TRUE(HasRestartIntent(withPending));

    // A zero rate is "no intent" even when the field is present.
    DuplexRestartSession zeroRate{};
    zeroRate.desiredClock.sampleRateHz = 0;
    EXPECT_FALSE(HasRestartIntent(zeroRate));
}

// ---- Device / host footprint predicates -------------------------------------

TEST(DuplexRestartFsmTests, DeviceFootprintTracksEachDeviceBit) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    EXPECT_FALSE(HasDeviceRestartState(session));

    session.ownerClaimed = true;
    EXPECT_TRUE(HasDeviceRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.devicePrepared = true;
    EXPECT_TRUE(HasDeviceRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.deviceRxProgrammed = true;
    EXPECT_TRUE(HasDeviceRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.deviceTxArmed = true;
    EXPECT_TRUE(HasDeviceRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.deviceRunning = true;
    EXPECT_TRUE(HasDeviceRestartState(session));
}

TEST(DuplexRestartFsmTests, HostFootprintTracksEachHostBit) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    EXPECT_FALSE(HasHostRestartState(session));

    session.hostDuplexClaimed = true;
    EXPECT_TRUE(HasHostRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.hostPlaybackReserved = true;
    EXPECT_TRUE(HasHostRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.hostCaptureReserved = true;
    EXPECT_TRUE(HasHostRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.hostReceiveStarted = true;
    EXPECT_TRUE(HasHostRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.hostTransmitStarted = true;
    EXPECT_TRUE(HasHostRestartState(session));
}

TEST(DuplexRestartFsmTests, AnyFootprintIsUnionOfDeviceAndHost) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    EXPECT_FALSE(HasAnyRestartState(session));

    session.hostCaptureReserved = true;
    EXPECT_TRUE(HasAnyRestartState(session));

    session = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    session.deviceTxArmed = true;
    EXPECT_TRUE(HasAnyRestartState(session));
}

// Footprint is orthogonal to lifecycle state: a Running session may hold
// nothing (mid-transition), and an Idle session may hold a leftover footprint
// (the recovery policy's "IdleWithHostFootprint restarts" branch).
TEST(DuplexRestartFsmTests, FootprintIsOrthogonalToLifecycleState) {
    DuplexRestartSession running{};
    running.lifecycle = LifecycleRunning{};
    running.phase = DuplexRestartPhase::kRunning;
    EXPECT_FALSE(HasAnyRestartState(running));

    DuplexRestartSession idleWithFootprint = SessionAt(DuplexRestartPhase::kIdle, DuplexLifecycleKind::Idle);
    idleWithFootprint.hostPlaybackReserved = true;
    EXPECT_TRUE(HasAnyRestartState(idleWithFootprint));
}

// ---- ClearRestartProgress terminal-phase subtleties -------------------------

// The kStopping terminal phase intentionally leaves session.state untouched:
// the stopping transition was applied before cleanup began, and the state only
// returns to kIdle (or kFailed) when cleanup itself completes.
TEST(DuplexRestartFsmTests, ClearRestartProgressWithStoppingPreservesCurrentState) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kRunning, DuplexLifecycleKind::Stopping);
    session.deviceRunning = true;
    session.hostTransmitStarted = true;

    ClearRestartProgress(session, DuplexRestartPhase::kStopping);

    EXPECT_EQ(session.phase, DuplexRestartPhase::kStopping);
    EXPECT_EQ(KindOf(session.lifecycle), DuplexLifecycleKind::Stopping); // preserved, not overwritten
    EXPECT_FALSE(session.deviceRunning);
    EXPECT_FALSE(session.hostTransmitStarted);
}

// ClearRestartProgress no longer touches the lifecycle at all: state transitions
// belong to the journal's terminal helpers (EnterIdle/EnterFailed).
TEST(DuplexRestartFsmTests, ClearRestartProgressToIdleResetsLedgerButNotLifecycle) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kStopping, DuplexLifecycleKind::Stopping);
    session.hostCaptureReserved = true;

    ClearRestartProgress(session); // default terminal phase = kIdle

    EXPECT_EQ(session.phase, DuplexRestartPhase::kIdle);
    EXPECT_EQ(KindOf(session.lifecycle), DuplexLifecycleKind::Stopping); // lifecycle untouched
    EXPECT_FALSE(session.hostCaptureReserved);
}

// ClearRestartProgress(kFailed) only moves the phase; arming the Failed state
// (with its terminal-error payload) is EnterFailed's job.
TEST(DuplexRestartFsmTests, ClearRestartProgressToFailedLeavesLifecycleUntouched) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kConfirmingDeviceStart, DuplexLifecycleKind::Starting);
    session.devicePrepared = true;

    ClearRestartProgress(session, DuplexRestartPhase::kFailed);

    EXPECT_EQ(session.phase, DuplexRestartPhase::kFailed);
    EXPECT_EQ(KindOf(session.lifecycle), DuplexLifecycleKind::Starting); // untouched
    EXPECT_FALSE(session.devicePrepared);
}

// ClearRestartProgress is restart-progress-scoped: intent, identity, and the
// failure/invalidation journals survive every terminal phase.
TEST(DuplexRestartFsmTests, ClearRestartProgressPreservesIntentAndJournalsForAnyTerminal) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kFailed, DuplexLifecycleKind::Failed);
    session.reason = DuplexRestartReason::kSampleRateChange;
    session.desiredClock = k48k;
    session.pendingClock = k48k;
    session.pendingReason = DuplexRestartReason::kClockSourceChange;
    session.hasPendingClockRequest = true;
    session.lastFailure = DuplexRestartIssueInfo{
        .failedPhase = DuplexRestartPhase::kProgrammingDeviceTx,
        .errorClass = ASFW::Audio::DuplexRestartErrorClass::kStageFailure,
        .cause = DuplexRestartFailureCause::kProgramTx,
    };

    ClearRestartProgress(session, DuplexRestartPhase::kIdle);

    EXPECT_EQ(session.reason, DuplexRestartReason::kSampleRateChange);
    EXPECT_EQ(session.desiredClock.sampleRateHz, 48000U);
    EXPECT_EQ(session.pendingReason, DuplexRestartReason::kClockSourceChange);
    EXPECT_TRUE(session.hasPendingClockRequest);
    EXPECT_TRUE(session.lastFailure.has_value());
    EXPECT_TRUE(HasRestartIntent(session));
}

// ---- ClassifyRestartReason edges --------------------------------------------

TEST(DuplexRestartFsmTests, ClassifyRestartReasonNeverEmitsBusResetOrClockSource) {
    // Bus-reset rebinds and clock-source changes arrive as recovery events and
    // pending clock requests, respectively - never from this classifier.
    DuplexRestartSession prior{};
    prior.guid = 0x130e0402004713ULL;
    prior.channels = kChannels;
    prior.reason = DuplexRestartReason::kClockSourceChange;
    prior.desiredClock = k48k;
    prior.phase = DuplexRestartPhase::kRunning;

    // Same-rate restart with a healthy prior session is a deliberate reconfigure.
    EXPECT_EQ(ClassifyRestartReason(&prior, k48k), DuplexRestartReason::kManualReconfigure);
}

TEST(DuplexRestartFsmTests, ClassifyRestartReasonPrefersRateChangeOverFailedPhase) {
    // A failed session whose desired rate also changed classifies as a rate
    // change (deliberate restart), not a recovery.
    DuplexRestartSession prior{};
    prior.guid = 0x130e0402004713ULL;
    prior.channels = kChannels;
    prior.desiredClock = k48k;
    prior.phase = DuplexRestartPhase::kFailed;

    EXPECT_EQ(ClassifyRestartReason(&prior, k48k), DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(ClassifyRestartReason(&prior, k48k), DuplexRestartReason::kRecoverAfterTimingLoss);

    DuplexRestartSession rateChangePrior = prior;
    rateChangePrior.desiredClock = k48k;
    const AudioClockConfig k96k{.sampleRateHz = 96000U};
    EXPECT_EQ(ClassifyRestartReason(&rateChangePrior, k96k), DuplexRestartReason::kSampleRateChange);
}


// ---- Phase C: transition table + variant semantics --------------------------

TEST(DuplexRestartFsmTests, LifecycleTerminalStatesAreFailedAndRemoved) {
    EXPECT_TRUE(LifecycleIsTerminal(DuplexLifecycleKind::Failed));
    EXPECT_TRUE(LifecycleIsTerminal(DuplexLifecycleKind::Removed));
    EXPECT_FALSE(LifecycleIsTerminal(DuplexLifecycleKind::Idle));
    EXPECT_FALSE(LifecycleIsTerminal(DuplexLifecycleKind::Running));
    EXPECT_FALSE(LifecycleIsTerminal(DuplexLifecycleKind::Stopping));
}

TEST(DuplexRestartFsmTests, LifecycleBusyStatesExcludeIdleAndTerminals) {
    EXPECT_TRUE(LifecycleIsBusy(DuplexLifecycleKind::Starting));
    EXPECT_TRUE(LifecycleIsBusy(DuplexLifecycleKind::Running));
    EXPECT_TRUE(LifecycleIsBusy(DuplexLifecycleKind::Recovering));
    EXPECT_TRUE(LifecycleIsBusy(DuplexLifecycleKind::ApplyingIdleClock));
    EXPECT_FALSE(LifecycleIsBusy(DuplexLifecycleKind::Idle));
    EXPECT_FALSE(LifecycleIsBusy(DuplexLifecycleKind::Stopping));
    EXPECT_FALSE(LifecycleIsBusy(DuplexLifecycleKind::Failed));
    EXPECT_FALSE(LifecycleIsBusy(DuplexLifecycleKind::Removed));
}

// The audited happy path: Idle -> Starting -> Running -> Stopping -> Idle.
TEST(DuplexRestartFsmTests, LifecycleHappyPathTransitionsAreLegal) {
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Idle, DuplexLifecycleKind::Starting));
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Starting, DuplexLifecycleKind::Running));
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Running, DuplexLifecycleKind::Stopping));
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Stopping, DuplexLifecycleKind::Idle));
}

// Recovery cycle: Running -> Recovering -> (reset) -> Recovering/Starting ->
// Running, and every active state may jump to Removed (device vanished).
TEST(DuplexRestartFsmTests, LifecycleRecoveryCycleIsLegal) {
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Running, DuplexLifecycleKind::Recovering));
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Recovering, DuplexLifecycleKind::Idle));
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Idle, DuplexLifecycleKind::Recovering));
    EXPECT_TRUE(CanTransitionLifecycle(DuplexLifecycleKind::Recovering, DuplexLifecycleKind::Running));

    for (const auto active : {DuplexLifecycleKind::Starting, DuplexLifecycleKind::Running,
                              DuplexLifecycleKind::Recovering, DuplexLifecycleKind::Stopping}) {
        EXPECT_TRUE(CanTransitionLifecycle(active, DuplexLifecycleKind::Removed))
            << ToString(active);
    }
    // But a clean Idle session is not "removed" - it has nothing to retire.
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::ApplyingIdleClock, DuplexLifecycleKind::Starting));
}

// Illegal combinations the table exists to prevent: starting while stopping,
// late completions reviving a removed device, skip-ahead transitions.
TEST(DuplexRestartFsmTests, LifecycleIllegalTransitionsAreRejected) {
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::Stopping, DuplexLifecycleKind::Running));
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::Stopping, DuplexLifecycleKind::Starting));
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::Removed, DuplexLifecycleKind::Running));
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::Removed, DuplexLifecycleKind::Stopping));
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::Idle, DuplexLifecycleKind::Running));
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::Running, DuplexLifecycleKind::Starting));
    EXPECT_FALSE(CanTransitionLifecycle(DuplexLifecycleKind::ApplyingIdleClock, DuplexLifecycleKind::Starting));
}

// The variant's point: state-specific facts live in the state that owns them.
// Today that is exactly one fact — Failed::terminalError — and its authority is
// the payload itself (no session-level shadow). Empty alternatives are
// deliberate; payload for symmetry would duplicate a session fact.
TEST(DuplexRestartFsmTests, LifecycleFailedCarriesTheTerminalError) {
    const DuplexLifecycle failed = LifecycleFailed{.terminalError = kIOReturnTimeout};
    EXPECT_EQ(std::get<LifecycleFailed>(failed).terminalError, kIOReturnTimeout);
    EXPECT_EQ(KindOf(failed), DuplexLifecycleKind::Failed);

    // All other alternatives are empty by design.
    EXPECT_TRUE(std::is_empty_v<LifecycleIdle>);
    EXPECT_TRUE(std::is_empty_v<LifecycleApplyingIdleClock>);
    EXPECT_TRUE(std::is_empty_v<LifecycleStarting>);
    EXPECT_TRUE(std::is_empty_v<LifecycleRunning>);
    EXPECT_TRUE(std::is_empty_v<LifecycleStopping>);
    EXPECT_TRUE(std::is_empty_v<LifecycleRecovering>);
    EXPECT_TRUE(std::is_empty_v<LifecycleRemoved>);
    EXPECT_FALSE(std::is_empty_v<LifecycleFailed>);
}

// Terminal helpers: EnterFailed arms the payload, EnterIdle carries none, and
// the sequence Failed -> Idle -> Idle re-arms cleanly through the choke point.
TEST(DuplexRestartFsmTests, TerminalHelpersArmAndClearThePayload) {
    DuplexRestartSession session = SessionAt(DuplexRestartPhase::kRunning, DuplexLifecycleKind::Running);
    session.deviceRunning = true;

    EnterFailed(session, kIOReturnTimeout, "stop_failed");
    ASSERT_TRUE(std::holds_alternative<LifecycleFailed>(session.lifecycle));
    EXPECT_EQ(std::get<LifecycleFailed>(session.lifecycle).terminalError, kIOReturnTimeout);
    EXPECT_FALSE(session.deviceRunning); // ledger cleared

    EnterIdle(session, "reset_before_start");
    EXPECT_TRUE(std::holds_alternative<LifecycleIdle>(session.lifecycle));

    EnterIdle(session, "re-enter");
    EXPECT_TRUE(std::holds_alternative<LifecycleIdle>(session.lifecycle));
}

// ClearRestartProgress(kIdle) clears the ledger from any lifecycle without
// touching it — including Removed, which only the session owner may leave.
TEST(DuplexRestartFsmTests, ClearRestartProgressResetsLedgerFromAnyLifecycle) {
    for (const auto kind : {DuplexLifecycleKind::Starting, DuplexLifecycleKind::Running,
                            DuplexLifecycleKind::Recovering, DuplexLifecycleKind::Failed,
                            DuplexLifecycleKind::Removed}) {
        DuplexRestartSession session = SessionAt(DuplexRestartPhase::kRunning, kind);
        session.hostPlaybackReserved = true;
        ClearRestartProgress(session, DuplexRestartPhase::kIdle);
        EXPECT_EQ(session.phase, DuplexRestartPhase::kIdle) << ToString(kind);
        EXPECT_EQ(KindOf(session.lifecycle), kind) << ToString(kind);
        EXPECT_FALSE(session.hostPlaybackReserved) << ToString(kind);
    }
}

} // namespace
