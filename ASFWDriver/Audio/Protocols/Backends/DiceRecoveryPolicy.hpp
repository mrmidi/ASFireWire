// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceRecoveryPolicy.hpp
//
// The recovery-policy classification vocabulary (FW-66): the pure `EvaluateRecoveryPolicy`
// kernel plus the reason classifiers (`RestartStateForStartReason`, `FailureCauseForReason`,
// `IsRecoveryReason`), the retryability predicate (`IsRetryableStatus`), the enums,
// context/decision structs and ToString overloads they depend on. Extracted from
// DiceDuplexRestartCoordinator.cpp so classification is a standalone, directly-testable unit
// with no dependency on the session store or logging (per FW-66). Classification returns
// facts; recording/logging them stays in the coordinator (FW-69 journal).
//
// The definitions are copied verbatim from the coordinator's anonymous namespace (only the
// linkage changes: anonymous-namespace to a named `ASFW::Audio::Backends` namespace so the
// header is includable). Behaviour is intentionally unchanged — this is a mechanical move.
// The names keep their `Dice*` prefix for now; the DICE->neutral rename is FW-71's job.

#pragma once

#include "../DICE/Core/DICERestartSession.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW::Audio::Backends {

// The recovery policy is expressed over the DICE restart-session vocabulary; bring those
// types into scope so the moved bodies stay byte-identical (they referenced them unqualified
// inside the coordinator via the same using-declarations).
using ASFW::Audio::DICE::DiceRestartFailureCause;
using ASFW::Audio::DICE::DiceRestartPhase;
using ASFW::Audio::DICE::DiceRestartReason;
using ASFW::Audio::DICE::DiceRestartState;

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
    DiceRestartReason triggerReason{DiceRestartReason::kManualReconfigure};
    DiceRestartState state{DiceRestartState::kIdle};
    DiceRestartPhase phase{DiceRestartPhase::kIdle};
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

[[nodiscard]] constexpr DiceRestartFailureCause FailureCauseForReason(
    DiceRestartReason reason) noexcept {
    switch (reason) {
        case DiceRestartReason::kBusResetRebind: return DiceRestartFailureCause::kBusResetRebind;
        case DiceRestartReason::kRecoverAfterTimingLoss: return DiceRestartFailureCause::kTimingLoss;
        case DiceRestartReason::kRecoverAfterCycleInconsistent:
            return DiceRestartFailureCause::kCycleInconsistent;
        case DiceRestartReason::kRecoverAfterLockLoss: return DiceRestartFailureCause::kLockLoss;
        case DiceRestartReason::kRecoverAfterTxFault: return DiceRestartFailureCause::kTxFault;
        case DiceRestartReason::kInitialStart:
        case DiceRestartReason::kSampleRateChange:
        case DiceRestartReason::kClockSourceChange:
        case DiceRestartReason::kManualReconfigure:
            return DiceRestartFailureCause::kNone;
    }
    return DiceRestartFailureCause::kNone;
}

[[nodiscard]] constexpr bool IsRecoveryReason(DiceRestartReason reason) noexcept {
    return FailureCauseForReason(reason) != DiceRestartFailureCause::kNone;
}

[[nodiscard]] constexpr DiceRestartState RestartStateForStartReason(
    DiceRestartReason reason) noexcept {
    switch (reason) {
        case DiceRestartReason::kBusResetRebind:
        case DiceRestartReason::kRecoverAfterTimingLoss:
        case DiceRestartReason::kRecoverAfterCycleInconsistent:
        case DiceRestartReason::kRecoverAfterLockLoss:
        case DiceRestartReason::kRecoverAfterTxFault:
            return DiceRestartState::kRecovering;
        case DiceRestartReason::kInitialStart:
        case DiceRestartReason::kSampleRateChange:
        case DiceRestartReason::kClockSourceChange:
        case DiceRestartReason::kManualReconfigure:
            return DiceRestartState::kStarting;
    }

    return DiceRestartState::kStarting;
}

[[nodiscard]] constexpr DiceRecoveryDecision EvaluateRecoveryPolicy(
    const DiceRecoveryContext& context) noexcept {
    if (context.stopRequested || context.state == DiceRestartState::kStopping) {
        return {
            .disposition = DiceRecoveryDisposition::kIgnore,
            .reason = DiceRecoveryPolicyReason::kSuppressedByStop,
        };
    }

    if (context.state == DiceRestartState::kApplyingIdleClock) {
        return {
            .disposition = DiceRecoveryDisposition::kIgnore,
            .reason = DiceRecoveryPolicyReason::kIdleApplyInvalidated,
        };
    }

    const bool hasRestartFootprint =
        context.hasRestartIntent || context.hasHostFootprint || context.hasDeviceFootprint;

    if (!context.hasDiceRecord || !context.hasProtocol) {
        const bool activeSession =
            context.state == DiceRestartState::kStarting ||
            context.state == DiceRestartState::kRunning ||
            context.state == DiceRestartState::kRecovering ||
            context.state == DiceRestartState::kFailed ||
            hasRestartFootprint;
        return {
            .disposition = activeSession ? DiceRecoveryDisposition::kFailSession
                                         : DiceRecoveryDisposition::kIgnore,
            .reason = activeSession ? DiceRecoveryPolicyReason::kMissingDependency
                                    : DiceRecoveryPolicyReason::kIdleWithoutFootprint,
        };
    }

    if (context.state == DiceRestartState::kFailed) {
        return {
            .disposition = context.lastFailureRetryable
                ? DiceRecoveryDisposition::kRestart
                : DiceRecoveryDisposition::kFailSession,
            .reason = context.lastFailureRetryable
                ? DiceRecoveryPolicyReason::kRetryableFailure
                : DiceRecoveryPolicyReason::kNonRetryableFailure,
        };
    }

    if (context.state == DiceRestartState::kStarting ||
        context.state == DiceRestartState::kRunning ||
        context.state == DiceRestartState::kRecovering ||
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
