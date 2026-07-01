// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceDuplexRestartCoordinator.hpp"
#include "SyncAsyncBridge.hpp"

#include "../../Core/AudioRuntimeRegistry.hpp"
#include "../../Model/ASFWAudioDevice.hpp"
#include "../../../Logging/Logging.hpp"
#include "../DICE/Core/IDICEDuplexProtocol.hpp"
#include "../DeviceProtocolFactory.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace ASFW::Audio {

namespace {

using ASFW::Audio::DICE::ClearRestartProgress;
using ASFW::Audio::DICE::DiceClockRequestCompletion;
using ASFW::Audio::DICE::DiceClockRequestOutcome;
using ASFW::Audio::DICE::DiceDesiredClockConfig;
using ASFW::Audio::DICE::DiceDuplexConfirmResult;
using ASFW::Audio::DICE::DiceDuplexHealthResult;
using ASFW::Audio::DICE::DiceDuplexPrepareResult;
using ASFW::Audio::DICE::DiceDuplexStageResult;
using ASFW::Audio::DICE::DiceRestartErrorClass;
using ASFW::Audio::DICE::DiceRestartFailureCause;
using ASFW::Audio::DICE::DiceRestartPhase;
using ASFW::Audio::DICE::DiceRestartReason;
using ASFW::Audio::DICE::DiceRestartIssueInfo;
using ASFW::Audio::DICE::DiceRestartState;
using ASFW::Audio::DICE::DiceRestartSession;
using ASFW::Audio::DICE::HasAnyRestartState;
using ASFW::Audio::DICE::HasDeviceRestartState;
using ASFW::Audio::DICE::HasHostRestartState;
using ASFW::Audio::DICE::HasRestartIntent;
using ASFW::Audio::DICE::IsSupportedClockConfig;
using ASFW::Audio::DICE::kDiceClockSelect48kInternal;

constexpr uint8_t kDefaultIrChannel = 1;
constexpr uint8_t kDefaultItChannel = 0;
constexpr uint32_t kPlaybackBandwidthUnits = 320;
constexpr uint32_t kCaptureBandwidthUnits = 576;
constexpr uint32_t kClockRequestWaitTimeoutMs = 15000;

[[nodiscard]] uint64_t UptimeMilliseconds() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
        timebase.denom == 0) {
        return 0;
    }
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_absolute_time()) *
        timebase.numer / timebase.denom;
    return static_cast<uint64_t>(nanos / 1'000'000U);
}

[[nodiscard]] bool IsValidIsoChannel(uint8_t channel) noexcept {
    return channel <= 0x3F;
}

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

[[nodiscard]] AudioDuplexChannels ResolveDuplexChannelsForRecord(
    const Discovery::DeviceRecord& record,
    const IDeviceProtocol* protocol) noexcept {
    AudioDuplexChannels channels{
        .deviceToHostIsoChannel = kDefaultIrChannel,
        .hostToDeviceIsoChannel = kDefaultItChannel,
    };

    AudioStreamRuntimeCaps caps{};
    bool haveCaps = protocol && protocol->GetRuntimeAudioStreamCaps(caps);

    // Stream counts come from the device's TX_NUMBER/RX_NUMBER (includes streams
    // currently reported with iso=-1 that the host must still arm). Single-stream
    // devices report 1/1 and take exactly the legacy code path below.
    auto clampCount = [](uint32_t n) noexcept -> uint32_t {
        if (n == 0) return 1;
        return (n > kMaxAudioStreamsPerDirection) ? kMaxAudioStreamsPerDirection : n;
    };
    channels.captureStreamCount =
        haveCaps ? clampCount(caps.deviceToHostStreamCount) : 1;
    channels.playbackStreamCount =
        haveCaps ? clampCount(caps.hostToDeviceStreamCount) : 1;

    // Assign distinct iso channels across both directions. stream[0] keeps the
    // device-reported channel (or the legacy default) so the single-stream host
    // path is byte-for-byte unchanged; additional streams get the lowest free
    // channels. The host allocates these (it owns the IRM reservation) and writes
    // them into the device's per-stream ISOC registers before GLOBAL_ENABLE.
    uint64_t used = 0; // bitset over channels 0..63
    auto markUsed = [&](uint8_t ch) noexcept {
        if (ch <= 0x3F) used |= (uint64_t{1} << ch);
    };
    auto nextFree = [&]() noexcept -> uint8_t {
        for (uint8_t ch = 0; ch <= 0x3F; ++ch) {
            if ((used & (uint64_t{1} << ch)) == 0) {
                used |= (uint64_t{1} << ch);
                return ch;
            }
        }
        return AudioStreamWireInfo::kInvalidIsoChannel;
    };

    channels.captureIsoChannels[0] =
        (haveCaps && IsValidIsoChannel(caps.deviceToHostIsoChannel))
            ? caps.deviceToHostIsoChannel
            : kDefaultIrChannel;
    channels.playbackIsoChannels[0] =
        (haveCaps && IsValidIsoChannel(caps.hostToDeviceIsoChannel))
            ? caps.hostToDeviceIsoChannel
            : kDefaultItChannel;
    markUsed(channels.captureIsoChannels[0]);
    markUsed(channels.playbackIsoChannels[0]);

    for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
        channels.captureIsoChannels[i] = nextFree();
    }
    for (uint32_t i = 1; i < channels.playbackStreamCount; ++i) {
        channels.playbackIsoChannels[i] = nextFree();
    }

    // Legacy single-channel fields mirror stream[0].
    channels.deviceToHostIsoChannel = channels.captureIsoChannels[0];
    channels.hostToDeviceIsoChannel = channels.playbackIsoChannels[0];
    return channels;
}

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

[[nodiscard]] constexpr uint32_t GenerationValue(FW::Generation generation) noexcept {
    return generation.value;
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

void LogFsmEvent(const char* eventName,
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

void LogStateTransition(const DiceRestartSession& session,
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

void LogPhaseTransition(const DiceRestartSession& session,
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

void SetSessionState(DiceRestartSession& session,
                     DiceRestartState newState,
                     const char* why) noexcept {
    const auto oldState = session.state;
    session.state = newState;
    LogStateTransition(session, oldState, newState, why);
}

void SetSessionPhase(DiceRestartSession& session, DiceRestartPhase newPhase) noexcept {
    const auto oldPhase = session.phase;
    session.phase = newPhase;
    LogPhaseTransition(session, oldPhase, newPhase);
}

void ApplyTerminalPhase(DiceRestartSession& session,
                        DiceRestartPhase terminalPhase,
                        const char* why) noexcept {
    const auto oldState = session.state;
    const auto oldPhase = session.phase;
    ClearRestartProgress(session, terminalPhase);
    LogPhaseTransition(session, oldPhase, session.phase);
    LogStateTransition(session, oldState, session.state, why);
}

void ClearFailureSnapshot(DiceRestartSession& session) noexcept {
    session.lastFailure.reset();
}

void RecordIssue(DiceRestartSession& session,
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

void LogInvalidation(const DiceRestartSession& session) noexcept {
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

void LogRecoveryPolicy(const DiceRestartSession& session,
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

void LogTerminal(const DiceRestartSession& session) noexcept {
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

inline uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

[[nodiscard]] constexpr Encoding::AudioWireFormat ResolveDicePlaybackWireFormat(
    const Discovery::DeviceRecord& record,
    const AudioStreamRuntimeCaps& caps) noexcept {
    if (record.vendorId == DeviceProtocolFactory::kFocusriteVendorId &&
        record.modelId == DeviceProtocolFactory::kSPro24DspModelId &&
        caps.hostOutputPcmChannels == 8 &&
        caps.hostToDeviceAm824Slots == 9) {
        return Encoding::AudioWireFormat::kRawPcm24In32;
    }
    return Encoding::AudioWireFormat::kAM824;
}

} // namespace

DiceDuplexRestartCoordinator::DiceDuplexRestartCoordinator(
    Discovery::DeviceRegistry& registry,
    AudioRuntimeRegistry& runtime,
    IDiceHostTransport& hostTransport,
    Driver::HardwareInterface& hardware,
    const std::atomic<bool>* cancel,
    DirectAudioBindingSourceProvider bindingSourceProvider) noexcept
    : registry_(registry)
    , runtime_(runtime)
    , hostTransport_(hostTransport)
    , hardware_(hardware)
    , cancel_(cancel)
    , bindingSourceProvider_(std::move(bindingSourceProvider)) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "DiceDuplexRestartCoordinator: failed to allocate lock");
        return;
    }
}

DiceDuplexRestartCoordinator::~DiceDuplexRestartCoordinator() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

IOReturn DiceDuplexRestartCoordinator::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: StartStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DiceRestartSession session = LoadSession(guid);
    LogFsmEvent("start",
                guid,
                session.restartId,
                session.topologyGeneration,
                session.state,
                session.phase,
                session.reason);

    bool acquired = false;
    for (uint32_t waited = 0; waited < kSyncBridgeTimeoutMs; waited += kSyncBridgePollMs) {
        if (TryAcquireGuid(guid)) {
            acquired = true;
            break;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        IOSleep(kSyncBridgePollMs);
    }
    if (!acquired) {
        ASFW_LOG_ERROR(Audio,
                       "DiceDuplexRestartCoordinator: StartStreaming timed out waiting for GUID claim GUID=%llx timeoutMs=%u",
                       guid,
                       kSyncBridgeTimeoutMs);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunStartStreaming(guid);
    ReleaseGuid(guid);
    return status;
}

IOReturn DiceDuplexRestartCoordinator::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: StopStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DiceRestartSession session = LoadSession(guid);
    LogFsmEvent("stop",
                guid,
                session.restartId,
                session.topologyGeneration,
                session.state,
                session.phase,
                session.reason);

    RequestStopIntent(guid);
    FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

    bool acquired = false;
    for (uint32_t waited = 0; waited < kSyncBridgeTimeoutMs; waited += kSyncBridgePollMs) {
        if (TryAcquireGuid(guid)) {
            acquired = true;
            break;
        }
        if (TeardownRequested()) {
            ClearStopIntent(guid);
            return kIOReturnAborted;
        }
        IOSleep(kSyncBridgePollMs);
    }
    if (!acquired) {
        ASFW_LOG_ERROR(Audio,
                       "DiceDuplexRestartCoordinator: StopStreaming timed out waiting for GUID claim GUID=%llx timeoutMs=%u",
                       guid,
                       kSyncBridgeTimeoutMs);
        ClearStopIntent(guid);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunStopStreaming(guid);
    FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
    ClearStopIntent(guid);
    ReleaseGuid(guid);
    return status;
}

IOReturn DiceDuplexRestartCoordinator::RequestClockConfig(
    uint64_t guid,
    const DiceDesiredClockConfig& desiredClock,
    DiceRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: RequestClockConfig refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }
    if (!IsSupportedClockConfig(desiredClock)) {
        return kIOReturnUnsupported;
    }

    PendingClockRequest request{
        .desiredClock = desiredClock,
        .reason = reason,
    };

    bool shouldLaunchLoop = false;
    std::optional<PendingClockRequest> supersededRequest{};
    DiceRestartSession session = LoadSession(guid);
    if (!lock_) {
        return kIOReturnNoResources;
    }

    IOLockLock(lock_);
    if (stopRequestedGuids_.find(guid) != stopRequestedGuids_.end()) {
        IOLockUnlock(lock_);
        return kIOReturnAborted;
    }

    request.token = nextClockToken_++;
    if (activeGuids_.find(guid) != activeGuids_.end()) {
        const auto existingIt = pendingClockRequests_.find(guid);
        if (existingIt != pendingClockRequests_.end()) {
            supersededRequest = existingIt->second;
        }
        pendingClockRequests_[guid] = request;
        const auto sessionIt = sessions_.find(guid);
        if (sessionIt != sessions_.end()) {
            sessionIt->second.pendingClock = request.desiredClock;
            sessionIt->second.pendingReason = request.reason;
            sessionIt->second.hasPendingClockRequest = true;
        }
    } else {
        activeGuids_.insert(guid);
        shouldLaunchLoop = true;
    }
    IOLockUnlock(lock_);

    session.pendingClock = request.desiredClock;
    session.pendingReason = request.reason;
    LogFsmEvent("clock",
                guid,
                session.restartId,
                session.topologyGeneration,
                session.state,
                session.phase,
                reason,
                request.token);

    if (supersededRequest.has_value()) {
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = supersededRequest->token,
                .desiredClock = supersededRequest->desiredClock,
                .reason = supersededRequest->reason,
                .outcome = DiceClockRequestOutcome::kSuperseded,
                .status = kIOReturnAborted,
                .restartId = session.restartId,
                .generation = session.topologyGeneration,
            },
            guid);
    }

    if (shouldLaunchLoop) {
        (void)RunClockRequestLoop(guid, request);
        ReleaseGuid(guid);
    }

    for (uint32_t waited = 0; waited < kClockRequestWaitTimeoutMs; waited += kSyncBridgePollMs) {
        DiceClockRequestCompletion completion{};
        if (TryTakeCompletedClockRequest(guid, request.token, completion)) {
            return completion.status;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }

        IOSleep(kSyncBridgePollMs);
    }

    return kIOReturnTimeout;
}

IOReturn DiceDuplexRestartCoordinator::RecoverStreaming(uint64_t guid,
                                                        DiceRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (TeardownRequested()) {
        ASFW_LOG(Audio,
                 "DiceDuplexRestartCoordinator: RecoverStreaming refused by teardown GUID=%llx",
                 guid);
        return kIOReturnAborted;
    }

    const DiceRestartSession session = LoadSession(guid);
    LogFsmEvent("recover",
                guid,
                session.restartId,
                session.topologyGeneration,
                session.state,
                session.phase,
                reason);

    FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

    bool acquired = false;
    for (uint32_t waited = 0; waited < kSyncBridgeTimeoutMs; waited += kSyncBridgePollMs) {
        if (TryAcquireGuid(guid)) {
            acquired = true;
            break;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        IOSleep(kSyncBridgePollMs);
    }
    if (!acquired) {
        ASFW_LOG_ERROR(Audio,
                       "DiceDuplexRestartCoordinator: RecoverStreaming timed out waiting for GUID claim GUID=%llx timeoutMs=%u",
                       guid,
                       kSyncBridgeTimeoutMs);
        return kIOReturnTimeout;
    }

    const IOReturn status = RunRecoveryStreaming(guid, reason);
    ReleaseGuid(guid);
    return status;
}

void DiceDuplexRestartCoordinator::ClearSession(uint64_t guid) noexcept {
    if (!lock_ || guid == 0) {
        return;
    }

    IOLockLock(lock_);
    sessions_.erase(guid);
    pendingClockRequests_.erase(guid);
    completedClockRequests_.erase(guid);
    activeGuids_.erase(guid);
    stopRequestedGuids_.erase(guid);
    IOLockUnlock(lock_);
}

std::optional<DiceRestartSession> DiceDuplexRestartCoordinator::GetSession(uint64_t guid) const noexcept {
    if (!lock_ || guid == 0) {
        return std::nullopt;
    }

    IOLockLock(lock_);
    const auto it = sessions_.find(guid);
    const auto session = (it != sessions_.end())
        ? std::optional<DiceRestartSession>(it->second)
        : std::nullopt;
    IOLockUnlock(lock_);
    return session;
}

IOReturn DiceDuplexRestartCoordinator::RunStartStreaming(uint64_t guid) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    if (!record || !diceProtocol) {
        FailPendingClockRequest(guid, DiceClockRequestOutcome::kFailed, kIOReturnNotReady);
        return kIOReturnNotReady;
    }

    DiceRestartSession session = LoadSession(guid);
    const DiceDesiredClockConfig desiredClock{
        .sampleRateHz = 48000U,
        .clockSelect = kDiceClockSelect48kInternal,
    };
    const DiceRestartReason reason =
        DICE::HasRestartIntent(session)
            ? DICE::ClassifyRestartReason(&session, desiredClock)
            : DiceRestartReason::kInitialStart;

    const IOReturn status = RunDuplexStart(guid, *record, *diceProtocol, session, desiredClock, reason);
    if (status != kIOReturnSuccess) {
        FailPendingClockRequest(guid, DiceClockRequestOutcome::kFailed, status);
        return status;
    }

    if (IsStopRequested(guid)) {
        FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
        return kIOReturnSuccess;
    }

    PendingClockRequest pending{};
    while (TryConsumePendingClockRequest(guid, pending)) {
        if (IsStopRequested(guid)) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = pending.token,
                    .desiredClock = pending.desiredClock,
                    .reason = pending.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
            break;
        }

        const IOReturn pendingStatus = ApplyClockRequest(guid, pending);
        if (IsStopRequested(guid)) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = pending.token,
                    .desiredClock = pending.desiredClock,
                    .reason = pending.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
            break;
        }

        const DiceRestartSession completionSession = LoadSession(guid);
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = pending.token,
                .desiredClock = pending.desiredClock,
                .reason = pending.reason,
                .outcome = (pendingStatus == kIOReturnSuccess)
                    ? DiceClockRequestOutcome::kApplied
                    : DiceClockRequestOutcome::kFailed,
                .status = pendingStatus,
                .restartId = completionSession.restartId,
                .generation = completionSession.topologyGeneration,
            },
            guid);
    }

    return kIOReturnSuccess;
}

IOReturn DiceDuplexRestartCoordinator::RunStopStreaming(uint64_t guid) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    if (!record || !diceProtocol) {
        DiceRestartSession session = LoadSession(guid);
        ClearFailureSnapshot(session);
        ClearRestartProgress(session);
        StoreSession(session);
        LogTerminal(session);
        return kIOReturnNotReady;
    }

    DiceRestartSession session = LoadSession(guid);
    return RunDuplexStop(guid, *record, *diceProtocol, session);
}

IOReturn DiceDuplexRestartCoordinator::RunRecoveryStreaming(uint64_t guid,
                                                            DiceRestartReason reason) noexcept {
    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    DiceRestartSession session = LoadSession(guid);
    session.guid = guid;
    if (IsRecoveryReason(reason)) {
        RecordIssue(session,
                    session.lastInvalidation,
                    session.phase,
                    DiceRestartErrorClass::kEpochInvalidated,
                    FailureCauseForReason(reason),
                    kIOReturnAborted,
                    true,
                    false,
                    kIOReturnSuccess,
                    true,
                    true);
        StoreSession(session);
        LogInvalidation(session);
    }

    const DiceRecoveryContext context{
        .triggerReason = reason,
        .state = session.state,
        .phase = session.phase,
        .stopRequested = IsStopRequested(guid),
        .hasRestartIntent = HasRestartIntent(session),
        .hasHostFootprint = HasHostRestartState(session),
        .hasDeviceFootprint = HasDeviceRestartState(session),
        .hasDiceRecord = (record != nullptr),
        .hasProtocol = (diceProtocol != nullptr),
        .lastFailureRetryable = session.lastFailure.has_value() && session.lastFailure->retryable,
    };
    const DiceRecoveryDecision decision = EvaluateRecoveryPolicy(context);
    LogRecoveryPolicy(session, reason, decision);

    if (decision.disposition == DiceRecoveryDisposition::kIgnore) {
        return (decision.reason == DiceRecoveryPolicyReason::kSuppressedByStop ||
                decision.reason == DiceRecoveryPolicyReason::kIdleApplyInvalidated)
            ? kIOReturnAborted
            : kIOReturnSuccess;
    }

    if (decision.disposition == DiceRecoveryDisposition::kFailSession) {
        const bool missingDependency = (!record || !diceProtocol);
        session.terminalError = missingDependency
            ? kIOReturnNotReady
            : (session.lastFailure.has_value() ? session.lastFailure->status : kIOReturnUnsupported);
        if (missingDependency) {
            RecordIssue(session,
                        session.lastFailure,
                        session.phase,
                        DiceRestartErrorClass::kMissingDependency,
                        FailureCauseForReason(reason),
                        session.terminalError,
                        false,
                        false,
                        kIOReturnSuccess,
                        false,
                        false);
        }
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, ToString(decision.reason));
        StoreSession(session);
        LogTerminal(session);
        return session.terminalError;
    }

    if (!record || !diceProtocol) {
        return kIOReturnNotReady;
    }

    const DiceDesiredClockConfig desiredClock =
        (session.desiredClock.sampleRateHz != 0 && session.desiredClock.clockSelect != 0)
            ? session.desiredClock
            : ((session.appliedClock.sampleRateHz != 0 && session.appliedClock.clockSelect != 0)
                   ? session.appliedClock
                   : DiceDesiredClockConfig{
                         .sampleRateHz = 48000U,
                         .clockSelect = kDiceClockSelect48kInternal,
                     });

    if (HasAnyRestartState(session) || session.phase == DiceRestartPhase::kRunning) {
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        const IOReturn stopStatus = RunDuplexStop(guid, *record, *diceProtocol, session);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
    }

    if (IsStopRequested(guid) || TeardownRequested()) {
        return kIOReturnAborted;
    }

    return RunDuplexStart(guid, *record, *diceProtocol, session, desiredClock, reason);
}

IOReturn DiceDuplexRestartCoordinator::RunClockRequestLoop(uint64_t guid,
                                                           PendingClockRequest initialRequest) noexcept {
    PendingClockRequest current = initialRequest;
    IOReturn lastStatus = kIOReturnSuccess;

    while (true) {
        if (IsStopRequested(guid) || TeardownRequested()) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = current.token,
                    .desiredClock = current.desiredClock,
                    .reason = current.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
            break;
        }

        lastStatus = ApplyClockRequest(guid, current);

        if (IsStopRequested(guid) || TeardownRequested()) {
            const DiceRestartSession completionSession = LoadSession(guid);
            CompleteClockRequest(
                DiceClockRequestCompletion{
                    .token = current.token,
                    .desiredClock = current.desiredClock,
                    .reason = current.reason,
                    .outcome = DiceClockRequestOutcome::kAbortedByStop,
                    .status = kIOReturnAborted,
                    .restartId = completionSession.restartId,
                    .generation = completionSession.topologyGeneration,
                },
                guid);
            FailPendingClockRequest(guid, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);
            break;
        }

        const DiceRestartSession completionSession = LoadSession(guid);
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = current.token,
                .desiredClock = current.desiredClock,
                .reason = current.reason,
                .outcome = (lastStatus == kIOReturnSuccess)
                    ? DiceClockRequestOutcome::kApplied
                    : DiceClockRequestOutcome::kFailed,
                .status = lastStatus,
                .restartId = completionSession.restartId,
                .generation = completionSession.topologyGeneration,
            },
            guid);

        PendingClockRequest next{};
        if (!TryConsumePendingClockRequest(guid, next)) {
            break;
        }
        current = next;
    }

    return lastStatus;
}

IOReturn DiceDuplexRestartCoordinator::ApplyClockRequest(uint64_t guid,
                                                         const PendingClockRequest& request) noexcept {
    if (IsStopRequested(guid) || TeardownRequested()) {
        return kIOReturnAborted;
    }

    DICE::IDICEDuplexProtocol* diceProtocol = nullptr;
    std::shared_ptr<IDeviceProtocol> protoHold; // keeps the protocol alive for this op
    auto* record = RequireDiceRecord(guid, diceProtocol, protoHold);
    if (!record || !diceProtocol) {
        return kIOReturnNotReady;
    }
    if (!IsSupportedClockConfig(request.desiredClock)) {
        return kIOReturnUnsupported;
    }

    DiceRestartSession session = LoadSession(guid);
    if (HasAnyRestartState(session) ||
        session.phase == DiceRestartPhase::kRunning ||
        session.state == DiceRestartState::kRunning ||
        session.state == DiceRestartState::kRecovering ||
        session.state == DiceRestartState::kFailed) {
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        const IOReturn stopStatus = RunDuplexStop(guid, *record, *diceProtocol, session);
        if (stopStatus != kIOReturnSuccess) {
            return stopStatus;
        }
        if (TeardownRequested()) {
            return kIOReturnAborted;
        }
        return RunDuplexStart(guid, *record, *diceProtocol, session, request.desiredClock, request.reason);
    }

    if (TeardownRequested()) {
        return kIOReturnAborted;
    }

    return RunIdleClockApply(guid,
                             *diceProtocol,
                             session,
                             record->gen,
                             request.desiredClock,
                             request.reason);
}

IOReturn DiceDuplexRestartCoordinator::RunDuplexStart(
    uint64_t guid,
    Discovery::DeviceRecord& record,
    DICE::IDICEDuplexProtocol& diceProtocol,
    DiceRestartSession& session,
    const DiceDesiredClockConfig& desiredClock,
    DiceRestartReason reason) noexcept {
    auto abortIfTeardown = [&](const char* stage) noexcept -> bool {
        if (!TeardownRequested()) {
            return false;
        }
        RecordTeardownAbort(stage, guid);
        return true;
    };

    if (abortIfTeardown("RunDuplexStart")) {
        return kIOReturnAborted;
    }
    const FW::Generation topologyGeneration = record.gen;
    auto runtimeProtocol = runtime_.FindShared(record.guid);

    // Pre-read the device's static stream format (DICE TX_NUMBER/RX_NUMBER +
    // per-stream channels) so the channel resolution + IRM reservation below see
    // the real stream count. EnsureRuntimeStreamGeometry publishes the per-stream
    // caps that ResolveDuplexChannelsForRecord consumes via GetRuntimeAudioStreamCaps.
    // Non-fatal: on failure we fall back to the legacy single-stream resolution
    // and PrepareDuplex will surface any genuine device error. A multi-stream
    // device (Venice F32 = 2×16) needs this to allocate a channel per stream;
    // cross-validated with FFADO dice_avdevice.cpp prepare() (m_nb_rx/m_nb_tx).
    const auto geometryLoad = WaitForAsyncResult<bool>(
        [&](auto callback) {
            diceProtocol.EnsureRuntimeStreamGeometry(
                [callback = std::move(callback)](IOReturn st) mutable { callback(st, true); });
        },
        kSyncBridgeTimeoutMs,
        kIOReturnTimeout,
        cancel_);
    if (geometryLoad.status != kIOReturnSuccess) {
        ASFW_LOG(DICE,
                 "RunDuplexStart: stream-geometry pre-read failed (0x%x); "
                 "resolving channels with existing caps",
                 geometryLoad.status);
    }

    const AudioDuplexChannels channels = ResolveDuplexChannelsForRecord(record, runtimeProtocol.get());
    const uint64_t restartId = AllocateRestartId();

    auto finalizeFailure = [&](IOReturn failureStatus,
                               DiceRestartPhase failedPhase,
                               DiceRestartFailureCause cause,
                               DiceRestartErrorClass errorClass,
                               bool rollbackAttempted,
                               IOReturn rollbackStatus,
                               bool hostStateKnown,
                               bool deviceStateKnown) noexcept {
        session.guid = guid;
        session.restartId = restartId;
        session.generation = topologyGeneration;
        session.topologyGeneration = topologyGeneration;
        session.channels = channels;
        session.reason = reason;
        session.desiredClock = desiredClock;
        session.terminalError = failureStatus;
        RecordIssue(session,
                    session.lastFailure,
                    failedPhase,
                    errorClass,
                    cause,
                    failureStatus,
                    IsRetryableStatus(failureStatus),
                    rollbackAttempted,
                    rollbackStatus,
                    hostStateKnown,
                    deviceStateKnown);
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, ToString(errorClass));
        StoreSession(session);
        LogTerminal(session);
        return failureStatus;
    };

    auto rollbackToFailure = [&](IOReturn failureStatus,
                                 DiceRestartPhase failedPhase,
                                 DiceRestartFailureCause cause) noexcept {
        if (failureStatus == kIOReturnAborted && TeardownRequested()) {
            return failureStatus;
        }
        const IOReturn rollbackStatus = RunDuplexStop(guid, record, diceProtocol, session);
        return finalizeFailure(failureStatus,
                               failedPhase,
                               cause,
                               DiceRestartErrorClass::kStageFailure,
                               true,
                               rollbackStatus,
                               true,
                               true);
    };

    auto rollbackToInvalidation = [&](IOReturn invalidationStatus,
                                      DiceRestartPhase failedPhase,
                                      DiceRestartFailureCause cause) noexcept {
        if (invalidationStatus == kIOReturnAborted && TeardownRequested()) {
            return invalidationStatus;
        }
        const IOReturn rollbackStatus = RunDuplexStop(guid, record, diceProtocol, session);
        if (rollbackStatus != kIOReturnSuccess) {
            return finalizeFailure(rollbackStatus,
                                   DiceRestartPhase::kStopping,
                                   DiceRestartFailureCause::kStop,
                                   DiceRestartErrorClass::kStageFailure,
                                   true,
                                   rollbackStatus,
                                   true,
                                   true);
        }

        session.guid = guid;
        session.restartId = restartId;
        session.generation = topologyGeneration;
        session.topologyGeneration = topologyGeneration;
        session.channels = channels;
        session.reason = reason;
        session.desiredClock = desiredClock;
        session.terminalError = kIOReturnSuccess;
        RecordIssue(session,
                    session.lastInvalidation,
                    failedPhase,
                    IsStopRequested(guid) ? DiceRestartErrorClass::kStopIntent
                                          : DiceRestartErrorClass::kEpochInvalidated,
                    cause,
                    invalidationStatus,
                    true,
                    true,
                    rollbackStatus,
                    true,
                    true);
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DiceRestartPhase::kIdle, ToString(session.lastInvalidation->errorClass));
        StoreSession(session);
        LogInvalidation(session);
        LogTerminal(session);
        return invalidationStatus;
    };

    auto* irmClient = diceProtocol.GetIRMClient();
    if (irmClient == nullptr) {
        ASFW_LOG_ERROR(Audio, "DiceDuplexRestartCoordinator: protocol missing IRM client GUID=%llx", guid);
        return finalizeFailure(kIOReturnNotReady,
                               DiceRestartPhase::kPreparingDevice,
                               DiceRestartFailureCause::kPrepare,
                               DiceRestartErrorClass::kMissingDependency,
                               false,
                               kIOReturnSuccess,
                               false,
                               false);
    }

    auto* bindingSource = GetDirectAudioBindingSource(guid);
    if (!bindingSource) {
        return finalizeFailure(kIOReturnNotReady,
                               DiceRestartPhase::kPreparingDevice,
                               DiceRestartFailureCause::kPrepare,
                               DiceRestartErrorClass::kMissingDependency,
                               false,
                               kIOReturnSuccess,
                               false,
                               true);
    }

    session.guid = guid;
    session.restartId = restartId;
    session.generation = topologyGeneration;
    session.topologyGeneration = topologyGeneration;
    session.channels = channels;
    session.reason = reason;
    session.desiredClock = desiredClock;
    session.terminalError = kIOReturnSuccess;
    ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "reset_before_start");
    SetSessionState(session, RestartStateForStartReason(reason), ToString(reason));
    SetSessionPhase(session, DiceRestartPhase::kPreparingDevice);
    StoreSession(session);
    LogFsmEvent("start",
                guid,
                restartId,
                topologyGeneration,
                session.state,
                session.phase,
                reason);
    ASFW_LOG(Audio,
             "DiceDuplexRestartCoordinator: using DICE iso channels d2h=%u h2d=%u GUID=%llx",
             channels.deviceToHostIsoChannel,
             channels.hostToDeviceIsoChannel,
             guid);

    if (abortIfTeardown("BeginSplitDuplex")) {
        return kIOReturnAborted;
    }
    const kern_return_t claimStatus = hostTransport_.BeginSplitDuplex(guid);
    if (claimStatus != kIOReturnSuccess) {
        return finalizeFailure(claimStatus,
                               DiceRestartPhase::kPreparingDevice,
                               DiceRestartFailureCause::kPrepare,
                               DiceRestartErrorClass::kStageFailure,
                               false,
                               kIOReturnSuccess,
                               true,
                               true);
    }
    session.hostDuplexClaimed = true;
    StoreSession(session);

    if (abortIfTeardown("PreparingDevice")) {
        return kIOReturnAborted;
    }
    const auto prepare = WaitForAsyncResult<DiceDuplexPrepareResult>(
        [&](auto callback) {
            diceProtocol.PrepareDuplex(channels, desiredClock, std::move(callback));
        },
        kSyncBridgeTimeoutMs,
        kIOReturnTimeout,
        cancel_);
    if (prepare.status != kIOReturnSuccess) {
        if (prepare.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("PreparingDevice", guid);
        }
        return rollbackToFailure(prepare.status,
                                 DiceRestartPhase::kPreparingDevice,
                                 DiceRestartFailureCause::kPrepare);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kPreparingDevice,
                                      DiceRestartFailureCause::kPrepare);
    }
    session.ownerClaimed = true;
    session.devicePrepared = true;
    session.generation = prepare.value.generation;
    session.appliedClock = prepare.value.appliedClock;
    session.runtimeCaps = prepare.value.runtimeCaps;
    SetSessionPhase(session, DiceRestartPhase::kPrepared);
    StoreSession(session);

    // Reserve IRM bandwidth + channel for EVERY playback (host IT -> device RX)
    // stream. A multi-stream DICE device (Venice F32) needs all of its RX iso
    // channels reserved before GLOBAL_ENABLE; single-stream devices loop once.
    if (abortIfTeardown("ReservingPlaybackResources")) {
        return kIOReturnAborted;
    }
    for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
        const kern_return_t reservePlaybackStatus = hostTransport_.ReservePlaybackResources(
            guid,
            *irmClient,
            channels.PlaybackChannel(i),
            kPlaybackBandwidthUnits);
        if (reservePlaybackStatus != kIOReturnSuccess) {
            return rollbackToFailure(reservePlaybackStatus,
                                     DiceRestartPhase::kReservingPlaybackResources,
                                     DiceRestartFailureCause::kReservePlayback);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DiceRestartPhase::kReservingPlaybackResources,
                                          DiceRestartFailureCause::kReservePlayback);
        }
    }
    SetSessionPhase(session, DiceRestartPhase::kReservingPlaybackResources);
    session.hostPlaybackReserved = true;
    StoreSession(session);

    // Reserve IRM bandwidth + channel for EVERY capture (device TX -> host IR) stream.
    if (abortIfTeardown("ReservingCaptureResources")) {
        return kIOReturnAborted;
    }
    for (uint32_t i = 0; i < channels.captureStreamCount; ++i) {
        const kern_return_t reserveCaptureStatus = hostTransport_.ReserveCaptureResources(
            guid,
            *irmClient,
            channels.CaptureChannel(i),
            kCaptureBandwidthUnits);
        if (reserveCaptureStatus != kIOReturnSuccess) {
            return rollbackToFailure(reserveCaptureStatus,
                                     DiceRestartPhase::kReservingCaptureResources,
                                     DiceRestartFailureCause::kReserveCapture);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DiceRestartPhase::kReservingCaptureResources,
                                          DiceRestartFailureCause::kReserveCapture);
        }
    }
    SetSessionPhase(session, DiceRestartPhase::kReservingCaptureResources);
    session.hostCaptureReserved = true;
    StoreSession(session);

    Encoding::AudioWireFormat rxWireFormat = Encoding::AudioWireFormat::kAM824;
    if (record.vendorId == DeviceProtocolFactory::kFocusriteVendorId &&
        record.modelId == DeviceProtocolFactory::kSPro24DspModelId &&
        session.runtimeCaps.hostInputPcmChannels == 8 &&
        session.runtimeCaps.deviceToHostAm824Slots == 9) {
        rxWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
    }
    const uint32_t rxAm824Slots = session.runtimeCaps.deviceToHostAm824Slots;
    const Encoding::AudioWireFormat wireFormat =
        ResolveDicePlaybackWireFormat(record, session.runtimeCaps);

    ASFW_LOG(Audio,
             "DICE DUPLEX START guid=0x%016llx ir=%u it=%u inCh=%u outCh=%u inSlots=%u outSlots=%u mode=blocking rxFmt=%u txFmt=%u",
             guid,
             channels.deviceToHostIsoChannel,
             channels.hostToDeviceIsoChannel,
             session.runtimeCaps.hostInputPcmChannels,
             session.runtimeCaps.hostOutputPcmChannels,
             session.runtimeCaps.deviceToHostAm824Slots,
             session.runtimeCaps.hostToDeviceAm824Slots,
             static_cast<uint32_t>(rxWireFormat),
             static_cast<uint32_t>(wireFormat));

    // Saffire.kext allocates every local/remote isoch port before it reports
    // the assigned channels to DICE. Keep the expensive DMA setup on the
    // disabled side of GLOBAL_ENABLE as well.
    // Multi-stream capture (e.g. Venice F32 = 2×16): each wire stream carries
    // its own 16-ch CIP (per-stream DBS), de-interleaved into the single shared
    // 32-ch input buffer at a channel offset. The master (stream 0) owns the
    // clock/ZTS/replay; secondary streams write PCM only. Single-stream devices
    // (captureStreamCount == 1) take the legacy path with streamChannels == 0
    // (full width) and the aggregate slot count.
    const bool multiCapture = channels.captureStreamCount > 1;
    const uint32_t masterCaptureSlots =
        multiCapture ? session.runtimeCaps.deviceToHostStreams[0].am824Slots
                     : rxAm824Slots;
    const uint32_t masterCaptureChannels =
        multiCapture ? session.runtimeCaps.deviceToHostStreams[0].pcmChannels
                     : 0;

    if (abortIfTeardown("PreparingHostReceive")) {
        return kIOReturnAborted;
    }
    const kern_return_t prepareReceiveStatus = hostTransport_.PrepareReceive(
        channels.CaptureChannel(0),
        hardware_,
        bindingSource,
        rxWireFormat,
        masterCaptureSlots,
        masterCaptureChannels);
    if (prepareReceiveStatus != kIOReturnSuccess) {
        return rollbackToFailure(prepareReceiveStatus,
                                 DiceRestartPhase::kStartingHostReceive,
                                 DiceRestartFailureCause::kStartReceive);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kStartingHostReceive,
                                      DiceRestartFailureCause::kStartReceive);
    }

    // Prepare each secondary capture stream on its own OHCI IR context, writing
    // its slice at the running channel offset.
    uint32_t captureChannelOffset = masterCaptureChannels;
    for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
        const auto& streamInfo = session.runtimeCaps.deviceToHostStreams[i];
        if (abortIfTeardown("PreparingHostReceiveStream")) {
            return kIOReturnAborted;
        }
        const kern_return_t status = hostTransport_.PrepareReceiveStream(
            i,
            channels.CaptureChannel(i),
            hardware_,
            bindingSource,
            captureChannelOffset,
            streamInfo.pcmChannels,
            rxWireFormat,
            streamInfo.am824Slots);
        if (status != kIOReturnSuccess) {
            return rollbackToFailure(status,
                                     DiceRestartPhase::kStartingHostReceive,
                                     DiceRestartFailureCause::kStartReceive);
        }
        if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
            return rollbackToInvalidation(kIOReturnAborted,
                                          DiceRestartPhase::kStartingHostReceive,
                                          DiceRestartFailureCause::kStartReceive);
        }
        captureChannelOffset += streamInfo.pcmChannels;
    }

    if (abortIfTeardown("PreparingHostTransmit")) {
        return kIOReturnAborted;
    }
    const kern_return_t prepareTransmitStatus = hostTransport_.PrepareTransmit(
        channels.hostToDeviceIsoChannel,
        hardware_,
        ReadLocalSid(hardware_));
    if (prepareTransmitStatus != kIOReturnSuccess) {
        return rollbackToFailure(prepareTransmitStatus,
                                 DiceRestartPhase::kStartingHostTransmit,
                                 DiceRestartFailureCause::kStartTransmit);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kStartingHostTransmit,
                                      DiceRestartFailureCause::kStartTransmit);
    }

    SetSessionPhase(session, DiceRestartPhase::kWaitingGlobalClock);
    StoreSession(session);
    if (abortIfTeardown("WaitingGlobalClock")) {
        return kIOReturnAborted;
    }
    const IOReturn clockLockStatus =
        WaitForStableGlobalClock(guid, diceProtocol, topologyGeneration, desiredClock);
    if (clockLockStatus != kIOReturnSuccess) {
        return rollbackToFailure(clockLockStatus,
                                 DiceRestartPhase::kWaitingGlobalClock,
                                 DiceRestartFailureCause::kGlobalClockLock);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kWaitingGlobalClock,
                                      DiceRestartFailureCause::kGlobalClockLock);
    }

    if (abortIfTeardown("ProgrammingDeviceRx")) {
        return kIOReturnAborted;
    }
    const auto programRx = WaitForAsyncResult<DiceDuplexStageResult>(
        [&](auto callback) { diceProtocol.ProgramRx(std::move(callback)); },
        kSyncBridgeTimeoutMs,
        kIOReturnTimeout,
        cancel_);
    if (programRx.status != kIOReturnSuccess) {
        if (programRx.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ProgrammingDeviceRx", guid);
        }
        return rollbackToFailure(programRx.status,
                                 DiceRestartPhase::kProgrammingDeviceRx,
                                 DiceRestartFailureCause::kProgramRx);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kProgrammingDeviceRx,
                                      DiceRestartFailureCause::kProgramRx);
    }
    session.generation = programRx.value.generation;
    SetSessionPhase(session, DiceRestartPhase::kProgrammingDeviceRx);
    session.deviceRxProgrammed = true;
    session.runtimeCaps = programRx.value.runtimeCaps.hostInputPcmChannels != 0
        ? programRx.value.runtimeCaps
        : session.runtimeCaps;
    SetSessionPhase(session, DiceRestartPhase::kDeviceRxProgrammed);
    StoreSession(session);

    if (abortIfTeardown("ProgrammingDeviceTx")) {
        return kIOReturnAborted;
    }
    const auto programTx = WaitForAsyncResult<DiceDuplexStageResult>(
        [&](auto callback) { diceProtocol.ProgramTxAndEnableDuplex(std::move(callback)); },
        kSyncBridgeTimeoutMs,
        kIOReturnTimeout,
        cancel_);
    if (programTx.status != kIOReturnSuccess) {
        if (programTx.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ProgrammingDeviceTx", guid);
        }
        return rollbackToFailure(programTx.status,
                                 DiceRestartPhase::kProgrammingDeviceTx,
                                 DiceRestartFailureCause::kProgramTx);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kProgrammingDeviceTx,
                                      DiceRestartFailureCause::kProgramTx);
    }
    session.generation = programTx.value.generation;
    SetSessionPhase(session, DiceRestartPhase::kProgrammingDeviceTx);
    session.deviceTxArmed = true;
    session.runtimeCaps = programTx.value.runtimeCaps.hostInputPcmChannels != 0
        ? programTx.value.runtimeCaps
        : session.runtimeCaps;
    SetSessionPhase(session, DiceRestartPhase::kDeviceTxArmed);
    StoreSession(session);

    // Saffire::StartStreams waits 2 ms after GLOBAL_ENABLE before starting
    // the already-allocated host isoch channels.
    IOSleep(2);
    if (abortIfTeardown("PostGlobalEnableDelay")) {
        return kIOReturnAborted;
    }

    // RX first: the device needs to be receiving before it can lock its TX
    // clock. Starting IT before IR causes the DICE PLL to see TX without
    // a valid RX reference, leading to timing instability.
    if (abortIfTeardown("StartingHostReceive")) {
        return kIOReturnAborted;
    }
    const kern_return_t startReceiveStatus =
        hostTransport_.StartPreparedReceive();
    if (startReceiveStatus != kIOReturnSuccess) {
        return rollbackToFailure(startReceiveStatus,
                                 DiceRestartPhase::kStartingHostReceive,
                                 DiceRestartFailureCause::kStartReceive);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kStartingHostReceive,
                                      DiceRestartFailureCause::kStartReceive);
    }
    SetSessionPhase(session, DiceRestartPhase::kStartingHostReceive);
    session.hostReceiveStarted = true;
    StoreSession(session);

    if (abortIfTeardown("StartingHostTransmit")) {
        return kIOReturnAborted;
    }
    const kern_return_t startTransmitStatus =
        hostTransport_.StartPreparedTransmit();
    if (startTransmitStatus != kIOReturnSuccess) {
        return rollbackToFailure(startTransmitStatus,
                                 DiceRestartPhase::kStartingHostTransmit,
                                 DiceRestartFailureCause::kStartTransmit);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kStartingHostTransmit,
                                      DiceRestartFailureCause::kStartTransmit);
    }
    SetSessionPhase(session, DiceRestartPhase::kStartingHostTransmit);
    session.hostTransmitStarted = true;
    StoreSession(session);

    if (abortIfTeardown("ConfirmingDeviceStart")) {
        return kIOReturnAborted;
    }
    const auto confirm = WaitForAsyncResult<DiceDuplexConfirmResult>(
        [&](auto callback) { diceProtocol.ConfirmDuplexStart(std::move(callback)); },
        kSyncBridgeTimeoutMs,
        kIOReturnTimeout,
        cancel_);
    if (confirm.status != kIOReturnSuccess) {
        if (confirm.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("ConfirmingDeviceStart", guid);
        }
        return rollbackToFailure(confirm.status,
                                 DiceRestartPhase::kConfirmingDeviceStart,
                                 DiceRestartFailureCause::kConfirmStart);
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        return rollbackToInvalidation(kIOReturnAborted,
                                      DiceRestartPhase::kConfirmingDeviceStart,
                                      DiceRestartFailureCause::kConfirmStart);
    }

    SetSessionPhase(session, DiceRestartPhase::kConfirmingDeviceStart);
    SetSessionPhase(session, DiceRestartPhase::kRunning);
    SetSessionState(session, DiceRestartState::kRunning, "confirmed_running");
    session.generation = confirm.value.generation;
    session.deviceRunning = true;
    session.appliedClock = confirm.value.appliedClock;
    session.runtimeCaps = confirm.value.runtimeCaps;
    session.terminalError = kIOReturnSuccess;
    ClearFailureSnapshot(session);
    StoreSession(session);
    LogTerminal(session);
    return kIOReturnSuccess;
}

IOReturn DiceDuplexRestartCoordinator::WaitForStableGlobalClock(
    uint64_t guid,
    DICE::IDICEDuplexProtocol& diceProtocol,
    const FW::Generation topologyGeneration,
    const DiceDesiredClockConfig& desiredClock) noexcept {
    uint32_t consecutiveLockedReads = 0;
    const uint64_t startMs = UptimeMilliseconds();
    const uint64_t deadlineMs = startMs + kGlobalClockLockTimeoutMs;

    while (UptimeMilliseconds() < deadlineMs) {
        if (TeardownRequested()) {
            RecordTeardownAbort("WaitingGlobalClock", guid);
            return kIOReturnAborted;
        }

        const uint64_t nowMs = UptimeMilliseconds();
        const uint32_t remainingMs = static_cast<uint32_t>(deadlineMs - nowMs);
        const auto health = WaitForAsyncResult<DiceDuplexHealthResult>(
            [&](auto callback) { diceProtocol.ReadDuplexHealth(std::move(callback)); },
            std::max(remainingMs, 1U),
            kIOReturnTimeout,
            cancel_);
        if (health.status != kIOReturnSuccess) {
            if (health.status == kIOReturnAborted && TeardownRequested()) {
                RecordTeardownAbort("WaitingGlobalClock", guid);
                return health.status;
            }
            ASFW_LOG_ERROR(
                Audio,
                "DICE global clock health read failed before isoch start kr=0x%x",
                health.status);
            return health.status;
        }
        if (health.value.generation != topologyGeneration) {
            ASFW_LOG_ERROR(
                Audio,
                "DICE global clock generation changed before isoch start expected=%u actual=%u",
                GenerationValue(topologyGeneration),
                GenerationValue(health.value.generation));
            return kIOReturnOffline;
        }

        const bool lockedAtTarget =
            DICE::IsSourceLocked(health.value.status) &&
            DICE::NominalRateHz(health.value.status) == desiredClock.sampleRateHz &&
            health.value.appliedClock.sampleRateHz == desiredClock.sampleRateHz &&
            health.value.appliedClock.clockSelect == desiredClock.clockSelect;
        consecutiveLockedReads = lockedAtTarget ? consecutiveLockedReads + 1 : 0;

        if (consecutiveLockedReads >= kGlobalClockStableReads) {
            ASFW_LOG(
                Audio,
                "DICE global clock stable before isoch start rate=%u status=0x%08x reads=%u",
                desiredClock.sampleRateHz,
                health.value.status,
                consecutiveLockedReads);
            return kIOReturnSuccess;
        }

        const uint64_t afterReadMs = UptimeMilliseconds();
        if (afterReadMs >= deadlineMs) {
            break;
        }
        if (TeardownRequested()) {
            RecordTeardownAbort("WaitingGlobalClock", guid);
            return kIOReturnAborted;
        }
        IOSleep(std::min<uint64_t>(
            kGlobalClockLockPollMs, deadlineMs - afterReadMs));
    }

    ASFW_LOG_ERROR(
        Audio,
        "DICE global clock failed to stabilize before isoch start rate=%u timeoutMs=%u",
        desiredClock.sampleRateHz,
        kGlobalClockLockTimeoutMs);
    return kIOReturnTimeout;
}

IOReturn DiceDuplexRestartCoordinator::RunDuplexStop(
    uint64_t guid,
    Discovery::DeviceRecord& record,
    DICE::IDICEDuplexProtocol& diceProtocol,
    DiceRestartSession& session) noexcept {
    (void)record;

    if (TeardownRequested()) {
        RecordTeardownAbort("RunDuplexStop", guid);
        return kIOReturnAborted;
    }

    IOReturn result = kIOReturnSuccess;
    SetSessionPhase(session, DiceRestartPhase::kStopping);
    SetSessionState(session, DiceRestartState::kStopping, "stop_requested");
    StoreSession(session);

    const kern_return_t hostStatus = hostTransport_.StopAll();
    if (hostStatus != kIOReturnSuccess) {
        result = hostStatus;
    }

    const IOReturn deviceStatus = diceProtocol.StopDuplex();
    if (deviceStatus == kIOReturnAborted && TeardownRequested()) {
        RecordTeardownAbort("DeviceStop", guid);
        return kIOReturnAborted;
    }
    if (deviceStatus != kIOReturnSuccess && deviceStatus != kIOReturnUnsupported && result == kIOReturnSuccess) {
        result = deviceStatus;
    }

    if (result == kIOReturnSuccess) {
        session.terminalError = kIOReturnSuccess;
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "stop_complete");
    } else {
        session.terminalError = result;
        RecordIssue(session,
                    session.lastFailure,
                    DiceRestartPhase::kStopping,
                    DiceRestartErrorClass::kStageFailure,
                    DiceRestartFailureCause::kStop,
                    result,
                    IsRetryableStatus(result),
                    false,
                    kIOReturnSuccess,
                    true,
                    true);
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, "stop_failed");
    }
    StoreSession(session);
    LogTerminal(session);
    return result;
}

IOReturn DiceDuplexRestartCoordinator::RunIdleClockApply(
    uint64_t guid,
    DICE::IDICEDuplexProtocol& diceProtocol,
    DiceRestartSession& session,
    FW::Generation topologyGeneration,
    const DiceDesiredClockConfig& desiredClock,
    DiceRestartReason reason) noexcept {
    if (TeardownRequested()) {
        RecordTeardownAbort("IdleClockApply", guid);
        return kIOReturnAborted;
    }

    const uint64_t restartId = AllocateRestartId();
    session.guid = guid;
    session.restartId = restartId;
    session.generation = topologyGeneration;
    session.topologyGeneration = topologyGeneration;
    session.reason = reason;
    session.desiredClock = desiredClock;
    session.terminalError = kIOReturnSuccess;
    ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "reset_before_idle_apply");
    SetSessionState(session, DiceRestartState::kApplyingIdleClock, ToString(reason));
    SetSessionPhase(session, DiceRestartPhase::kPreparingDevice);
    StoreSession(session);

    const auto apply = WaitForAsyncResult<DICE::DiceClockApplyResult>(
        [&](auto callback) { diceProtocol.ApplyClockConfig(desiredClock, std::move(callback)); },
        kSyncBridgeTimeoutMs,
        kIOReturnTimeout,
        cancel_);
    if (apply.status != kIOReturnSuccess) {
        if (apply.status == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("IdleClockApply", guid);
        }
        session.terminalError = apply.status;
        RecordIssue(session,
                    session.lastFailure,
                    DiceRestartPhase::kPreparingDevice,
                    DiceRestartErrorClass::kStageFailure,
                    DiceRestartFailureCause::kIdleClockApply,
                    apply.status,
                    IsRetryableStatus(apply.status),
                    false,
                    kIOReturnSuccess,
                    true,
                    true);
        ApplyTerminalPhase(session, DiceRestartPhase::kFailed, "idle_apply_failed");
        StoreSession(session);
        LogTerminal(session);
        return apply.status;
    }
    if (!IsRestartEpochCurrent(guid, restartId, topologyGeneration)) {
        session.terminalError = kIOReturnSuccess;
        RecordIssue(session,
                    session.lastInvalidation,
                    DiceRestartPhase::kPreparingDevice,
                    IsStopRequested(guid) ? DiceRestartErrorClass::kStopIntent
                                          : DiceRestartErrorClass::kEpochInvalidated,
                    DiceRestartFailureCause::kIdleClockApply,
                    kIOReturnAborted,
                    true,
                    false,
                    kIOReturnSuccess,
                    true,
                    true);
        ClearFailureSnapshot(session);
        ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "idle_apply_invalidated");
        StoreSession(session);
        LogInvalidation(session);
        LogTerminal(session);
        return kIOReturnAborted;
    }

    session.generation = apply.value.generation;
    session.appliedClock = apply.value.appliedClock;
    session.runtimeCaps = apply.value.runtimeCaps;
    session.terminalError = kIOReturnSuccess;
    ClearFailureSnapshot(session);
    ApplyTerminalPhase(session, DiceRestartPhase::kIdle, "idle_apply_complete");
    StoreSession(session);
    LogTerminal(session);
    return kIOReturnSuccess;
}

Discovery::DeviceRecord* DiceDuplexRestartCoordinator::RequireDiceRecord(
    uint64_t guid,
    DICE::IDICEDuplexProtocol*& outDiceProtocol,
    std::shared_ptr<IDeviceProtocol>& outHold) noexcept {
    outDiceProtocol = nullptr;
    outHold.reset();
    auto* record = registry_.FindByGuid(guid);
    if (!record) {
        return nullptr;
    }

    auto protocol = runtime_.FindShared(guid);
    if (!protocol) {
        return nullptr;
    }

    outDiceProtocol = protocol->AsDiceDuplexProtocol();
    if (outDiceProtocol == nullptr) {
        return nullptr;
    }
    outDiceProtocol->SetTeardownCancelToken(cancel_);

    outHold = std::move(protocol);
    return record;
}

ASFW::Audio::Runtime::IDirectAudioBindingSource* DiceDuplexRestartCoordinator::GetDirectAudioBindingSource(uint64_t guid) const noexcept {
    if (!bindingSourceProvider_) {
        return nullptr;
    }
    return bindingSourceProvider_(guid);
}

bool DiceDuplexRestartCoordinator::TryAcquireGuid(uint64_t guid) noexcept {
    if (!lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto [_, inserted] = activeGuids_.insert(guid);
    IOLockUnlock(lock_);
    return inserted;
}

void DiceDuplexRestartCoordinator::ReleaseGuid(uint64_t guid) noexcept {
    if (!lock_) {
        return;
    }

    IOLockLock(lock_);
    activeGuids_.erase(guid);
    IOLockUnlock(lock_);
}

void DiceDuplexRestartCoordinator::RequestStopIntent(uint64_t guid) noexcept {
    if (!lock_ || guid == 0) {
        return;
    }

    IOLockLock(lock_);
    stopRequestedGuids_.insert(guid);
    IOLockUnlock(lock_);
}

void DiceDuplexRestartCoordinator::ClearStopIntent(uint64_t guid) noexcept {
    if (!lock_ || guid == 0) {
        return;
    }

    IOLockLock(lock_);
    stopRequestedGuids_.erase(guid);
    IOLockUnlock(lock_);
}

bool DiceDuplexRestartCoordinator::IsStopRequested(uint64_t guid) const noexcept {
    if (!lock_ || guid == 0) {
        return false;
    }

    IOLockLock(lock_);
    const bool requested = stopRequestedGuids_.find(guid) != stopRequestedGuids_.end();
    IOLockUnlock(lock_);
    return requested;
}

uint64_t DiceDuplexRestartCoordinator::AllocateRestartId() noexcept {
    if (!lock_) {
        return 0;
    }

    IOLockLock(lock_);
    const uint64_t restartId = nextRestartId_++;
    IOLockUnlock(lock_);
    return restartId;
}

bool DiceDuplexRestartCoordinator::IsRestartEpochCurrent(uint64_t guid,
                                                         uint64_t restartId,
                                                         FW::Generation topologyGeneration) const noexcept {
    if (guid == 0 || restartId == 0) {
        return false;
    }
    if (IsStopRequested(guid)) {
        return false;
    }

    if (!lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto sessionIt = sessions_.find(guid);
    const bool sessionMatches = (sessionIt != sessions_.end()) &&
        sessionIt->second.restartId == restartId &&
        sessionIt->second.topologyGeneration == topologyGeneration;
    IOLockUnlock(lock_);
    if (!sessionMatches) {
        return false;
    }

    const auto* liveRecord = registry_.FindByGuid(guid);
    if (!liveRecord || liveRecord->gen != topologyGeneration) {
        return false;
    }

    return true;
}

bool DiceDuplexRestartCoordinator::TryConsumePendingClockRequest(uint64_t guid,
                                                                 PendingClockRequest& outRequest) noexcept {
    if (!lock_) {
        return false;
    }

    IOLockLock(lock_);
    const auto it = pendingClockRequests_.find(guid);
    if (it == pendingClockRequests_.end()) {
        IOLockUnlock(lock_);
        return false;
    }

    outRequest = it->second;
    pendingClockRequests_.erase(it);
    const auto sessionIt = sessions_.find(guid);
    if (sessionIt != sessions_.end()) {
        sessionIt->second.pendingClock = {};
        sessionIt->second.pendingReason = DiceRestartReason::kInitialStart;
        sessionIt->second.hasPendingClockRequest = false;
    }
    IOLockUnlock(lock_);
    return true;
}

bool DiceDuplexRestartCoordinator::TryTakeCompletedClockRequest(
    uint64_t guid,
    uint64_t token,
    DiceClockRequestCompletion& outCompletion) noexcept {
    if (!lock_) {
        return false;
    }

    IOLockLock(lock_);
    auto storeIt = completedClockRequests_.find(guid);
    if (storeIt == completedClockRequests_.end()) {
        IOLockUnlock(lock_);
        return false;
    }

    auto completionIt = storeIt->second.byToken.find(token);
    if (completionIt == storeIt->second.byToken.end()) {
        IOLockUnlock(lock_);
        return false;
    }

    outCompletion = completionIt->second;
    storeIt->second.byToken.erase(completionIt);
    auto& order = storeIt->second.insertionOrder;
    order.erase(std::remove(order.begin(), order.end(), token), order.end());
    if (storeIt->second.byToken.empty() && order.empty()) {
        completedClockRequests_.erase(storeIt);
    }
    IOLockUnlock(lock_);
    return true;
}

void DiceDuplexRestartCoordinator::CompleteClockRequest(const DiceClockRequestCompletion& completion,
                                                        uint64_t guid) noexcept {
    if (!lock_) {
        return;
    }

    IOLockLock(lock_);
    auto& store = completedClockRequests_[guid];
    if (store.byToken.find(completion.token) == store.byToken.end()) {
        store.insertionOrder.push_back(completion.token);
    }
    store.byToken[completion.token] = completion;
    while (store.insertionOrder.size() > kMaxCompletedClockRequestsPerGuid) {
        const uint64_t evictedToken = store.insertionOrder.front();
        store.insertionOrder.pop_front();
        store.byToken.erase(evictedToken);
    }

    auto sessionIt = sessions_.find(guid);
    if (sessionIt != sessions_.end()) {
        sessionIt->second.lastClockCompletion = completion;
    }
    IOLockUnlock(lock_);

    ASFW_LOG_V2(DICE,
                "[FSM] clock token=%llu outcome=%{public}s status=0x%08x guid=0x%llx restartId=%llu gen=%u",
                completion.token,
                ToString(completion.outcome),
                static_cast<unsigned>(completion.status),
                guid,
                completion.restartId,
                GenerationValue(completion.generation));
}

void DiceDuplexRestartCoordinator::FailPendingClockRequest(uint64_t guid,
                                                           DiceClockRequestOutcome outcome,
                                                           IOReturn status) noexcept {
    PendingClockRequest request{};
    if (TryConsumePendingClockRequest(guid, request)) {
        const DiceRestartSession session = LoadSession(guid);
        CompleteClockRequest(
            DiceClockRequestCompletion{
                .token = request.token,
                .desiredClock = request.desiredClock,
                .reason = request.reason,
                .outcome = outcome,
                .status = status,
                .restartId = session.restartId,
                .generation = session.topologyGeneration,
            },
            guid);
    }
}

void DiceDuplexRestartCoordinator::RecordTeardownAbort(const char* stage,
                                                       uint64_t guid) noexcept {
    teardownAbortCount_.fetch_add(1, std::memory_order_acq_rel);
    ASFW_LOG(Audio,
             "DiceDuplexRestartCoordinator: recovery aborted by teardown stage=%{public}s GUID=%llx kr=0x%x",
             stage ? stage : "unknown",
             guid,
             kIOReturnAborted);
}

DiceRestartSession DiceDuplexRestartCoordinator::LoadSession(uint64_t guid) const noexcept {
    if (!lock_ || guid == 0) {
        return DiceRestartSession{.guid = guid};
    }

    IOLockLock(lock_);
    const auto it = sessions_.find(guid);
    DiceRestartSession session = (it != sessions_.end())
        ? it->second
        : DiceRestartSession{.guid = guid};
    IOLockUnlock(lock_);
    return session;
}

void DiceDuplexRestartCoordinator::StoreSession(const DiceRestartSession& session) noexcept {
    if (!lock_ || session.guid == 0) {
        return;
    }

    IOLockLock(lock_);
    sessions_[session.guid] = session;
    IOLockUnlock(lock_);
}

} // namespace ASFW::Audio
