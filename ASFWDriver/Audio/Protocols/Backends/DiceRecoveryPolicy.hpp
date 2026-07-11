// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceRecoveryPolicy.hpp
//
// The recovery-policy classification vocabulary (FW-66): the pure `EvaluateRecoveryPolicy`
// kernel plus the reason classifiers (`RestartStateForStartReason`, `FailureCauseForReason`,
// `IsRecoveryReason`), the retryability predicate (`IsRetryableStatus`), the enums,
// context/decision structs and ToString overloads they depend on. Extracted from
// AudioDuplexCoordinator.cpp so classification is a standalone, directly-testable unit
// with no dependency on the session store or logging (per FW-66). Classification returns
// facts; recording/logging them stays in the coordinator (FW-69 journal).
//
// The definitions are copied verbatim from the coordinator's anonymous namespace (only the
// linkage changes: anonymous-namespace to a named `ASFW::Audio::Backends` namespace so the
// header is includable). Behaviour is intentionally unchanged — this is a mechanical move.
// The names keep their `Dice*` prefix for now; the DICE->neutral rename is FW-71's job.

#pragma once

#include "../Duplex/DuplexRestartSession.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW::Audio::Backends {

// The recovery policy is expressed over the DICE restart-session vocabulary; bring those
// types into scope so the moved bodies stay byte-identical (they referenced them unqualified
// inside the coordinator via the same using-declarations).
using ASFW::Audio::DuplexRestartFailureCause;
using ASFW::Audio::DuplexRestartPhase;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::DuplexRestartState;

enum class DiceRecoveryDisposition : uint8_t {
    kIgnore,
    kRestart,
    kFailSession,
};

enum class DiceRecoveryPolicyReason : uint8_t {
    kRunningWithFootprint,
    kRetryableFailure,
    kIdleWithoutFootprint,
    kSuppressedByStop,
    kIdleApplyInvalidated,
    kMissingDependency,
    kNonRetryableFailure,
};

struct DiceRecoveryContext {
    DuplexRestartReason triggerReason{DuplexRestartReason::kManualReconfigure};
    DuplexRestartState state{DuplexRestartState::kIdle};
    DuplexRestartPhase phase{DuplexRestartPhase::kIdle};
    bool stopRequested{false};
    bool hasRestartIntent{false};
    bool hasHostFootprint{false};
    bool hasDeviceFootprint{false};
    bool hasDiceRecord{false};
    bool hasProtocol{false};
    bool lastFailureRetryable{false};
};

struct DiceRecoveryDecision {
    DiceRecoveryDisposition disposition{DiceRecoveryDisposition::kIgnore};
    DiceRecoveryPolicyReason reason{DiceRecoveryPolicyReason::kIdleWithoutFootprint};
};

[[nodiscard]] constexpr const char* ToString(DiceRecoveryDisposition disposition) noexcept {
    switch (disposition) {
        case DiceRecoveryDisposition::kIgnore: return "Ignore";
        case DiceRecoveryDisposition::kRestart: return "Restart";
        case DiceRecoveryDisposition::kFailSession: return "FailSession";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DiceRecoveryPolicyReason reason) noexcept {
    switch (reason) {
        case DiceRecoveryPolicyReason::kRunningWithFootprint: return "running_with_footprint";
        case DiceRecoveryPolicyReason::kRetryableFailure: return "retryable_failure";
        case DiceRecoveryPolicyReason::kIdleWithoutFootprint: return "idle_without_footprint";
        case DiceRecoveryPolicyReason::kSuppressedByStop: return "suppressed_by_stop";
        case DiceRecoveryPolicyReason::kIdleApplyInvalidated: return "idle_apply_invalidated";
        case DiceRecoveryPolicyReason::kMissingDependency: return "missing_dependency";
        case DiceRecoveryPolicyReason::kNonRetryableFailure: return "non_retryable_failure";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool IsRetryableStatus(IOReturn status) noexcept {
    return status == kIOReturnTimeout ||
           status == kIOReturnAborted ||
           status == kIOReturnNotReady ||
           status == kIOReturnNoDevice;
}

[[nodiscard]] constexpr DuplexRestartFailureCause FailureCauseForReason(
    DuplexRestartReason reason) noexcept {
    switch (reason) {
        case DuplexRestartReason::kBusResetRebind: return DuplexRestartFailureCause::kBusResetRebind;
        case DuplexRestartReason::kRecoverAfterTimingLoss: return DuplexRestartFailureCause::kTimingLoss;
        case DuplexRestartReason::kRecoverAfterCycleInconsistent:
            return DuplexRestartFailureCause::kCycleInconsistent;
        case DuplexRestartReason::kRecoverAfterLockLoss: return DuplexRestartFailureCause::kLockLoss;
        case DuplexRestartReason::kRecoverAfterTxFault: return DuplexRestartFailureCause::kTxFault;
        case DuplexRestartReason::kInitialStart:
        case DuplexRestartReason::kSampleRateChange:
        case DuplexRestartReason::kClockSourceChange:
        case DuplexRestartReason::kManualReconfigure:
            return DuplexRestartFailureCause::kNone;
    }
    return DuplexRestartFailureCause::kNone;
}

[[nodiscard]] constexpr bool IsRecoveryReason(DuplexRestartReason reason) noexcept {
    return FailureCauseForReason(reason) != DuplexRestartFailureCause::kNone;
}

[[nodiscard]] constexpr DuplexRestartState RestartStateForStartReason(
    DuplexRestartReason reason) noexcept {
    switch (reason) {
        case DuplexRestartReason::kBusResetRebind:
        case DuplexRestartReason::kRecoverAfterTimingLoss:
        case DuplexRestartReason::kRecoverAfterCycleInconsistent:
        case DuplexRestartReason::kRecoverAfterLockLoss:
        case DuplexRestartReason::kRecoverAfterTxFault:
            return DuplexRestartState::kRecovering;
        case DuplexRestartReason::kInitialStart:
        case DuplexRestartReason::kSampleRateChange:
        case DuplexRestartReason::kClockSourceChange:
        case DuplexRestartReason::kManualReconfigure:
            return DuplexRestartState::kStarting;
    }

    return DuplexRestartState::kStarting;
}

[[nodiscard]] constexpr DiceRecoveryDecision EvaluateRecoveryPolicy(
    const DiceRecoveryContext& context) noexcept {
    if (context.stopRequested || context.state == DuplexRestartState::kStopping) {
        return {
            .disposition = DiceRecoveryDisposition::kIgnore,
            .reason = DiceRecoveryPolicyReason::kSuppressedByStop,
        };
    }

    if (context.state == DuplexRestartState::kApplyingIdleClock) {
        return {
            .disposition = DiceRecoveryDisposition::kIgnore,
            .reason = DiceRecoveryPolicyReason::kIdleApplyInvalidated,
        };
    }

    const bool hasRestartFootprint =
        context.hasRestartIntent || context.hasHostFootprint || context.hasDeviceFootprint;

    if (!context.hasDiceRecord || !context.hasProtocol) {
        const bool activeSession =
            context.state == DuplexRestartState::kStarting ||
            context.state == DuplexRestartState::kRunning ||
            context.state == DuplexRestartState::kRecovering ||
            context.state == DuplexRestartState::kFailed ||
            hasRestartFootprint;
        return {
            .disposition = activeSession ? DiceRecoveryDisposition::kFailSession
                                         : DiceRecoveryDisposition::kIgnore,
            .reason = activeSession ? DiceRecoveryPolicyReason::kMissingDependency
                                    : DiceRecoveryPolicyReason::kIdleWithoutFootprint,
        };
    }

    if (context.state == DuplexRestartState::kFailed) {
        return {
            .disposition = context.lastFailureRetryable
                ? DiceRecoveryDisposition::kRestart
                : DiceRecoveryDisposition::kFailSession,
            .reason = context.lastFailureRetryable
                ? DiceRecoveryPolicyReason::kRetryableFailure
                : DiceRecoveryPolicyReason::kNonRetryableFailure,
        };
    }

    if (context.state == DuplexRestartState::kStarting ||
        context.state == DuplexRestartState::kRunning ||
        context.state == DuplexRestartState::kRecovering ||
        hasRestartFootprint) {
        return {
            .disposition = DiceRecoveryDisposition::kRestart,
            .reason = DiceRecoveryPolicyReason::kRunningWithFootprint,
        };
    }

    return {
        .disposition = DiceRecoveryDisposition::kIgnore,
        .reason = DiceRecoveryPolicyReason::kIdleWithoutFootprint,
    };
}

} // namespace ASFW::Audio::Backends
