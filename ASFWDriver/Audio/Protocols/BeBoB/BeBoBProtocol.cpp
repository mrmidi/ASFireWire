// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBProtocol.cpp — Abstract BridgeCo BeBoB protocol base implementation.
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "BeBoBProtocol.hpp"

#include "../../../Bus/IRM/IRMClient.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../Protocols/AVC/AVCCommand.hpp"
#include "../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"
#include "../../../Protocols/AVC/AudioFunctionBlockCommand.hpp"

#include <DriverKit/IOLib.h>

#include <memory>

namespace ASFW::Audio::BeBoB {
namespace {

using SignalFormatCommand = Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand;
using SignalSampleRate = Protocols::AVC::StreamFormats::SampleRate;

[[nodiscard]] IOReturn MapAVCResultToIOReturn(Protocols::AVC::AVCResult result) noexcept {
    using Protocols::AVC::AVCResult;
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

[[nodiscard]] bool MatchesConnectedPCR(uint32_t value, uint8_t expectedChannel) noexcept {
    return CMP::PCRBits::IsOnline(value) && CMP::PCRBits::GetP2P(value) == 1U &&
           CMP::PCRBits::GetChannel(value) == expectedChannel;
}

[[nodiscard]] SampleRate RateToSignalRate(uint32_t hz) noexcept {
    switch (hz) {
        case 32000U: return SignalSampleRate::k32000Hz;
        case 44100U: return SignalSampleRate::k44100Hz;
        case 48000U: return SignalSampleRate::k48000Hz;
        case 88200U: return SignalSampleRate::k88200Hz;
        case 96000U: return SignalSampleRate::k96000Hz;
        case 176400U: return SignalSampleRate::k176400Hz;
        case 192000U: return SignalSampleRate::k192000Hz;
        default: return SignalSampleRate::kUnknown;
    }
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
                             uint16_t nodeId,
                             IRM::IRMClient* irmClient,
                             CMP::CMPClient* cmpClient,
                             uint64_t deviceGuid,
                             Scheduling::ITimerScheduler* timerScheduler) noexcept
    : busInfo_(busInfo), nodeId_(nodeId), irmClient_(irmClient), cmpClient_(cmpClient),
      deviceGuid_(deviceGuid), timerScheduler_(timerScheduler) {
    (void)busOps;
}

IOReturn BeBoBProtocol::Initialize() {
    return (irmClient_ && cmpClient_ && deviceGuid_ != 0 && timerScheduler_ != nullptr)
               ? kIOReturnSuccess
               : kIOReturnNotReady;
}

IOReturn BeBoBProtocol::Shutdown() {
    CancelClockApply();
    const IOReturn status = StopDuplex();
    if (cmpClient_ && deviceGuid_ != 0) {
        cmpClient_->InvalidateDevice(deviceGuid_);
    }
    preparedGeneration_ = FW::Generation{0};
    return status;
}

void BeBoBProtocol::UpdateRuntimeContext(uint16_t nodeId,
                                         Protocols::AVC::FCPTransport* transport) {
    if ((nodeId_ != nodeId || fcpTransport_ != transport) && cmpClient_ && deviceGuid_ != 0) {
        CancelClockApply();
        cmpClient_->InvalidateDevice(deviceGuid_);
        inputConnected_ = false;
        outputConnected_ = false;
        preparedGeneration_ = FW::Generation{0};
    }
    nodeId_ = nodeId;
    fcpTransport_ = transport;
}

CMP::CMPDevice BeBoBProtocol::CurrentCMPDevice() const noexcept {
    return CMP::CMPDevice{
        .guid = deviceGuid_,
        .nodeId = FW::NodeId{nodeId_ <= 0x3fU ? static_cast<uint8_t>(nodeId_)
                                              : static_cast<uint8_t>(0xffU)},
        .generation = busInfo_.GetGeneration(),
    };
}

IOReturn BeBoBProtocol::ResetEpochIfNeeded() noexcept {
    const FW::Generation current = busInfo_.GetGeneration();
    if (preparedGeneration_ == current) return kIOReturnSuccess;
    CancelClockApply();
    if (cmpClient_ && deviceGuid_ != 0) cmpClient_->InvalidateDevice(deviceGuid_);
    inputConnected_ = false;
    outputConnected_ = false;
    preparedGeneration_ = current;
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
                                                                        ClockApplyResult clock) mutable {
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
    if (!fcpTransport_) {
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
            epoch->completion = std::move(callback);
            epoch->appliedClock = desiredClock;
            activeClockApply_ = epoch.get();

            const uint64_t settleNs = static_cast<uint64_t>(kFormatSettleMs) * 1000ULL * 1000ULL;
            epoch->settleTimer = timerScheduler_->ScheduleAfter(
                settleNs, [this, epoch]() {
                    // Guard: epoch may have been cancelled by Shutdown/bus reset.
                    if (activeClockApply_ != epoch.get()) return;
                    appliedClock_ = epoch->appliedClock;
                    BBPTRACE("ApplyClockConfig settle complete: rate=%uHz",
                             epoch->appliedClock.sampleRateHz);
                    FinishClockApply(epoch.get(), kIOReturnSuccess);
                });
        });
    });
}

void BeBoBProtocol::ProgramSignalFormat(const AudioClockConfig& desiredClock,
                                        std::function<void(IOReturn)> completion) {
    const uint8_t outPlug = StreamPlug(false);
    const auto rate = RateToSignalRate(desiredClock.sampleRateHz);
    if (rate == SignalSampleRate::kUnknown) {
        completion(kIOReturnUnsupported);
        return;
    }
    auto output = std::make_shared<SignalFormatCommand>(*fcpTransport_, outPlug, false, rate);
    output->Submit([this, desiredClock, completion = std::move(output), completion](
                       Protocols::AVC::AVCResult outputResult,
                       const SignalFormatCommand::SignalFormat& outputFormat) mutable {
        const IOReturn outputStatus = MapAVCResultToIOReturn(outputResult);
        if (outputStatus != kIOReturnSuccess) {
            completion(outputStatus);
            return;
        }

        const uint8_t inPlug = StreamPlug(true);
        auto input = std::make_shared<SignalFormatCommand>(*fcpTransport_, inPlug, true, rate);
        input->Submit([completion = std::move(completion), input](
                           Protocols::AVC::AVCResult inputResult,
                           const SignalFormatCommand::SignalFormat& inputFormat) mutable {
            completion(MapAVCResultToIOReturn(inputResult));
        });
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

void BeBoBProtocol::SetSelectorBlock(uint8_t fbId, uint8_t value, MixerCompletion completion) {
    if (!fcpTransport_) {
        completion(kIOReturnNotReady);
        return;
    }
    auto cmd = std::make_shared<Protocols::AVC::AudioFunctionBlockCommand>(
        *this, 0x08,
        Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
        Protocols::AVC::AudioFunctionBlockCommand::BlockType::kSelector,
        fbId,
        Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kSelectorControl,
        std::vector<uint8_t>{value});
    cmd->Submit([completion = std::move(completion)](Protocols::AVC::AVCResult result, const std::vector<uint8_t>&) mutable {
        completion(MapAVCResultToIOReturn(result));
    });
}

void BeBoBProtocol::SetFeatureMute(uint8_t fbId, uint8_t channel, bool unmute,
                                   MixerCompletion completion) {
    if (!fcpTransport_) {
        completion(kIOReturnNotReady);
        return;
    }
    // AV/C Feature block mute encoding: {channel, 0x01, 0x60=unmute / 0x00=mute}
    auto cmd = std::make_shared<Protocols::AVC::AudioFunctionBlockCommand>(
        *this, 0x08,
        Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
        Protocols::AVC::AudioFunctionBlockCommand::BlockType::kFeature,
        fbId,
        Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kMute,
        std::vector<uint8_t>{channel, 0x01, unmute ? 0x60U : 0x00U});
    cmd->Submit([completion = std::move(completion)](Protocols::AVC::AVCResult result, const std::vector<uint8_t>&) mutable {
        completion(MapAVCResultToIOReturn(result));
    });
}

void BeBoBProtocol::SetFeatureVolume(uint8_t fbId, uint8_t channel, uint16_t value,
                                     MixerCompletion completion) {
    if (!fcpTransport_) {
        completion(kIOReturnNotReady);
        return;
    }
    const uint8_t hi = static_cast<uint8_t>((value >> 8) & 0xFFU);
    const uint8_t lo = static_cast<uint8_t>(value & 0xFFU);
    auto cmd = std::make_shared<Protocols::AVC::AudioFunctionBlockCommand>(
        *this, 0x08,
        Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
        Protocols::AVC::AudioFunctionBlockCommand::BlockType::kFeature,
        fbId,
        Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kVolume,
        std::vector<uint8_t>{channel, 0x02, hi, lo});
    cmd->Submit([completion = std::move(completion)](Protocols::AVC::AVCResult result, const std::vector<uint8_t>&) mutable {
        completion(MapAVCResultToIOReturn(result));
    });
}

void BeBoBProtocol::FinishClockApply(ClockApplyEpoch* epoch, IOReturn status) {
    if (!epoch->completed.exchange(true)) {
        activeClockApply_ = nullptr;
        ClockApplyCallback cb = std::move(epoch->completion);
        cb(status, ClockApplyResult{.generation = busInfo_.GetGeneration(),
                                     .appliedClock = epoch->appliedClock,
                                     .runtimeCaps = DeviceCaps()});
    }
}

void BeBoBProtocol::CancelClockApply() {
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
                         inputPCR, outputPCR, deviceGuid_);
                callback(kIOReturnSuccess,
                         DuplexConfirmResult{.generation = busInfo_.GetGeneration(),
                                             .channels = channels,
                                             .appliedClock = appliedClock_,
                                             .runtimeCaps = DeviceCaps()});
            });
    });
}

void BeBoBProtocol::DisconnectPlayback(VoidCallback callback) {
    if (!cmpClient_ || !inputConnected_) {
        inputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }
    inputConnected_ = false;
    cmpClient_->DisconnectIPCR(CurrentCMPDevice(), StreamPlug(true),
                               [callback = std::move(callback)](CMP::CMPStatus status) mutable {
        callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
    });
}

void BeBoBProtocol::DisconnectCapture(VoidCallback callback) {
    if (!cmpClient_ || !outputConnected_) {
        outputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }
    outputConnected_ = false;
    cmpClient_->DisconnectOPCR(CurrentCMPDevice(), StreamPlug(false),
                               [callback = std::move(callback)](CMP::CMPStatus status) mutable {
        callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
    });
}

IOReturn BeBoBProtocol::StopDuplex() {
    DisconnectPlayback([](IOReturn) {});
    DisconnectCapture([](IOReturn) {});
    return kIOReturnSuccess;
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
    if (!cmpClient_) {
        callback(kIOReturnNotReady);
        return;
    }
    cmpClient_->BreakBothConnections(CurrentCMPDevice(), StreamPlug(true),
                                     [callback = std::move(callback)](CMP::CMPStatus status) {
        callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
    });
}

void BeBoBProtocol::SubmitCommand(const Protocols::AVC::AVCCdb& cdb,
                                  Protocols::AVC::AVCCompletion completion) {
    if (!fcpTransport_) {
        completion(Protocols::AVC::AVCResult::kTransportError, cdb);
        return;
    }
    auto cmd = std::make_shared<Protocols::AVC::AVCCommand>(*fcpTransport_, cdb);
    cmd->Submit([cmd, completion = std::move(completion)](
                    Protocols::AVC::AVCResult result, const Protocols::AVC::AVCCdb& responseCdb) {
        completion(result, responseCdb);
    });
}

} // namespace ASFW::Audio::BeBoB
