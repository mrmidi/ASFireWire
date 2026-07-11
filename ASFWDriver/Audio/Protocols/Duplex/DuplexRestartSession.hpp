// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexRestartSession.hpp - Explicit restart/session state for duplex control

#pragma once

#include "../AudioTypes.hpp"
#include "AudioClockConfig.hpp"
#include "../../../Async/AsyncTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <optional>

namespace ASFW::Audio {

enum class DuplexRestartReason : uint8_t {
    kInitialStart,
    kSampleRateChange,
    kClockSourceChange,
    kBusResetRebind,
    kRecoverAfterTimingLoss,
    kRecoverAfterCycleInconsistent,
    kRecoverAfterLockLoss,
    kRecoverAfterTxFault,
    kManualReconfigure,
};

enum class DuplexRestartPhase : uint8_t {
    kIdle,
    kPreparingDevice,
    kPrepared,
    kReservingPlaybackResources,
    kProgrammingDeviceRx,
    kDeviceRxProgrammed,
    kReservingCaptureResources,
    kStartingHostReceive,
    kProgrammingDeviceTx,
    kDeviceTxArmed,
    kWaitingGlobalClock,
    kStartingHostTransmit,
    kConfirmingDeviceStart,
    kRunning,
    kStopping,
    kFailed,
};

enum class DuplexRestartState : uint8_t {
    kIdle,
    kApplyingIdleClock,
    kStarting,
    kRunning,
    kStopping,
    kRecovering,
    kFailed,
};

enum class DuplexClockRequestOutcome : uint8_t {
    kApplied,
    kSuperseded,
    kAbortedByStop,
    kFailed,
};

enum class DuplexRestartErrorClass : uint8_t {
    kUnsupportedConfig,
    kMissingDependency,
    kStageFailure,
    kEpochInvalidated,
    kStopIntent,
};

enum class DuplexRestartFailureCause : uint8_t {
    kNone,
    kPrepare,
    kReservePlayback,
    kProgramRx,
    kReserveCapture,
    kStartReceive,
    kProgramTx,
    kGlobalClockLock,
    kStartTransmit,
    kConfirmStart,
    kIdleClockApply,
    kStop,
    kBusResetRebind,
    kTimingLoss,
    kCycleInconsistent,
    kLockLoss,
    kTxFault,
};

struct DuplexClockRequestCompletion {
    uint64_t token{0};
    AudioClockConfig desiredClock{};
    DuplexRestartReason reason{DuplexRestartReason::kManualReconfigure};
    DuplexClockRequestOutcome outcome{DuplexClockRequestOutcome::kFailed};
    IOReturn status{kIOReturnSuccess};
    uint64_t restartId{0};
    FW::Generation generation{0};
};

struct DuplexRestartIssueInfo {
    DuplexRestartPhase failedPhase{DuplexRestartPhase::kIdle};
    DuplexRestartErrorClass errorClass{DuplexRestartErrorClass::kStageFailure};
    DuplexRestartFailureCause cause{DuplexRestartFailureCause::kNone};
    IOReturn status{kIOReturnSuccess};
    bool retryable{false};
    bool rollbackAttempted{false};
    IOReturn rollbackStatus{kIOReturnSuccess};
    bool hostStateKnown{true};
    bool deviceStateKnown{true};
    uint64_t restartId{0};
    FW::Generation generation{0};
};

struct DuplexPrepareResult {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
};

struct DuplexStageResult {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    DuplexRestartPhase phase{DuplexRestartPhase::kIdle};
    AudioStreamRuntimeCaps runtimeCaps{};
};

struct DuplexConfirmResult {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
    uint32_t notification{0};
    uint32_t status{0};
    uint32_t extStatus{0};
};

struct ClockApplyResult {
    FW::Generation generation{0};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
};

struct DuplexHealthResult {
    FW::Generation generation{0};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
    bool sourceLocked{false};
    bool clockReferenceHealthy{true};
    uint32_t nominalRateHz{0};
    uint32_t notification{0};
    uint32_t status{0};
    uint32_t extStatus{0};
};

struct DuplexRestartSession {
    uint64_t guid{0};
    uint64_t restartId{0};
    FW::Generation generation{0};
    FW::Generation topologyGeneration{0};
    AudioDuplexChannels channels{};
    DuplexRestartReason reason{DuplexRestartReason::kInitialStart};
    AudioClockConfig desiredClock{};
    AudioClockConfig appliedClock{};
    AudioClockConfig pendingClock{};
    DuplexRestartReason pendingReason{DuplexRestartReason::kInitialStart};
    AudioStreamRuntimeCaps runtimeCaps{};
    DuplexRestartPhase phase{DuplexRestartPhase::kIdle};
    DuplexRestartState state{DuplexRestartState::kIdle};
    IOReturn terminalError{kIOReturnSuccess};
    std::optional<DuplexRestartIssueInfo> lastFailure{};
    std::optional<DuplexRestartIssueInfo> lastInvalidation{};
    std::optional<DuplexClockRequestCompletion> lastClockCompletion{};

    bool ownerClaimed{false};
    bool devicePrepared{false};
    bool deviceRxProgrammed{false};
    bool deviceTxArmed{false};
    bool deviceRunning{false};
    bool hasPendingClockRequest{false};

    bool hostDuplexClaimed{false};
    bool hostPlaybackReserved{false};
    bool hostCaptureReserved{false};
    bool hostReceiveStarted{false};
    bool hostTransmitStarted{false};
};

[[nodiscard]] constexpr bool HasRestartIntent(const DuplexRestartSession& session) noexcept {
    return session.desiredClock.sampleRateHz != 0 || session.hasPendingClockRequest;
}

[[nodiscard]] constexpr bool HasDeviceRestartState(const DuplexRestartSession& session) noexcept {
    return session.ownerClaimed ||
           session.devicePrepared ||
           session.deviceRxProgrammed ||
           session.deviceTxArmed ||
           session.deviceRunning;
}

[[nodiscard]] constexpr bool HasHostRestartState(const DuplexRestartSession& session) noexcept {
    return session.hostDuplexClaimed ||
           session.hostPlaybackReserved ||
           session.hostCaptureReserved ||
           session.hostReceiveStarted ||
           session.hostTransmitStarted;
}

[[nodiscard]] constexpr bool HasAnyRestartState(const DuplexRestartSession& session) noexcept {
    return HasDeviceRestartState(session) || HasHostRestartState(session);
}

constexpr void ClearRestartProgress(DuplexRestartSession& session,
                                    DuplexRestartPhase terminalPhase = DuplexRestartPhase::kIdle) noexcept {
    session.phase = terminalPhase;
    session.terminalError = (terminalPhase == DuplexRestartPhase::kFailed)
        ? session.terminalError
        : kIOReturnSuccess;
    if (terminalPhase == DuplexRestartPhase::kIdle) {
        session.state = DuplexRestartState::kIdle;
    } else if (terminalPhase == DuplexRestartPhase::kFailed) {
        session.state = DuplexRestartState::kFailed;
    }

    session.ownerClaimed = false;
    session.devicePrepared = false;
    session.deviceRxProgrammed = false;
    session.deviceTxArmed = false;
    session.deviceRunning = false;

    session.hostDuplexClaimed = false;
    session.hostPlaybackReserved = false;
    session.hostCaptureReserved = false;
    session.hostReceiveStarted = false;
    session.hostTransmitStarted = false;
}

[[nodiscard]] constexpr DuplexRestartReason ClassifyRestartReason(
    const DuplexRestartSession* previousSession,
    const AudioClockConfig& desiredClock) noexcept {
    if (previousSession == nullptr || !HasRestartIntent(*previousSession)) {
        return DuplexRestartReason::kInitialStart;
    }

    if (previousSession->desiredClock.sampleRateHz != 0 &&
        previousSession->desiredClock.sampleRateHz != desiredClock.sampleRateHz) {
        return DuplexRestartReason::kSampleRateChange;
    }

    if (previousSession->phase == DuplexRestartPhase::kFailed) {
        return DuplexRestartReason::kRecoverAfterTimingLoss;
    }

    return DuplexRestartReason::kManualReconfigure;
}

} // namespace ASFW::Audio
