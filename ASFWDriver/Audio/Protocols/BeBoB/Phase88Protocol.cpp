// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c:400-465, 500-523, 593-674;
// no reference source is copied.

#include "Phase88Protocol.hpp"

#include "../../../Bus/IRM/IRMClient.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../Protocols/AVC/FCPTransport.hpp"
#include "../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <memory>

namespace ASFW::Audio::BeBoB {
namespace {

constexpr uint32_t kPhase88PcmChannels = 10;
constexpr uint32_t kPhase88MidiDataBlocks = 1;
constexpr uint32_t kPhase88SampleRateHz = 48000;
constexpr uint32_t kCMPTimeoutMs = 250;
constexpr uint32_t kCMPPollMs = 5;
constexpr uint32_t kFormatSettleMs = 300;

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

[[nodiscard]] bool Matches48kAm824(const SignalFormatCommand::SignalFormat& format) noexcept {
    return format.plugID == 0 && format.format == 0x90U &&
           format.frequency == 0x02U;
}

[[nodiscard]] bool MatchesConnectedPCR(uint32_t value, uint8_t expectedChannel) noexcept {
    return CMP::PCRBits::IsOnline(value) && CMP::PCRBits::GetP2P(value) == 1U &&
           CMP::PCRBits::GetChannel(value) == expectedChannel;
}

struct CMPWaitState {
    std::atomic<bool> done{false};
    std::atomic<CMP::CMPStatus> status{CMP::CMPStatus::Failed};
};

[[nodiscard]] IOReturn WaitForCMP(const std::shared_ptr<CMPWaitState>& state) noexcept {
    for (uint32_t elapsed = 0; elapsed < kCMPTimeoutMs; elapsed += kCMPPollMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return state->status.load(std::memory_order_acquire) == CMP::CMPStatus::Success
                       ? kIOReturnSuccess
                       : kIOReturnError;
        }
        IOSleep(kCMPPollMs);
    }
    return kIOReturnTimeout;
}

[[nodiscard]] AudioStreamRuntimeCaps Phase88Caps() noexcept {
    // The exact PHASE 88 model map is ten PCM channels plus one AM824 MIDI
    // conformant-data block in each direction (DBS=11 at 48 kHz). The one
    // data block can multiplex the unit's two physical MIDI ports.
    // Cross-validated with alsa-userspace-control-protocols-impl/
    // protocols/bebob/src/terratec/phase88.rs:11-46.  The audio engine still
    // exposes the PCM channels only; the one extra slot is preserved on-wire
    // for AM824 MIDI routing rather than being misinterpreted as PCM.
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = kPhase88PcmChannels,
        .hostOutputPcmChannels = kPhase88PcmChannels,
        .deviceToHostAm824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks,
        .hostToDeviceAm824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks,
        .sampleRateHz = kPhase88SampleRateHz,
        .deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    caps.deviceToHostStreams[0] = {.pcmChannels = kPhase88PcmChannels,
                                   .am824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks};
    caps.hostToDeviceStreams[0] = {.pcmChannels = kPhase88PcmChannels,
                                   .am824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks};
    return caps;
}

[[nodiscard]] DuplexStageResult StageResult(const AudioDuplexChannels& channels,
                                             FW::Generation generation,
                                             DuplexRestartPhase phase) noexcept {
    return DuplexStageResult{
        .generation = generation,
        .channels = channels,
        .phase = phase,
        .runtimeCaps = Phase88Caps(),
    };
}

} // namespace

Phase88Protocol::Phase88Protocol(Protocols::Ports::FireWireBusOps& busOps,
                                 Protocols::Ports::FireWireBusInfo& busInfo,
                                 uint16_t nodeId,
                                 IRM::IRMClient* irmClient,
                                 CMP::CMPClient* cmpClient,
                                 uint64_t deviceGuid) noexcept
    : busInfo_(busInfo), nodeId_(nodeId), irmClient_(irmClient), cmpClient_(cmpClient),
      deviceGuid_(deviceGuid) {
    // The BeBoB PCR path is CMP-only. Keep the common factory shape so this
    // protocol can participate in the same runtime registry as AV/C controls.
    (void)busOps;
}

IOReturn Phase88Protocol::Initialize() {
    return (irmClient_ && cmpClient_ && deviceGuid_ != 0) ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn Phase88Protocol::Shutdown() {
    const IOReturn status = StopDuplex();
    if (cmpClient_ && deviceGuid_ != 0) {
        cmpClient_->InvalidateDevice(deviceGuid_);
    }
    preparedGeneration_ = FW::Generation{0};
    return status;
}

bool Phase88Protocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = Phase88Caps();
    return true;
}

void Phase88Protocol::UpdateRuntimeContext(uint16_t nodeId,
                                           Protocols::AVC::FCPTransport* transport) {
    // A replacement transport or node identity denotes a newly discovered bus
    // epoch. Do not retain a rate transition or CMP lease across that boundary.
    if ((nodeId_ != nodeId || fcpTransport_ != transport) && cmpClient_ && deviceGuid_ != 0) {
        cmpClient_->InvalidateDevice(deviceGuid_);
        inputConnected_ = false;
        outputConnected_ = false;
        preparedGeneration_ = FW::Generation{0};
    }
    nodeId_ = nodeId;
    fcpTransport_ = transport;
}

CMP::CMPDevice Phase88Protocol::CurrentCMPDevice() const noexcept {
    return CMP::CMPDevice{
        .guid = deviceGuid_,
        .nodeId = FW::NodeId{nodeId_ <= 0x3fU ? static_cast<uint8_t>(nodeId_)
                                              : static_cast<uint8_t>(0xffU)},
        .generation = busInfo_.GetGeneration(),
    };
}

IOReturn Phase88Protocol::ResetEpochIfNeeded() noexcept {
    const FW::Generation current = busInfo_.GetGeneration();
    if (preparedGeneration_ == current) return kIOReturnSuccess;
    if (cmpClient_ && deviceGuid_ != 0) cmpClient_->InvalidateDevice(deviceGuid_);
    inputConnected_ = false;
    outputConnected_ = false;
    preparedGeneration_ = current;
    return kIOReturnSuccess;
}

void Phase88Protocol::PrepareDuplex(const AudioDuplexChannels& channels,
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

void Phase88Protocol::SetAssignedChannels(const AudioDuplexChannels& channels) noexcept {
    duplexChannels_ = channels;
}

void Phase88Protocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                       ClockApplyCallback callback) {
    if (desiredClock.sampleRateHz != kPhase88SampleRateHz) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    if (!fcpTransport_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    // Linux's BeBoB start sequence explicitly writes OUTPUT plug 0 first,
    // then INPUT plug 0, using AM824/SFC=48k, and waits 300 ms before CMP.
    // Cross-validated with linux-sound-firewire-stack/firewire/bebob/
    // bebob_stream.c:96-115 and fcp.c:28-81.  Each command is retained until
    // FCP completes so the callback never outlives its CDB.
    auto output = std::make_shared<SignalFormatCommand>(*fcpTransport_, 0, false,
                                                         SignalSampleRate::k48000Hz);
    output->Submit([this, desiredClock, callback = std::move(callback), output](
                       Protocols::AVC::AVCResult outputResult,
                       const SignalFormatCommand::SignalFormat& outputFormat) mutable {
        const IOReturn outputStatus = MapAVCResultToIOReturn(outputResult);
        if (outputStatus != kIOReturnSuccess || !Matches48kAm824(outputFormat)) {
            callback(outputStatus != kIOReturnSuccess ? outputStatus : kIOReturnError, {});
            return;
        }

        if (!fcpTransport_) {
            callback(kIOReturnNotReady, {});
            return;
        }
        auto input = std::make_shared<SignalFormatCommand>(*fcpTransport_, 0, true,
                                                            SignalSampleRate::k48000Hz);
        input->Submit([this, desiredClock, callback = std::move(callback), input](
                          Protocols::AVC::AVCResult inputResult,
                          const SignalFormatCommand::SignalFormat& inputFormat) mutable {
            const IOReturn inputStatus = MapAVCResultToIOReturn(inputResult);
            if (inputStatus != kIOReturnSuccess || !Matches48kAm824(inputFormat)) {
                callback(inputStatus != kIOReturnSuccess ? inputStatus : kIOReturnError, {});
                return;
            }

            // Control-plane choreography only; this is never on the packet path.
            IOSleep(kFormatSettleMs);
            appliedClock_ = desiredClock;
            ASFW_LOG(Audio, "[BeBoB] AV/C unit plugs set to AM824 48k GUID=0x%016llx",
                     deviceGuid_);
            callback(kIOReturnSuccess,
                     ClockApplyResult{.generation = busInfo_.GetGeneration(),
                                      .appliedClock = appliedClock_,
                                      .runtimeCaps = Phase88Caps()});
        });
    });
}

void Phase88Protocol::ProgramRx(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    auto state = std::make_shared<CMPWaitState>();
    // "Rx" is device receive: the remote iPCR consumes the host IT stream.
    // This is the first CMP leg in Linux's BeBoB start transaction.
    cmpClient_->ConnectIPCR(CurrentCMPDevice(), 0, duplexChannels_.hostToDeviceIsoChannel,
                            [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const IOReturn status = WaitForCMP(state);
    inputConnected_ = status == kIOReturnSuccess;
    callback(status, StageResult(duplexChannels_, busInfo_.GetGeneration(),
                                 DuplexRestartPhase::kDeviceRxProgrammed));
}

void Phase88Protocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    auto state = std::make_shared<CMPWaitState>();
    // The second leg is remote oPCR for device-to-host capture (host IR).
    cmpClient_->ConnectOPCR(CurrentCMPDevice(), 0, duplexChannels_.deviceToHostIsoChannel,
                            [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const IOReturn status = WaitForCMP(state);
    outputConnected_ = status == kIOReturnSuccess;
    callback(status, StageResult(duplexChannels_, busInfo_.GetGeneration(),
                                 DuplexRestartPhase::kDeviceTxArmed));
}

void Phase88Protocol::ConfirmDuplexStart(ConfirmCallback callback) {
    if (!cmpClient_ || !inputConnected_ || !outputConnected_) {
        callback(kIOReturnNotReady, {});
        return;
    }

    // Re-read both PCRs after host IR/IT starts. The successful CMP compare-
    // swap alone is insufficient: this verifies that the remote still exposes
    // the exclusive p2p lease and the exact channels we reserved.
    const CMP::CMPDevice device = CurrentCMPDevice();
    const AudioDuplexChannels channels = duplexChannels_;
    cmpClient_->ReadIPCR(device, 0,
                         [this, device, channels, callback = std::move(callback)](
                             bool inputRead, uint32_t inputPCR) mutable {
        if (!inputRead || !MatchesConnectedPCR(inputPCR, channels.hostToDeviceIsoChannel)) {
            callback(kIOReturnNotResponding, {});
            return;
        }
        if (!cmpClient_) {
            callback(kIOReturnNotReady, {});
            return;
        }
        cmpClient_->ReadOPCR(device, 0,
                             [this, channels, inputPCR, callback = std::move(callback)](
                                 bool outputRead, uint32_t outputPCR) mutable {
            if (!outputRead || !MatchesConnectedPCR(outputPCR,
                                                     channels.deviceToHostIsoChannel)) {
                callback(kIOReturnNotResponding, {});
                return;
            }
            ASFW_LOG(Audio,
                     "[BeBoB] CMP verified iPCR=0x%08x oPCR=0x%08x GUID=0x%016llx",
                     inputPCR, outputPCR, deviceGuid_);
            callback(kIOReturnSuccess,
                     DuplexConfirmResult{.generation = busInfo_.GetGeneration(),
                                         .channels = channels,
                                         .appliedClock = appliedClock_,
                                         .runtimeCaps = Phase88Caps()});
        });
    });
}

void Phase88Protocol::ReadDuplexHealth(HealthCallback callback) {
    callback(kIOReturnSuccess, DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                                   .appliedClock = appliedClock_,
                                                   .runtimeCaps = Phase88Caps(),
                                                   .sourceLocked = inputConnected_ && outputConnected_,
                                                   .clockReferenceHealthy = true,
                                                   .nominalRateHz = kPhase88SampleRateHz});
}

void Phase88Protocol::DisconnectPlayback(VoidCallback callback) {
    if (!cmpClient_ || !inputConnected_) {
        inputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }
    auto state = std::make_shared<CMPWaitState>();
    cmpClient_->DisconnectIPCR(CurrentCMPDevice(), 0, [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const IOReturn status = WaitForCMP(state);
    inputConnected_ = false;
    callback(status);
}

void Phase88Protocol::DisconnectCapture(VoidCallback callback) {
    if (!cmpClient_ || !outputConnected_) {
        outputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }
    auto state = std::make_shared<CMPWaitState>();
    cmpClient_->DisconnectOPCR(CurrentCMPDevice(), 0, [state](CMP::CMPStatus status) {
        state->status.store(status, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    });
    const IOReturn status = WaitForCMP(state);
    outputConnected_ = false;
    callback(status);
}

IOReturn Phase88Protocol::StopDuplex() {
    IOReturn playback = kIOReturnSuccess;
    DisconnectPlayback([&playback](IOReturn status) { playback = status; });
    IOReturn capture = kIOReturnSuccess;
    DisconnectCapture([&capture](IOReturn status) { capture = status; });
    return playback != kIOReturnSuccess ? playback : capture;
}

} // namespace ASFW::Audio::BeBoB
