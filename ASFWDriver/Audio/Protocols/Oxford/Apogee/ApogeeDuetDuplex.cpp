// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetDuplex.cpp - Duet duplex lifecycle and clock-transition FSM (FW-127).
//
// Moved out of ApogeeDuetProtocol unchanged. The epoch and generation guards on
// ClockTransition are the teardown and bus-reset safety mechanism, so they move
// intact: every asynchronous continuation re-checks IsActive before touching the
// transition, and the settle timer additionally re-checks that neither the
// transport nor the bus generation moved underneath it.
//
// Every CMP stage is continuation-passing (FW-141). This is a hard constraint,
// not a style: CMP completions are delivered on the core queue, so a stage that
// blocks waiting for its own completion prevents that completion from ever being
// dispatched. Phase88 carried the identical WaitForCMP/IOSleep poll and
// self-deadlocked on exactly that. Do not reintroduce a wait here — if a stage
// needs a result, it belongs in the completion.
//
// Teardown continuations (Disconnect*, StopDuplex) additionally capture no
// `this`: StopDuplex is fire-and-forget, so a completion that outlived the
// object would dereference freed memory (the FW-60 use-after-free class). The
// connected-flag writes happen before the request is issued for that reason.

#include "ApogeeDuetDuplex.hpp"

#include "../../Duplex/FamilyStageWait.hpp"

#include "ApogeeCaps.hpp"

#include "../../../../Bus/IRM/IRMClient.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Protocols/AVC/AVCDefs.hpp"
#include "../../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include "../../../../Protocols/AVC/Core/RateCodes.hpp"

#include <DriverKit/IOLib.h>
#include <memory>

namespace ASFW::Audio::Oxford::Apogee {

namespace {

[[nodiscard]] constexpr IOReturn MapAvcErrorToIOReturn(const AVC::AvcError& error) noexcept {
    return AVC::ToIOReturn(error);
}

} // namespace

struct ApogeeDuetDuplex::ClockTransition {
    enum class Phase : uint8_t {
        kReadInputBefore,
        kReadOutputBefore,
        kSetInput,
        kSetOutput,
        kSettle,
        kReadInputAfter,
        kReadOutputAfter,
        kRestoreInput,
        kRestoreOutput,
    };

    uint64_t epoch{0};
    FW::Generation generation{FW::Generation{0}};
    std::shared_ptr<ASFW::AVC::IAvcUnit> unitAtStart;
    Discovery::DeviceRouteToken routeAtStart{};
    Scheduling::TimerToken settleTimer{Scheduling::kInvalidTimerToken};
    std::atomic<bool> completed{false};

    AudioClockConfig desiredClock{};
    AVC::CipSfc desiredSfc{AVC::CipSfc::k48000};
    AVC::Cmd::PlugSignalFormat inputBefore{};
    AVC::Cmd::PlugSignalFormat outputBefore{};
    AVC::Cmd::PlugSignalFormat inputAfter{};
    AVC::Cmd::PlugSignalFormat outputAfter{};
    ClockApplyCallback completion{};
    Phase phase{Phase::kReadInputBefore};
    IOReturn failureStatus{kIOReturnSuccess};
    bool inputChanged{false};
    bool outputChanged{false};
};

namespace {

[[nodiscard]] bool IsAM824Format(const AVC::Cmd::PlugSignalFormat& format) noexcept {
    return format.fmt == AVC::Cmd::kFmtAm824 && AVC::Cmd::SfcOf(format).has_value();
}

[[nodiscard]] bool MatchesRequestedRate(const AVC::Cmd::PlugSignalFormat& format,
                                        AVC::CipSfc requestedSfc) noexcept {
    return IsAM824Format(format) && AVC::Cmd::SfcOf(format) == requestedSfc;
}

} // namespace

bool ApogeeDuetDuplex::IsActive(const ClockTransition& transition) const noexcept {
    return !transition.completed.load(std::memory_order_acquire) &&
           activeClockTransition_ &&
           activeClockTransition_.get() == &transition &&
           transition.epoch == activeClockTransitionEpoch_;
}

CMP::CMPDevice ApogeeDuetDuplex::CurrentCMPDevice() const noexcept { return CMP::CMPDevice{.route = runtime_.route}; }

void ApogeeDuetDuplex::Shutdown() noexcept {
    CancelClockTransition(kIOReturnAborted);
    clockConfigApplied_ = false;
    outputConnected_ = false;
    inputConnected_ = false;
}

void ApogeeDuetDuplex::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                              std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    // A replacement unit or node identity denotes a newly discovered bus
    // epoch. Do not carry the AV/C configuration cache across that boundary.
    if (runtime_.route != route || runtime_.avcUnit != avcUnit) {
        CancelClockTransition(kIOReturnAborted);
        clockConfigApplied_ = false;
        if (runtime_.cmpClient && runtime_.route) {
            runtime_.cmpClient->InvalidateRoute(runtime_.route);
        }
        preparedRouteEpoch_ = 0;
    }
    runtime_.route = route;
    runtime_.avcUnit = std::move(avcUnit);
}

bool ApogeeDuetDuplex::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = AudioStreamRuntimeCaps{
        .hostInputPcmChannels = 2,
        .hostOutputPcmChannels = 2,
        .deviceToHostAm824Slots = 2,
        .hostToDeviceAm824Slots = 2,
        .sampleRateHz = appliedClock_.sampleRateHz != 0 ? appliedClock_.sampleRateHz : 48000U,
        .deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    return true;
}

void ApogeeDuetDuplex::PrepareDuplex(const AudioDuplexChannels& channels,
                                       const AudioClockConfig& desiredClock,
                                       PrepareCallback callback) {
    if (!runtime_.cmpClient || !runtime_.irmClient || !runtime_.avcUnit) {
        ASFW_LOG_ERROR(Oxfw, "PrepareDuplex: not ready (cmp=%d irm=%d fcp=%d)",
                       runtime_.cmpClient != nullptr, runtime_.irmClient != nullptr, runtime_.avcUnit != nullptr);
        callback(kIOReturnNotReady, {});
        return;
    }
    // The shared resolver validates the discovered rate-specific formation.
    // This adapter rejects unencodable rates, not the old 1x-only build limit.
    // OXFW programs the selected formation input then output at any supported
    // rate (Linux oxfw-stream.c:41-54).
    if (!AVC::CipSfcFromHz(desiredClock.sampleRateHz).has_value()) {
        ASFW_LOG_ERROR(Oxfw, "PrepareDuplex: unsupported clock %u Hz", desiredClock.sampleRateHz);
        callback(kIOReturnUnsupported, {});
        return;
    }

    ASFW_LOG(Oxfw, "PrepareDuplex: rate=%u node=%u gen=%u epoch=%llu",
             desiredClock.sampleRateHz, static_cast<unsigned>(runtime_.route.nodeId),
             static_cast<unsigned>(runtime_.route.generation.value),
             static_cast<unsigned long long>(runtime_.route.routeEpoch));

    if (preparedRouteEpoch_ != runtime_.route.routeEpoch) {
        // PCR state and device stream formation are reset-scoped. Preserve no
        // old connection as a candidate for BREAK/reuse; recovery must reserve
        // fresh resources and establish fresh PCRs in the new generation.
        if (runtime_.cmpClient && runtime_.route) {
            runtime_.cmpClient->InvalidateRoute(runtime_.route);
        }
        clockConfigApplied_ = false;
        outputConnected_ = false;
        inputConnected_ = false;
        preparedRouteEpoch_ = runtime_.route.routeEpoch;
    }

    // A normal start is a control-plane boundary, not a packet hot path.  Do
    // not trust a prior in-process success: re-read both device formations so
    // the 48 kHz start contract is checked before IRM/CMP allocation.
    clockConfigApplied_ = false;
    duplexChannels_ = channels;
    ApplyClockConfig(
        desiredClock,
        [this, channels, callback = std::move(callback)](IOReturn status,
                                                         DuplexClockApplyResult result) mutable {
            callback(status,
                     DuplexPrepareResult{
                         .generation = result.generation,
                         .channels = channels,
                         .appliedClock = result.appliedClock,
                         .runtimeCaps = result.runtimeCaps,
                     });
        });
}

void ApogeeDuetDuplex::SetAssignedChannels(const AudioDuplexChannels& channels) noexcept {
    // PrepareDuplex runs before IRM allocation so it can establish clock and
    // geometry. The coordinator calls this hook with the committed allocation
    // before either CMP plug is programmed.
    duplexChannels_ = channels;
}

void ApogeeDuetDuplex::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                          ClockApplyCallback callback) {
    if (!runtime_.avcUnit) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!AVC::CipSfcFromHz(desiredClock.sampleRateHz).has_value()) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    if (activeClockTransition_) {
        callback(kIOReturnBusy, {});
        return;
    }

    if (clockConfigApplied_ && appliedClock_.sampleRateHz == desiredClock.sampleRateHz) {
        AudioStreamRuntimeCaps caps{};
        (void)GetRuntimeAudioStreamCaps(caps);
        callback(kIOReturnSuccess,
                 DuplexClockApplyResult{
                     .generation = runtime_.busInfo.GetGeneration(),
                     .appliedClock = appliedClock_,
                     .runtimeCaps = caps,
                 });
        return;
    }

    const auto sampleRate = AVC::CipSfcFromHz(desiredClock.sampleRateHz);
    if (!sampleRate.has_value()) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    // The device-side format transition is deliberately profile-owned. Linux
    // OXFW sets input before output (oxfw-stream.c:41-54), then waits after a
    // format write before further traffic (oxfw-stream.c:93-100). We first
    // capture both formations so an unsuccessful transition can restore the
    // device state; the generic duplex coordinator owns host/CMP/IRM rollback
    // because this method runs before those resources are committed.
    auto transition = std::make_shared<ClockTransition>();
    transition->epoch = ++nextClockTransitionEpoch_;
    transition->generation = runtime_.busInfo.GetGeneration();
    transition->unitAtStart = runtime_.avcUnit;
    transition->routeAtStart = runtime_.route;
    transition->desiredClock = desiredClock;
    transition->desiredSfc = *sampleRate;
    transition->completion = std::move(callback);

    activeClockTransition_ = transition;
    activeClockTransitionEpoch_ = transition->epoch;

    AdvanceClockTransition(transition);
}

void ApogeeDuetDuplex::AdvanceClockTransition(
    const std::shared_ptr<ClockTransition>& transition) {
    if (!transition || !IsActive(*transition) || !runtime_.avcUnit) {
        if (transition && !transition->completed.load(std::memory_order_acquire)) {
            FailClockTransition(transition, kIOReturnNotReady);
        }
        return;
    }

    const auto submitStatus = [this, transition](bool isInput,
                                                   bool captureBefore,
                                                   ClockTransition::Phase nextPhase) {
        AVC::Cmd::PlugSignalFormatCommand cmd{
            .operands = {
                .direction = isInput ? AVC::Cmd::PlugSignalDirection::kInput : AVC::Cmd::PlugSignalDirection::kOutput,
                .plugId = 0,
                .query = AVC::Cmd::SignalFormatQuery::kAllWildcard,
            }
        };
        runtime_.avcUnit->Status(cmd, [this, transition, isInput, captureBefore, nextPhase](
                                               AVC::Expected<AVC::Cmd::PlugSignalFormat> reply) {
            if (!IsActive(*transition)) {
                return;
            }
            if (!reply) {
                FailClockTransition(transition, MapAvcErrorToIOReturn(reply.error()));
                return;
            }
            if (isInput && captureBefore) {
                transition->inputBefore = *reply;
            } else if (isInput) {
                transition->inputAfter = *reply;
            } else if (captureBefore) {
                transition->outputBefore = *reply;
            } else {
                transition->outputAfter = *reply;
            }
            transition->phase = nextPhase;
            AdvanceClockTransition(transition);
        });
    };

    switch (transition->phase) {
        case ClockTransition::Phase::kReadInputBefore:
            submitStatus(true, true, ClockTransition::Phase::kReadOutputBefore);
            return;
        case ClockTransition::Phase::kReadOutputBefore:
            submitStatus(false, true, ClockTransition::Phase::kSetInput);
            return;
        case ClockTransition::Phase::kSetInput:
            if (!IsAM824Format(transition->inputBefore) ||
                !IsAM824Format(transition->outputBefore)) {
                FailClockTransition(transition, kIOReturnUnsupported);
                return;
            }
            if (MatchesRequestedRate(transition->inputBefore, transition->desiredSfc)) {
                transition->phase = ClockTransition::Phase::kSetOutput;
                AdvanceClockTransition(transition);
                return;
            }
            {
                AVC::Cmd::PlugSignalFormatCommand cmd{
                    .operands = {
                        .direction = AVC::Cmd::PlugSignalDirection::kInput,
                        .plugId = 0,
                        .format = AVC::Cmd::Am824SignalFormat(0, transition->desiredSfc),
                    }
                };
                runtime_.avcUnit->Control(cmd, [this, transition](
                                                       AVC::Expected<AVC::Cmd::PlugSignalFormat> reply) {
                    if (!IsActive(*transition)) {
                        return;
                    }
                    if (!reply) {
                        FailClockTransition(transition, MapAvcErrorToIOReturn(reply.error()));
                        return;
                    }
                    transition->inputChanged = true;
                    transition->phase = ClockTransition::Phase::kSetOutput;
                    AdvanceClockTransition(transition);
                });
            }
            return;
        case ClockTransition::Phase::kSetOutput:
            if (MatchesRequestedRate(transition->outputBefore, transition->desiredSfc)) {
                transition->phase = ClockTransition::Phase::kSettle;
                AdvanceClockTransition(transition);
                return;
            }
            {
                AVC::Cmd::PlugSignalFormatCommand cmd{
                    .operands = {
                        .direction = AVC::Cmd::PlugSignalDirection::kOutput,
                        .plugId = 0,
                        .format = AVC::Cmd::Am824SignalFormat(0, transition->desiredSfc),
                    }
                };
                runtime_.avcUnit->Control(cmd, [this, transition](
                                                       AVC::Expected<AVC::Cmd::PlugSignalFormat> reply) {
                    if (!IsActive(*transition)) {
                        return;
                    }
                    if (!reply) {
                        FailClockTransition(transition, MapAvcErrorToIOReturn(reply.error()));
                        return;
                    }
                    transition->outputChanged = true;
                    transition->phase = ClockTransition::Phase::kSettle;
                    AdvanceClockTransition(transition);
                });
            }
            return;
        case ClockTransition::Phase::kSettle: {
            transition->phase = ClockTransition::Phase::kReadInputAfter;
            const bool needsSettle = transition->inputChanged || transition->outputChanged;
            if (!needsSettle || runtime_.formatSettleDelayMs == 0U) {
                AdvanceClockTransition(transition);
                return;
            }
            if (!runtime_.timerScheduler) {
                FailClockTransition(transition, kIOReturnNotReady);
                return;
            }
            const uint64_t currentEpoch = transition->epoch;
            transition->settleTimer = runtime_.timerScheduler->ScheduleAfter(
                static_cast<uint64_t>(runtime_.formatSettleDelayMs) * 1000000ULL,
                [this, transition, currentEpoch]() {
                    transition->settleTimer = Scheduling::kInvalidTimerToken;
                    if (!IsActive(*transition) ||
                        transition->unitAtStart != runtime_.avcUnit ||
                        transition->generation != runtime_.busInfo.GetGeneration()) {
                        return;
                    }
                    AdvanceClockTransition(transition);
                });
            if (transition->settleTimer == Scheduling::kInvalidTimerToken) {
                FailClockTransition(transition, kIOReturnNoResources);
            }
            return;
        }
        case ClockTransition::Phase::kReadInputAfter:
            submitStatus(true, false, ClockTransition::Phase::kReadOutputAfter);
            return;
        case ClockTransition::Phase::kReadOutputAfter:
            submitStatus(false, false, ClockTransition::Phase::kRestoreInput);
            return;
        case ClockTransition::Phase::kRestoreInput:
            if (!MatchesRequestedRate(transition->inputAfter, transition->desiredSfc) ||
                !MatchesRequestedRate(transition->outputAfter, transition->desiredSfc)) {
                FailClockTransition(transition, kIOReturnError);
                return;
            }
            CompleteClockTransition(transition, kIOReturnSuccess);
            return;
        case ClockTransition::Phase::kRestoreOutput:
            if (transition->outputChanged) {
                const auto origSfc = AVC::Cmd::SfcOf(transition->outputBefore);
                if (origSfc.has_value()) {
                    AVC::Cmd::PlugSignalFormatCommand cmd{
                        .operands = {
                            .direction = AVC::Cmd::PlugSignalDirection::kOutput,
                            .plugId = 0,
                            .format = AVC::Cmd::Am824SignalFormat(0, *origSfc),
                        }
                    };
                    runtime_.avcUnit->Control(cmd, [this, transition](
                                                           AVC::Expected<AVC::Cmd::PlugSignalFormat>) {
                        CompleteClockTransition(transition, transition->failureStatus);
                    });
                    return;
                }
            }
            CompleteClockTransition(transition, transition->failureStatus);
            return;
    }
}

void ApogeeDuetDuplex::CancelClockTransition(IOReturn status) {
    if (!activeClockTransition_) {
        return;
    }
    auto transition = activeClockTransition_;
    activeClockTransition_.reset();
    activeClockTransitionEpoch_ = 0;
    FinishClockTransition(transition, status);
}

void ApogeeDuetDuplex::FailClockTransition(const std::shared_ptr<ClockTransition>& transition,
                                             IOReturn status) {
    if (!transition) {
        return;
    }
    clockConfigApplied_ = false;
    if (transition->failureStatus == kIOReturnSuccess) {
        transition->failureStatus = status;
        // Only the first failure names the real cause; the restore phases that
        // follow re-enter here and would otherwise overwrite it in the log too.
        ASFW_LOG_ERROR(Oxfw, "clock transition epoch=%llu failed in phase=%d status=0x%08x",
                       static_cast<unsigned long long>(transition->epoch),
                       static_cast<int>(transition->phase), static_cast<unsigned>(status));
    }

    if (transition->phase == ClockTransition::Phase::kRestoreInput ||
        transition->phase == ClockTransition::Phase::kRestoreOutput) {
        CompleteClockTransition(transition, transition->failureStatus);
        return;
    }

    if (transition->inputChanged) {
        transition->phase = ClockTransition::Phase::kRestoreInput;
        const auto origSfc = AVC::Cmd::SfcOf(transition->inputBefore);
        if (origSfc.has_value()) {
            AVC::Cmd::PlugSignalFormatCommand cmd{
                .operands = {
                    .direction = AVC::Cmd::PlugSignalDirection::kInput,
                    .plugId = 0,
                    .format = AVC::Cmd::Am824SignalFormat(0, *origSfc),
                }
            };
            runtime_.avcUnit->Control(cmd, [this, transition](
                                                   AVC::Expected<AVC::Cmd::PlugSignalFormat>) {
                if (!IsActive(*transition)) {
                    return;
                }
                transition->phase = ClockTransition::Phase::kRestoreOutput;
                AdvanceClockTransition(transition);
            });
            return;
        }
    }

    transition->phase = ClockTransition::Phase::kRestoreOutput;
    AdvanceClockTransition(transition);
}

void ApogeeDuetDuplex::CompleteClockTransition(
    const std::shared_ptr<ClockTransition>& transition,
    IOReturn status) {
    FinishClockTransition(transition, status);
}

void ApogeeDuetDuplex::FinishClockTransition(
    const std::shared_ptr<ClockTransition>& transition,
    IOReturn status) {
    if (!transition || transition->completed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (transition->settleTimer != Scheduling::kInvalidTimerToken && runtime_.timerScheduler) {
        const auto timerToCancel = transition->settleTimer;
        transition->settleTimer = Scheduling::kInvalidTimerToken;
        runtime_.timerScheduler->Cancel(timerToCancel);
    }

    if (activeClockTransition_ && activeClockTransition_.get() == transition.get()) {
        activeClockTransition_.reset();
        activeClockTransitionEpoch_ = 0;
    }

    if (!transition->completion) {
        return;
    }

    auto completion = std::move(transition->completion);
    if (status != kIOReturnSuccess) {
        clockConfigApplied_ = false;
        completion(status, {});
        return;
    }

    appliedClock_ = transition->desiredClock;
    if (runtime_.avcUnit) {
        transition->unitAtStart->RememberConfirmedDuplexRate(transition->routeAtStart, appliedClock_.sampleRateHz);
    }
    clockConfigApplied_ = true;
    ASFW_LOG(Oxfw, "clock transition epoch=%llu applied rate=%u",
             static_cast<unsigned long long>(transition->epoch),
             appliedClock_.sampleRateHz);
    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    completion(kIOReturnSuccess,
               DuplexClockApplyResult{
                   .generation = runtime_.busInfo.GetGeneration(),
                   .appliedClock = appliedClock_,
                   .runtimeCaps = caps,
               });
}

namespace {

/// A CMP completion always carries a status, so the caller no longer has to
/// distinguish "the operation reported failure" from "our own poll loop gave
/// up". Timeout here means the device or IRM genuinely did not answer.
[[nodiscard]] IOReturn MapCMPStatus(CMP::CMPStatus status) noexcept {
    switch (status) {
        case CMP::CMPStatus::Success:
            return kIOReturnSuccess;
        case CMP::CMPStatus::Timeout:
            return kIOReturnTimeout;
        default:
            return kIOReturnError;
    }
}

} // namespace

void ApogeeDuetDuplex::ProgramRx(StageCallback callback) {
    if (!runtime_.cmpClient) {
        callback(kIOReturnNotReady, {});
        return;
    }

    const auto channel = duplexChannels_.deviceToHostIsoChannel;
    runtime_.cmpClient->ConnectOPCR(
        CurrentCMPDevice(), 0, channel,
        [this, channel, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            const IOReturn kr = MapCMPStatus(status);
            outputConnected_ = kr == kIOReturnSuccess;

            if (outputConnected_) {
                ASFW_LOG(Oxfw, "ProgramRx: oPCR0 connected ch=%u", channel);
            } else {
                ASFW_LOG_ERROR(Oxfw, "ProgramRx: oPCR0 ch=%u failed (%{public}s)", channel,
                               IRM::ToString(status));
            }

            AudioStreamRuntimeCaps caps{};
            (void)GetRuntimeAudioStreamCaps(caps);
            callback(kr, DuplexStageResult{
                             .generation = runtime_.busInfo.GetGeneration(),
                             .channels = duplexChannels_,
                             .phase = DuplexRestartPhase::kDeviceRxProgrammed,
                             .runtimeCaps = caps,
                         });
        });
}

void ApogeeDuetDuplex::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!runtime_.cmpClient) {
        callback(kIOReturnNotReady, {});
        return;
    }

    const auto channel = duplexChannels_.hostToDeviceIsoChannel;
    runtime_.cmpClient->ConnectIPCR(
        CurrentCMPDevice(), 0, channel,
        [this, channel, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            const IOReturn kr = MapCMPStatus(status);
            inputConnected_ = kr == kIOReturnSuccess;

            if (inputConnected_) {
                ASFW_LOG(Oxfw, "ProgramTx: iPCR0 connected ch=%u", channel);
            } else {
                ASFW_LOG_ERROR(Oxfw, "ProgramTx: iPCR0 ch=%u failed (%{public}s)", channel,
                               IRM::ToString(status));
            }

            AudioStreamRuntimeCaps caps{};
            (void)GetRuntimeAudioStreamCaps(caps);
            callback(kr, DuplexStageResult{
                             .generation = runtime_.busInfo.GetGeneration(),
                             .channels = duplexChannels_,
                             .phase = DuplexRestartPhase::kDeviceTxArmed,
                             .runtimeCaps = caps,
                         });
        });
}

void ApogeeDuetDuplex::ConfirmDuplexStart(ConfirmCallback callback) {
    if (outputConnected_ && inputConnected_) {
        ASFW_LOG(Oxfw, "ConfirmDuplexStart: duplex up rate=%u rx=%u tx=%u",
                 appliedClock_.sampleRateHz, duplexChannels_.deviceToHostIsoChannel,
                 duplexChannels_.hostToDeviceIsoChannel);
    } else {
        ASFW_LOG_ERROR(Oxfw, "ConfirmDuplexStart: incomplete (out=%d in=%d)",
                       outputConnected_, inputConnected_);
    }

    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    callback((outputConnected_ && inputConnected_) ? kIOReturnSuccess : kIOReturnNotReady,
             DuplexConfirmResult{
                 .generation = runtime_.busInfo.GetGeneration(),
                 .channels = duplexChannels_,
                 .appliedClock = appliedClock_,
                 .runtimeCaps = caps,
             });
}

void ApogeeDuetDuplex::ReadDuplexHealth(HealthCallback callback) {
    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    callback(kIOReturnSuccess,
             DuplexHealthResult{
                 .generation = runtime_.busInfo.GetGeneration(),
                 .appliedClock = appliedClock_,
                 .runtimeCaps = caps,
                 .sourceLocked = appliedClock_.sampleRateHz != 0,
                 .clockReferenceHealthy = true,
                 .nominalRateHz = appliedClock_.sampleRateHz,
             });
}

void ApogeeDuetDuplex::DisconnectPlayback(VoidCallback callback) {
    if (!runtime_.cmpClient || !inputConnected_) {
        inputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }

    // Cleared and the device resolved *before* the request goes out, so the
    // completion captures no `this`: StopDuplex issues this fire-and-forget, and
    // a continuation that outlived the object would dereference freed memory.
    inputConnected_ = false;
    runtime_.cmpClient->DisconnectIPCR(
        CurrentCMPDevice(), 0,
        [callback = std::move(callback)](CMP::CMPStatus status) mutable {
            // Anomaly-only: a break that fails leaves the plug's
            // point-to-point count set, and the next start then fails to
            // connect for a reason that looks unrelated. StopDuplex discards
            // this status, so without a line here the cause is unrecoverable.
            if (status != CMP::CMPStatus::Success) {
                ASFW_LOG_ERROR(Oxfw, "DisconnectPlayback: iPCR0 break failed (%{public}s)",
                               IRM::ToString(status));
            }
            callback(MapCMPStatus(status));
        });
}

void ApogeeDuetDuplex::DisconnectCapture(VoidCallback callback) {
    if (!runtime_.cmpClient || !outputConnected_) {
        outputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }

    outputConnected_ = false;
    runtime_.cmpClient->DisconnectOPCR(
        CurrentCMPDevice(), 0,
        [callback = std::move(callback)](CMP::CMPStatus status) mutable {
            if (status != CMP::CMPStatus::Success) {
                ASFW_LOG_ERROR(Oxfw, "DisconnectCapture: oPCR0 break failed (%{public}s)",
                               IRM::ToString(status));
            }
            callback(MapCMPStatus(status));
        });
}

IOReturn ApogeeDuetDuplex::StopDuplex() {
    // The one record that says the stream went away deliberately. Without it a
    // teardown and a silent stall look identical in the ring.
    ASFW_LOG(Oxfw, "StopDuplex: breaking both plugs (out=%d in=%d)", outputConnected_,
             inputConnected_);

    // Best-effort and fire-and-forget. Waiting for the breaks would occupy the
    // very queue that delivers their completions, and teardown is the context
    // least able to afford that. Linux does the same (bebob_stream.c:602-606):
    // the next start re-verifies the plugs and clears any residual
    // point-to-point count, so an unacknowledged break is recoverable.
    DisconnectPlayback([](IOReturn) {});
    DisconnectCapture([](IOReturn) {});
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// FamilyDriver
// ---------------------------------------------------------------------------

void ApogeeDuetDuplex::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
}

IOReturn ApogeeDuetDuplex::LoadGeometry() {
    // Nothing to read at start: the Duet's stream geometry is fixed
    // (ApogeeCaps) and does not depend on device registers.
    return kIOReturnSuccess;
}

std::optional<AudioStreamRuntimeCaps> ApogeeDuetDuplex::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (!GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> ApogeeDuetDuplex::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    return AwaitStage<DuplexPrepareResult>(
        [&](auto callback) { PrepareDuplex(channels, clock, std::move(callback)); },
        teardownCancel_);
}

std::expected<AudioDuplexChannels, IOReturn> ApogeeDuetDuplex::AssignChannels(const AudioDuplexChannels& channels) {
    SetAssignedChannels(channels);
    return channels;
}

std::expected<DuplexHealthResult, IOReturn> ApogeeDuetDuplex::ReadHealth(uint32_t timeoutMs) {
    return AwaitStage<DuplexHealthResult>(
        [&](auto callback) { ReadDuplexHealth(std::move(callback)); }, teardownCancel_, timeoutMs);
}

std::expected<DuplexStageResult, IOReturn> ApogeeDuetDuplex::ArmDeviceRx() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramRx(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexStageResult, IOReturn> ApogeeDuetDuplex::ArmDeviceTxAndEnable() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramTxAndEnableDuplex(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexConfirmResult, IOReturn> ApogeeDuetDuplex::Confirm() {
    return AwaitStage<DuplexConfirmResult>(
        [&](auto callback) { ConfirmDuplexStart(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexClockApplyResult, IOReturn> ApogeeDuetDuplex::ApplyClockIdle(
    const AudioClockConfig& clock) {
    return AwaitStage<DuplexClockApplyResult>(
        [&](auto callback) { ApplyClockConfig(clock, std::move(callback)); }, teardownCancel_);
}

IOReturn ApogeeDuetDuplex::DisconnectPlayback() {
    return AwaitStageStatus([&](auto callback) { DisconnectPlayback(std::move(callback)); },
                            teardownCancel_);
}

IOReturn ApogeeDuetDuplex::DisconnectCapture() {
    return AwaitStageStatus([&](auto callback) { DisconnectCapture(std::move(callback)); },
                            teardownCancel_);
}

IOReturn ApogeeDuetDuplex::BreakConnections() {
    // Not offered: the Duet's staged stop disconnects each direction in turn
    // (DisconnectPlayback, DisconnectCapture), and StopDuplex drops whatever
    // is left.
    return kIOReturnUnsupported;
}

IOReturn ApogeeDuetDuplex::Stop() {
    return StopDuplex();
}

} // namespace ASFW::Audio::Oxford::Apogee
