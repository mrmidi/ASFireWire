// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuetControlModel.hpp"
#include "ApogeeParamsSerdes.hpp"
#include "../../../Model/Topology/Validate.hpp"
#include <algorithm>
#include <cmath>
namespace ASFW::Audio::Oxford::Apogee {
enum class DuetControlGroup { Input, Output, Mixer, Display, Knob, Action };
struct DuetControlWrite {
    DuetControlsState desired;
    DuetControlGroup group;
    std::vector<ApogeeVendorCommand> commands;
};
inline std::vector<ApogeeVendorCommand> ChangedCommands(
    const std::vector<ApogeeVendorCommand>& old, const std::vector<ApogeeVendorCommand>& next) {
    std::vector<ApogeeVendorCommand> result;
    if (old.size() != next.size()) return result;
    for (size_t k = 0; k < next.size(); ++k) {
        auto before = old[k].BuildOperandBase(), after = next[k].BuildOperandBase();
        old[k].AppendControlValue(before); next[k].AppendControlValue(after);
        if (before != after) result.push_back(next[k]);
    }
    return result;
}
inline std::optional<DuetControlWrite> BuildDuetParameterWrite(const DuetControlsState& current,
    ASFW::AudioModel::ParameterId parameter, const ASFW::AudioModel::ParameterValue& value) {
    namespace AM = ASFW::AudioModel;
    const auto model = BuildDuetControls(current, 1, 0);
    if (!AM::validateParameterWrite(model.topology, model.state, parameter, value)) return {};
    auto next = current;
    const auto id = parameter.value;
    DuetControlGroup group = DuetControlGroup::Input;
    if (id >= DuetControl::Gain && id < DuetControl::Gain + 2) {
        const auto ch = id - DuetControl::Gain;
        next.input.gains[ch] = static_cast<uint8_t>(std::lround(std::get<double>(value)) +
            (next.input.sources[ch] == InputSource::Phone ? 10 : 0));
    } else if (id >= DuetControl::Phantom && id < DuetControl::Phantom + 2)
        next.input.phantomPowerings[id - DuetControl::Phantom] = std::get<bool>(value);
    else if (id >= DuetControl::Polarity && id < DuetControl::Polarity + 2)
        next.input.polarities[id - DuetControl::Polarity] = std::get<bool>(value);
    else if (id >= DuetControl::InputLevel && id < DuetControl::InputLevel + 2)
        next.input.xlrNominalLevels[id - DuetControl::InputLevel] = static_cast<InputXlrNominalLevel>(std::get<int64_t>(value));
    else if (id == DuetControl::Clickless) next.input.clickless = std::get<bool>(value);
    else if (id >= DuetControl::OutputVolume && id <= DuetControl::HeadphoneMuteMode) {
        group = DuetControlGroup::Output;
        switch (id) {
            case DuetControl::OutputVolume: next.output.volume = static_cast<uint8_t>(std::lround(-std::get<double>(value))); break;
            case DuetControl::OutputMute: next.output.mute = std::get<bool>(value); break;
            case DuetControl::OutputSource: next.output.source = static_cast<OutputSource>(std::get<int64_t>(value)); break;
            case DuetControl::OutputLevel: next.output.nominalLevel = static_cast<OutputNominalLevel>(std::get<int64_t>(value)); break;
            case DuetControl::LineMuteMode: next.output.lineMuteMode = static_cast<OutputMuteMode>(std::get<int64_t>(value)); break;
            case DuetControl::HeadphoneMuteMode: next.output.hpMuteMode = static_cast<OutputMuteMode>(std::get<int64_t>(value)); break;
        }
    } else if (id >= DuetControl::Mixer && id < DuetControl::Mixer + 8) {
        group = DuetControlGroup::Mixer;
        const auto out = (id - DuetControl::Mixer) / 4, in = (id - DuetControl::Mixer) % 4;
        auto& coefficient = in < 2 ? next.mixer.outputs[out].analogInputs[in] : next.mixer.outputs[out].streamInputs[in - 2];
        coefficient = static_cast<uint16_t>(std::get<double>(value));
    } else if (id == DuetControl::KnobTarget) {
        group = DuetControlGroup::Knob; next.knob.target = static_cast<KnobTarget>(std::get<int64_t>(value));
    } else if (id >= DuetControl::DisplayTarget && id <= DuetControl::DisplayOverhold) {
        group = DuetControlGroup::Display;
        if (id == DuetControl::DisplayTarget) next.display.target = static_cast<DisplayTarget>(std::get<int64_t>(value));
        if (id == DuetControl::DisplayMode) next.display.mode = static_cast<DisplayMode>(std::get<int64_t>(value));
        if (id == DuetControl::DisplayOverhold) next.display.overhold = static_cast<DisplayOverhold>(std::get<int64_t>(value));
    } else if (id == DuetControl::ClearDisplay) group = DuetControlGroup::Action;
    else return {};
    std::vector<ApogeeVendorCommand> before, after;
    switch (group) {
        case DuetControlGroup::Input: before = ParamsSerdes::BuildInputParamsControl(current.input); after = ParamsSerdes::BuildInputParamsControl(next.input); break;
        case DuetControlGroup::Output: before = ParamsSerdes::BuildOutputParamsControl(current.output); after = ParamsSerdes::BuildOutputParamsControl(next.output); break;
        case DuetControlGroup::Mixer: before = ParamsSerdes::BuildMixerParamsControl(current.mixer); after = ParamsSerdes::BuildMixerParamsControl(next.mixer); break;
        case DuetControlGroup::Display: before = ParamsSerdes::BuildDisplayParamsControl(current.display); after = ParamsSerdes::BuildDisplayParamsControl(next.display); break;
        case DuetControlGroup::Knob: before = {ParamsSerdes::BuildKnobStateControl(current.knob)}; after = {ParamsSerdes::BuildKnobStateControl(next.knob)}; break;
        case DuetControlGroup::Action: break;
    }
    return DuetControlWrite{std::move(next), group, ChangedCommands(before, after)};
}
} // namespace ASFW::Audio::Oxford::Apogee
