// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.cpp - Protocol implementation for Apogee Duet FireWire
// Reference: snd-firewire-ctl-services/protocols/oxfw/src/apogee.rs

#include "ApogeeDuetProtocol.hpp"

#include "../../../../Logging/Logging.hpp"
#include "../../../AVC/AVCDefs.hpp"
#include "../../../AVC/FCPTransport.hpp"

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
constexpr uint32_t kClassIdPhantomPower = static_cast<uint32_t>('phan');
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

struct ApogeeDuetProtocol::VendorCommand {
    enum class Code : uint8_t {
        MicPolarity = 0x00,
        XlrIsMicLevel = 0x01,
        XlrIsConsumerLevel = 0x02,
        MicPhantom = 0x03,
        OutIsConsumerLevel = 0x04,
        InGain = 0x05,
        HwState = 0x07,
        OutMute = 0x09,
        InputSourceIsPhone = 0x0C,
        MixerSrc = 0x10,
        OutSourceIsMixer = 0x11,
        DisplayOverholdTwoSec = 0x13,
        DisplayClear = 0x14,
        OutVolume = 0x15,
        MuteForLineOut = 0x16,
        MuteForHpOut = 0x17,
        UnmuteForLineOut = 0x18,
        UnmuteForHpOut = 0x19,
        DisplayIsInput = 0x1B,
        InClickless = 0x1E,
        DisplayFollowToKnob = 0x22,
    };

    Code code{};
    uint8_t index{0};
    uint8_t index2{0};
    bool boolValue{false};
    uint8_t u8Value{0};
    uint16_t u16Value{0};
    std::array<uint8_t, 11> hwState{};

    static VendorCommand Bool(Code code, bool value) {
        VendorCommand command{};
        command.code = code;
        command.boolValue = value;
        return command;
    }

    static VendorCommand IndexedBool(Code code, uint8_t index, bool value) {
        VendorCommand command{};
        command.code = code;
        command.index = index;
        command.boolValue = value;
        return command;
    }

    static VendorCommand InGain(uint8_t index, uint8_t value) {
        VendorCommand command{};
        command.code = Code::InGain;
        command.index = index;
        command.u8Value = value;
        return command;
    }

    static VendorCommand OutVolume(uint8_t value) {
        VendorCommand command{};
        command.code = Code::OutVolume;
        command.u8Value = value;
        return command;
    }

    static VendorCommand MixerSrc(uint8_t source, uint8_t destination, uint16_t gain) {
        VendorCommand command{};
        command.code = Code::MixerSrc;
        command.index = source;
        command.index2 = destination;
        command.u16Value = gain;
        return command;
    }

    static VendorCommand HwState(const std::array<uint8_t, 11>& raw) {
        VendorCommand command{};
        command.code = Code::HwState;
        command.hwState = raw;
        return command;
    }

    static VendorCommand Make(Code code) {
        VendorCommand command{};
        command.code = code;
        return command;
    }

    [[nodiscard]] std::vector<uint8_t> BuildOperandBase() const {
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

    void AppendControlValue(std::vector<uint8_t>& operands) const {
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

    [[nodiscard]] bool ParseStatusPayload(std::span<const uint8_t> payload) {
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
};

ApogeeDuetProtocol::ApogeeDuetProtocol(Async::AsyncSubsystem& subsystem,
                                       uint16_t nodeId,
                                       Protocols::AVC::FCPTransport* fcpTransport)
    : subsystem_(subsystem)
    , nodeId_(nodeId)
    , fcpTransport_(fcpTransport) {
}

IOReturn ApogeeDuetProtocol::Initialize() {
    return kIOReturnSuccess;
}

IOReturn ApogeeDuetProtocol::Shutdown() {
    return kIOReturnSuccess;
}

void ApogeeDuetProtocol::UpdateRuntimeContext(uint16_t nodeId,
                                              Protocols::AVC::FCPTransport* transport) {
    nodeId_ = nodeId;
    fcpTransport_ = transport;
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
        [&](IOReturn commandStatus, const VendorCommand& response) {
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
        [&](IOReturn commandStatus, const VendorCommand&) {
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
    if (!fcpTransport_) {
        callback(kIOReturnNotReady, command);
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
        callback(kIOReturnBadArgument, command);
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
        [callback, command, isStatus](FCPStatus status, const FCPFrame& response) {
            const IOReturn transportStatus = MapFCPStatusToIOReturn(status);
            if (transportStatus != kIOReturnSuccess) {
                callback(transportStatus, command);
                return;
            }

            if (response.length < Protocols::AVC::kAVCFrameMinSize) {
                callback(kIOReturnBadMessageID, command);
                return;
            }

            const AVCResult avcResult = CTypeToResult(response.data[0]);
            const IOReturn avcStatus = MapAVCResultToIOReturn(avcResult);
            if (avcStatus != kIOReturnSuccess) {
                callback(avcStatus, command);
                return;
            }

            VendorCommand parsed = command;
            if (isStatus) {
                const size_t operandLength = response.length - 3U;
                std::span<const uint8_t> payload{response.data.data() + 3U, operandLength};
                if (!parsed.ParseStatusPayload(payload)) {
                    callback(kIOReturnBadMessageID, command);
                    return;
                }
            }

            callback(kIOReturnSuccess, parsed);
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
    ExecuteVendorSequence(
        BuildKnobStateQuery(),
        true,
        [callback](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess || responses.empty()) {
                callback(status != kIOReturnSuccess ? status : kIOReturnError, {});
                return;
            }
            callback(kIOReturnSuccess, ParseKnobState(responses[0]));
        });
}

void ApogeeDuetProtocol::SetKnobState(const KnobState& state, VoidCallback callback) {
    ExecuteVendorSequence(
        {BuildKnobStateControl(state)},
        false,
        [callback](IOReturn status, const std::vector<VendorCommand>&) { callback(status); });
}

void ApogeeDuetProtocol::GetOutputParams(ResultCallback<OutputParams> callback) {
    ExecuteVendorSequence(
        BuildOutputParamsQuery(),
        true,
        [callback](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            callback(kIOReturnSuccess, ParseOutputParams(responses));
        });
}

void ApogeeDuetProtocol::SetOutputParams(const OutputParams& params, VoidCallback callback) {
    ExecuteVendorSequence(
        BuildOutputParamsControl(params),
        false,
        [callback](IOReturn status, const std::vector<VendorCommand>&) { callback(status); });
}

void ApogeeDuetProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    ExecuteVendorSequence(
        BuildInputParamsQuery(),
        true,
        [callback](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            callback(kIOReturnSuccess, ParseInputParams(responses));
        });
}

void ApogeeDuetProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    ExecuteVendorSequence(
        BuildInputParamsControl(params),
        false,
        [callback](IOReturn status, const std::vector<VendorCommand>&) { callback(status); });
}

void ApogeeDuetProtocol::GetMixerParams(ResultCallback<MixerParams> callback) {
    ExecuteVendorSequence(
        BuildMixerParamsQuery(),
        true,
        [callback](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            callback(kIOReturnSuccess, ParseMixerParams(responses));
        });
}

void ApogeeDuetProtocol::SetMixerParams(const MixerParams& params, VoidCallback callback) {
    ExecuteVendorSequence(
        BuildMixerParamsControl(params),
        false,
        [callback](IOReturn status, const std::vector<VendorCommand>&) { callback(status); });
}

void ApogeeDuetProtocol::GetDisplayParams(ResultCallback<DisplayParams> callback) {
    ExecuteVendorSequence(
        BuildDisplayParamsQuery(),
        true,
        [callback](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            callback(kIOReturnSuccess, ParseDisplayParams(responses));
        });
}

void ApogeeDuetProtocol::SetDisplayParams(const DisplayParams& params, VoidCallback callback) {
    ExecuteVendorSequence(
        BuildDisplayParamsControl(params),
        false,
        [callback](IOReturn status, const std::vector<VendorCommand>&) { callback(status); });
}

void ApogeeDuetProtocol::ClearDisplay(VoidCallback callback) {
    ExecuteVendorSequence(
        {VendorCommand::Make(VendorCommand::Code::DisplayClear)},
        false,
        [callback](IOReturn status, const std::vector<VendorCommand>&) { callback(status); });
}

void ApogeeDuetProtocol::GetInputMeter(ResultCallback<InputMeterState> callback) {
    Async::ReadParams params{};
    params.destinationID = nodeId_;
    const uint64_t addr = kMeterBaseAddress + kMeterInputOffset;
    params.addressHigh = static_cast<uint32_t>((addr >> 32U) & 0xFFFFU);
    params.addressLow = static_cast<uint32_t>(addr & 0xFFFFFFFFU);
    params.length = 8;

    subsystem_.Read(
        params,
        [callback](Async::AsyncHandle,
                   Async::AsyncStatus status,
                   std::span<const uint8_t> payload) {
            if (status != Async::AsyncStatus::kSuccess || payload.size() < 8U) {
                callback(kIOReturnError, {});
                return;
            }

            InputMeterState state{};
            state.levels[0] = static_cast<int32_t>(ReadQuadletBE(payload.data()));
            state.levels[1] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 4U));
            callback(kIOReturnSuccess, state);
        });
}

void ApogeeDuetProtocol::GetMixerMeter(ResultCallback<MixerMeterState> callback) {
    Async::ReadParams params{};
    params.destinationID = nodeId_;
    const uint64_t addr = kMeterBaseAddress + kMeterMixerOffset;
    params.addressHigh = static_cast<uint32_t>((addr >> 32U) & 0xFFFFU);
    params.addressLow = static_cast<uint32_t>(addr & 0xFFFFFFFFU);
    params.length = 16;

    subsystem_.Read(
        params,
        [callback](Async::AsyncHandle,
                   Async::AsyncStatus status,
                   std::span<const uint8_t> payload) {
            if (status != Async::AsyncStatus::kSuccess || payload.size() < 16U) {
                callback(kIOReturnError, {});
                return;
            }

            MixerMeterState state{};
            state.streamInputs[0] = static_cast<int32_t>(ReadQuadletBE(payload.data()));
            state.streamInputs[1] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 4U));
            state.mixerOutputs[0] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 8U));
            state.mixerOutputs[1] = static_cast<int32_t>(ReadQuadletBE(payload.data() + 12U));
            callback(kIOReturnSuccess, state);
        });
}

void ApogeeDuetProtocol::GetFirmwareId(ResultCallback<uint32_t> callback) {
    Async::ReadParams params{};
    params.destinationID = nodeId_;

    const uint64_t addr = kOxfordCsrBase + kOxfordFirmwareIdOffset;
    params.addressHigh = static_cast<uint32_t>((addr >> 32U) & 0xFFFFU);
    params.addressLow = static_cast<uint32_t>(addr & 0xFFFFFFFFU);
    params.length = 4;

    subsystem_.Read(
        params,
        [callback](Async::AsyncHandle,
                   Async::AsyncStatus status,
                   std::span<const uint8_t> payload) {
            if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                callback(kIOReturnError, 0);
                return;
            }
            callback(kIOReturnSuccess, ReadQuadletBE(payload.data()));
        });
}

void ApogeeDuetProtocol::GetHardwareId(ResultCallback<uint32_t> callback) {
    Async::ReadParams params{};
    params.destinationID = nodeId_;

    const uint64_t addr = kOxfordCsrBase + kOxfordHardwareIdOffset;
    params.addressHigh = static_cast<uint32_t>((addr >> 32U) & 0xFFFFU);
    params.addressLow = static_cast<uint32_t>(addr & 0xFFFFFFFFU);
    params.length = 4;

    subsystem_.Read(
        params,
        [callback](Async::AsyncHandle,
                   Async::AsyncStatus status,
                   std::span<const uint8_t> payload) {
            if (status != Async::AsyncStatus::kSuccess || payload.size() < 4U) {
                callback(kIOReturnError, 0);
                return;
            }
            callback(kIOReturnSuccess, ReadQuadletBE(payload.data()));
        });
}

} // namespace ASFW::Audio::Oxford::Apogee
