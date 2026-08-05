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
// Known deviation, deliberately preserved: the four CMP stages below still block
// on WaitForCMP/IOSleep rather than running as continuations. That is a real
// defect (it is the CMP stage work-loop self-deadlock class), but converting it
// is a behaviour change to a path that is working on hardware, so it stays a
// verbatim move here and is tracked separately.

#include "ApogeeDuetDuplex.hpp"

#include "ApogeeCaps.hpp"

#include "../../../../Bus/IRM/IRMClient.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Protocols/AVC/AVCDefs.hpp"
#include "../../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"

#include <DriverKit/IOLib.h>
#include <memory>

namespace ASFW::Audio::Oxford::Apogee {

using Protocols::AVC::AVCResult;

using SignalFormatCommand = Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand;
using SignalSampleRate = Protocols::AVC::StreamFormats::SampleRate;

namespace {

constexpr uint32_t kCMPTimeoutMs = 250;
constexpr uint32_t kCMPPollMs = 5;

// Still needed here by the clock-transition path, which maps AV/C signal-format
// results. The FCP-status mapping moved out with dispatch (FW-129).
[[nodiscard]] IOReturn MapAVCResultToIOReturn(AVCResult result) noexcept {
    switch (result) {
        case AVCResult::kAccepted:
        case AVCResult::kImplementedStable:
        case AVCResult::kChanged:
            return kIOReturnSuccess;
        case AVCResult::kNotImplemented:
            return kIOReturnUnsupported;
        case AVCResult::kInTransition:
        case AVCResult::kInterim:
        case AVCResult::kBusy:
            return kIOReturnBusy;
        case AVCResult::kTimeout:
            return kIOReturnTimeout;
        case AVCResult::kBusReset:
            return kIOReturnNotResponding;
        default:
            return kIOReturnError;
    }
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
    Protocols::AVC::FCPTransport* transportAtStart{nullptr};
    Scheduling::TimerToken settleTimer{Scheduling::kInvalidTimerToken};
    std::atomic<bool> completed{false};

    AudioClockConfig desiredClock{};
    SignalSampleRate desiredRate{SignalSampleRate::kUnknown};
    SignalFormatCommand::SignalFormat inputBefore{};
    SignalFormatCommand::SignalFormat outputBefore{};
    SignalFormatCommand::SignalFormat inputAfter{};
    SignalFormatCommand::SignalFormat outputAfter{};
    IDuplexDeviceControl::ClockApplyCallback completion{};
    Phase phase{Phase::kReadInputBefore};
    IOReturn failureStatus{kIOReturnSuccess};
    bool inputChanged{false};
    bool outputChanged{false};
};

namespace {

[[nodiscard]] bool IsAM824Format(const SignalFormatCommand::SignalFormat& format) noexcept {
    return format.format == 0x90U &&
           SignalFormatCommand::FrequencyToSampleRate(format.frequency) != SignalSampleRate::kUnknown;
}

[[nodiscard]] bool MatchesRequestedRate(const SignalFormatCommand::SignalFormat& format,
                                        SignalSampleRate requestedRate) noexcept {
    return IsAM824Format(format) &&
           SignalFormatCommand::FrequencyToSampleRate(format.frequency) == requestedRate;
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
                                              Protocols::AVC::FCPTransport* transport) {
    // A replacement transport or node identity denotes a newly discovered bus
    // epoch. Do not carry the AV/C configuration cache across that boundary.
    if (runtime_.route != route || runtime_.fcpTransport != transport) {
        CancelClockTransition(kIOReturnAborted);
        clockConfigApplied_ = false;
        if (runtime_.cmpClient && runtime_.route) {
            runtime_.cmpClient->InvalidateRoute(runtime_.route);
        }
        preparedRouteEpoch_ = 0;
    }
    runtime_.route = route;
    runtime_.fcpTransport = transport;
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
    if (!runtime_.cmpClient || !runtime_.irmClient || !runtime_.fcpTransport) {
        ASFW_LOG_ERROR(Oxfw, "PrepareDuplex: not ready (cmp=%d irm=%d fcp=%d)",
                       runtime_.cmpClient != nullptr, runtime_.irmClient != nullptr, runtime_.fcpTransport != nullptr);
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!IsSupportedAudioClockConfig(desiredClock)) {
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
                                                         ClockApplyResult result) mutable {
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
    if (!runtime_.fcpTransport) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!IsSupportedAudioClockConfig(desiredClock)) {
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
                 ClockApplyResult{
                     .generation = runtime_.busInfo.GetGeneration(),
                     .appliedClock = appliedClock_,
                     .runtimeCaps = caps,
                 });
        return;
    }

    const SignalSampleRate sampleRate = [&desiredClock]() noexcept {
        switch (desiredClock.sampleRateHz) {
            case 32000U:
                return SignalSampleRate::k32000Hz;
            case 44100U:
                return SignalSampleRate::k44100Hz;
            case 48000U:
                return SignalSampleRate::k48000Hz;
            default:
                return SignalSampleRate::kUnknown;
        }
    }();

    if (sampleRate == SignalSampleRate::kUnknown) {
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
    transition->transportAtStart = runtime_.fcpTransport;
    transition->desiredClock = desiredClock;
    transition->desiredRate = sampleRate;
    transition->completion = std::move(callback);

    activeClockTransition_ = transition;
    activeClockTransitionEpoch_ = transition->epoch;

    AdvanceClockTransition(transition);
}

void ApogeeDuetDuplex::AdvanceClockTransition(
    const std::shared_ptr<ClockTransition>& transition) {
    if (!transition || !IsActive(*transition) || !runtime_.fcpTransport) {
        if (transition && !transition->completed.load(std::memory_order_acquire)) {
            FailClockTransition(transition, kIOReturnNotReady);
        }
        return;
    }

    const auto submitStatus = [this, transition](bool isInput,
                                                   bool captureBefore,
                                                   ClockTransition::Phase nextPhase) {
        auto command = std::make_shared<SignalFormatCommand>(*runtime_.fcpTransport, 0, isInput);
        command->Submit([this, transition, isInput, captureBefore, nextPhase, command](
                            Protocols::AVC::AVCResult result,
                            const SignalFormatCommand::SignalFormat& format) {
            if (!IsActive(*transition)) {
                return;
            }
            const IOReturn status = MapAVCResultToIOReturn(result);
            if (status != kIOReturnSuccess) {
                FailClockTransition(transition, status);
                return;
            }
            if (isInput && captureBefore) {
                transition->inputBefore = format;
            } else if (isInput) {
                transition->inputAfter = format;
            } else if (captureBefore) {
                transition->outputBefore = format;
            } else {
                transition->outputAfter = format;
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
            if (MatchesRequestedRate(transition->inputBefore, transition->desiredRate)) {
                transition->phase = ClockTransition::Phase::kSetOutput;
                AdvanceClockTransition(transition);
                return;
            }
            {
                auto command = std::make_shared<SignalFormatCommand>(
                    *runtime_.fcpTransport, 0, true, transition->desiredRate);
                command->Submit([this, transition, command](
                                    Protocols::AVC::AVCResult result,
                                    const SignalFormatCommand::SignalFormat&) {
                    if (!IsActive(*transition)) {
                        return;
                    }
                    const IOReturn status = MapAVCResultToIOReturn(result);
                    if (status != kIOReturnSuccess) {
                        FailClockTransition(transition, status);
                        return;
                    }
                    transition->inputChanged = true;
                    transition->phase = ClockTransition::Phase::kSetOutput;
                    AdvanceClockTransition(transition);
                });
            }
            return;
        case ClockTransition::Phase::kSetOutput:
            if (MatchesRequestedRate(transition->outputBefore, transition->desiredRate)) {
                transition->phase = ClockTransition::Phase::kSettle;
                AdvanceClockTransition(transition);
                return;
            }
            {
                auto command = std::make_shared<SignalFormatCommand>(
                    *runtime_.fcpTransport, 0, false, transition->desiredRate);
                command->Submit([this, transition, command](
                                    Protocols::AVC::AVCResult result,
                                    const SignalFormatCommand::SignalFormat&) {
                    if (!IsActive(*transition)) {
                        return;
                    }
                    const IOReturn status = MapAVCResultToIOReturn(result);
                    if (status != kIOReturnSuccess) {
                        FailClockTransition(transition, status);
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
                        transition->transportAtStart != runtime_.fcpTransport ||
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
            if (!MatchesRequestedRate(transition->inputAfter, transition->desiredRate) ||
                !MatchesRequestedRate(transition->outputAfter, transition->desiredRate)) {
                FailClockTransition(transition, kIOReturnError);
                return;
            }
            CompleteClockTransition(transition, kIOReturnSuccess);
            return;
        case ClockTransition::Phase::kRestoreOutput:
            if (transition->outputChanged) {
                auto command = std::make_shared<SignalFormatCommand>(
                    *runtime_.fcpTransport, 0, false,
                    SignalFormatCommand::FrequencyToSampleRate(transition->outputBefore.frequency));
                command->Submit([this, transition, command](Protocols::AVC::AVCResult,
                                                              const SignalFormatCommand::SignalFormat&) {
                    CompleteClockTransition(transition, transition->failureStatus);
                });
                return;
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
        auto command = std::make_shared<SignalFormatCommand>(
            *runtime_.fcpTransport, 0, true,
            SignalFormatCommand::FrequencyToSampleRate(transition->inputBefore.frequency));
        command->Submit([this, transition, command](Protocols::AVC::AVCResult,
                                                      const SignalFormatCommand::SignalFormat&) {
            if (!IsActive(*transition)) {
                return;
            }
            transition->phase = ClockTransition::Phase::kRestoreOutput;
            AdvanceClockTransition(transition);
        });
        return;
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
    clockConfigApplied_ = true;
    ASFW_LOG(Oxfw, "clock transition epoch=%llu applied rate=%u",
             static_cast<unsigned long long>(transition->epoch),
             appliedClock_.sampleRateHz);
    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    completion(kIOReturnSuccess,
               ClockApplyResult{
                   .generation = runtime_.busInfo.GetGeneration(),
                   .appliedClock = appliedClock_,
                   .runtimeCaps = caps,
               });
}

namespace {

struct CMPWaitState {
    std::atomic<bool> done{false};
    std::atomic<CMP::CMPStatus> status{CMP::CMPStatus::Failed};
};

struct CMPWaitResult {
    bool completed{false};
    CMP::CMPStatus status{CMP::CMPStatus::Failed};
};

[[nodiscard]] CMPWaitResult WaitForCMP(const std::shared_ptr<CMPWaitState>& state) noexcept {
    for (uint32_t waited = 0; waited < kCMPTimeoutMs; waited += kCMPPollMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return CMPWaitResult{
                .completed = true,
                .status = state->status.load(std::memory_order_acquire),
            };
        }
        IOSleep(kCMPPollMs);
    }
    return {};
}

} // namespace

void ApogeeDuetDuplex::ProgramRx(StageCallback callback) {
    if (!runtime_.cmpClient) {
        callback(kIOReturnNotReady, {});
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    runtime_.cmpClient->ConnectOPCR(device, 0, duplexChannels_.deviceToHostIsoChannel,
                            [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const CMPWaitResult wait = WaitForCMP(state);
    const bool connected = wait.completed && wait.status == CMP::CMPStatus::Success;
    outputConnected_ = connected;

    if (connected) {
        ASFW_LOG(Oxfw, "ProgramRx: oPCR0 connected ch=%u", duplexChannels_.deviceToHostIsoChannel);
    } else {
        ASFW_LOG_ERROR(Oxfw, "ProgramRx: oPCR0 ch=%u failed (%{public}s)",
                       duplexChannels_.deviceToHostIsoChannel,
                       wait.completed ? IRM::ToString(wait.status) : "wait timeout");
    }

    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    callback(connected ? kIOReturnSuccess
                       : (wait.completed ? kIOReturnError : kIOReturnTimeout),
             DuplexStageResult{
                 .generation = runtime_.busInfo.GetGeneration(),
                 .channels = duplexChannels_,
                 .phase = DuplexRestartPhase::kDeviceRxProgrammed,
                 .runtimeCaps = caps,
             });
}

void ApogeeDuetDuplex::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!runtime_.cmpClient) {
        callback(kIOReturnNotReady, {});
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    runtime_.cmpClient->ConnectIPCR(device, 0, duplexChannels_.hostToDeviceIsoChannel,
                            [state](CMP::CMPStatus status) {
                                state->status.store(status, std::memory_order_release);
                                state->done.store(true, std::memory_order_release);
                            });
    const CMPWaitResult wait = WaitForCMP(state);
    const bool connected = wait.completed && wait.status == CMP::CMPStatus::Success;
    inputConnected_ = connected;

    if (connected) {
        ASFW_LOG(Oxfw, "ProgramTx: iPCR0 connected ch=%u", duplexChannels_.hostToDeviceIsoChannel);
    } else {
        ASFW_LOG_ERROR(Oxfw, "ProgramTx: iPCR0 ch=%u failed (%{public}s)",
                       duplexChannels_.hostToDeviceIsoChannel,
                       wait.completed ? IRM::ToString(wait.status) : "wait timeout");
    }

    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    callback(connected ? kIOReturnSuccess
                       : (wait.completed ? kIOReturnError : kIOReturnTimeout),
             DuplexStageResult{
                 .generation = runtime_.busInfo.GetGeneration(),
                 .channels = duplexChannels_,
                 .phase = DuplexRestartPhase::kDeviceTxArmed,
                 .runtimeCaps = caps,
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

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    runtime_.cmpClient->DisconnectIPCR(device, 0, [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const CMPWaitResult wait = WaitForCMP(state);
    const bool disconnected = wait.completed && wait.status == CMP::CMPStatus::Success;
    inputConnected_ = false;
    callback(disconnected ? kIOReturnSuccess : kIOReturnTimeout);
}

void ApogeeDuetDuplex::DisconnectCapture(VoidCallback callback) {
    if (!runtime_.cmpClient || !outputConnected_) {
        outputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    runtime_.cmpClient->DisconnectOPCR(device, 0, [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const CMPWaitResult wait = WaitForCMP(state);
    const bool disconnected = wait.completed && wait.status == CMP::CMPStatus::Success;
    outputConnected_ = false;
    callback(disconnected ? kIOReturnSuccess : kIOReturnTimeout);
}

IOReturn ApogeeDuetDuplex::StopDuplex() {
    // The one record that says the stream went away deliberately. Without it a
    // teardown and a silent stall look identical in the ring.
    ASFW_LOG(Oxfw, "StopDuplex: breaking both plugs (out=%d in=%d)", outputConnected_,
             inputConnected_);

    IOReturn playbackStatus = kIOReturnSuccess;
    DisconnectPlayback([&playbackStatus](IOReturn status) { playbackStatus = status; });

    IOReturn captureStatus = kIOReturnSuccess;
    DisconnectCapture([&captureStatus](IOReturn status) { captureStatus = status; });

    if (playbackStatus != kIOReturnSuccess || captureStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Oxfw, "StopDuplex: playback=0x%08x capture=0x%08x",
                       static_cast<unsigned>(playbackStatus),
                       static_cast<unsigned>(captureStatus));
    }

    if (playbackStatus != kIOReturnSuccess) {
        return playbackStatus;
    }
    return captureStatus;
}

} // namespace ASFW::Audio::Oxford::Apogee
