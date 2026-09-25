// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexControlTypes.hpp - Values exchanged across IDuplexDeviceControl.
//
// Why a restart happens, the result each device stage reports, and the
// device-side progress a DICE bring-up tracks. Session state (what should run,
// what runs) belongs to Audio/Session/SessionScheduler; the lifecycle variant,
// clock-request tokens and rollback ledger that used to live here went with
// AudioDuplexCoordinator (documentation/AUDIO_SESSION_REDESIGN.md, stage S2).

#pragma once

#include "../AudioTypes.hpp"
#include "AudioClockConfig.hpp"
#include "../../../Async/AsyncTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>

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
    // The device announced a stream-configuration change (DICE RX/TX_CFG_CHG).
    // TCAT restarts streaming on it (NotificationWriteCallback).
    kDeviceConfigChange,
};

// How far a device-side bring-up got. Reported in stage results and logs.
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

struct DuplexClockApplyResult {
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

// A DICE bring-up's device-side progress: what it claimed, armed and enabled,
// so its stop and rollback know what to undo.
struct DuplexRestartSession {
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    DuplexRestartReason reason{DuplexRestartReason::kInitialStart};
    AudioClockConfig desiredClock{};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
    DuplexRestartPhase phase{DuplexRestartPhase::kIdle};

    bool ownerClaimed{false};
    bool devicePrepared{false};
    bool deviceRxProgrammed{false};
    bool deviceTxArmed{false};
    bool deviceRunning{false};
};

[[nodiscard]] constexpr bool HasDeviceRestartState(const DuplexRestartSession& session) noexcept {
    return session.ownerClaimed ||
           session.devicePrepared ||
           session.deviceRxProgrammed ||
           session.deviceTxArmed ||
           session.deviceRunning;
}

// Clear the progress flags and move to `terminalPhase`.
constexpr void ClearRestartProgress(DuplexRestartSession& session,
                                    DuplexRestartPhase terminalPhase = DuplexRestartPhase::kIdle) noexcept {
    session.phase = terminalPhase;
    session.ownerClaimed = false;
    session.devicePrepared = false;
    session.deviceRxProgrammed = false;
    session.deviceTxArmed = false;
    session.deviceRunning = false;
}

} // namespace ASFW::Audio
