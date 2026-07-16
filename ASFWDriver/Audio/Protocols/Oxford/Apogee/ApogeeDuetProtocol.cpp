// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.cpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#include "ApogeeDuetProtocol.hpp"

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

constexpr uint8_t kArgDefault = 0xFF;
constexpr uint8_t kArgIndexed = 0x80;
constexpr uint8_t kBoolOn = 0x70;
constexpr uint8_t kBoolOff = 0x60;

constexpr uint8_t kCTypeControl = 0x00;
constexpr uint8_t kCTypeStatus = 0x01;
constexpr uint8_t kSubunitUnit = 0xFF;
constexpr uint8_t kOpcodeVendorDependent = 0x00;
constexpr uint32_t kControlSyncTimeoutMs = 1500;
constexpr uint32_t kCMPTimeoutMs = 250;
constexpr uint32_t kCMPPollMs = 5;
constexpr uint32_t kClassIdPhaseInvert = static_cast<uint32_t>('phsi');

constexpr size_t kVendorHeaderSize = 9; // OUI(3) + Prefix(3) + Code + Arg1 + Arg2.

[[nodiscard]] uint8_t ToWireBool(bool value) noexcept {
    return value ? kBoolOn : kBoolOff;
}

[[nodiscard]] bool FromWireBool(uint8_t value) noexcept {
    return value == kBoolOn;
}

[[nodiscard]] IOReturn MapFCPStatusToIOReturn(FCPStatus status) noexcept {
    switch (status) {
        case FCPStatus::kOk:
            return kIOReturnSuccess;
        case FCPStatus::kTimeout:
            return kIOReturnTimeout;
        case FCPStatus::kBusReset:
            return kIOReturnNotResponding;
        case FCPStatus::kBusy:
            return kIOReturnBusy;
        case FCPStatus::kInvalidPayload:
            return kIOReturnBadArgument;
        default:
            return kIOReturnError;
    }
}

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

[[nodiscard]] uint8_t EncodeMixerSource(uint8_t source) noexcept {
    return static_cast<uint8_t>(((source / 2U) << 4U) | (source % 2U));
}

[[nodiscard]] OutputMuteMode ParseMuteMode(bool mute, bool unmute) noexcept {
    if (mute && unmute) {
        return OutputMuteMode::Never;
    }
    if (mute && !unmute) {
        return OutputMuteMode::Swapped;
    }
    if (!mute && unmute) {
        return OutputMuteMode::Normal;
    }
    return OutputMuteMode::Never;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void BuildMuteMode(OutputMuteMode mode, bool& mute, bool& unmute) noexcept {
    switch (mode) {
        case OutputMuteMode::Never:
            mute = true;
            unmute = true;
            break;
        case OutputMuteMode::Normal:
            mute = false;
            unmute = true;
            break;
        case OutputMuteMode::Swapped:
            mute = true;
            unmute = false;
            break;
    }
}

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

ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::Bool(Code code, bool value) {
    VendorCommand command{};
    command.code = code;
    command.boolValue = value;
    return command;
}

ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::IndexedBool(Code code, uint8_t index, bool value) {
    VendorCommand command{};
    command.code = code;
    command.index = index;
    command.boolValue = value;
    return command;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::InGain(uint8_t index, uint8_t value) {
    VendorCommand command{};
    command.code = Code::InGain;
    command.index = index;
    command.u8Value = value;
    return command;
}

ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::OutVolume(uint8_t value) {
    VendorCommand command{};
    command.code = Code::OutVolume;
    command.u8Value = value;
    return command;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::MixerSrc(uint8_t source, uint8_t destination, uint16_t gain) {
    VendorCommand command{};
    command.code = Code::MixerSrc;
    command.index = source;
    command.index2 = destination;
    command.u16Value = gain;
    return command;
}

ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::HwState(const std::array<uint8_t, 11>& raw) {
    VendorCommand command{};
    command.code = Code::HwState;
    command.hwState = raw;
    return command;
}

ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::VendorCommand::Make(Code code) {
    VendorCommand command{};
    command.code = code;
    return command;
}

std::vector<uint8_t> ApogeeDuetProtocol::VendorCommand::BuildOperandBase() const {
    std::vector<uint8_t> operands;
    operands.reserve(kVendorHeaderSize);

    operands.push_back(ApogeeDuetProtocol::kOUI[0]);
    operands.push_back(ApogeeDuetProtocol::kOUI[1]);
    operands.push_back(ApogeeDuetProtocol::kOUI[2]);

    operands.push_back(ApogeeDuetProtocol::kPrefix[0]);
    operands.push_back(ApogeeDuetProtocol::kPrefix[1]);
    operands.push_back(ApogeeDuetProtocol::kPrefix[2]);

    operands.push_back(static_cast<uint8_t>(code));
    operands.push_back(kArgDefault);
    operands.push_back(kArgDefault);

    switch (code) {
        case Code::MicPolarity:
        case Code::XlrIsMicLevel:
        case Code::XlrIsConsumerLevel:
        case Code::MicPhantom:
        case Code::InGain:
        case Code::InputSourceIsPhone:
            operands[7] = kArgIndexed;
            operands[8] = index;
            break;
        case Code::OutIsConsumerLevel:
        case Code::OutMute:
        case Code::OutVolume:
        case Code::MuteForLineOut:
        case Code::MuteForHpOut:
        case Code::UnmuteForLineOut:
        case Code::UnmuteForHpOut:
            operands[7] = kArgIndexed;
            break;
        case Code::MixerSrc:
            operands[7] = EncodeMixerSource(index);
            operands[8] = index2;
            break;
        case Code::HwState:
        case Code::OutSourceIsMixer:
        case Code::DisplayOverholdTwoSec:
        case Code::DisplayClear:
        case Code::DisplayIsInput:
        case Code::InClickless:
        case Code::DisplayFollowToKnob:
            break;
    }

    return operands;
}

void ApogeeDuetProtocol::VendorCommand::AppendControlValue(std::vector<uint8_t>& operands) const {
    switch (code) {
        case Code::MicPolarity:
        case Code::XlrIsMicLevel:
        case Code::XlrIsConsumerLevel:
        case Code::MicPhantom:
        case Code::OutIsConsumerLevel:
        case Code::OutMute:
        case Code::InputSourceIsPhone:
        case Code::OutSourceIsMixer:
        case Code::DisplayOverholdTwoSec:
        case Code::MuteForLineOut:
        case Code::MuteForHpOut:
        case Code::UnmuteForLineOut:
        case Code::UnmuteForHpOut:
        case Code::DisplayIsInput:
        case Code::InClickless:
        case Code::DisplayFollowToKnob:
            operands.push_back(ToWireBool(boolValue));
            break;
        case Code::InGain:
        case Code::OutVolume:
            operands.push_back(u8Value);
            break;
        case Code::MixerSrc:
            operands.push_back(static_cast<uint8_t>((u16Value >> 8U) & 0xFFU));
            operands.push_back(static_cast<uint8_t>(u16Value & 0xFFU));
            break;
        case Code::HwState:
            operands.insert(operands.end(), hwState.begin(), hwState.end());
            break;
        case Code::DisplayClear:
            break;
    }
}

bool ApogeeDuetProtocol::VendorCommand::ParseStatusPayload(std::span<const uint8_t> payload) {
    if (payload.size() < kVendorHeaderSize) {
        return false;
    }

    if (payload[0] != ApogeeDuetProtocol::kOUI[0] ||
        payload[1] != ApogeeDuetProtocol::kOUI[1] ||
        payload[2] != ApogeeDuetProtocol::kOUI[2]) {
        return false;
    }

    if (payload[3] != ApogeeDuetProtocol::kPrefix[0] ||
        payload[4] != ApogeeDuetProtocol::kPrefix[1] ||
        payload[5] != ApogeeDuetProtocol::kPrefix[2]) {
        return false;
    }

    if (payload[6] != static_cast<uint8_t>(code)) {
        return false;
    }

    switch (code) {
        case Code::MicPolarity:
        case Code::XlrIsMicLevel:
        case Code::XlrIsConsumerLevel:
        case Code::MicPhantom:
        case Code::InputSourceIsPhone:
            if (payload[8] != index || payload.size() < (kVendorHeaderSize + 1U)) {
                return false;
            }
            boolValue = FromWireBool(payload[9]);
            return true;
        case Code::OutIsConsumerLevel:
        case Code::OutMute:
        case Code::OutSourceIsMixer:
        case Code::DisplayOverholdTwoSec:
        case Code::MuteForLineOut:
        case Code::MuteForHpOut:
        case Code::UnmuteForLineOut:
        case Code::UnmuteForHpOut:
        case Code::DisplayIsInput:
        case Code::InClickless:
        case Code::DisplayFollowToKnob:
            if (payload.size() < (kVendorHeaderSize + 1U)) {
                return false;
            }
            boolValue = FromWireBool(payload[9]);
            return true;
        case Code::InGain:
            if (payload[8] != index || payload.size() < (kVendorHeaderSize + 1U)) {
                return false;
            }
            u8Value = payload[9];
            return true;
        case Code::MixerSrc:
            if (payload[7] != EncodeMixerSource(index) ||
                payload[8] != index2 ||
                payload.size() < (kVendorHeaderSize + 2U)) {
                return false;
            }
            u16Value = static_cast<uint16_t>((static_cast<uint16_t>(payload[9]) << 8U) |
                                             static_cast<uint16_t>(payload[10]));
            return true;
        case Code::HwState:
            if (payload.size() < (kVendorHeaderSize + hwState.size())) {
                return false;
            }
            for (size_t i = 0; i < hwState.size(); ++i) {
                hwState[i] = payload[kVendorHeaderSize + i];
            }
            return true;
        case Code::OutVolume:
            if (payload.size() < (kVendorHeaderSize + 1U)) {
                return false;
            }
            u8Value = payload[9];
            return true;
        case Code::DisplayClear:
            return true;
    }

    return false;
}

ApogeeDuetProtocol::ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                       Protocols::Ports::FireWireBusInfo& busInfo,
                                       uint16_t nodeId,
                                       Protocols::AVC::FCPTransport* fcpTransport,
                                       IRM::IRMClient* irmClient,
                                       CMP::CMPClient* cmpClient,
                                       uint64_t deviceGuid,
                                       uint32_t formatSettleDelayMs)
    : busOps_(busOps)
    , busInfo_(busInfo)
    , nodeId_(nodeId)
    , fcpTransport_(fcpTransport)
    , irmClient_(irmClient)
    , cmpClient_(cmpClient)
    , deviceGuid_(deviceGuid)
    , formatSettleDelayMs_(formatSettleDelayMs) {
}

CMP::CMPDevice ApogeeDuetProtocol::CurrentCMPDevice() const noexcept {
    const uint16_t liveNodeId = fcpTransport_ ? fcpTransport_->GetTargetNodeID() : nodeId_;
    return CMP::CMPDevice{
        .guid = deviceGuid_,
        .nodeId = FW::NodeId{liveNodeId <= 0x3FU ? static_cast<uint8_t>(liveNodeId)
                                                  : static_cast<uint8_t>(0xFFU)},
        .generation = busInfo_.GetGeneration(),
    };
}

IOReturn ApogeeDuetProtocol::Initialize() {
    return kIOReturnSuccess;
}

IOReturn ApogeeDuetProtocol::Shutdown() {
    clockConfigApplied_ = false;
    outputConnected_ = false;
    inputConnected_ = false;
    return kIOReturnSuccess;
}

void ApogeeDuetProtocol::UpdateRuntimeContext(uint16_t nodeId,
                                              Protocols::AVC::FCPTransport* transport) {
    // A replacement transport or node identity denotes a newly discovered bus
    // epoch. Do not carry the AV/C configuration cache across that boundary.
    if (nodeId_ != nodeId || fcpTransport_ != transport) {
        clockConfigApplied_ = false;
        if (cmpClient_ && deviceGuid_ != 0) {
            cmpClient_->InvalidateDevice(deviceGuid_);
        }
        preparedGeneration_ = FW::Generation{0};
    }
    nodeId_ = nodeId;
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
        callback(kIOReturnNotReady, {});
        return;
    }
    if (!IsSupportedAudioClockConfig(desiredClock)) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    const FW::Generation currentGeneration = busInfo_.GetGeneration();
    if (preparedGeneration_ != currentGeneration) {
        // PCR state and device stream formation are reset-scoped. Preserve no
        // old connection as a candidate for BREAK/reuse; recovery must reserve
        // fresh resources and establish fresh PCRs in the new generation.
        if (cmpClient_ && deviceGuid_ != 0) {
            cmpClient_->InvalidateDevice(deviceGuid_);
        }
        clockConfigApplied_ = false;
        outputConnected_ = false;
        inputConnected_ = false;
        preparedGeneration_ = currentGeneration;
    }

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
    transition->desiredClock = desiredClock;
    transition->desiredRate = sampleRate;
    transition->completion = std::move(callback);
    AdvanceClockTransition(transition);
}

void ApogeeDuetProtocol::AdvanceClockTransition(
    const std::shared_ptr<ClockTransition>& transition) {
    if (!transition || !fcpTransport_) {
        if (transition) {
            CompleteClockTransition(transition, kIOReturnNotReady);
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
        case ClockTransition::Phase::kSettle:
            if (transition->inputChanged || transition->outputChanged) {
                // Control-plane only; never runs on the packet hot path.
                IOSleep(formatSettleDelayMs_);
            }
            transition->phase = ClockTransition::Phase::kReadInputAfter;
            AdvanceClockTransition(transition);
            return;
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

void ApogeeDuetProtocol::FailClockTransition(const std::shared_ptr<ClockTransition>& transition,
                                             IOReturn status) {
    if (!transition) {
        return;
    }
    clockConfigApplied_ = false;
    if (transition->failureStatus == kIOReturnSuccess) {
        transition->failureStatus = status;
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
    if (!transition || !transition->completion) {
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
    IOReturn playbackStatus = kIOReturnSuccess;
    DisconnectPlayback([&playbackStatus](IOReturn status) { playbackStatus = status; });

    IOReturn captureStatus = kIOReturnSuccess;
    DisconnectCapture([&captureStatus](IOReturn status) { captureStatus = status; });

    if (playbackStatus != kIOReturnSuccess) {
        return playbackStatus;
    }
    return captureStatus;
}

bool ApogeeDuetProtocol::SupportsBooleanControl(uint32_t classIdFourCC,
                                                uint32_t element) const {
    uint8_t index = 0;
    return TryMapBooleanControl(classIdFourCC, element, index);
}

IOReturn ApogeeDuetProtocol::GetBooleanControlValue(uint32_t classIdFourCC,
                                                    uint32_t element,
                                                    bool& outValue) {
    uint8_t channelIndex = 0;
    if (!TryMapBooleanControl(classIdFourCC, element, channelIndex)) {
        return kIOReturnUnsupported;
    }
    if (!fcpTransport_) {
        return kIOReturnNotReady;
    }
    const VendorCommand::Code commandCode =
        (classIdFourCC == kClassIdPhaseInvert)
            ? VendorCommand::Code::MicPolarity
            : VendorCommand::Code::MicPhantom;

    std::atomic<bool> completed{false};
    std::atomic<IOReturn> status{kIOReturnNotReady};
    std::atomic<bool> value{false};

    SendVendorCommand(
        VendorCommand::IndexedBool(commandCode, channelIndex, false),
        true,
        [&status, &value, &completed](IOReturn commandStatus, const VendorCommand& response) {
            status.store(commandStatus, std::memory_order_release);
            if (commandStatus == kIOReturnSuccess) {
                value.store(response.boolValue, std::memory_order_release);
            }
            completed.store(true, std::memory_order_release);
        });

    uint32_t waitedMs = 0;
    while (!completed.load(std::memory_order_acquire)) {
        if (waitedMs >= kControlSyncTimeoutMs) {
            return kIOReturnTimeout;
        }
        IOSleep(1);
        ++waitedMs;
    }

    const IOReturn result = status.load(std::memory_order_acquire);
    if (result == kIOReturnSuccess) {
        outValue = value.load(std::memory_order_acquire);
    }
    return result;
}

IOReturn ApogeeDuetProtocol::SetBooleanControlValue(uint32_t classIdFourCC,
                                                    uint32_t element,
                                                    bool value) {
    uint8_t channelIndex = 0;
    if (!TryMapBooleanControl(classIdFourCC, element, channelIndex)) {
        return kIOReturnUnsupported;
    }
    if (!fcpTransport_) {
        return kIOReturnNotReady;
    }
    const VendorCommand::Code commandCode =
        (classIdFourCC == kClassIdPhaseInvert)
            ? VendorCommand::Code::MicPolarity
            : VendorCommand::Code::MicPhantom;

    std::atomic<bool> completed{false};
    std::atomic<IOReturn> status{kIOReturnNotReady};

    SendVendorCommand(
        VendorCommand::IndexedBool(commandCode, channelIndex, value),
        false,
        [&status, &completed](IOReturn commandStatus, const VendorCommand&) {
            status.store(commandStatus, std::memory_order_release);
            completed.store(true, std::memory_order_release);
        });

    uint32_t waitedMs = 0;
    while (!completed.load(std::memory_order_acquire)) {
        if (waitedMs >= kControlSyncTimeoutMs) {
            return kIOReturnTimeout;
        }
        IOSleep(1);
        ++waitedMs;
    }

    return status.load(std::memory_order_acquire);
}

void ApogeeDuetProtocol::SendVendorCommand(const VendorCommand& command,
                                           bool isStatus,
                                           VendorResultCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    if (!fcpTransport_) {
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, command);
        return;
    }

    std::vector<uint8_t> operands = command.BuildOperandBase();
    if (!isStatus) {
        command.AppendControlValue(operands);
    }

    const size_t unpaddedLength = 3 + operands.size();
    const size_t paddedLength = (unpaddedLength + 3U) & ~3U;

    if (paddedLength < Protocols::AVC::kAVCFrameMinSize ||
        paddedLength > Protocols::AVC::kAVCFrameMaxSize) {
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument, command);
        return;
    }

    FCPFrame frame{};
    frame.data[0] = isStatus ? kCTypeStatus : kCTypeControl;
    frame.data[1] = kSubunitUnit;
    frame.data[2] = kOpcodeVendorDependent;

    if (!operands.empty()) {
        std::copy(operands.begin(), operands.end(), frame.data.begin() + 3);
    }

    if (paddedLength > unpaddedLength) {
        std::fill(frame.data.begin() + unpaddedLength,
                  frame.data.begin() + paddedLength,
                  0);
    }

    frame.length = paddedLength;

    const auto handle = fcpTransport_->SubmitCommand(
        frame,
        [callbackState, command, isStatus](FCPStatus status, const FCPFrame& response) {
            const IOReturn transportStatus = MapFCPStatusToIOReturn(status);
            if (transportStatus != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, transportStatus, command);
                return;
            }

            if (response.length < Protocols::AVC::kAVCFrameMinSize) {
                Common::InvokeSharedCallback(callbackState, kIOReturnBadMessageID, command);
                return;
            }

            const AVCResult avcResult = CTypeToResult(response.data[0]);
            const IOReturn avcStatus = MapAVCResultToIOReturn(avcResult);
            if (avcStatus != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, avcStatus, command);
                return;
            }

            VendorCommand parsed = command;
            if (isStatus) {
                const size_t operandLength = response.length - 3U;
                std::span<const uint8_t> payload{response.data.data() + 3U, operandLength};
                if (!parsed.ParseStatusPayload(payload)) {
                    Common::InvokeSharedCallback(callbackState, kIOReturnBadMessageID, command);
                    return;
                }
            }

            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, parsed);
        });
    (void)handle;
}

void ApogeeDuetProtocol::ExecuteVendorSequence(const std::vector<VendorCommand>& commands,
                                               bool isStatus,
                                               VendorSequenceCallback callback) {
    if (commands.empty()) {
        callback(kIOReturnSuccess, {});
        return;
    }

    struct SequenceState {
        std::vector<VendorCommand> commands;
        std::vector<VendorCommand> responses;
        size_t index{0};
        bool isStatus{false};
        VendorSequenceCallback completion;
    };

    auto state = std::make_shared<SequenceState>();
    state->commands = commands;
    state->responses.reserve(commands.size());
    state->isStatus = isStatus;
    state->completion = std::move(callback);

    auto step = std::make_shared<std::function<void()>>();
    *step = [this, state, step]() {
        if (state->index >= state->commands.size()) {
            state->completion(kIOReturnSuccess, state->responses);
            return;
        }

        const VendorCommand command = state->commands[state->index];
        SendVendorCommand(
            command,
            state->isStatus,
            [state, step](IOReturn status, const VendorCommand& response) {
                if (status != kIOReturnSuccess) {
                    state->completion(status, {});
                    return;
                }
                state->responses.push_back(response);
                ++state->index;
                (*step)();
            });
    };

    (*step)();
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildKnobStateQuery() {
    return {VendorCommand::Make(VendorCommand::Code::HwState)};
}

ApogeeDuetProtocol::VendorCommand ApogeeDuetProtocol::BuildKnobStateControl(
    const KnobState& state) {
    std::array<uint8_t, 11> raw{};
    raw[0] = state.outputMute ? 1U : 0U;
    raw[1] = static_cast<uint8_t>(state.target);
    raw[3] = static_cast<uint8_t>(KnobState::kOutputVolMax - state.outputVolume);
    raw[4] = state.inputGains[0];
    raw[5] = state.inputGains[1];
    return VendorCommand::HwState(raw);
}

KnobState ApogeeDuetProtocol::ParseKnobState(const VendorCommand& command) {
    KnobState state{};
    if (command.code != VendorCommand::Code::HwState) {
        return state;
    }

    state.outputMute = command.hwState[0] > 0;
    switch (command.hwState[1]) {
        case 1:
            state.target = KnobTarget::InputPair0;
            break;
        case 2:
            state.target = KnobTarget::InputPair1;
            break;
        default:
            state.target = KnobTarget::OutputPair0;
            break;
    }

    state.outputVolume = static_cast<uint8_t>(KnobState::kOutputVolMax - command.hwState[3]);
    state.inputGains[0] = command.hwState[4];
    state.inputGains[1] = command.hwState[5];
    return state;
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildOutputParamsQuery() {
    return {
        VendorCommand::Bool(VendorCommand::Code::OutMute, false),
        VendorCommand::OutVolume(0),
        VendorCommand::Bool(VendorCommand::Code::OutSourceIsMixer, false),
        VendorCommand::Bool(VendorCommand::Code::OutIsConsumerLevel, false),
        VendorCommand::Bool(VendorCommand::Code::MuteForLineOut, false),
        VendorCommand::Bool(VendorCommand::Code::UnmuteForLineOut, false),
        VendorCommand::Bool(VendorCommand::Code::MuteForHpOut, false),
        VendorCommand::Bool(VendorCommand::Code::UnmuteForHpOut, false),
    };
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildOutputParamsControl(
    const OutputParams& params) {
    bool lineMute = false;
    bool lineUnmute = false;
    bool hpMute = false;
    bool hpUnmute = false;

    BuildMuteMode(params.lineMuteMode, lineMute, lineUnmute);
    BuildMuteMode(params.hpMuteMode, hpMute, hpUnmute);

    return {
        VendorCommand::Bool(VendorCommand::Code::OutMute, params.mute),
        VendorCommand::OutVolume(params.volume),
        VendorCommand::Bool(VendorCommand::Code::OutSourceIsMixer,
                            params.source == OutputSource::MixerOutputPair0),
        VendorCommand::Bool(VendorCommand::Code::OutIsConsumerLevel,
                            params.nominalLevel == OutputNominalLevel::Consumer),
        VendorCommand::Bool(VendorCommand::Code::MuteForLineOut, lineMute),
        VendorCommand::Bool(VendorCommand::Code::UnmuteForLineOut, lineUnmute),
        VendorCommand::Bool(VendorCommand::Code::MuteForHpOut, hpMute),
        VendorCommand::Bool(VendorCommand::Code::UnmuteForHpOut, hpUnmute),
    };
}

OutputParams ApogeeDuetProtocol::ParseOutputParams(const std::vector<VendorCommand>& commands) {
    OutputParams params{};

    bool lineMute = false;
    bool lineUnmute = false;
    bool hpMute = false;
    bool hpUnmute = false;

    for (const auto& command : commands) {
        switch (command.code) {
            case VendorCommand::Code::OutMute:
                params.mute = command.boolValue;
                break;
            case VendorCommand::Code::OutVolume:
                params.volume = command.u8Value;
                break;
            case VendorCommand::Code::OutSourceIsMixer:
                params.source = command.boolValue ? OutputSource::MixerOutputPair0
                                                  : OutputSource::StreamInputPair0;
                break;
            case VendorCommand::Code::OutIsConsumerLevel:
                params.nominalLevel = command.boolValue ? OutputNominalLevel::Consumer
                                                        : OutputNominalLevel::Instrument;
                break;
            case VendorCommand::Code::MuteForLineOut:
                lineMute = command.boolValue;
                break;
            case VendorCommand::Code::UnmuteForLineOut:
                lineUnmute = command.boolValue;
                break;
            case VendorCommand::Code::MuteForHpOut:
                hpMute = command.boolValue;
                break;
            case VendorCommand::Code::UnmuteForHpOut:
                hpUnmute = command.boolValue;
                break;
            default:
                break;
        }
    }

    params.lineMuteMode = ParseMuteMode(lineMute, lineUnmute);
    params.hpMuteMode = ParseMuteMode(hpMute, hpUnmute);
    return params;
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildInputParamsQuery() {
    return {
        VendorCommand::InGain(0, 0),
        VendorCommand::InGain(1, 0),
        VendorCommand::IndexedBool(VendorCommand::Code::MicPolarity, 0, false),
        VendorCommand::IndexedBool(VendorCommand::Code::MicPolarity, 1, false),
        VendorCommand::IndexedBool(VendorCommand::Code::XlrIsMicLevel, 0, false),
        VendorCommand::IndexedBool(VendorCommand::Code::XlrIsMicLevel, 1, false),
        VendorCommand::IndexedBool(VendorCommand::Code::XlrIsConsumerLevel, 0, false),
        VendorCommand::IndexedBool(VendorCommand::Code::XlrIsConsumerLevel, 1, false),
        VendorCommand::IndexedBool(VendorCommand::Code::MicPhantom, 0, false),
        VendorCommand::IndexedBool(VendorCommand::Code::MicPhantom, 1, false),
        VendorCommand::IndexedBool(VendorCommand::Code::InputSourceIsPhone, 0, false),
        VendorCommand::IndexedBool(VendorCommand::Code::InputSourceIsPhone, 1, false),
        VendorCommand::Bool(VendorCommand::Code::InClickless, false),
    };
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildInputParamsControl(
    const InputParams& params) {
    std::vector<VendorCommand> commands;
    commands.reserve(13);

    for (size_t i = 0; i < params.gains.size(); ++i) {
        commands.push_back(VendorCommand::InGain(static_cast<uint8_t>(i), params.gains[i]));
    }
    for (size_t i = 0; i < params.polarities.size(); ++i) {
        commands.push_back(VendorCommand::IndexedBool(VendorCommand::Code::MicPolarity,
                                                      static_cast<uint8_t>(i),
                                                      params.polarities[i]));
    }
    for (size_t i = 0; i < params.phantomPowerings.size(); ++i) {
        commands.push_back(VendorCommand::IndexedBool(VendorCommand::Code::MicPhantom,
                                                      static_cast<uint8_t>(i),
                                                      params.phantomPowerings[i]));
    }
    for (size_t i = 0; i < params.sources.size(); ++i) {
        commands.push_back(VendorCommand::IndexedBool(VendorCommand::Code::InputSourceIsPhone,
                                                      static_cast<uint8_t>(i),
                                                      params.sources[i] == InputSource::Phone));
    }

    commands.push_back(VendorCommand::Bool(VendorCommand::Code::InClickless, params.clickless));

    for (size_t i = 0; i < params.xlrNominalLevels.size(); ++i) {
        commands.push_back(VendorCommand::IndexedBool(
            VendorCommand::Code::XlrIsMicLevel,
            static_cast<uint8_t>(i),
            params.xlrNominalLevels[i] == InputXlrNominalLevel::Microphone));
    }
    for (size_t i = 0; i < params.xlrNominalLevels.size(); ++i) {
        commands.push_back(VendorCommand::IndexedBool(
            VendorCommand::Code::XlrIsConsumerLevel,
            static_cast<uint8_t>(i),
            params.xlrNominalLevels[i] == InputXlrNominalLevel::Consumer));
    }

    return commands;
}

InputParams ApogeeDuetProtocol::ParseInputParams(const std::vector<VendorCommand>& commands) {
    InputParams params{};
    std::array<bool, 2> isMicLevels{};
    std::array<bool, 2> isConsumerLevels{};

    for (const auto& command : commands) {
        switch (command.code) {
            case VendorCommand::Code::InGain:
                if (command.index < params.gains.size()) {
                    params.gains[command.index] = command.u8Value;
                }
                break;
            case VendorCommand::Code::MicPolarity:
                if (command.index < params.polarities.size()) {
                    params.polarities[command.index] = command.boolValue;
                }
                break;
            case VendorCommand::Code::XlrIsMicLevel:
                if (command.index < isMicLevels.size()) {
                    isMicLevels[command.index] = command.boolValue;
                }
                break;
            case VendorCommand::Code::XlrIsConsumerLevel:
                if (command.index < isConsumerLevels.size()) {
                    isConsumerLevels[command.index] = command.boolValue;
                }
                break;
            case VendorCommand::Code::MicPhantom:
                if (command.index < params.phantomPowerings.size()) {
                    params.phantomPowerings[command.index] = command.boolValue;
                }
                break;
            case VendorCommand::Code::InputSourceIsPhone:
                if (command.index < params.sources.size()) {
                    params.sources[command.index] = command.boolValue ? InputSource::Phone
                                                                       : InputSource::Xlr;
                }
                break;
            case VendorCommand::Code::InClickless:
                params.clickless = command.boolValue;
                break;
            default:
                break;
        }
    }

    for (size_t i = 0; i < params.xlrNominalLevels.size(); ++i) {
        if (isMicLevels[i]) {
            params.xlrNominalLevels[i] = InputXlrNominalLevel::Microphone;
        } else if (isConsumerLevels[i]) {
            params.xlrNominalLevels[i] = InputXlrNominalLevel::Consumer;
        } else {
            params.xlrNominalLevels[i] = InputXlrNominalLevel::Professional;
        }
    }

    return params;
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildMixerParamsQuery() {
    return {
        VendorCommand::MixerSrc(0, 0, 0),
        VendorCommand::MixerSrc(1, 0, 0),
        VendorCommand::MixerSrc(2, 0, 0),
        VendorCommand::MixerSrc(3, 0, 0),
        VendorCommand::MixerSrc(0, 1, 0),
        VendorCommand::MixerSrc(1, 1, 0),
        VendorCommand::MixerSrc(2, 1, 0),
        VendorCommand::MixerSrc(3, 1, 0),
    };
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildMixerParamsControl(
    const MixerParams& params) {
    std::vector<VendorCommand> commands;
    commands.reserve(8);

    for (size_t dst = 0; dst < params.outputs.size(); ++dst) {
        const auto& coefs = params.outputs[dst];
        commands.push_back(VendorCommand::MixerSrc(0, static_cast<uint8_t>(dst),
                                                   coefs.analogInputs[0]));
        commands.push_back(VendorCommand::MixerSrc(1, static_cast<uint8_t>(dst),
                                                   coefs.analogInputs[1]));
        commands.push_back(VendorCommand::MixerSrc(2, static_cast<uint8_t>(dst),
                                                   coefs.streamInputs[0]));
        commands.push_back(VendorCommand::MixerSrc(3, static_cast<uint8_t>(dst),
                                                   coefs.streamInputs[1]));
    }

    return commands;
}

MixerParams ApogeeDuetProtocol::ParseMixerParams(const std::vector<VendorCommand>& commands) {
    MixerParams params{};

    for (const auto& command : commands) {
        if (command.code != VendorCommand::Code::MixerSrc ||
            command.index2 >= params.outputs.size()) {
            continue;
        }

        if (command.index < 2) {
            params.outputs[command.index2].analogInputs[command.index] = command.u16Value;
        } else if (command.index < 4) {
            params.outputs[command.index2].streamInputs[command.index - 2] = command.u16Value;
        }
    }

    return params;
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildDisplayParamsQuery() {
    return {
        VendorCommand::Bool(VendorCommand::Code::DisplayIsInput, false),
        VendorCommand::Bool(VendorCommand::Code::DisplayFollowToKnob, false),
        VendorCommand::Bool(VendorCommand::Code::DisplayOverholdTwoSec, false),
    };
}

std::vector<ApogeeDuetProtocol::VendorCommand> ApogeeDuetProtocol::BuildDisplayParamsControl(
    const DisplayParams& params) {
    return {
        VendorCommand::Bool(VendorCommand::Code::DisplayIsInput,
                            params.target == DisplayTarget::Input),
        VendorCommand::Bool(VendorCommand::Code::DisplayFollowToKnob,
                            params.mode == DisplayMode::FollowingToKnobTarget),
        VendorCommand::Bool(VendorCommand::Code::DisplayOverholdTwoSec,
                            params.overhold == DisplayOverhold::TwoSeconds),
    };
}

DisplayParams ApogeeDuetProtocol::ParseDisplayParams(const std::vector<VendorCommand>& commands) {
    DisplayParams params{};

    for (const auto& command : commands) {
        switch (command.code) {
            case VendorCommand::Code::DisplayIsInput:
                params.target = command.boolValue ? DisplayTarget::Input : DisplayTarget::Output;
                break;
            case VendorCommand::Code::DisplayFollowToKnob:
                params.mode = command.boolValue
                                  ? DisplayMode::FollowingToKnobTarget
                                  : DisplayMode::Independent;
                break;
            case VendorCommand::Code::DisplayOverholdTwoSec:
                params.overhold = command.boolValue ? DisplayOverhold::TwoSeconds
                                                    : DisplayOverhold::Infinite;
                break;
            default:
                break;
        }
    }

    return params;
}

uint32_t ApogeeDuetProtocol::ReadQuadletBE(const uint8_t* data) noexcept {
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) |
           static_cast<uint32_t>(data[3]);
}

void ApogeeDuetProtocol::GetKnobState(ResultCallback<KnobState> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildKnobStateQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess || responses.empty()) {
                Common::InvokeSharedCallback(callbackState,
                                             status != kIOReturnSuccess ? status : kIOReturnError,
                                             KnobState{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParseKnobState(responses[0]));
        });
}

void ApogeeDuetProtocol::SetKnobState(const KnobState& state, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {BuildKnobStateControl(state)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetOutputParams(ResultCallback<OutputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildOutputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, OutputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParseOutputParams(responses));
        });
}

void ApogeeDuetProtocol::SetOutputParams(const OutputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildOutputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildInputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, InputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParseInputParams(responses));
        });
}

void ApogeeDuetProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildInputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetMixerParams(ResultCallback<MixerParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildMixerParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, MixerParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParseMixerParams(responses));
        });
}

void ApogeeDuetProtocol::SetMixerParams(const MixerParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildMixerParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetDisplayParams(ResultCallback<DisplayParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildDisplayParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, DisplayParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParseDisplayParams(responses));
        });
}

void ApogeeDuetProtocol::SetDisplayParams(const DisplayParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        BuildDisplayParamsControl(params),
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
    auto callbackState = Common::ShareCallback(std::move(callback));
    const auto gen = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3Fu)};
    const uint64_t addr64 = kMeterBaseAddress + kMeterInputOffset;
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((addr64 >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(addr64 & 0xFFFFFFFFU),
    }};

    busOps_.ReadBlock(gen,
                      node,
                      addr,
                      8,
                      FW::FwSpeed::S100,
                      [callbackState](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) {
                          if (status != Async::AsyncStatus::kSuccess || payload.size() < 8U) {
                              Common::InvokeSharedCallback(callbackState, kIOReturnError, InputMeterState{});
                              return;
                          }

                          InputMeterState state{};
                          state.levels[0] = static_cast<int32_t>(ReadQuadletBE(payload.data()));
                          state.levels[1] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 4U));
                          Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, state);
                      });
}

void ApogeeDuetProtocol::GetMixerMeter(ResultCallback<MixerMeterState> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const auto gen = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3Fu)};
    const uint64_t addr64 = kMeterBaseAddress + kMeterMixerOffset;
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((addr64 >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(addr64 & 0xFFFFFFFFU),
    }};

    busOps_.ReadBlock(gen,
                      node,
                      addr,
                      16,
                      FW::FwSpeed::S100,
                      [callbackState](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) {
                          if (status != Async::AsyncStatus::kSuccess || payload.size() < 16U) {
                              Common::InvokeSharedCallback(callbackState, kIOReturnError, MixerMeterState{});
                              return;
                          }

                          MixerMeterState state{};
                          state.streamInputs[0] = static_cast<int32_t>(ReadQuadletBE(payload.data()));
                          state.streamInputs[1] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 4U));
                          state.mixerOutputs[0] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 8U));
                          state.mixerOutputs[1] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 12U));
                          Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, state);
                      });
}

void ApogeeDuetProtocol::GetFirmwareId(ResultCallback<uint32_t> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const auto gen = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3Fu)};
    const uint64_t addr64 = kOxfordCsrBase + kOxfordFirmwareIdOffset;
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((addr64 >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(addr64 & 0xFFFFFFFFU),
    }};

    busOps_.ReadBlock(gen,
                      node,
                      addr,
                      4,
                      FW::FwSpeed::S100,
                      [callbackState](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) {
                          if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                              Common::InvokeSharedCallback(callbackState, kIOReturnError, 0u);
                              return;
                          }
                          Common::InvokeSharedCallback(callbackState, kIOReturnSuccess,
                                                       ReadQuadletBE(payload.data()));
                      });
}

void ApogeeDuetProtocol::GetHardwareId(ResultCallback<uint32_t> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const auto gen = busInfo_.GetGeneration();
    const auto node = FW::NodeId{static_cast<uint8_t>(nodeId_ & 0x3Fu)};
    const uint64_t addr64 = kOxfordCsrBase + kOxfordHardwareIdOffset;
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((addr64 >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(addr64 & 0xFFFFFFFFU),
    }};

    busOps_.ReadBlock(gen,
                      node,
                      addr,
                      4,
                      FW::FwSpeed::S100,
                      [callbackState](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) {
                          if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                              Common::InvokeSharedCallback(callbackState, kIOReturnError, 0u);
                              return;
                          }
                          Common::InvokeSharedCallback(callbackState, kIOReturnSuccess,
                                                       ReadQuadletBE(payload.data()));
                      });
}

} // namespace ASFW::Audio::Oxford::Apogee
