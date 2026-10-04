// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBProtocol.cpp — Abstract BridgeCo BeBoB protocol base implementation.
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "BeBoBProtocol.hpp"

#include "../Duplex/FamilyStageWait.hpp"

#include "../../../Bus/IRM/IRMClient.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../Protocols/AVC/Commands/FunctionBlockCommand.hpp"
#include "../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include "../../../Protocols/AVC/Core/RateCodes.hpp"

#include <DriverKit/IOLib.h>

#include <memory>

namespace ASFW::Audio::BeBoB {
namespace {


[[nodiscard]] bool MatchesConnectedPCR(uint32_t value, uint8_t expectedChannel) noexcept {
    return CMP::PCRBits::IsOnline(value) && CMP::PCRBits::GetP2P(value) == 1U &&
           CMP::PCRBits::GetChannel(value) == expectedChannel;
}

#ifndef ASFW_HOST_TEST
// !!! DIAGNOSTIC: BeBoB CMP path tracing. Remove after FW-94 resolution.
#define BBPTRACE(fmt, ...) \
    ASFW_LOG(Audio, "!!! [BeBoB] " fmt, ##__VA_ARGS__)
#else
#define BBPTRACE(fmt, ...) ((void)0)
#endif

} // namespace

BeBoBProtocol::BeBoBProtocol(Protocols::Ports::FireWireBusOps& busOps,
                             Protocols::Ports::FireWireBusInfo& busInfo,
                             Discovery::DeviceRouteToken route,
                             IRM::IRMClient* irmClient,
                             CMP::CMPClient* cmpClient,
                             Scheduling::ITimerScheduler* timerScheduler) noexcept
    : busInfo_(busInfo), route_(route), irmClient_(irmClient), cmpClient_(cmpClient),
      timerScheduler_(timerScheduler) {
    (void)busOps;
}

IOReturn BeBoBProtocol::Initialize() {
    return (irmClient_ && cmpClient_ && route_ && timerScheduler_ != nullptr)
               ? kIOReturnSuccess
               : kIOReturnNotReady;
}

IOReturn BeBoBProtocol::Shutdown() {
    CancelClockApply();
    CancelSignalFormatInterlock();
    const IOReturn status = StopDuplex();
    if (cmpClient_ && route_) {
        cmpClient_->InvalidateRoute(route_);
    }
    preparedRouteEpoch_ = 0;
    return status;
}

void BeBoBProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                         std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    if (route_ != route || avcUnit_ != avcUnit) {
        CancelClockApply();
        if (cmpClient_ && route_) {
            cmpClient_->InvalidateRoute(route_);
        }
        inputConnected_ = false;
        outputConnected_ = false;
        preparedRouteEpoch_ = 0;
    }
    route_ = route;
    avcUnit_ = std::move(avcUnit);
}

CMP::CMPDevice BeBoBProtocol::CurrentCMPDevice() const noexcept {
    return CMP::CMPDevice{
        .route = route_,
    };
}

IOReturn BeBoBProtocol::ResetEpochIfNeeded() noexcept {
    if (preparedRouteEpoch_ == route_.routeEpoch) return kIOReturnSuccess;
    CancelClockApply();
    if (cmpClient_ && route_) cmpClient_->InvalidateRoute(route_);
    inputConnected_ = false;
    outputConnected_ = false;
    preparedRouteEpoch_ = route_.routeEpoch;
    return kIOReturnSuccess;
}

void BeBoBProtocol::PrepareDuplex(const AudioDuplexChannels& channels,
                                  const AudioClockConfig& desiredClock,
                                  PrepareCallback callback) {
    if (!cmpClient_ || !irmClient_ || !CurrentCMPDevice().IsValid()) {
        callback(kIOReturnNotReady, {});
        return;
    }
    if (const IOReturn reset = ResetEpochIfNeeded(); reset != kIOReturnSuccess) {
        callback(reset, {});
        return;
    }
    duplexChannels_ = channels;
    ApplyClockConfig(desiredClock,
                     [this, channels, callback = std::move(callback)](IOReturn status,
                                                                        DuplexClockApplyResult clock) mutable {
        callback(status, DuplexPrepareResult{.generation = clock.generation,
                                             .channels = channels,
                                             .appliedClock = clock.appliedClock,
                                             .runtimeCaps = clock.runtimeCaps});
    });
}

void BeBoBProtocol::SetAssignedChannels(const AudioDuplexChannels& channels) noexcept {
    duplexChannels_ = channels;
}

void BeBoBProtocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                     ClockApplyCallback callback) {
    if (!IsRateSupported(desiredClock.sampleRateHz)) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    if (!avcUnit_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    // Linux's BeBoB start sequence explicitly writes OUTPUT plug first, then INPUT
    // plug, using AM824 at the negotiated rate. Cross-validated with
    // linux-sound-firewire-stack/firewire/bebob/bebob_stream.c:96-115.
    ProgramSignalFormat(desiredClock, [this, desiredClock, callback = std::move(callback)](IOReturn fmtStatus) mutable {
        if (fmtStatus != kIOReturnSuccess) {
            callback(fmtStatus, {});
            return;
        }

        // Async mixer configuration (device-specific, may be no-op).
        ConfigureMixer(MixerFailurePolicy::kBestEffort,
                       [this, desiredClock, callback = std::move(callback)](IOReturn mixerStatus) mutable {
            if (mixerStatus != kIOReturnSuccess) {
                callback(mixerStatus, {});
                return;
            }

            // Settle before CMP — async via ITimerScheduler, never IOSleep.
            // Cross-validated with Linux bebob_stream.c:96-115 (300 ms settle).
            auto epoch = std::make_shared<ClockApplyEpoch>();
            epoch->generation = busInfo_.GetGeneration();
            epoch->routeAtStart = route_;
            epoch->completion = std::move(callback);
            epoch->appliedClock = desiredClock;
            activeClockApply_ = epoch.get();

            const uint64_t settleNs = static_cast<uint64_t>(kFormatSettleMs) * 1000ULL * 1000ULL;
            epoch->settleTimer = timerScheduler_->ScheduleAfter(
                settleNs, [this, epoch]() {
                    // Guard: epoch may have been cancelled by Shutdown/bus reset.
                    if (activeClockApply_ != epoch.get()) return;
                    appliedClock_ = epoch->appliedClock;
                    if (avcUnit_) avcUnit_->RememberConfirmedDuplexRate(epoch->routeAtStart, appliedClock_.sampleRateHz);
                    BBPTRACE("ApplyClockConfig settle complete: rate=%uHz",
                             epoch->appliedClock.sampleRateHz);
                    FinishClockApply(epoch.get(), kIOReturnSuccess);
                });
        });
    });
}

void BeBoBProtocol::ProgramSignalFormat(const AudioClockConfig& desiredClock,
                                        std::function<void(IOReturn)> completion) {
    if (!avcUnit_) {
        completion(kIOReturnNotReady);
        return;
    }
    const auto sfc = AVC::CipSfcFromHz(desiredClock.sampleRateHz);
    if (!sfc) {
        completion(kIOReturnUnsupported);
        return;
    }
    const uint8_t outPlug = StreamPlug(false);
    avcUnit_->Control(
        AVC::Cmd::PlugSignalFormatCommand{
            .operands = AVC::Cmd::PlugSignalFormatOperands{
                .direction = AVC::Cmd::PlugSignalDirection::kOutput,
                .plugId = outPlug,
                .format = AVC::Cmd::Am824SignalFormat(outPlug, *sfc),
            },
        },
        [this, sfc, completion = std::move(completion)](AVC::Expected<AVC::Cmd::PlugSignalFormat> outputResult) mutable {
            if (!outputResult) {
                completion(kIOReturnError);
                return;
            }

            if (!avcUnit_) {
                completion(kIOReturnNotReady);
                return;
            }
            const uint8_t inPlug = StreamPlug(true);
            auto finalCompletion = std::make_shared<std::function<void(IOReturn)>>(std::move(completion));
            auto submitInput = [this, inPlug, sfc, finalCompletion]() mutable {
                if (!avcUnit_) {
                    (*finalCompletion)(kIOReturnNotReady);
                    return;
                }
                avcUnit_->Control(
                    AVC::Cmd::PlugSignalFormatCommand{
                        .operands = AVC::Cmd::PlugSignalFormatOperands{
                            .direction = AVC::Cmd::PlugSignalDirection::kInput,
                            .plugId = inPlug,
                            .format = AVC::Cmd::Am824SignalFormat(inPlug, *sfc),
                        },
                    },
                    [finalCompletion](AVC::Expected<AVC::Cmd::PlugSignalFormat> inputResult) mutable {
                        (*finalCompletion)(inputResult ? kIOReturnSuccess : kIOReturnError);
                    });
            };
            const uint32_t interlockMs = SignalFormatInterlockMs();
            if (interlockMs == 0) {
                submitInput();
                return;
            }
            if (!timerScheduler_) {
                (*finalCompletion)(kIOReturnNotReady);
                return;
            }
            signalFormatInterlockCompletion_ = finalCompletion;
            signalFormatInterlockTimer_ = timerScheduler_->ScheduleAfter(
                static_cast<uint64_t>(interlockMs) * 1000ULL * 1000ULL,
                [this, finalCompletion, submitInput = std::move(submitInput)]() mutable {
                    signalFormatInterlockTimer_ = Scheduling::kInvalidTimerToken;
                    if (signalFormatInterlockCompletion_ == finalCompletion) {
                        signalFormatInterlockCompletion_.reset();
                    }
                    submitInput();
                });
            if (signalFormatInterlockTimer_ == Scheduling::kInvalidTimerToken) {
                signalFormatInterlockCompletion_.reset();
                (*finalCompletion)(kIOReturnNoResources);
            }
        });
}

void BeBoBProtocol::ConfigureMixer(MixerFailurePolicy /*policy*/,
                                   MixerCompletion completion) {
    // Default: no mixer programming (matches Linux/FFADO behavior).
    completion(kIOReturnSuccess);
}

bool BeBoBProtocol::IsRateSupported(uint32_t hz) const {
    for (const auto r : SupportedRates()) {
        if (r == hz) return true;
    }
    return false;
}

void BeBoBProtocol::RunMixerMap(const MixerMap& map, MixerFailurePolicy policy, MixerCompletion completion) {
    // One labelled step per CONTROL, so a failure names the block it hit.
    struct Step {
        const char* kind;
        uint8_t fbId;
        uint8_t channel;
        int32_t value;
        std::function<void(MixerCompletion)> submit;
    };
    struct State {
        std::vector<Step> steps;
        size_t next{0};
        uint32_t failed{0};
        MixerFailurePolicy policy{MixerFailurePolicy::kRequired};
        MixerCompletion completion;
        const char* device{nullptr};
    };
    auto state = std::make_shared<State>();
    state->policy = policy;
    state->completion = std::move(completion);
    state->device = DeviceName();
    for (const auto& sel : map.selectors)
        state->steps.push_back({"selector", sel.fbId, 0, sel.value,
            [this, sel](MixerCompletion cb) { SetSelectorBlock(sel.fbId, sel.value, std::move(cb)); }});
    for (const auto& mute : map.mutes) {
        if (avcUnit_ && avcUnit_->HasUserFeaturePreference(0, mute.fbId)) continue;
        state->steps.push_back({mute.unmute ? "unmute" : "mute", mute.fbId, mute.channel, 0,
            [this, mute](MixerCompletion cb) { SetFeatureMute(mute.fbId, mute.channel, mute.unmute, std::move(cb)); }});
    }
    for (const auto& vol : map.volumes) {
        if (avcUnit_ && avcUnit_->HasUserFeaturePreference(0, vol.fbId)) continue;
        // Volume is signed 1/256 dB; the log shows whole dB.
        state->steps.push_back({"volume", vol.fbId, vol.channel, static_cast<int16_t>(vol.value) / 256,
            [this, vol](MixerCompletion cb) { SetFeatureVolume(vol.fbId, vol.channel, vol.value, std::move(cb)); }});
    }
    ASFW_LOG(Audio, "[BeBoB] %{public}s startup mixer: %zu selectors, %zu mutes, %zu volumes (%{public}s)",
             state->device, map.selectors.size(), map.mutes.size(), map.volumes.size(),
             policy == MixerFailurePolicy::kRequired ? "required" : "best effort");
    // Each completion schedules the next step; the state lives only in the
    // pending completion, so it is released when the map finishes.
    struct Runner {
        static void Finish(const std::shared_ptr<State>& state, IOReturn status) {
            const auto total = state->steps.size();
            if (status == kIOReturnSuccess && state->failed == 0) {
                ASFW_LOG(Audio, "[BeBoB] %{public}s startup mixer applied %zu/%zu",
                         state->device, total, total);
            } else {
                ASFW_LOG_WARNING(Audio,
                                 "[BeBoB] %{public}s startup mixer applied %zu/%zu, %u failed%{public}s",
                                 state->device, state->next - state->failed, total, state->failed,
                                 status == kIOReturnSuccess ? "" : ", stopped at the first failure");
            }
            state->completion(status);
        }
        static void Next(const std::shared_ptr<State>& state, IOReturn last) {
            if (last != kIOReturnSuccess) {
                const auto& step = state->steps[state->next - 1];
                ++state->failed;
                ASFW_LOG_WARNING(Audio, "[BeBoB] %{public}s mixer %{public}s fb=0x%02x ch=%u value=%d failed: 0x%x",
                                 state->device, step.kind, step.fbId, step.channel, step.value, last);
                if (state->policy == MixerFailurePolicy::kRequired) {
                    Finish(state, last);
                    return;
                }
            }
            if (state->next >= state->steps.size()) {
                Finish(state, kIOReturnSuccess);
                return;
            }
            auto& step = state->steps[state->next++];
            step.submit([state](IOReturn status) { Next(state, status); });
        }
    };
    Runner::Next(state, kIOReturnSuccess);
}

void BeBoBProtocol::SetSelectorBlock(uint8_t fbId, uint8_t value, MixerCompletion completion) {
    if (!avcUnit_) {
        completion(kIOReturnNotReady);
        return;
    }
    avcUnit_->Control(
        AVC::Cmd::SelectorCommand{
            .address = AVC::kAudioSubunit0,
            .operands = AVC::Cmd::SelectorOperands{
                .functionBlockId = fbId,
                .inputPlug = value,
            },
        },
        [completion = std::move(completion)](AVC::Expected<AVC::Cmd::SelectorValue> res) mutable {
            completion(res ? kIOReturnSuccess : AVC::ToIOReturn(res.error()));
        });
}

void BeBoBProtocol::SetFeatureMute(uint8_t fbId, uint8_t channel, bool unmute,
                                   MixerCompletion completion) {
    if (!avcUnit_) {
        completion(kIOReturnNotReady);
        return;
    }
    avcUnit_->Control(
        AVC::Cmd::FeatureCommand{
            .address = AVC::kAudioSubunit0,
            .operands = AVC::Cmd::FeatureOperands::Mute(fbId, channel, !unmute),
        },
        [completion = std::move(completion)](AVC::Expected<AVC::Cmd::FeatureReply> res) mutable {
            completion(res ? kIOReturnSuccess : AVC::ToIOReturn(res.error()));
        });
}

void BeBoBProtocol::SetFeatureVolume(uint8_t fbId, uint8_t channel, uint16_t value,
                                     MixerCompletion completion) {
    if (!avcUnit_) {
        completion(kIOReturnNotReady);
        return;
    }
    avcUnit_->Control(
        AVC::Cmd::FeatureCommand{
            .address = AVC::kAudioSubunit0,
            .operands = AVC::Cmd::FeatureOperands::Volume(
                fbId, channel, AVC::AvcVolume::FromRaw(static_cast<int16_t>(value))),
        },
        [completion = std::move(completion)](AVC::Expected<AVC::Cmd::FeatureReply> res) mutable {
            completion(res ? kIOReturnSuccess : AVC::ToIOReturn(res.error()));
        });
}

void BeBoBProtocol::FinishClockApply(ClockApplyEpoch* epoch, IOReturn status) {
    if (!epoch->completed.exchange(true)) {
        activeClockApply_ = nullptr;
        ClockApplyCallback cb = std::move(epoch->completion);
        cb(status, DuplexClockApplyResult{.generation = busInfo_.GetGeneration(),
                                     .appliedClock = epoch->appliedClock,
                                     .runtimeCaps = DeviceCaps()});
    }
}

void BeBoBProtocol::CancelClockApply() {
    CancelSignalFormatInterlock();
    if (auto* epoch = activeClockApply_) {
        activeClockApply_ = nullptr;
        timerScheduler_->Cancel(epoch->settleTimer);
        // Mark completed so the timer callback (if already dispatched) is a no-op.
        if (!epoch->completed.exchange(true)) {
            ClockApplyCallback cb = std::move(epoch->completion);
            cb(kIOReturnAborted, {});
        }
    }
}

void BeBoBProtocol::CancelSignalFormatInterlock() noexcept {
    if (timerScheduler_ &&
        signalFormatInterlockTimer_ != Scheduling::kInvalidTimerToken) {
        timerScheduler_->Cancel(signalFormatInterlockTimer_);
        signalFormatInterlockTimer_ = Scheduling::kInvalidTimerToken;
    }
    if (signalFormatInterlockCompletion_) {
        auto completion = std::move(signalFormatInterlockCompletion_);
        (*completion)(kIOReturnAborted);
    }
}

void BeBoBProtocol::ProgramRx(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    BBPTRACE("ProgramRx entry: ch=%u", duplexChannels_.hostToDeviceIsoChannel);
    EnsurePlugFree(CMP::PCRDirection::kInput, StreamPlug(true),
                   [this, callback = std::move(callback)](IOReturn err) mutable {
        if (err != kIOReturnSuccess) {
            BBPTRACE("ProgramRx: EnsurePlugFree failed kr=0x%x", err);
            callback(err, {});
            return;
        }
        BBPTRACE("ProgramRx: submitting ConnectIPCR ch=%u", duplexChannels_.hostToDeviceIsoChannel);
        cmpClient_->ConnectIPCR(CurrentCMPDevice(), StreamPlug(true),
                                duplexChannels_.hostToDeviceIsoChannel,
                                [this, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            const IOReturn kr = status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError;
            inputConnected_ = kr == kIOReturnSuccess;
            BBPTRACE("ProgramRx: ConnectIPCR callback status=%u kr=0x%x inputConnected=%u",
                     status, kr, inputConnected_);
            callback(kr, DuplexStageResult{.generation = busInfo_.GetGeneration(),
                                            .channels = duplexChannels_,
                                            .phase = DuplexRestartPhase::kDeviceRxProgrammed,
                                            .runtimeCaps = DeviceCaps()});
        });
    });
}

void BeBoBProtocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    BBPTRACE("ProgramTx entry: ch=%u", duplexChannels_.deviceToHostIsoChannel);
    EnsurePlugFree(CMP::PCRDirection::kOutput, StreamPlug(false),
                   [this, callback = std::move(callback)](IOReturn err) mutable {
        if (err != kIOReturnSuccess) {
            BBPTRACE("ProgramTx: EnsurePlugFree failed kr=0x%x", err);
            callback(err, {});
            return;
        }
        BBPTRACE("ProgramTx: submitting ConnectOPCR ch=%u", duplexChannels_.deviceToHostIsoChannel);
        cmpClient_->ConnectOPCR(CurrentCMPDevice(), StreamPlug(false),
                                duplexChannels_.deviceToHostIsoChannel,
                                [this, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            const IOReturn kr = status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError;
            outputConnected_ = kr == kIOReturnSuccess;
            BBPTRACE("ProgramTx: ConnectOPCR callback status=%u kr=0x%x outputConnected=%u",
                     status, kr, outputConnected_);
            callback(kr, DuplexStageResult{.generation = busInfo_.GetGeneration(),
                                            .channels = duplexChannels_,
                                            .phase = DuplexRestartPhase::kDeviceTxArmed,
                                            .runtimeCaps = DeviceCaps()});
        });
    });
}

void BeBoBProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    if (!cmpClient_ || !inputConnected_ || !outputConnected_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    const CMP::CMPDevice device = CurrentCMPDevice();
    const AudioDuplexChannels channels = duplexChannels_;
    const uint8_t inPlug = StreamPlug(true);
    const uint8_t outPlug = StreamPlug(false);

    cmpClient_->ReadIPCR(device, inPlug,
                         [this, device, channels, inPlug, outPlug, callback = std::move(callback)](
                             bool inputRead, uint32_t inputPCR) mutable {
        if (!inputRead || !MatchesConnectedPCR(inputPCR, channels.hostToDeviceIsoChannel)) {
            callback(kIOReturnNotResponding, {});
            return;
        }
        if (!cmpClient_) {
            callback(kIOReturnNotReady, {});
            return;
        }
        cmpClient_->ReadOPCR(device, outPlug,
                             [this, channels, inputPCR, callback = std::move(callback)](
                                 bool outputRead, uint32_t outputPCR) mutable {
                if (!outputRead || !MatchesConnectedPCR(outputPCR,
                                                         channels.deviceToHostIsoChannel)) {
                    callback(kIOReturnNotResponding, {});
                    return;
                }
                ASFW_LOG(Audio, "[BeBoB] CMP verified iPCR=0x%08x oPCR=0x%08x GUID=0x%016llx",
                         inputPCR, outputPCR, route_.guid);
                callback(kIOReturnSuccess,
                         DuplexConfirmResult{.generation = busInfo_.GetGeneration(),
                                             .channels = channels,
                                             .appliedClock = appliedClock_,
                                             .runtimeCaps = DeviceCaps()});
            });
    });
}

void BeBoBProtocol::ReadDuplexHealth(HealthCallback callback) {
    ReadClockHealth(std::move(callback));
}

void BeBoBProtocol::ReadClockHealth(HealthCallback callback) {
    const auto caps = DeviceCaps();
    callback(kIOReturnSuccess,
             DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                .appliedClock = appliedClock_,
                                .runtimeCaps = caps,
                                .sourceLocked = inputConnected_ && outputConnected_,
                                .clockReferenceHealthy = true,
                                .nominalRateHz = caps.sampleRateHz});
}

void BeBoBProtocol::DisconnectPlayback(VoidCallback callback) {
    if (!cmpClient_) { callback(kIOReturnNotReady); return; }
    // CMP owns the lease, including an uncertain connect. The boolean is only
    // a streaming-health observation, not authority to skip remote cleanup.
    cmpClient_->DisconnectIPCR(CurrentCMPDevice(), StreamPlug(true),
        [this, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            if (status == CMP::CMPStatus::Success) inputConnected_ = false;
            callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
        });
}

void BeBoBProtocol::DisconnectCapture(VoidCallback callback) {
    if (!cmpClient_) { callback(kIOReturnNotReady); return; }
    cmpClient_->DisconnectOPCR(CurrentCMPDevice(), StreamPlug(false),
        [this, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            if (status == CMP::CMPStatus::Success) outputConnected_ = false;
            callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
        });
}

IOReturn BeBoBProtocol::StopDuplex() {
    // Stop must report completion, not release IRM resources while BREAK is
    // still pending. Reuse the existing asynchronous stage wait.
    return BreakConnections();
}

void BeBoBProtocol::EnsurePlugFree(CMP::PCRDirection dir, uint8_t plug,
                                   std::function<void(IOReturn)> cb) {
    if (!cmpClient_) {
        cb(kIOReturnNotReady);
        return;
    }
    const auto device = CurrentCMPDevice();
    cmpClient_->CheckPlugUsed(device, dir, plug,
                              [this, device, plug, dir, cb = std::move(cb)](bool success, bool used) mutable {
        if (!success) {
            cb(kIOReturnNotResponding);
            return;
        }
        if (used) {
            ASFW_LOG(Audio, "[BeBoB] Plug %u (direction %s) in use, breaking connections",
                     plug, dir == CMP::PCRDirection::kInput ? "Input" : "Output");
            cmpClient_->BreakBothConnections(device, plug, [cb](CMP::CMPStatus status) {
                cb(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
            });
        } else {
            cb(kIOReturnSuccess);
        }
    });
}

void BeBoBProtocol::BreakBothConnections(VoidCallback callback) {
    DisconnectPlayback([this, callback = std::move(callback)](IOReturn playback) mutable {
        DisconnectCapture([playback, callback = std::move(callback)](IOReturn capture) mutable {
            callback(playback != kIOReturnSuccess ? playback : capture);
        });
    });
}

// ---------------------------------------------------------------------------
// FamilyDriver
// ---------------------------------------------------------------------------

void BeBoBProtocol::SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept {
    teardownCancel_ = cancel;
}

IOReturn BeBoBProtocol::LoadGeometry() {
    // Nothing to read at start: BeBoB stream geometry is fixed per device
    // (DeviceCaps) or was resolved from the plugs at discovery.
    return kIOReturnSuccess;
}

std::optional<AudioStreamRuntimeCaps> BeBoBProtocol::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (!GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> BeBoBProtocol::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    return AwaitStage<DuplexPrepareResult>(
        [&](auto callback) { PrepareDuplex(channels, clock, std::move(callback)); },
        teardownCancel_);
}

std::expected<AudioDuplexChannels, IOReturn> BeBoBProtocol::AssignChannels(const AudioDuplexChannels& channels) {
    SetAssignedChannels(channels);
    return channels;
}

std::expected<DuplexHealthResult, IOReturn> BeBoBProtocol::ReadHealth(uint32_t timeoutMs) {
    return AwaitStage<DuplexHealthResult>(
        [&](auto callback) { ReadDuplexHealth(std::move(callback)); }, teardownCancel_, timeoutMs);
}

std::expected<DuplexStageResult, IOReturn> BeBoBProtocol::ArmDeviceRx() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramRx(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexStageResult, IOReturn> BeBoBProtocol::ArmDeviceTxAndEnable() {
    return AwaitStage<DuplexStageResult>(
        [&](auto callback) { ProgramTxAndEnableDuplex(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexConfirmResult, IOReturn> BeBoBProtocol::Confirm() {
    // ConfirmDuplexStart is virtual: M-Audio special firmware confirms differently.
    return AwaitStage<DuplexConfirmResult>(
        [&](auto callback) { ConfirmDuplexStart(std::move(callback)); }, teardownCancel_);
}

std::expected<DuplexClockApplyResult, IOReturn> BeBoBProtocol::ApplyClockIdle(
    const AudioClockConfig& clock) {
    return AwaitStage<DuplexClockApplyResult>(
        [&](auto callback) { ApplyClockConfig(clock, std::move(callback)); }, teardownCancel_);
}

IOReturn BeBoBProtocol::DisconnectPlayback() {
    return AwaitStageStatus([&](auto callback) { DisconnectPlayback(std::move(callback)); },
                            teardownCancel_);
}

IOReturn BeBoBProtocol::DisconnectCapture() {
    return AwaitStageStatus([&](auto callback) { DisconnectCapture(std::move(callback)); },
                            teardownCancel_);
}

IOReturn BeBoBProtocol::BreakConnections() {
    return AwaitStageStatus([&](auto callback) { BreakBothConnections(std::move(callback)); },
                            teardownCancel_);
}

IOReturn BeBoBProtocol::Stop() {
    return StopDuplex();
}

} // namespace ASFW::Audio::BeBoB
