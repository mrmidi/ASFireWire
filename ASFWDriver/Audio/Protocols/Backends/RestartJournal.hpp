// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RestartJournal.hpp
//
// FW-69a (Step 5 of FW-64, journal half): the duplex restart FSM journal — the stateless
// transition/logging free functions extracted from AudioDuplexCoordinator.cpp. Each
// operates only on a passed DuplexRestartSession& (or value params) and emits the [FSM] field
// trace via ASFW_LOG_V*; none touch the coordinator, the session store (sessions_), lock_, or
// any member. Also carries the restart-enum ToString formatters + GenerationValue they depend on
// (used by the coordinator's own logging too, so kept visible via this header).
//
// Behaviour-preserving: bodies copied byte-for-byte, so every log format string, the DICE
// verbosity gate, and the anomaly-only no-op guards are textually unchanged (identical field
// trace). The session STORE (sessions_
// + Load/Store/Get/Clear + epoch/restartId) is a separate component (FW-69b).

#pragma once

#include "DiceRecoveryPolicy.hpp"

#include "../Duplex/DuplexRestartSession.hpp"
#include "../../../Logging/Logging.hpp"

#include <cstdint>

namespace ASFW::Audio::Backends {

// The journal is expressed over the duplex restart-session vocabulary; bring those names into
// scope so the moved bodies stay byte-identical (they referenced them unqualified in the
// coordinator's anonymous namespace via the same using-declarations).
using ASFW::Audio::ClearRestartProgress;
using ASFW::Audio::DuplexClockRequestOutcome;
using ASFW::Audio::DuplexRestartErrorClass;
using ASFW::Audio::DuplexRestartFailureCause;
using ASFW::Audio::DuplexRestartIssueInfo;
using ASFW::Audio::DuplexRestartPhase;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::DuplexRestartState;
using ASFW::Audio::DuplexRestartSession;

[[nodiscard]] constexpr const char* ToString(DuplexRestartReason reason) noexcept {
    switch (reason) {
        case DuplexRestartReason::kInitialStart: return "InitialStart";
        case DuplexRestartReason::kSampleRateChange: return "SampleRateChange";
        case DuplexRestartReason::kClockSourceChange: return "ClockSourceChange";
        case DuplexRestartReason::kBusResetRebind: return "BusResetRebind";
        case DuplexRestartReason::kRecoverAfterTimingLoss: return "TimingLoss";
        case DuplexRestartReason::kRecoverAfterCycleInconsistent: return "CycleInconsistent";
        case DuplexRestartReason::kRecoverAfterLockLoss: return "LockLoss";
        case DuplexRestartReason::kRecoverAfterTxFault: return "TxFault";
        case DuplexRestartReason::kManualReconfigure: return "ManualReconfigure";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DuplexRestartPhase phase) noexcept {
    switch (phase) {
        case DuplexRestartPhase::kIdle: return "Idle";
        case DuplexRestartPhase::kPreparingDevice: return "PreparingDevice";
        case DuplexRestartPhase::kPrepared: return "Prepared";
        case DuplexRestartPhase::kReservingPlaybackResources: return "ReservingPlaybackResources";
        case DuplexRestartPhase::kProgrammingDeviceRx: return "ProgrammingDeviceRx";
        case DuplexRestartPhase::kDeviceRxProgrammed: return "DeviceRxProgrammed";
        case DuplexRestartPhase::kReservingCaptureResources: return "ReservingCaptureResources";
        case DuplexRestartPhase::kStartingHostReceive: return "StartingHostReceive";
        case DuplexRestartPhase::kProgrammingDeviceTx: return "ProgrammingDeviceTx";
        case DuplexRestartPhase::kDeviceTxArmed: return "DeviceTxArmed";
        case DuplexRestartPhase::kWaitingGlobalClock: return "WaitingGlobalClock";
        case DuplexRestartPhase::kStartingHostTransmit: return "StartingHostTransmit";
        case DuplexRestartPhase::kConfirmingDeviceStart: return "ConfirmingDeviceStart";
        case DuplexRestartPhase::kRunning: return "Running";
        case DuplexRestartPhase::kStopping: return "Stopping";
        case DuplexRestartPhase::kFailed: return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DuplexRestartState state) noexcept {
    switch (state) {
        case DuplexRestartState::kIdle: return "Idle";
        case DuplexRestartState::kApplyingIdleClock: return "ApplyingIdleClock";
        case DuplexRestartState::kStarting: return "Starting";
        case DuplexRestartState::kRunning: return "Running";
        case DuplexRestartState::kStopping: return "Stopping";
        case DuplexRestartState::kRecovering: return "Recovering";
        case DuplexRestartState::kFailed: return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DuplexClockRequestOutcome outcome) noexcept {
    switch (outcome) {
        case DuplexClockRequestOutcome::kApplied: return "Applied";
        case DuplexClockRequestOutcome::kSuperseded: return "Superseded";
        case DuplexClockRequestOutcome::kAbortedByStop: return "AbortedByStop";
        case DuplexClockRequestOutcome::kFailed: return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DuplexRestartErrorClass errorClass) noexcept {
    switch (errorClass) {
        case DuplexRestartErrorClass::kUnsupportedConfig: return "UnsupportedConfig";
        case DuplexRestartErrorClass::kMissingDependency: return "MissingDependency";
        case DuplexRestartErrorClass::kStageFailure: return "StageFailure";
        case DuplexRestartErrorClass::kEpochInvalidated: return "EpochInvalidated";
        case DuplexRestartErrorClass::kStopIntent: return "StopIntent";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DuplexRestartFailureCause cause) noexcept {
    switch (cause) {
        case DuplexRestartFailureCause::kNone: return "None";
        case DuplexRestartFailureCause::kPrepare: return "Prepare";
        case DuplexRestartFailureCause::kReservePlayback: return "ReservePlayback";
        case DuplexRestartFailureCause::kProgramRx: return "ProgramRx";
        case DuplexRestartFailureCause::kReserveCapture: return "ReserveCapture";
        case DuplexRestartFailureCause::kStartReceive: return "StartReceive";
        case DuplexRestartFailureCause::kProgramTx: return "ProgramTx";
        case DuplexRestartFailureCause::kGlobalClockLock: return "GlobalClockLock";
        case DuplexRestartFailureCause::kStartTransmit: return "StartTransmit";
        case DuplexRestartFailureCause::kConfirmStart: return "ConfirmStart";
        case DuplexRestartFailureCause::kIdleClockApply: return "IdleClockApply";
        case DuplexRestartFailureCause::kStop: return "Stop";
        case DuplexRestartFailureCause::kBusResetRebind: return "BusResetRebind";
        case DuplexRestartFailureCause::kTimingLoss: return "TimingLoss";
        case DuplexRestartFailureCause::kCycleInconsistent: return "CycleInconsistent";
        case DuplexRestartFailureCause::kLockLoss: return "LockLoss";
        case DuplexRestartFailureCause::kTxFault: return "TxFault";
    }
    return "Unknown";
}

[[nodiscard]] constexpr uint32_t GenerationValue(FW::Generation generation) noexcept {
    return generation.value;
}

inline void LogFsmEvent(const char* eventName,
                 uint64_t guid,
                 uint64_t restartId,
                 FW::Generation generation,
                 DuplexRestartState state,
                 DuplexRestartPhase phase,
                 DuplexRestartReason reason,
                 uint64_t token = 0) noexcept {
    if (token != 0) {
        ASFW_LOG_V2(DICE,
                    "[FSM] event=%{public}s guid=0x%llx restartId=%llu state=%{public}s phase=%{public}s gen=%u token=%llu reason=%{public}s",
                    eventName,
                    guid,
                    restartId,
                    ToString(state),
                    ToString(phase),
                    GenerationValue(generation),
                    token,
                    ToString(reason));
        return;
    }

    ASFW_LOG_V2(DICE,
                "[FSM] event=%{public}s guid=0x%llx restartId=%llu state=%{public}s phase=%{public}s gen=%u reason=%{public}s",
                eventName,
                guid,
                restartId,
                ToString(state),
                ToString(phase),
                GenerationValue(generation),
                ToString(reason));
}

inline void LogStateTransition(const DuplexRestartSession& session,
                        DuplexRestartState oldState,
                        DuplexRestartState newState,
                        const char* why) noexcept {
    if (oldState == newState) {
        return;
    }

    ASFW_LOG_V2(DICE,
                "[FSM] state %{public}s -> %{public}s guid=0x%llx restartId=%llu phase=%{public}s gen=%u why=%{public}s",
                ToString(oldState),
                ToString(newState),
                session.guid,
                session.restartId,
                ToString(session.phase),
                GenerationValue(session.topologyGeneration),
                why);
}

inline void LogPhaseTransition(const DuplexRestartSession& session,
                        DuplexRestartPhase oldPhase,
                        DuplexRestartPhase newPhase) noexcept {
    if (oldPhase == newPhase) {
        return;
    }

    ASFW_LOG_V2(DICE,
                "[FSM] phase %{public}s -> %{public}s guid=0x%llx restartId=%llu state=%{public}s gen=%u",
                ToString(oldPhase),
                ToString(newPhase),
                session.guid,
                session.restartId,
                ToString(session.state),
                GenerationValue(session.topologyGeneration));
}

inline void SetSessionState(DuplexRestartSession& session,
                     DuplexRestartState newState,
                     const char* why) noexcept {
    const auto oldState = session.state;
    session.state = newState;
    LogStateTransition(session, oldState, newState, why);
}

inline void SetSessionPhase(DuplexRestartSession& session, DuplexRestartPhase newPhase) noexcept {
    const auto oldPhase = session.phase;
    session.phase = newPhase;
    LogPhaseTransition(session, oldPhase, newPhase);
}

inline void ApplyTerminalPhase(DuplexRestartSession& session,
                        DuplexRestartPhase terminalPhase,
                        const char* why) noexcept {
    const auto oldState = session.state;
    const auto oldPhase = session.phase;
    ClearRestartProgress(session, terminalPhase);
    LogPhaseTransition(session, oldPhase, session.phase);
    LogStateTransition(session, oldState, session.state, why);
}

inline void ClearFailureSnapshot(DuplexRestartSession& session) noexcept {
    session.lastFailure.reset();
}

inline void RecordIssue(DuplexRestartSession& session,
                 std::optional<DuplexRestartIssueInfo>& destination,
                 DuplexRestartPhase failedPhase,
                 DuplexRestartErrorClass errorClass,
                 DuplexRestartFailureCause cause,
                 IOReturn status,
                 bool retryable,
                 bool rollbackAttempted,
                 IOReturn rollbackStatus,
                 bool hostStateKnown,
                 bool deviceStateKnown) noexcept {
    destination = DuplexRestartIssueInfo{
        .failedPhase = failedPhase,
        .errorClass = errorClass,
        .cause = cause,
        .status = status,
        .retryable = retryable,
        .rollbackAttempted = rollbackAttempted,
        .rollbackStatus = rollbackStatus,
        .hostStateKnown = hostStateKnown,
        .deviceStateKnown = deviceStateKnown,
        .restartId = session.restartId,
        .generation = session.topologyGeneration,
    };
}

inline void LogInvalidation(const DuplexRestartSession& session) noexcept {
    if (!session.lastInvalidation.has_value()) {
        return;
    }

    const auto& invalidation = *session.lastInvalidation;
    ASFW_LOG_V3(DICE,
                "[FSM] invalidation class=%{public}s cause=%{public}s retryable=%d status=0x%08x guid=0x%llx restartId=%llu state=%{public}s phase=%{public}s gen=%u",
                ToString(invalidation.errorClass),
                ToString(invalidation.cause),
                invalidation.retryable ? 1 : 0,
                static_cast<unsigned>(invalidation.status),
                session.guid,
                session.restartId,
                ToString(session.state),
                ToString(session.phase),
                GenerationValue(session.topologyGeneration));
}

inline void LogRecoveryPolicy(const DuplexRestartSession& session,
                       DuplexRestartReason triggerReason,
                       const DiceRecoveryDecision& decision) noexcept {
    ASFW_LOG_V3(DICE,
                "[FSM] policy disposition=%{public}s cause=%{public}s why=%{public}s guid=0x%llx restartId=%llu state=%{public}s phase=%{public}s gen=%u",
                ToString(decision.disposition),
                ToString(FailureCauseForReason(triggerReason)),
                ToString(decision.reason),
                session.guid,
                session.restartId,
                ToString(session.state),
                ToString(session.phase),
                GenerationValue(session.topologyGeneration));
}

inline void LogTerminal(const DuplexRestartSession& session) noexcept {
    if (session.state == DuplexRestartState::kFailed && session.lastFailure.has_value()) {
        const auto& failure = *session.lastFailure;
        ASFW_LOG_V1(DICE,
                    "[FSM] terminal state=%{public}s phase=%{public}s class=%{public}s cause=%{public}s retryable=%d rollback=0x%08x status=0x%08x guid=0x%llx restartId=%llu gen=%u",
                    ToString(session.state),
                    ToString(session.phase),
                    ToString(failure.errorClass),
                    ToString(failure.cause),
                    failure.retryable ? 1 : 0,
                    static_cast<unsigned>(failure.rollbackStatus),
                    static_cast<unsigned>(failure.status),
                    session.guid,
                    session.restartId,
                    GenerationValue(session.topologyGeneration));
        return;
    }

    ASFW_LOG_V1(DICE,
                "[FSM] terminal state=%{public}s phase=%{public}s status=0x%08x guid=0x%llx restartId=%llu gen=%u",
                ToString(session.state),
                ToString(session.phase),
                static_cast<unsigned>(session.terminalError),
                session.guid,
                session.restartId,
                GenerationValue(session.topologyGeneration));
}

} // namespace ASFW::Audio::Backends
