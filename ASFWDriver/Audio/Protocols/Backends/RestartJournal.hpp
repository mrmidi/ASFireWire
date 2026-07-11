// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RestartJournal.hpp
//
// FW-69a (Step 5 of FW-64, journal half): the DICE restart FSM journal — the stateless
// transition/logging free functions extracted from AudioDuplexCoordinator.cpp. Each
// operates only on a passed DiceRestartSession& (or value params) and emits the [FSM] field
// trace via ASFW_LOG_V*; none touch the coordinator, the session store (sessions_), lock_, or
// any member. Also carries the DICE-enum ToString formatters + GenerationValue they depend on
// (used by the coordinator's own logging too, so kept visible via this header).
//
// Behaviour-preserving: bodies copied byte-for-byte, so every log format string, the DICE
// verbosity gate, and the anomaly-only no-op guards are textually unchanged (identical field
// trace). Names keep their Dice* prefix (neutral rename is FW-73b). The session STORE (sessions_
// + Load/Store/Get/Clear + epoch/restartId) is a separate component (FW-69b).

#pragma once

#include "DiceRecoveryPolicy.hpp"

#include "../DICE/Core/DICERestartSession.hpp"
#include "../../../Logging/Logging.hpp"

#include <cstdint>

namespace ASFW::Audio::Backends {

// The journal is expressed over the DICE restart-session vocabulary; bring those names into
// scope so the moved bodies stay byte-identical (they referenced them unqualified in the
// coordinator's anonymous namespace via the same using-declarations).
using ASFW::Audio::DICE::ClearRestartProgress;
using ASFW::Audio::DICE::DiceClockRequestOutcome;
using ASFW::Audio::DICE::DiceRestartErrorClass;
using ASFW::Audio::DICE::DiceRestartFailureCause;
using ASFW::Audio::DICE::DiceRestartIssueInfo;
using ASFW::Audio::DICE::DiceRestartPhase;
using ASFW::Audio::DICE::DiceRestartReason;
using ASFW::Audio::DICE::DiceRestartState;
using ASFW::Audio::DICE::DiceRestartSession;

[[nodiscard]] constexpr const char* ToString(DiceRestartReason reason) noexcept {
    switch (reason) {
        case DiceRestartReason::kInitialStart: return "InitialStart";
        case DiceRestartReason::kSampleRateChange: return "SampleRateChange";
        case DiceRestartReason::kClockSourceChange: return "ClockSourceChange";
        case DiceRestartReason::kBusResetRebind: return "BusResetRebind";
        case DiceRestartReason::kRecoverAfterTimingLoss: return "TimingLoss";
        case DiceRestartReason::kRecoverAfterCycleInconsistent: return "CycleInconsistent";
        case DiceRestartReason::kRecoverAfterLockLoss: return "LockLoss";
        case DiceRestartReason::kRecoverAfterTxFault: return "TxFault";
        case DiceRestartReason::kManualReconfigure: return "ManualReconfigure";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DiceRestartPhase phase) noexcept {
    switch (phase) {
        case DiceRestartPhase::kIdle: return "Idle";
        case DiceRestartPhase::kPreparingDevice: return "PreparingDevice";
        case DiceRestartPhase::kPrepared: return "Prepared";
        case DiceRestartPhase::kReservingPlaybackResources: return "ReservingPlaybackResources";
        case DiceRestartPhase::kProgrammingDeviceRx: return "ProgrammingDeviceRx";
        case DiceRestartPhase::kDeviceRxProgrammed: return "DeviceRxProgrammed";
        case DiceRestartPhase::kReservingCaptureResources: return "ReservingCaptureResources";
        case DiceRestartPhase::kStartingHostReceive: return "StartingHostReceive";
        case DiceRestartPhase::kProgrammingDeviceTx: return "ProgrammingDeviceTx";
        case DiceRestartPhase::kDeviceTxArmed: return "DeviceTxArmed";
        case DiceRestartPhase::kWaitingGlobalClock: return "WaitingGlobalClock";
        case DiceRestartPhase::kStartingHostTransmit: return "StartingHostTransmit";
        case DiceRestartPhase::kConfirmingDeviceStart: return "ConfirmingDeviceStart";
        case DiceRestartPhase::kRunning: return "Running";
        case DiceRestartPhase::kStopping: return "Stopping";
        case DiceRestartPhase::kFailed: return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DiceRestartState state) noexcept {
    switch (state) {
        case DiceRestartState::kIdle: return "Idle";
        case DiceRestartState::kApplyingIdleClock: return "ApplyingIdleClock";
        case DiceRestartState::kStarting: return "Starting";
        case DiceRestartState::kRunning: return "Running";
        case DiceRestartState::kStopping: return "Stopping";
        case DiceRestartState::kRecovering: return "Recovering";
        case DiceRestartState::kFailed: return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DiceClockRequestOutcome outcome) noexcept {
    switch (outcome) {
        case DiceClockRequestOutcome::kApplied: return "Applied";
        case DiceClockRequestOutcome::kSuperseded: return "Superseded";
        case DiceClockRequestOutcome::kAbortedByStop: return "AbortedByStop";
        case DiceClockRequestOutcome::kFailed: return "Failed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DiceRestartErrorClass errorClass) noexcept {
    switch (errorClass) {
        case DiceRestartErrorClass::kUnsupportedConfig: return "UnsupportedConfig";
        case DiceRestartErrorClass::kMissingDependency: return "MissingDependency";
        case DiceRestartErrorClass::kStageFailure: return "StageFailure";
        case DiceRestartErrorClass::kEpochInvalidated: return "EpochInvalidated";
        case DiceRestartErrorClass::kStopIntent: return "StopIntent";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DiceRestartFailureCause cause) noexcept {
    switch (cause) {
        case DiceRestartFailureCause::kNone: return "None";
        case DiceRestartFailureCause::kPrepare: return "Prepare";
        case DiceRestartFailureCause::kReservePlayback: return "ReservePlayback";
        case DiceRestartFailureCause::kProgramRx: return "ProgramRx";
        case DiceRestartFailureCause::kReserveCapture: return "ReserveCapture";
        case DiceRestartFailureCause::kStartReceive: return "StartReceive";
        case DiceRestartFailureCause::kProgramTx: return "ProgramTx";
        case DiceRestartFailureCause::kGlobalClockLock: return "GlobalClockLock";
        case DiceRestartFailureCause::kStartTransmit: return "StartTransmit";
        case DiceRestartFailureCause::kConfirmStart: return "ConfirmStart";
        case DiceRestartFailureCause::kIdleClockApply: return "IdleClockApply";
        case DiceRestartFailureCause::kStop: return "Stop";
        case DiceRestartFailureCause::kBusResetRebind: return "BusResetRebind";
        case DiceRestartFailureCause::kTimingLoss: return "TimingLoss";
        case DiceRestartFailureCause::kCycleInconsistent: return "CycleInconsistent";
        case DiceRestartFailureCause::kLockLoss: return "LockLoss";
        case DiceRestartFailureCause::kTxFault: return "TxFault";
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
                 DiceRestartState state,
                 DiceRestartPhase phase,
                 DiceRestartReason reason,
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

inline void LogStateTransition(const DiceRestartSession& session,
                        DiceRestartState oldState,
                        DiceRestartState newState,
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

inline void LogPhaseTransition(const DiceRestartSession& session,
                        DiceRestartPhase oldPhase,
                        DiceRestartPhase newPhase) noexcept {
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

inline void SetSessionState(DiceRestartSession& session,
                     DiceRestartState newState,
                     const char* why) noexcept {
    const auto oldState = session.state;
    session.state = newState;
    LogStateTransition(session, oldState, newState, why);
}

inline void SetSessionPhase(DiceRestartSession& session, DiceRestartPhase newPhase) noexcept {
    const auto oldPhase = session.phase;
    session.phase = newPhase;
    LogPhaseTransition(session, oldPhase, newPhase);
}

inline void ApplyTerminalPhase(DiceRestartSession& session,
                        DiceRestartPhase terminalPhase,
                        const char* why) noexcept {
    const auto oldState = session.state;
    const auto oldPhase = session.phase;
    ClearRestartProgress(session, terminalPhase);
    LogPhaseTransition(session, oldPhase, session.phase);
    LogStateTransition(session, oldState, session.state, why);
}

inline void ClearFailureSnapshot(DiceRestartSession& session) noexcept {
    session.lastFailure.reset();
}

inline void RecordIssue(DiceRestartSession& session,
                 std::optional<DiceRestartIssueInfo>& destination,
                 DiceRestartPhase failedPhase,
                 DiceRestartErrorClass errorClass,
                 DiceRestartFailureCause cause,
                 IOReturn status,
                 bool retryable,
                 bool rollbackAttempted,
                 IOReturn rollbackStatus,
                 bool hostStateKnown,
                 bool deviceStateKnown) noexcept {
    destination = DiceRestartIssueInfo{
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

inline void LogInvalidation(const DiceRestartSession& session) noexcept {
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

inline void LogRecoveryPolicy(const DiceRestartSession& session,
                       DiceRestartReason triggerReason,
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

inline void LogTerminal(const DiceRestartSession& session) noexcept {
    if (session.state == DiceRestartState::kFailed && session.lastFailure.has_value()) {
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
