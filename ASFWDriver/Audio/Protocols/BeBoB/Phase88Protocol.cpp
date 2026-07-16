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

#include <DriverKit/IOLib.h>

#include <atomic>
#include <memory>

namespace ASFW::Audio::BeBoB {
namespace {

constexpr uint32_t kPhase88PcmChannels = 10;
constexpr uint32_t kPhase88SampleRateHz = 48000;
constexpr uint32_t kCMPTimeoutMs = 250;
constexpr uint32_t kCMPPollMs = 5;

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
    // The exact PHASE 88 model map is ten PCM channels in each direction.
    // Cross-validated with alsa-userspace-control-protocols-impl/
    // protocols/bebob/src/terratec/phase88.rs:11-46.  MIDI is intentionally
    // zero until the BridgeCo format-list probe reports AM824 0x0d slots.
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = kPhase88PcmChannels,
        .hostOutputPcmChannels = kPhase88PcmChannels,
        .deviceToHostAm824Slots = kPhase88PcmChannels,
        .hostToDeviceAm824Slots = kPhase88PcmChannels,
        .sampleRateHz = kPhase88SampleRateHz,
        .deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    caps.deviceToHostStreams[0] = {.pcmChannels = kPhase88PcmChannels,
                                   .am824Slots = kPhase88PcmChannels};
    caps.hostToDeviceStreams[0] = {.pcmChannels = kPhase88PcmChannels,
                                   .am824Slots = kPhase88PcmChannels};
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
    // BeBoB streaming needs no FCP transaction after the ROM match.  A node
    // move is nonetheless a new CMP epoch and all local leases must be dropped.
    (void)transport;
    if (nodeId_ != nodeId && cmpClient_ && deviceGuid_ != 0) {
        cmpClient_->InvalidateDevice(deviceGuid_);
        inputConnected_ = false;
        outputConnected_ = false;
        preparedGeneration_ = FW::Generation{0};
    }
    nodeId_ = nodeId;
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
    // Phase 1 intentionally does not switch rate: this exact backend only
    // admits the requested 48 kHz mode.  The device in front of us has no FCP
    // response path yet, so writing a speculative signal format would leave
    // it in an unknown state.  The later BridgeCo formation probe supplies the
    // evidence required to make a rate transition safe.
    if (desiredClock.sampleRateHz != kPhase88SampleRateHz) {
        callback(kIOReturnUnsupported, {});
        return;
    }
    appliedClock_ = desiredClock;
    callback(kIOReturnSuccess, ClockApplyResult{.generation = busInfo_.GetGeneration(),
                                                .appliedClock = appliedClock_,
                                                .runtimeCaps = Phase88Caps()});
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
    callback((inputConnected_ && outputConnected_) ? kIOReturnSuccess : kIOReturnNotReady,
             DuplexConfirmResult{.generation = busInfo_.GetGeneration(),
                                 .channels = duplexChannels_,
                                 .appliedClock = appliedClock_,
                                 .runtimeCaps = Phase88Caps()});
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
