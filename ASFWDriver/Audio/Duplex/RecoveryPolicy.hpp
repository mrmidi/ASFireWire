// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RecoveryPolicy.hpp - family-neutral duplex recovery classification
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
// The policy is intentionally independent from every family provider and from
// the session store. It classifies facts; the coordinator owns effects.

#pragma once

#include "DuplexControlTypes.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW::Audio::Duplex {

enum class RecoveryDisposition : uint8_t {
    kIgnore,
    kRestart,
    kFailSession,
};

enum class RecoveryPolicyReason : uint8_t {
    kRunningWithFootprint,
    kRetryableFailure,
    kIdleWithoutFootprint,
    kSuppressedByStop,
    kIdleApplyInvalidated,
    kMissingDependency,
    kNonRetryableFailure,
};

struct RecoveryContext {
    DuplexRestartReason triggerReason{DuplexRestartReason::kManualReconfigure};
    DuplexRestartState state{DuplexRestartState::kIdle};
    DuplexRestartPhase phase{DuplexRestartPhase::kIdle};
    bool stopRequested{false};
    bool hasRestartIntent{false};
    bool hasHostFootprint{false};
    bool hasDeviceFootprint{false};
    bool hasDeviceRecord{false};
    bool hasProtocol{false};
    bool lastFailureRetryable{false};
};

struct RecoveryDecision {
    RecoveryDisposition disposition{RecoveryDisposition::kIgnore};
    RecoveryPolicyReason reason{RecoveryPolicyReason::kIdleWithoutFootprint};
};

[[nodiscard]] constexpr const char* ToString(RecoveryDisposition disposition) noexcept {
    switch (disposition) {
        case RecoveryDisposition::kIgnore: return "Ignore";
        case RecoveryDisposition::kRestart: return "Restart";
        case RecoveryDisposition::kFailSession: return "FailSession";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(RecoveryPolicyReason reason) noexcept {
    switch (reason) {
        case RecoveryPolicyReason::kRunningWithFootprint: return "running_with_footprint";
        case RecoveryPolicyReason::kRetryableFailure: return "retryable_failure";
        case RecoveryPolicyReason::kIdleWithoutFootprint: return "idle_without_footprint";
        case RecoveryPolicyReason::kSuppressedByStop: return "suppressed_by_stop";
        case RecoveryPolicyReason::kIdleApplyInvalidated: return "idle_apply_invalidated";
        case RecoveryPolicyReason::kMissingDependency: return "missing_dependency";
        case RecoveryPolicyReason::kNonRetryableFailure: return "non_retryable_failure";
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

[[nodiscard]] constexpr RecoveryDecision EvaluateRecoveryPolicy(
    const RecoveryContext& context) noexcept {
    if (context.stopRequested || context.state == DuplexRestartState::kStopping) {
        return {
            .disposition = RecoveryDisposition::kIgnore,
            .reason = RecoveryPolicyReason::kSuppressedByStop,
        };
    }

    if (context.state == DuplexRestartState::kApplyingIdleClock) {
        return {
            .disposition = RecoveryDisposition::kIgnore,
            .reason = RecoveryPolicyReason::kIdleApplyInvalidated,
        };
    }

    const bool hasRestartFootprint =
        context.hasRestartIntent || context.hasHostFootprint || context.hasDeviceFootprint;

    if (!context.hasDeviceRecord || !context.hasProtocol) {
        const bool activeSession =
            context.state == DuplexRestartState::kStarting ||
            context.state == DuplexRestartState::kRunning ||
            context.state == DuplexRestartState::kRecovering ||
            context.state == DuplexRestartState::kFailed ||
            hasRestartFootprint;
        return {
            .disposition = activeSession ? RecoveryDisposition::kFailSession
                                         : RecoveryDisposition::kIgnore,
            .reason = activeSession ? RecoveryPolicyReason::kMissingDependency
                                    : RecoveryPolicyReason::kIdleWithoutFootprint,
        };
    }

    if (context.state == DuplexRestartState::kFailed) {
        return {
            .disposition = context.lastFailureRetryable
                ? RecoveryDisposition::kRestart
                : RecoveryDisposition::kFailSession,
            .reason = context.lastFailureRetryable
                ? RecoveryPolicyReason::kRetryableFailure
                : RecoveryPolicyReason::kNonRetryableFailure,
        };
    }

    if (context.state == DuplexRestartState::kStarting ||
        context.state == DuplexRestartState::kRunning ||
        context.state == DuplexRestartState::kRecovering ||
        hasRestartFootprint) {
        return {
            .disposition = RecoveryDisposition::kRestart,
            .reason = RecoveryPolicyReason::kRunningWithFootprint,
        };
    }

    return {
        .disposition = RecoveryDisposition::kIgnore,
        .reason = RecoveryPolicyReason::kIdleWithoutFootprint,
    };
}

} // namespace ASFW::Audio::Duplex
