// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexRecoveryPolicy.hpp (FW-66 kernel; renamed from DiceRecoveryPolicy.hpp in
// Stage 2b Phase B - the planned DICE->neutral rename)
//
// The recovery-policy classification vocabulary (FW-66): the pure `EvaluateRecoveryPolicy`
// kernel plus the reason classifiers (`RestartStateForStartReason`, `FailureCauseForReason`,
// `IsRecoveryReason`), the retryability predicate (`IsRetryableStatus`), the enums,
// context/decision structs and ToString overloads they depend on. Extracted from
// AudioDuplexCoordinator.cpp so classification is a standalone, directly-testable unit
// with no dependency on the session store or logging (per FW-66). Classification returns
// facts; recording/logging them stays in the coordinator (FW-69 journal).
//
// Behaviour is intentionally unchanged; the vocabulary now comes from the neutral
// DuplexControlTypes.hpp (Stage 2b Phase B renamed the types, semantics pinned by
// DuplexRecoveryPolicyTests).

#pragma once

#include "../Duplex/DuplexControlTypes.hpp"

#include <DriverKit/IOLib.h>

#include <cstdint>

namespace ASFW::Audio::Backends {

// The recovery policy is expressed over the neutral duplex restart vocabulary (owned by
// DuplexControlTypes.hpp since Stage 2b Phase B).

enum class DuplexRecoveryDisposition : uint8_t {
    kIgnore,
    kRestart,
    kFailSession,
};

enum class DuplexRecoveryPolicyReason : uint8_t {
    kRunningWithFootprint,
    kRetryableFailure,
    kIdleWithoutFootprint,
    kSuppressedByStop,
    kIdleApplyInvalidated,
    kMissingDependency,
    kNonRetryableFailure,
};

struct DuplexRecoveryContext {
    DuplexRestartReason triggerReason{DuplexRestartReason::kManualReconfigure};
    DuplexLifecycleKind state{DuplexLifecycleKind::Idle};
    DuplexRestartPhase phase{DuplexRestartPhase::kIdle};
    bool stopRequested{false};
    bool hasRestartIntent{false};
    bool hasHostFootprint{false};
    bool hasDeviceFootprint{false};
    bool hasDiceRecord{false};
    bool hasProtocol{false};
    bool lastFailureRetryable{false};
};

struct DuplexRecoveryDecision {
    DuplexRecoveryDisposition disposition{DuplexRecoveryDisposition::kIgnore};
    DuplexRecoveryPolicyReason reason{DuplexRecoveryPolicyReason::kIdleWithoutFootprint};
};

[[nodiscard]] constexpr const char* ToString(DuplexRecoveryDisposition disposition) noexcept {
    switch (disposition) {
        case DuplexRecoveryDisposition::kIgnore: return "Ignore";
        case DuplexRecoveryDisposition::kRestart: return "Restart";
        case DuplexRecoveryDisposition::kFailSession: return "FailSession";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* ToString(DuplexRecoveryPolicyReason reason) noexcept {
    switch (reason) {
        case DuplexRecoveryPolicyReason::kRunningWithFootprint: return "running_with_footprint";
        case DuplexRecoveryPolicyReason::kRetryableFailure: return "retryable_failure";
        case DuplexRecoveryPolicyReason::kIdleWithoutFootprint: return "idle_without_footprint";
        case DuplexRecoveryPolicyReason::kSuppressedByStop: return "suppressed_by_stop";
        case DuplexRecoveryPolicyReason::kIdleApplyInvalidated: return "idle_apply_invalidated";
        case DuplexRecoveryPolicyReason::kMissingDependency: return "missing_dependency";
        case DuplexRecoveryPolicyReason::kNonRetryableFailure: return "non_retryable_failure";
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

/// Phase C: the kind decides the alternative. Alternatives are empty — the
/// restart reason is session-scoped (session.reason), not state data.
[[nodiscard]] constexpr DuplexLifecycle LifecycleForStart(
    DuplexLifecycleKind kind, DuplexRestartReason reason) noexcept {
    return kind == DuplexLifecycleKind::Recovering
        ? DuplexLifecycle{LifecycleRecovering{}}
        : DuplexLifecycle{LifecycleStarting{}};
}

[[nodiscard]] constexpr DuplexLifecycleKind RestartStateForStartReason(
    DuplexRestartReason reason) noexcept {
    switch (reason) {
        case DuplexRestartReason::kBusResetRebind:
        case DuplexRestartReason::kRecoverAfterTimingLoss:
        case DuplexRestartReason::kRecoverAfterCycleInconsistent:
        case DuplexRestartReason::kRecoverAfterLockLoss:
        case DuplexRestartReason::kRecoverAfterTxFault:
            return DuplexLifecycleKind::Recovering;
        case DuplexRestartReason::kInitialStart:
        case DuplexRestartReason::kSampleRateChange:
        case DuplexRestartReason::kClockSourceChange:
        case DuplexRestartReason::kManualReconfigure:
            return DuplexLifecycleKind::Starting;
    }

    return DuplexLifecycleKind::Starting;
}



[[nodiscard]] constexpr DuplexRecoveryDecision EvaluateRecoveryPolicy(
    const DuplexRecoveryContext& context) noexcept {
    if (context.stopRequested || context.state == DuplexLifecycleKind::Stopping) {
        return {
            .disposition = DuplexRecoveryDisposition::kIgnore,
            .reason = DuplexRecoveryPolicyReason::kSuppressedByStop,
        };
    }

    if (context.state == DuplexLifecycleKind::ApplyingIdleClock) {
        return {
            .disposition = DuplexRecoveryDisposition::kIgnore,
            .reason = DuplexRecoveryPolicyReason::kIdleApplyInvalidated,
        };
    }

    const bool hasRestartFootprint =
        context.hasRestartIntent || context.hasHostFootprint || context.hasDeviceFootprint;

    if (!context.hasDiceRecord || !context.hasProtocol) {
        const bool activeSession =
            context.state == DuplexLifecycleKind::Starting ||
            context.state == DuplexLifecycleKind::Running ||
            context.state == DuplexLifecycleKind::Recovering ||
            context.state == DuplexLifecycleKind::Failed ||
            hasRestartFootprint;
        return {
            .disposition = activeSession ? DuplexRecoveryDisposition::kFailSession
                                         : DuplexRecoveryDisposition::kIgnore,
            .reason = activeSession ? DuplexRecoveryPolicyReason::kMissingDependency
                                    : DuplexRecoveryPolicyReason::kIdleWithoutFootprint,
        };
    }

    if (context.state == DuplexLifecycleKind::Failed) {
        return {
            .disposition = context.lastFailureRetryable
                ? DuplexRecoveryDisposition::kRestart
                : DuplexRecoveryDisposition::kFailSession,
            .reason = context.lastFailureRetryable
                ? DuplexRecoveryPolicyReason::kRetryableFailure
                : DuplexRecoveryPolicyReason::kNonRetryableFailure,
        };
    }

    if (context.state == DuplexLifecycleKind::Starting ||
        context.state == DuplexLifecycleKind::Running ||
        context.state == DuplexLifecycleKind::Recovering ||
        hasRestartFootprint) {
        return {
            .disposition = DuplexRecoveryDisposition::kRestart,
            .reason = DuplexRecoveryPolicyReason::kRunningWithFootprint,
        };
    }

    return {
        .disposition = DuplexRecoveryDisposition::kIgnore,
        .reason = DuplexRecoveryPolicyReason::kIdleWithoutFootprint,
    };
}

} // namespace ASFW::Audio::Backends
