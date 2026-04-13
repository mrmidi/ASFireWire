// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DICERestartSession.hpp - Explicit restart/session state for DICE duplex control

#pragma once

#include "DICETypes.hpp"
#include "../../AudioTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <optional>

namespace ASFW::Audio::DICE {

constexpr uint32_t kDiceClockSelect48kInternal =
    (ClockRateIndex::k48000 << ClockSelect::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);

enum class DiceRestartReason : uint8_t {
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

enum class DiceRestartPhase : uint8_t {
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
    kStartingHostTransmit,
    kConfirmingDeviceStart,
    kRunning,
    kStopping,
    kFailed,
};

enum class DiceRestartState : uint8_t {
    kIdle,
    kApplyingIdleClock,
    kStarting,
    kRunning,
    kStopping,
    kRecovering,
    kFailed,
};

enum class DiceClockRequestOutcome : uint8_t {
    kApplied,
    kSuperseded,
    kAbortedByStop,
    kFailed,
};

enum class DiceRestartErrorClass : uint8_t {
    kUnsupportedConfig,
    kMissingDependency,
    kStageFailure,
    kEpochInvalidated,
    kStopIntent,
};

enum class DiceRestartFailureCause : uint8_t {
    kNone,
    kPrepare,
    kReservePlayback,
    kProgramRx,
    kReserveCapture,
    kStartReceive,
    kProgramTx,
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

struct DiceDesiredClockConfig {
    uint32_t sampleRateHz{0};
    uint32_t clockSelect{0};
};

struct DiceClockRequestCompletion {
    uint64_t token{0};
    DiceDesiredClockConfig desiredClock{};
    DiceRestartReason reason{DiceRestartReason::kManualReconfigure};
    DiceClockRequestOutcome outcome{DiceClockRequestOutcome::kFailed};
    IOReturn status{kIOReturnSuccess};
    uint64_t restartId{0};
    FW::Generation generation{0};
};

struct DiceRestartIssueInfo {
    DiceRestartPhase failedPhase{DiceRestartPhase::kIdle};
    DiceRestartErrorClass errorClass{DiceRestartErrorClass::kStageFailure};
    DiceRestartFailureCause cause{DiceRestartFailureCause::kNone};
    IOReturn status{kIOReturnSuccess};
    bool retryable{false};
    bool rollbackAttempted{false};
    IOReturn rollbackStatus{kIOReturnSuccess};
    bool hostStateKnown{true};
    bool deviceStateKnown{true};
    uint64_t restartId{0};
    FW::Generation generation{0};
};

struct DiceDuplexPrepareResult {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    DiceDesiredClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
};

struct DiceDuplexStageResult {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    DiceRestartPhase phase{DiceRestartPhase::kIdle};
    AudioStreamRuntimeCaps runtimeCaps{};
};

struct DiceDuplexConfirmResult {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    DiceDesiredClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
    uint32_t notification{0};
    uint32_t status{0};
    uint32_t extStatus{0};
};

struct DiceClockApplyResult {
    FW::Generation generation{0};
    DiceDesiredClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
};

struct DiceDuplexHealthResult {
    FW::Generation generation{0};
    DiceDesiredClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
    uint32_t notification{0};
    uint32_t status{0};
    uint32_t extStatus{0};
};

struct DiceRestartSession {
    uint64_t guid{0};
    uint64_t restartId{0};
    FW::Generation generation{0};
    FW::Generation topologyGeneration{0};
    AudioDuplexChannels channels{};
    DiceRestartReason reason{DiceRestartReason::kInitialStart};
    DiceDesiredClockConfig desiredClock{};
    DiceDesiredClockConfig appliedClock{};
    DiceDesiredClockConfig pendingClock{};
    DiceRestartReason pendingReason{DiceRestartReason::kInitialStart};
    AudioStreamRuntimeCaps runtimeCaps{};
    DiceRestartPhase phase{DiceRestartPhase::kIdle};
    DiceRestartState state{DiceRestartState::kIdle};
    IOReturn terminalError{kIOReturnSuccess};
    std::optional<DiceRestartIssueInfo> lastFailure{};
    std::optional<DiceRestartIssueInfo> lastInvalidation{};
    std::optional<DiceClockRequestCompletion> lastClockCompletion{};

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

[[nodiscard]] constexpr bool HasRestartIntent(const DiceRestartSession& session) noexcept {
    return session.desiredClock.sampleRateHz != 0 ||
           session.desiredClock.clockSelect != 0 ||
           session.hasPendingClockRequest;
}

[[nodiscard]] constexpr bool HasDeviceRestartState(const DiceRestartSession& session) noexcept {
    return session.ownerClaimed ||
           session.devicePrepared ||
           session.deviceRxProgrammed ||
           session.deviceTxArmed ||
           session.deviceRunning;
}

[[nodiscard]] constexpr bool HasHostRestartState(const DiceRestartSession& session) noexcept {
    return session.hostDuplexClaimed ||
           session.hostPlaybackReserved ||
           session.hostCaptureReserved ||
           session.hostReceiveStarted ||
           session.hostTransmitStarted;
}

[[nodiscard]] constexpr bool HasAnyRestartState(const DiceRestartSession& session) noexcept {
    return HasDeviceRestartState(session) || HasHostRestartState(session);
}

constexpr void ClearRestartProgress(DiceRestartSession& session,
                                    DiceRestartPhase terminalPhase = DiceRestartPhase::kIdle) noexcept {
    session.phase = terminalPhase;
    session.terminalError = (terminalPhase == DiceRestartPhase::kFailed)
        ? session.terminalError
        : kIOReturnSuccess;
    if (terminalPhase == DiceRestartPhase::kIdle) {
        session.state = DiceRestartState::kIdle;
    } else if (terminalPhase == DiceRestartPhase::kFailed) {
        session.state = DiceRestartState::kFailed;
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

[[nodiscard]] constexpr bool IsSupportedClockConfig(
    const DiceDesiredClockConfig& desiredClock) noexcept {
    return desiredClock.sampleRateHz == 48000U &&
           desiredClock.clockSelect == kDiceClockSelect48kInternal;
}

[[nodiscard]] constexpr DiceRestartReason ClassifyRestartReason(
    const DiceRestartSession* previousSession,
    const DiceDesiredClockConfig& desiredClock) noexcept {
    if (previousSession == nullptr || !HasRestartIntent(*previousSession)) {
        return DiceRestartReason::kInitialStart;
    }

    if (previousSession->desiredClock.sampleRateHz != 0 &&
        previousSession->desiredClock.sampleRateHz != desiredClock.sampleRateHz) {
        return DiceRestartReason::kSampleRateChange;
    }

    if (previousSession->desiredClock.clockSelect != 0 &&
        previousSession->desiredClock.clockSelect != desiredClock.clockSelect) {
        return DiceRestartReason::kClockSourceChange;
    }

    if (previousSession->phase == DiceRestartPhase::kFailed) {
        return DiceRestartReason::kRecoverAfterTimingLoss;
    }

    return DiceRestartReason::kManualReconfigure;
}

} // namespace ASFW::Audio::DICE
