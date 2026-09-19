// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexControlTypes.hpp - Protocol-neutral duplex restart vocabulary.
//
// Stage 2b Phase B: this header now OWNS the shared restart lifecycle state
// machine vocabulary (formerly defined in
// Protocols/DICE/Core/DICERestartSession.hpp and re-exported from here via
// aliases). The seam's vocabulary is neutral in fact, not just in name;
// genuinely DICE-specific protocol payload remains under Protocols/DICE/.
// The rename was mechanical: definitions moved verbatim, `Dice*` lifecycle
// type names became `Duplex*`, namespace DICE was dropped, and the alias
// block was deleted (no double paths). Behaviour is pinned by
// DICERestartSessionTests / DuplexRestartFsmTests / RestartJournalTests /
// DiceRecoveryPolicyTests, which were green before the move.

#pragma once

#include "../AudioTypes.hpp"
#include "AudioClockConfig.hpp"
#include "../../../Async/AsyncTypes.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

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

// ---------------------------------------------------------------------------
// Lifecycle states (Stage 2b Phase C).
//
// The lifecycle is a std::variant, not an enum: state-specific facts live in
// the alternative that owns them, so they cannot exist in the wrong state.
// Today exactly one alternative carries payload - Failed::terminalError -
// because the terminal error is the only fact whose authority window is a
// single state: it is written as the machine enters Failed, read by terminal
// logging, and no other state may even represent it. Cross-state facts (the
// restart reason, identity, route) stay on DuplexRestartSession: they span
// transitions and are consumed in several states (recovery consumes the
// reason of a Failed session; logging reads it in Running).
//
// Empty alternatives are deliberate. Adding payload for symmetry would
// duplicate a session fact and recreate the two-authorities defect this
// structure exists to kill. DuplexLifecycleKind remains a pure view (policy
// input, logging); the variant is the storage authority.
//
// The device/host progress bits on DuplexRestartSession are deliberately NOT
// lifecycle state: they are the rollback ledger (which resources were
// acquired), needed while Stopping to know what to unwind. They survive
// transitions into Stopping; only cleanup completion clears them.
//
// Verified on hardware 2026-09-19, Saffire Pro 24 DSP (DICE) and Apogee Duet
// (OXFW), read from the driver ring with droppedRecords=0:
//   - state == phase at all six observed terminals, across a cold start, a
//     mid-stream unplug, a normal stop and a restart. That pair is what the
//     previous two-enum shape could contradict and this one cannot.
//   - The mid-stream unplug terminated Failed/Failed with
//     rollback=kIOReturnSuccess and retryable=0: the ledger unwound what
//     Starting had acquired, and no recovery was attempted for a device that
//     was already gone.
//   - Both Stopping destinations were exercised - Removed for the vanished
//     device, Idle for a normal stop - and the normal stop preserved its
//     restartId, so stopping does not burn the epoch.
//   - The restart allocated a fresh restartId (2 -> 3), so a late completion
//     from the previous run cannot be accepted into the new one.
// ---------------------------------------------------------------------------

struct LifecycleIdle {};

/// Clock apply while no session is running (a stopped device's clock may be
/// changed without any duplex traffic).
struct LifecycleApplyingIdleClock {};

struct LifecycleStarting {};

struct LifecycleRunning {};

/// Cleanup in progress. The destination (stop-for-restart vs
/// stop-for-retirement) is a session-owner decision taken at cleanup
/// completion, not lifecycle data; the machine re-enters at Idle or Removed.
/// Both destinations observed on hardware 2026-09-19: Removed on a mid-stream
/// unplug, Idle on a normal stop.
struct LifecycleStopping {};

struct LifecycleRecovering {};

/// Terminal failure. terminalError's authority window is exactly this state.
struct LifecycleFailed {
    IOReturn terminalError{kIOReturnSuccess};
};

/// Terminal retirement of this session incarnation. Re-admission of a NEW
/// incarnation (same GUID, new route epoch) is legal; enforcement of "work
/// for the retired incarnation is rejected" stays with the operation gate and
/// the route-epoch check (single authority). Today retirement is logged at
/// ClearSession and the session record is erased - a resident Removed state
/// has no reader until the Stage 3 session owner owns the registry. The
/// transition itself is exercised: a mid-stream unplug on 2026-09-19 logged
/// the retirement, nothing referenced the retired session afterwards, and its
/// endpoint left the audio-health projection.
struct LifecycleRemoved {};

using DuplexLifecycle = std::variant<LifecycleIdle, LifecycleApplyingIdleClock,
                                     LifecycleStarting, LifecycleRunning,
                                     LifecycleStopping, LifecycleRecovering,
                                     LifecycleFailed, LifecycleRemoved>;

enum class DuplexLifecycleKind : uint8_t {
    Idle,
    ApplyingIdleClock,
    Starting,
    Running,
    Stopping,
    Recovering,
    Failed,
    Removed,
};

[[nodiscard]] constexpr DuplexLifecycleKind KindOf(const DuplexLifecycle& lifecycle) noexcept {
    switch (lifecycle.index()) {
        case 0: return DuplexLifecycleKind::Idle;
        case 1: return DuplexLifecycleKind::ApplyingIdleClock;
        case 2: return DuplexLifecycleKind::Starting;
        case 3: return DuplexLifecycleKind::Running;
        case 4: return DuplexLifecycleKind::Stopping;
        case 5: return DuplexLifecycleKind::Recovering;
        case 6: return DuplexLifecycleKind::Failed;
        case 7: default: return DuplexLifecycleKind::Removed;
    }
}

[[nodiscard]] constexpr bool LifecycleIsTerminal(DuplexLifecycleKind kind) noexcept {
    return kind == DuplexLifecycleKind::Failed || kind == DuplexLifecycleKind::Removed;
}

[[nodiscard]] constexpr bool LifecycleIsBusy(DuplexLifecycleKind kind) noexcept {
    return kind == DuplexLifecycleKind::Starting ||
           kind == DuplexLifecycleKind::Running ||
           kind == DuplexLifecycleKind::Recovering ||
           kind == DuplexLifecycleKind::ApplyingIdleClock;
}

// Transition legality, derived from the audited transition sites (one choke
// point: the journal's SetLifecycle):
//   Idle               -> Starting | ApplyingIdleClock | Recovering |
//                         Removed
//   ApplyingIdleClock  -> Idle | Failed
//   Starting           -> Running | Stopping | Recovering | Failed | Removed
//   Running            -> Stopping | Recovering | Failed | Removed
//   Stopping           -> Idle | Failed | Removed
//   Recovering         -> Running | Stopping | Failed | Removed
//   Failed             -> Starting | Recovering | Stopping | Removed |
//                         ApplyingIdleClock | Idle
//   Removed            -> Starting | Recovering (new incarnation only;
//                         route-epoch check rejects the retired one)
// (Removed-entry from every active state: the device may vanish at any point.
//  Failed -> ApplyingIdleClock: an idle clock apply on a failed session is
//  legal today and resets the session to Idle on completion.)
[[nodiscard]] constexpr bool CanTransitionLifecycle(DuplexLifecycleKind from,
                                                    DuplexLifecycleKind to) noexcept {
    switch (from) {
        case DuplexLifecycleKind::Idle:
            // Recovering: reset_before_start on the recovery restart path.
            // Failed: a recovery event may fail a session that never started.
            return to == DuplexLifecycleKind::Starting ||
                   to == DuplexLifecycleKind::ApplyingIdleClock ||
                   to == DuplexLifecycleKind::Recovering ||
                   to == DuplexLifecycleKind::Removed ||
                   to == DuplexLifecycleKind::Failed;
        case DuplexLifecycleKind::ApplyingIdleClock:
            return to == DuplexLifecycleKind::Idle ||
                   to == DuplexLifecycleKind::Failed;
        case DuplexLifecycleKind::Starting:
            // Idle: reset_before_start (a restart resets the machine first).
            return to == DuplexLifecycleKind::Idle ||
                   to == DuplexLifecycleKind::Running ||
                   to == DuplexLifecycleKind::Stopping ||
                   to == DuplexLifecycleKind::Recovering ||
                   to == DuplexLifecycleKind::Failed ||
                   to == DuplexLifecycleKind::Removed;
        case DuplexLifecycleKind::Running:
            // Idle: reset_before_start (recovery restarts from Running).
            return to == DuplexLifecycleKind::Idle ||
                   to == DuplexLifecycleKind::Stopping ||
                   to == DuplexLifecycleKind::Recovering ||
                   to == DuplexLifecycleKind::Failed ||
                   to == DuplexLifecycleKind::Removed;
        case DuplexLifecycleKind::Stopping:
            return to == DuplexLifecycleKind::Idle ||
                   to == DuplexLifecycleKind::Failed ||
                   to == DuplexLifecycleKind::Removed;
        case DuplexLifecycleKind::Recovering:
            // Idle: reset_before_start (recovery restarts from Recovering).
            return to == DuplexLifecycleKind::Idle ||
                   to == DuplexLifecycleKind::Running ||
                   to == DuplexLifecycleKind::Stopping ||
                   to == DuplexLifecycleKind::Failed ||
                   to == DuplexLifecycleKind::Removed;
        case DuplexLifecycleKind::Failed:
            // Failed -> Failed: a recovery event on an already-failed session
            // may re-fail it; the terminal payload is re-armed (last failure
            // wins). No Removed -> Removed: the record is erased at retirement.
            return to == DuplexLifecycleKind::Starting ||
                   to == DuplexLifecycleKind::Recovering ||
                   to == DuplexLifecycleKind::Stopping ||
                   to == DuplexLifecycleKind::Removed ||
                   to == DuplexLifecycleKind::ApplyingIdleClock ||
                   to == DuplexLifecycleKind::Idle ||
                   to == DuplexLifecycleKind::Failed;
        case DuplexLifecycleKind::Removed:
            // Idle covers the reset-before-start re-admission path.
            return to == DuplexLifecycleKind::Starting ||
                   to == DuplexLifecycleKind::Recovering ||
                   to == DuplexLifecycleKind::Idle;
    }
    return false;
}

[[nodiscard]] constexpr const char* ToString(DuplexLifecycleKind kind) noexcept {
    switch (kind) {
        case DuplexLifecycleKind::Idle: return "Idle";
        case DuplexLifecycleKind::ApplyingIdleClock: return "ApplyingIdleClock";
        case DuplexLifecycleKind::Starting: return "Starting";
        case DuplexLifecycleKind::Running: return "Running";
        case DuplexLifecycleKind::Stopping: return "Stopping";
        case DuplexLifecycleKind::Recovering: return "Recovering";
        case DuplexLifecycleKind::Failed: return "Failed";
        case DuplexLifecycleKind::Removed: return "Removed";
    }
    return "Unknown";
}

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

struct DuplexRestartSession {
    uint64_t guid{0};
    uint64_t restartId{0};
    Discovery::DeviceRouteToken route{};
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
    DuplexLifecycle lifecycle{LifecycleIdle{}};
    // NOTE: no terminal-error field. The terminal error lives in the
    // LifecycleFailed alternative (single authority); an Idle terminal carries
    // no error by construction.
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

// Rollback-ledger reset: clears the progress flags recording which resources
// were acquired, and moves the choreography phase. Deliberately does NOT touch
// the lifecycle - state transitions belong to the journal's terminal helpers
// (single authority over the lifecycle), and the terminal error lives in the
// Failed alternative, not on the session.
constexpr void ClearRestartProgress(DuplexRestartSession& session,
                                    DuplexRestartPhase terminalPhase = DuplexRestartPhase::kIdle) noexcept {
    session.phase = terminalPhase;

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
