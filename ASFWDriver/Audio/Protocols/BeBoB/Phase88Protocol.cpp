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
#include "../../../Protocols/AVC/AVCCommand.hpp"
#include "../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"
#include "../../../Protocols/AVC/AudioFunctionBlockCommand.hpp"

#include <DriverKit/IOLib.h>

#include <memory>

namespace ASFW::Audio::BeBoB {
namespace {

constexpr uint32_t kPhase88PcmChannels = 10;
constexpr uint32_t kPhase88MidiDataBlocks = 1;
constexpr uint32_t kPhase88SampleRateHz = 48000;
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

// !!! DIAGNOSTIC: Phase88 CMP path tracing. Remove after FW-94 resolution.
#define PH88TRACE(fmt, ...) \
    ASFW_LOG(Audio, "!!! [BeBoB] " fmt, ##__VA_ARGS__)

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

            // PHASE 88 ships with Internal clock source (FB9 selector reads position 0 =
            // Internal). Linux never programs clock source during stream start; it only
            // reads it later for clock sync.
            // Cross-validated with linux-sound-firewire-stack/firewire/bebob/
            // bebob_stream.c:619 (read-only) and bebob_terratec.c:10-36.

            // Configure the Phase 88 internal hardware mixer for stereo playback:
            // 1. Set Mixer Destination (FB 0x06) to analog-output-1/2 (0x01)
            auto setMixerDest = std::make_shared<Protocols::AVC::AudioFunctionBlockCommand>(
                *this, 0x08,
                Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
                Protocols::AVC::AudioFunctionBlockCommand::BlockType::kSelector,
                0x06, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kSelectorControl,
                std::vector<uint8_t>{0x01}
            );

            setMixerDest->Submit([this, desiredClock, callback = std::move(callback), setMixerDest](
                                     Protocols::AVC::AVCResult destResult,
                                     const std::vector<uint8_t>&) mutable {
                if (destResult != Protocols::AVC::AVCResult::kAccepted) {
                    ASFW_LOG(Audio, "[BeBoB] Warning: failed to set Phase 88 mixer destination: result=%u",
                             static_cast<uint32_t>(destResult));
                }

                // 2. Set Mixer Stream Source (FB 0x07) to stream-input-1/2 (0x01)
                auto setMixerSrc = std::make_shared<Protocols::AVC::AudioFunctionBlockCommand>(
                    *this, 0x08,
                    Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
                    Protocols::AVC::AudioFunctionBlockCommand::BlockType::kSelector,
                    0x07, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kSelectorControl,
                    std::vector<uint8_t>{0x01}
                );
                setMixerSrc->Submit([this, desiredClock, callback = std::move(callback), setMixerSrc](
                                        Protocols::AVC::AVCResult srcResult,
                                        const std::vector<uint8_t>&) mutable {
                    if (srcResult != Protocols::AVC::AVCResult::kAccepted) {
                        ASFW_LOG(Audio, "[BeBoB] Warning: failed to set Phase 88 mixer stream source: result=%u",
                                 static_cast<uint32_t>(srcResult));
                    }

                    // Define helper structure to chain the volume/mute commands
                    struct MixerInitStep {
                        uint8_t fbId;
                        Protocols::AVC::AudioFunctionBlockCommand::ControlSelector selector;
                        std::vector<uint8_t> data;
                    };

                    auto steps = std::make_shared<std::vector<MixerInitStep>>(std::vector<MixerInitStep>{
                        // Unmute Stream Playback Left/Right
                        {0x07, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kMute, {0x01, 0x01, 0x60}},
                        {0x07, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kMute, {0x02, 0x01, 0x60}},
                        // Max Vol Stream Playback Left/Right
                        {0x07, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kVolume, {0x01, 0x02, 0x00, 0x00}},
                        {0x07, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kVolume, {0x02, 0x02, 0x00, 0x00}},
                        // Unmute Mixer Output Left/Right (FB 0 & 1)
                        {0x00, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kMute, {0x01, 0x01, 0x60}},
                        {0x01, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kMute, {0x02, 0x01, 0x60}},
                        // Max Vol Mixer Output Left/Right
                        {0x00, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kVolume, {0x01, 0x02, 0x00, 0x00}},
                        {0x01, Protocols::AVC::AudioFunctionBlockCommand::ControlSelector::kVolume, {0x02, 0x02, 0x00, 0x00}}
                    });

                    auto runNextStep = std::make_shared<std::function<void(size_t)>>();
                    *runNextStep = [this, desiredClock, callback = std::move(callback), steps, runNextStep](size_t index) mutable {
                        if (index >= steps->size()) {
                            IOSleep(kFormatSettleMs);
                            appliedClock_ = desiredClock;
                            ASFW_LOG(Audio, "[BeBoB] Phase 88 hardware mixer configured and unmuted");
                            callback(kIOReturnSuccess,
                                     ClockApplyResult{.generation = busInfo_.GetGeneration(),
                                                      .appliedClock = appliedClock_,
                                                      .runtimeCaps = Phase88Caps()});
                            return;
                        }

                        const auto& step = (*steps)[index];
                        auto cmd = std::make_shared<Protocols::AVC::AudioFunctionBlockCommand>(
                            *this, 0x08,
                            Protocols::AVC::AudioFunctionBlockCommand::CommandType::kControl,
                            Protocols::AVC::AudioFunctionBlockCommand::BlockType::kFeature,
                            step.fbId, step.selector, step.data
                        );
                        cmd->Submit([index, runNextStep, cmd](Protocols::AVC::AVCResult res, const std::vector<uint8_t>&) mutable {
                            (*runNextStep)(index + 1);
                        });
                    };

                    (*runNextStep)(0);
                });
            });
        });
    });
}

void Phase88Protocol::ProgramRx(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    PH88TRACE("ProgramRx entry: ch=%u", duplexChannels_.hostToDeviceIsoChannel);
    EnsurePlugFree(CMP::PCRDirection::kInput, 0, [this, callback = std::move(callback)](IOReturn err) mutable {
        if (err != kIOReturnSuccess) {
            PH88TRACE("ProgramRx: EnsurePlugFree failed kr=0x%x", err);
            callback(err, {});
            return;
        }

        // "Rx" is device receive: the remote iPCR consumes the host IT stream.
        // This is the first CMP leg in Linux's BeBoB start transaction. CMP
        // ESTABLISH is an async remote lock transaction whose completion is
        // delivered on the same transport queue that runs this EnsurePlugFree
        // callback. Continue the stage from that completion — never block the
        // queue polling for the result. A blocking wait here self-deadlocks:
        // the completion that would satisfy it can never run while we hold the
        // queue (FW-94).
        PH88TRACE("ProgramRx: submitting ConnectIPCR ch=%u", duplexChannels_.hostToDeviceIsoChannel);
        cmpClient_->ConnectIPCR(CurrentCMPDevice(), 0, duplexChannels_.hostToDeviceIsoChannel,
                                [this, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            const IOReturn kr = status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError;
            inputConnected_ = kr == kIOReturnSuccess;
            PH88TRACE("ProgramRx: ConnectIPCR callback status=%u kr=0x%x inputConnected=%u",
                      status, kr, inputConnected_);
            callback(kr, StageResult(duplexChannels_, busInfo_.GetGeneration(),
                                     DuplexRestartPhase::kDeviceRxProgrammed));
        });
    });
}

void Phase88Protocol::ProgramTxAndEnableDuplex(StageCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    PH88TRACE("ProgramTx entry: ch=%u", duplexChannels_.deviceToHostIsoChannel);
    EnsurePlugFree(CMP::PCRDirection::kOutput, 0, [this, callback = std::move(callback)](IOReturn err) mutable {
        if (err != kIOReturnSuccess) {
            PH88TRACE("ProgramTx: EnsurePlugFree failed kr=0x%x", err);
            callback(err, {});
            return;
        }

        // The second leg is remote oPCR for device-to-host capture (host IR).
        // Same continuation-passing rule as ProgramRx: finish the stage from the
        // CMP completion, never from a blocking poll on this queue (FW-94).
        PH88TRACE("ProgramTx: submitting ConnectOPCR ch=%u", duplexChannels_.deviceToHostIsoChannel);
        cmpClient_->ConnectOPCR(CurrentCMPDevice(), 0, duplexChannels_.deviceToHostIsoChannel,
                                [this, callback = std::move(callback)](CMP::CMPStatus status) mutable {
            const IOReturn kr = status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError;
            outputConnected_ = kr == kIOReturnSuccess;
            PH88TRACE("ProgramTx: ConnectOPCR callback status=%u kr=0x%x outputConnected=%u",
                      status, kr, outputConnected_);
            callback(kr, StageResult(duplexChannels_, busInfo_.GetGeneration(),
                                     DuplexRestartPhase::kDeviceTxArmed));
        });
    });
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
    // Commit the teardown intent synchronously, then break the remote iPCR with
    // an async lock transaction. The completion captures only the caller
    // callback — never `this` — so a fire-and-forget StopDuplex() may outlive
    // this object without dereferencing freed state when the CMP completion
    // lands on the transport queue (FW-94 / FW-60).
    inputConnected_ = false;
    cmpClient_->DisconnectIPCR(CurrentCMPDevice(), 0,
                               [callback = std::move(callback)](CMP::CMPStatus status) mutable {
        callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
    });
}

void Phase88Protocol::DisconnectCapture(VoidCallback callback) {
    if (!cmpClient_ || !outputConnected_) {
        outputConnected_ = false;
        callback(kIOReturnSuccess);
        return;
    }
    // See DisconnectPlayback: reset state synchronously, break the remote oPCR
    // asynchronously, and keep `this` out of the completion capture.
    outputConnected_ = false;
    cmpClient_->DisconnectOPCR(CurrentCMPDevice(), 0,
                               [callback = std::move(callback)](CMP::CMPStatus status) mutable {
        callback(status == CMP::CMPStatus::Success ? kIOReturnSuccess : kIOReturnError);
    });
}

IOReturn Phase88Protocol::StopDuplex() {
    // Break both remote CMP connections best-effort. Each disconnect is an async
    // remote lock transaction; issuing them fire-and-forget keeps StopDuplex
    // non-blocking, so it is safe on any queue — a blocking wait here
    // self-deadlocks when StopDuplex runs on the same queue that delivers the CMP
    // completion (FW-94). Wire cleanup of the remote iPCR/oPCR completes on the
    // transport queue; the next start's EnsurePlugFree re-verifies and clears any
    // residual p2p lease, and CMPClient carries the async chain with value-copied
    // lease state so it is unaffected by Shutdown()'s InvalidateDevice. The
    // completions above capture no `this`, so the transaction may safely outlive
    // this object. Cross-validated with Linux
    // sound/firewire/bebob/bebob_stream.c:602-606 (break_both_connections on stop
    // is best-effort); no reference source is copied.
    DisconnectPlayback([](IOReturn) {});
    DisconnectCapture([](IOReturn) {});
    return kIOReturnSuccess;
}

void Phase88Protocol::EnsurePlugFree(CMP::PCRDirection dir, uint8_t plug, std::function<void(IOReturn)> cb) {
    if (!cmpClient_) {
        cb(kIOReturnNotReady);
        return;
    }
    const auto device = CurrentCMPDevice();
    cmpClient_->CheckPlugUsed(device, dir, plug, [this, device, plug, dir, cb = std::move(cb)](bool success, bool used) {
        if (!success) {
            cb(kIOReturnNotResponding);
            return;
        }
        if (used) {
            ASFW_LOG(Audio, "[BeBoB] Plug %u (direction %s) in use, breaking connections",
                     plug, dir == CMP::PCRDirection::kInput ? "Input" : "Output");
            cmpClient_->BreakBothConnections(device, plug, [cb](CMP::CMPStatus status) {
                if (status == CMP::CMPStatus::Success) {
                    cb(kIOReturnSuccess);
                } else {
                    cb(kIOReturnError);
                }
            });
        } else {
            cb(kIOReturnSuccess);
        }
    });
}

void Phase88Protocol::BreakBothConnections(VoidCallback callback) {
    if (!cmpClient_) {
        callback(kIOReturnNotReady);
        return;
    }
    cmpClient_->BreakBothConnections(CurrentCMPDevice(), 0, [callback](CMP::CMPStatus status) {
        if (status == CMP::CMPStatus::Success) {
            callback(kIOReturnSuccess);
        } else {
            callback(kIOReturnError);
        }
    });
}

void Phase88Protocol::SubmitCommand(const Protocols::AVC::AVCCdb& cdb,
                                    Protocols::AVC::AVCCompletion completion) {
    if (!fcpTransport_) {
        completion(Protocols::AVC::AVCResult::kTransportError, cdb);
        return;
    }
    auto cmd = std::make_shared<Protocols::AVC::AVCCommand>(*fcpTransport_, cdb);
    // Capture cmd so the AVCCommand (and its weak_from_this()) stays alive
    // until the FCP response arrives and the completion fires.
    cmd->Submit([cmd, completion = std::move(completion)](
                    Protocols::AVC::AVCResult result,
                    const Protocols::AVC::AVCCdb& responseCdb) {
        completion(result, responseCdb);
    });
}

} // namespace ASFW::Audio::BeBoB
