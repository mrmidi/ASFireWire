// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeParamsSerdes.cpp - Duet parameter serialization (FW-128).
//
// Moved out of ApogeeDuetProtocol unchanged. Every function here is pure and
// static, so the round-trip tests need no bus, no transport and no device.

#include "ApogeeParamsSerdes.hpp"

namespace ASFW::Audio::Oxford::Apogee::ParamsSerdes {

namespace {

/// The Duet spends two independent booleans on what is really a tri-state, and
/// both-set is the same "never mute" the device reports when neither applies.
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

//==============================================================================
// Knob
//==============================================================================

std::vector<Command> BuildKnobStateQuery() {
    return {Command::Make(Command::Code::HwState)};
}

Command BuildKnobStateControl(const KnobState& state) {
    std::array<uint8_t, 11> raw{};
    raw[0] = state.outputMute ? 1U : 0U;
    raw[1] = static_cast<uint8_t>(state.target);
    raw[3] = static_cast<uint8_t>(KnobState::kOutputVolMax - state.outputVolume);
    raw[4] = state.inputGains[0];
    raw[5] = state.inputGains[1];
    return Command::HwState(raw);
}

KnobState ParseKnobState(const Command& command) {
    KnobState state{};
    if (command.code != Command::Code::HwState) {
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

//==============================================================================
// Output
//==============================================================================

std::vector<Command> BuildOutputParamsQuery() {
    return {
        Command::Bool(Command::Code::OutMute, false),
        Command::OutVolume(0),
        Command::Bool(Command::Code::OutSourceIsMixer, false),
        Command::Bool(Command::Code::OutIsConsumerLevel, false),
        Command::Bool(Command::Code::MuteForLineOut, false),
        Command::Bool(Command::Code::UnmuteForLineOut, false),
        Command::Bool(Command::Code::MuteForHpOut, false),
        Command::Bool(Command::Code::UnmuteForHpOut, false),
    };
}

std::vector<Command> BuildOutputParamsControl(const OutputParams& params) {
    bool lineMute = false;
    bool lineUnmute = false;
    bool hpMute = false;
    bool hpUnmute = false;

    BuildMuteMode(params.lineMuteMode, lineMute, lineUnmute);
    BuildMuteMode(params.hpMuteMode, hpMute, hpUnmute);

    return {
        Command::Bool(Command::Code::OutMute, params.mute),
        Command::OutVolume(params.volume),
        Command::Bool(Command::Code::OutSourceIsMixer,
                      params.source == OutputSource::MixerOutputPair0),
        Command::Bool(Command::Code::OutIsConsumerLevel,
                      params.nominalLevel == OutputNominalLevel::Consumer),
        Command::Bool(Command::Code::MuteForLineOut, lineMute),
        Command::Bool(Command::Code::UnmuteForLineOut, lineUnmute),
        Command::Bool(Command::Code::MuteForHpOut, hpMute),
        Command::Bool(Command::Code::UnmuteForHpOut, hpUnmute),
    };
}

OutputParams ParseOutputParams(const std::vector<Command>& commands) {
    OutputParams params{};

    bool lineMute = false;
    bool lineUnmute = false;
    bool hpMute = false;
    bool hpUnmute = false;

    for (const auto& command : commands) {
        switch (command.code) {
            case Command::Code::OutMute:
                params.mute = command.boolValue;
                break;
            case Command::Code::OutVolume:
                params.volume = command.u8Value;
                break;
            case Command::Code::OutSourceIsMixer:
                params.source = command.boolValue ? OutputSource::MixerOutputPair0
                                                  : OutputSource::StreamInputPair0;
                break;
            case Command::Code::OutIsConsumerLevel:
                params.nominalLevel = command.boolValue ? OutputNominalLevel::Consumer
                                                        : OutputNominalLevel::Instrument;
                break;
            case Command::Code::MuteForLineOut:
                lineMute = command.boolValue;
                break;
            case Command::Code::UnmuteForLineOut:
                lineUnmute = command.boolValue;
                break;
            case Command::Code::MuteForHpOut:
                hpMute = command.boolValue;
                break;
            case Command::Code::UnmuteForHpOut:
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

//==============================================================================
// Input
//==============================================================================

std::vector<Command> BuildInputParamsQuery() {
    return {
        Command::InGain(0, 0),
        Command::InGain(1, 0),
        Command::IndexedBool(Command::Code::MicPolarity, 0, false),
        Command::IndexedBool(Command::Code::MicPolarity, 1, false),
        Command::IndexedBool(Command::Code::XlrIsMicLevel, 0, false),
        Command::IndexedBool(Command::Code::XlrIsMicLevel, 1, false),
        Command::IndexedBool(Command::Code::XlrIsConsumerLevel, 0, false),
        Command::IndexedBool(Command::Code::XlrIsConsumerLevel, 1, false),
        Command::IndexedBool(Command::Code::MicPhantom, 0, false),
        Command::IndexedBool(Command::Code::MicPhantom, 1, false),
        Command::IndexedBool(Command::Code::InputSourceIsPhone, 0, false),
        Command::IndexedBool(Command::Code::InputSourceIsPhone, 1, false),
        Command::Bool(Command::Code::InClickless, false),
    };
}

std::vector<Command> BuildInputParamsControl(const InputParams& params) {
    std::vector<Command> commands;
    commands.reserve(13);

    for (size_t i = 0; i < params.gains.size(); ++i) {
        commands.push_back(Command::InGain(static_cast<uint8_t>(i), params.gains[i]));
    }
    for (size_t i = 0; i < params.polarities.size(); ++i) {
        commands.push_back(Command::IndexedBool(Command::Code::MicPolarity,
                                                static_cast<uint8_t>(i),
                                                params.polarities[i]));
    }
    for (size_t i = 0; i < params.phantomPowerings.size(); ++i) {
        commands.push_back(Command::IndexedBool(Command::Code::MicPhantom,
                                                static_cast<uint8_t>(i),
                                                params.phantomPowerings[i]));
    }
    for (size_t i = 0; i < params.sources.size(); ++i) {
        commands.push_back(Command::IndexedBool(Command::Code::InputSourceIsPhone,
                                                static_cast<uint8_t>(i),
                                                params.sources[i] == InputSource::Phone));
    }

    commands.push_back(Command::Bool(Command::Code::InClickless, params.clickless));

    for (size_t i = 0; i < params.xlrNominalLevels.size(); ++i) {
        commands.push_back(Command::IndexedBool(
            Command::Code::XlrIsMicLevel,
            static_cast<uint8_t>(i),
            params.xlrNominalLevels[i] == InputXlrNominalLevel::Microphone));
    }
    for (size_t i = 0; i < params.xlrNominalLevels.size(); ++i) {
        commands.push_back(Command::IndexedBool(
            Command::Code::XlrIsConsumerLevel,
            static_cast<uint8_t>(i),
            params.xlrNominalLevels[i] == InputXlrNominalLevel::Consumer));
    }

    return commands;
}

InputParams ParseInputParams(const std::vector<Command>& commands) {
    InputParams params{};
    std::array<bool, 2> isMicLevels{};
    std::array<bool, 2> isConsumerLevels{};

    for (const auto& command : commands) {
        switch (command.code) {
            case Command::Code::InGain:
                if (command.index < params.gains.size()) {
                    params.gains[command.index] = command.u8Value;
                }
                break;
            case Command::Code::MicPolarity:
                if (command.index < params.polarities.size()) {
                    params.polarities[command.index] = command.boolValue;
                }
                break;
            case Command::Code::XlrIsMicLevel:
                if (command.index < isMicLevels.size()) {
                    isMicLevels[command.index] = command.boolValue;
                }
                break;
            case Command::Code::XlrIsConsumerLevel:
                if (command.index < isConsumerLevels.size()) {
                    isConsumerLevels[command.index] = command.boolValue;
                }
                break;
            case Command::Code::MicPhantom:
                if (command.index < params.phantomPowerings.size()) {
                    params.phantomPowerings[command.index] = command.boolValue;
                }
                break;
            case Command::Code::InputSourceIsPhone:
                if (command.index < params.sources.size()) {
                    params.sources[command.index] = command.boolValue ? InputSource::Phone
                                                                      : InputSource::Xlr;
                }
                break;
            case Command::Code::InClickless:
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

//==============================================================================
// Mixer
//==============================================================================

std::vector<Command> BuildMixerParamsQuery() {
    return {
        Command::MixerSrc(0, 0, 0),
        Command::MixerSrc(1, 0, 0),
        Command::MixerSrc(2, 0, 0),
        Command::MixerSrc(3, 0, 0),
        Command::MixerSrc(0, 1, 0),
        Command::MixerSrc(1, 1, 0),
        Command::MixerSrc(2, 1, 0),
        Command::MixerSrc(3, 1, 0),
    };
}

std::vector<Command> BuildMixerParamsControl(const MixerParams& params) {
    std::vector<Command> commands;
    commands.reserve(8);

    for (size_t dst = 0; dst < params.outputs.size(); ++dst) {
        const auto& coefs = params.outputs[dst];
        commands.push_back(Command::MixerSrc(0, static_cast<uint8_t>(dst), coefs.analogInputs[0]));
        commands.push_back(Command::MixerSrc(1, static_cast<uint8_t>(dst), coefs.analogInputs[1]));
        commands.push_back(Command::MixerSrc(2, static_cast<uint8_t>(dst), coefs.streamInputs[0]));
        commands.push_back(Command::MixerSrc(3, static_cast<uint8_t>(dst), coefs.streamInputs[1]));
    }

    return commands;
}

MixerParams ParseMixerParams(const std::vector<Command>& commands) {
    MixerParams params{};

    for (const auto& command : commands) {
        if (command.code != Command::Code::MixerSrc ||
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

//==============================================================================
// Display
//==============================================================================

std::vector<Command> BuildDisplayParamsQuery() {
    return {
        Command::Bool(Command::Code::DisplayIsInput, false),
        Command::Bool(Command::Code::DisplayFollowToKnob, false),
        Command::Bool(Command::Code::DisplayOverholdTwoSec, false),
    };
}

std::vector<Command> BuildDisplayParamsControl(const DisplayParams& params) {
    return {
        Command::Bool(Command::Code::DisplayIsInput, params.target == DisplayTarget::Input),
        Command::Bool(Command::Code::DisplayFollowToKnob,
                      params.mode == DisplayMode::FollowingToKnobTarget),
        Command::Bool(Command::Code::DisplayOverholdTwoSec,
                      params.overhold == DisplayOverhold::TwoSeconds),
    };
}

DisplayParams ParseDisplayParams(const std::vector<Command>& commands) {
    DisplayParams params{};

    for (const auto& command : commands) {
        switch (command.code) {
            case Command::Code::DisplayIsInput:
                params.target = command.boolValue ? DisplayTarget::Input : DisplayTarget::Output;
                break;
            case Command::Code::DisplayFollowToKnob:
                params.mode = command.boolValue ? DisplayMode::FollowingToKnobTarget
                                                : DisplayMode::Independent;
                break;
            case Command::Code::DisplayOverholdTwoSec:
                params.overhold = command.boolValue ? DisplayOverhold::TwoSeconds
                                                    : DisplayOverhold::Infinite;
                break;
            default:
                break;
        }
    }

    return params;
}

} // namespace ASFW::Audio::Oxford::Apogee::ParamsSerdes
