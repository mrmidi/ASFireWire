// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.cpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#include "ApogeeDuetProtocol.hpp"

#include "ApogeeParamsSerdes.hpp"
#include "ApogeeTransport.hpp"

#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Protocols/AVC/AVCDefs.hpp"
#include "../../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"
#include "../../../../Bus/IRM/IRMClient.hpp"

#include <DriverKit/IOLib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace ASFW::Audio::Oxford::Apogee {

using Protocols::AVC::AVCResult;
using Protocols::AVC::CTypeToResult;
using Protocols::AVC::FCPFrame;
using Protocols::AVC::FCPStatus;

namespace {

// The AV/C framing constants that used to sit here left with the vendor codec
// (FW-126) and dispatch (FW-129); only the CMP poll geometry is still read from
// this file.
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

// The mute-mode tri-state helpers moved with the serdes (FW-128).

} // namespace

using SignalFormatCommand = Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand;
using SignalSampleRate = Protocols::AVC::StreamFormats::SampleRate;

struct ApogeeDuetProtocol::ClockTransition {
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

ApogeeDuetProtocol::ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                       Protocols::Ports::FireWireBusInfo& busInfo,
                                       Discovery::DeviceRouteToken route,
                                       Protocols::AVC::FCPTransport* fcpTransport,
                                       IRM::IRMClient* irmClient,
                                       CMP::CMPClient* cmpClient,
                                       uint32_t formatSettleDelayMs,
                                       Scheduling::ITimerScheduler* timerScheduler)
    : busOps_(busOps)
    , busInfo_(busInfo)
    , route_(route)
    , fcpTransport_(fcpTransport)
    , irmClient_(irmClient)
    , cmpClient_(cmpClient)
    , formatSettleDelayMs_(formatSettleDelayMs)
    , timerScheduler_(timerScheduler) {
}

bool ApogeeDuetProtocol::IsActive(const ClockTransition& transition) const noexcept {
    return !transition.completed.load(std::memory_order_acquire) &&
           activeClockTransition_ &&
           activeClockTransition_.get() == &transition &&
           transition.epoch == activeClockTransitionEpoch_;
}

CMP::CMPDevice ApogeeDuetProtocol::CurrentCMPDevice() const noexcept { return CMP::CMPDevice{.route = route_}; }

IOReturn ApogeeDuetProtocol::Initialize() {
    return kIOReturnSuccess;
}

IOReturn ApogeeDuetProtocol::Shutdown() {
    CancelClockTransition(kIOReturnAborted);
    clockConfigApplied_ = false;
    outputConnected_ = false;
    inputConnected_ = false;
    return kIOReturnSuccess;
}

void ApogeeDuetProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                              Protocols::AVC::FCPTransport* transport) {
    // A replacement transport or node identity denotes a newly discovered bus
    // epoch. Do not carry the AV/C configuration cache across that boundary.
    if (route_ != route || fcpTransport_ != transport) {
        CancelClockTransition(kIOReturnAborted);
        clockConfigApplied_ = false;
        if (cmpClient_ && route_) {
            cmpClient_->InvalidateRoute(route_);
        }
        preparedRouteEpoch_ = 0;
    }
    route_ = route;
    fcpTransport_ = transport;
}

bool ApogeeDuetProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
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

void ApogeeDuetProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                       const AudioClockConfig& desiredClock,
                                       PrepareCallback callback) {
    if (!cmpClient_ || !irmClient_ || !fcpTransport_) {
        ASFW_LOG_ERROR(Oxfw, "PrepareDuplex: not ready (cmp=%d irm=%d fcp=%d)",
                       cmpClient_ != nullptr, irmClient_ != nullptr, fcpTransport_ != nullptr);
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!IsSupportedAudioClockConfig(desiredClock)) {
        ASFW_LOG_ERROR(Oxfw, "PrepareDuplex: unsupported clock %u Hz", desiredClock.sampleRateHz);
        callback(kIOReturnUnsupported, {});
        return;
    }

    ASFW_LOG(Oxfw, "PrepareDuplex: rate=%u node=%u gen=%u epoch=%llu",
             desiredClock.sampleRateHz, static_cast<unsigned>(route_.nodeId),
             static_cast<unsigned>(route_.generation.value),
             static_cast<unsigned long long>(route_.routeEpoch));

    if (preparedRouteEpoch_ != route_.routeEpoch) {
        // PCR state and device stream formation are reset-scoped. Preserve no
        // old connection as a candidate for BREAK/reuse; recovery must reserve
        // fresh resources and establish fresh PCRs in the new generation.
        if (cmpClient_ && route_) {
            cmpClient_->InvalidateRoute(route_);
        }
        clockConfigApplied_ = false;
        outputConnected_ = false;
        inputConnected_ = false;
        preparedRouteEpoch_ = route_.routeEpoch;
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

void ApogeeDuetProtocol::SetAssignedChannels(const AudioDuplexChannels& channels) noexcept {
    // PrepareDuplex runs before IRM allocation so it can establish clock and
    // geometry. The coordinator calls this hook with the committed allocation
    // before either CMP plug is programmed.
    duplexChannels_ = channels;
}

void ApogeeDuetProtocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                          ClockApplyCallback callback) {
    if (!fcpTransport_) {
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
                     .generation = busInfo_.GetGeneration(),
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
    transition->generation = busInfo_.GetGeneration();
    transition->transportAtStart = fcpTransport_;
    transition->desiredClock = desiredClock;
    transition->desiredRate = sampleRate;
    transition->completion = std::move(callback);

    activeClockTransition_ = transition;
    activeClockTransitionEpoch_ = transition->epoch;

    AdvanceClockTransition(transition);
}

void ApogeeDuetProtocol::AdvanceClockTransition(
    const std::shared_ptr<ClockTransition>& transition) {
    if (!transition || !IsActive(*transition) || !fcpTransport_) {
        if (transition && !transition->completed.load(std::memory_order_acquire)) {
            FailClockTransition(transition, kIOReturnNotReady);
        }
        return;
    }

    const auto submitStatus = [this, transition](bool isInput,
                                                   bool captureBefore,
                                                   ClockTransition::Phase nextPhase) {
        auto command = std::make_shared<SignalFormatCommand>(*fcpTransport_, 0, isInput);
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
                    *fcpTransport_, 0, true, transition->desiredRate);
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
                    *fcpTransport_, 0, false, transition->desiredRate);
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
            if (!needsSettle || formatSettleDelayMs_ == 0U) {
                AdvanceClockTransition(transition);
                return;
            }
            if (!timerScheduler_) {
                FailClockTransition(transition, kIOReturnNotReady);
                return;
            }
            const uint64_t currentEpoch = transition->epoch;
            transition->settleTimer = timerScheduler_->ScheduleAfter(
                static_cast<uint64_t>(formatSettleDelayMs_) * 1000000ULL,
                [this, transition, currentEpoch]() {
                    transition->settleTimer = Scheduling::kInvalidTimerToken;
                    if (!IsActive(*transition) ||
                        transition->transportAtStart != fcpTransport_ ||
                        transition->generation != busInfo_.GetGeneration()) {
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
                    *fcpTransport_, 0, false,
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

void ApogeeDuetProtocol::CancelClockTransition(IOReturn status) {
    if (!activeClockTransition_) {
        return;
    }
    auto transition = activeClockTransition_;
    activeClockTransition_.reset();
    activeClockTransitionEpoch_ = 0;
    FinishClockTransition(transition, status);
}

void ApogeeDuetProtocol::FailClockTransition(const std::shared_ptr<ClockTransition>& transition,
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
            *fcpTransport_, 0, true,
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

void ApogeeDuetProtocol::CompleteClockTransition(
    const std::shared_ptr<ClockTransition>& transition,
    IOReturn status) {
    FinishClockTransition(transition, status);
}

void ApogeeDuetProtocol::FinishClockTransition(
    const std::shared_ptr<ClockTransition>& transition,
    IOReturn status) {
    if (!transition || transition->completed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (transition->settleTimer != Scheduling::kInvalidTimerToken && timerScheduler_) {
        const auto timerToCancel = transition->settleTimer;
        transition->settleTimer = Scheduling::kInvalidTimerToken;
        timerScheduler_->Cancel(timerToCancel);
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
                   .generation = busInfo_.GetGeneration(),
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

void ApogeeDuetProtocol::ProgramRx(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    cmpClient_->ConnectOPCR(device, 0, duplexChannels_.deviceToHostIsoChannel,
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
                 .generation = busInfo_.GetGeneration(),
                 .channels = duplexChannels_,
                 .phase = DuplexRestartPhase::kDeviceRxProgrammed,
                 .runtimeCaps = caps,
             });
}

void ApogeeDuetProtocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    cmpClient_->ConnectIPCR(device, 0, duplexChannels_.hostToDeviceIsoChannel,
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
                 .generation = busInfo_.GetGeneration(),
                 .channels = duplexChannels_,
                 .phase = DuplexRestartPhase::kDeviceTxArmed,
                 .runtimeCaps = caps,
             });
}

void ApogeeDuetProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
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
                 .generation = busInfo_.GetGeneration(),
                 .channels = duplexChannels_,
                 .appliedClock = appliedClock_,
                 .runtimeCaps = caps,
             });
}

void ApogeeDuetProtocol::ReadDuplexHealth(HealthCallback callback) {
    AudioStreamRuntimeCaps caps{};
    (void)GetRuntimeAudioStreamCaps(caps);
    callback(kIOReturnSuccess,
             DuplexHealthResult{
                 .generation = busInfo_.GetGeneration(),
                 .appliedClock = appliedClock_,
                 .runtimeCaps = caps,
                 .sourceLocked = appliedClock_.sampleRateHz != 0,
                 .clockReferenceHealthy = true,
                 .nominalRateHz = appliedClock_.sampleRateHz,
             });
}

void ApogeeDuetProtocol::DisconnectPlayback(VoidCallback callback) {
    if (!cmpClient_ || !inputConnected_) {
        inputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    cmpClient_->DisconnectIPCR(device, 0, [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const CMPWaitResult wait = WaitForCMP(state);
    const bool disconnected = wait.completed && wait.status == CMP::CMPStatus::Success;
    inputConnected_ = false;
    callback(disconnected ? kIOReturnSuccess : kIOReturnTimeout);
}

void ApogeeDuetProtocol::DisconnectCapture(VoidCallback callback) {
    if (!cmpClient_ || !outputConnected_) {
        outputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }

    auto state = std::make_shared<CMPWaitState>();
    const CMP::CMPDevice device = CurrentCMPDevice();
    cmpClient_->DisconnectOPCR(device, 0, [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const CMPWaitResult wait = WaitForCMP(state);
    const bool disconnected = wait.completed && wait.status == CMP::CMPStatus::Success;
    outputConnected_ = false;
    callback(disconnected ? kIOReturnSuccess : kIOReturnTimeout);
}

IOReturn ApogeeDuetProtocol::StopDuplex() {
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

// Dispatch lives in ApogeeTransport (FW-129); these forward the protocol's
// current transport into it.
void ApogeeDuetProtocol::SendVendorCommand(const VendorCommand& command,
                                           bool isStatus,
                                           VendorResultCallback callback) {
    VendorFcp::Send(fcpTransport_, command, isStatus, std::move(callback));
}

void ApogeeDuetProtocol::ExecuteVendorSequence(const std::vector<VendorCommand>& commands,
                                               bool isStatus,
                                               VendorSequenceCallback callback) {
    VendorFcp::ExecuteSequence(fcpTransport_, commands, isStatus, std::move(callback));
}

void ApogeeDuetProtocol::GetKnobState(ResultCallback<KnobState> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildKnobStateQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess || responses.empty()) {
                Common::InvokeSharedCallback(callbackState,
                                             status != kIOReturnSuccess ? status : kIOReturnError,
                                             KnobState{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseKnobState(responses[0]));
        });
}

void ApogeeDuetProtocol::SetKnobState(const KnobState& state, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {ParamsSerdes::BuildKnobStateControl(state)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetOutputParams(ResultCallback<OutputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildOutputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, OutputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseOutputParams(responses));
        });
}

void ApogeeDuetProtocol::SetOutputParams(const OutputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildOutputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildInputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, InputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseInputParams(responses));
        });
}

void ApogeeDuetProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildInputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetMixerParams(ResultCallback<MixerParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildMixerParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, MixerParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseMixerParams(responses));
        });
}

void ApogeeDuetProtocol::SetMixerParams(const MixerParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildMixerParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetDisplayParams(ResultCallback<DisplayParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildDisplayParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, DisplayParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseDisplayParams(responses));
        });
}

void ApogeeDuetProtocol::SetDisplayParams(const DisplayParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildDisplayParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::ClearDisplay(VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {VendorCommand::Make(VendorCommand::Code::DisplayClear)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputMeter(ResultCallback<InputMeterState> callback) {
    MeterRegisters::ReadInput(busOps_, route_, MakeRouteValidator(), std::move(callback));
}

void ApogeeDuetProtocol::GetMixerMeter(ResultCallback<MixerMeterState> callback) {
    MeterRegisters::ReadMixer(busOps_, route_, MakeRouteValidator(), std::move(callback));
}

// The Oxford ASIC registers are chip-common, not Duet-specific (FW-137), so
// the register map and decode live in Oxford/OxfordCsr. What stays here is the
// Duet's own route-liveness policy, expressed as the validator that layer takes.
Oxford::RouteValidator ApogeeDuetProtocol::MakeRouteValidator() const {
    return [this, route = route_] {
        return cmpClient_ != nullptr && cmpClient_->IsRouteCurrent(route);
    };
}

void ApogeeDuetProtocol::GetFirmwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadFirmwareId(busOps_, route_, MakeRouteValidator(), std::move(callback));
}

void ApogeeDuetProtocol::GetHardwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadHardwareId(busOps_, route_, MakeRouteValidator(), std::move(callback));
}

} // namespace ASFW::Audio::Oxford::Apogee
