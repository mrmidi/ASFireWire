// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "OrpheusControlModel.hpp"
#include "OrpheusVendorCodec.hpp"
#include <optional>
#include <vector>

namespace ASFW::Audio::Prism::Orpheus {

enum class OrpheusControlGroup {
    Input,
    Output,
    Mixer,
    Settings,
    ClockSource
};

struct OrpheusControlWrite {
    OrpheusControlsState desired;
    OrpheusControlGroup group;
    uint8_t index{0};
    std::vector<std::vector<uint8_t>> frames;
};

inline std::optional<OrpheusControlWrite> BuildOrpheusParameterWrite(const OrpheusControlsState& current,
                                                                    AudioModel::ParameterId id,
                                                                    const AudioModel::ParameterValue& value) {
    auto next = current;
    OrpheusControlGroup group = OrpheusControlGroup::Input;
    uint8_t index = 0;
    std::vector<std::vector<uint8_t>> frames;

    if (id.value >= OrpheusControl::Gain && id.value < OrpheusControl::Gain + 8) {
        const auto ch = id.value - OrpheusControl::Gain;
        if (ch >= 4) return std::nullopt; // Only Ch 1..4 have preamps
        const auto* db = std::get_if<double>(&value);
        if (!db) return std::nullopt;
        const auto gainVal = static_cast<uint8_t>(std::clamp(std::lround(*db), 10L, 65L));

        if (next.analog[ch].inputType == 2) {
            next.analog[ch].instrumentGain = gainVal;
            const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::InstrumentGain, ch, gainVal);
            if (f) frames.push_back(*f);
        } else {
            next.analog[ch].micGain = gainVal;
            const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::MicGain, ch, gainVal);
            if (f) frames.push_back(*f);
        }
        group = OrpheusControlGroup::Input;
        index = static_cast<uint8_t>(ch);
    } else if (id.value >= OrpheusControl::Phantom && id.value < OrpheusControl::Phantom + 4) {
        const auto ch = id.value - OrpheusControl::Phantom;
        const auto* enabled = std::get_if<bool>(&value);
        if (!enabled) return std::nullopt;
        next.analog[ch].phantom = *enabled;
        const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::Phantom, ch, *enabled ? 1 : 0);
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::Input;
        index = static_cast<uint8_t>(ch);
    } else if (id.value >= OrpheusControl::Polarity && id.value < OrpheusControl::Polarity + 4) {
        const auto ch = id.value - OrpheusControl::Polarity;
        const auto* enabled = std::get_if<bool>(&value);
        if (!enabled) return std::nullopt;
        next.analog[ch].phase = *enabled;
        const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::Phase, ch, *enabled ? 1 : 0);
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::Input;
        index = static_cast<uint8_t>(ch);
    } else if (id.value >= OrpheusControl::HighPassFilter && id.value < OrpheusControl::HighPassFilter + 4) {
        const auto ch = id.value - OrpheusControl::HighPassFilter;
        const auto* filter = std::get_if<int64_t>(&value);
        if (!filter) return std::nullopt;
        next.analog[ch].filter = static_cast<uint8_t>(*filter);
        const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::Filter, ch, static_cast<uint8_t>(*filter));
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::Input;
        index = static_cast<uint8_t>(ch);
    } else if (id.value >= OrpheusControl::Overkiller && id.value < OrpheusControl::Overkiller + 8) {
        const auto ch = id.value - OrpheusControl::Overkiller;
        const auto* enabled = std::get_if<bool>(&value);
        if (!enabled) return std::nullopt;
        next.analog[ch].overkiller = *enabled;
        const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::Overkiller, ch, *enabled ? 1 : 0);
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::Input;
        index = static_cast<uint8_t>(ch);
    } else if (id.value >= OrpheusControl::InputLevel && id.value < OrpheusControl::InputLevel + 8) {
        const auto ch = id.value - OrpheusControl::InputLevel;
        const auto* level = std::get_if<int64_t>(&value);
        if (!level) return std::nullopt;
        next.analog[ch].lineInputLevel = static_cast<uint8_t>(*level);
        const auto f = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::LineInputLevel, ch, static_cast<uint8_t>(*level));
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::Input;
        index = static_cast<uint8_t>(ch);
    } else if (id.value == OrpheusControl::OutputVolume) {
        const auto* db = std::get_if<double>(&value);
        if (!db) return std::nullopt;
        const int16_t raw = static_cast<int16_t>(std::lround(*db * 256.0));
        const size_t pos = Scales::BigKnobPositionForRaw(raw);
        next.unit.masterVolume = Scales::kBigKnobValues[pos];
        frames.push_back(OrpheusVendorCodec::BuildMasterVolumeControl(next.unit.masterVolume));
        group = OrpheusControlGroup::Output;
    } else if (id.value == OrpheusControl::OutputMute) {
        const auto* mute = std::get_if<bool>(&value);
        if (!mute) return std::nullopt;
        next.unit.masterMute = *mute;
        frames.push_back(OrpheusVendorCodec::BuildMasterMuteControl(*mute));
        group = OrpheusControlGroup::Output;
    } else if (id.value == OrpheusControl::AdatMode) {
        const auto* mode = std::get_if<int64_t>(&value);
        if (!mode) return std::nullopt;
        next.unit.adatMode = static_cast<uint8_t>(*mode);
        const uint8_t op[] = {static_cast<uint8_t>(*mode)};
        frames.push_back(OrpheusVendorCodec::BuildControlFrame(OrpheusCommand::AdatMode, op));
        group = OrpheusControlGroup::Settings;
    } else if (id.value == OrpheusControl::ClockOutput) {
        const auto* out = std::get_if<int64_t>(&value);
        if (!out) return std::nullopt;
        next.unit.clockOutput = static_cast<uint8_t>(*out);
        const uint8_t op[] = {static_cast<uint8_t>(*out)};
        frames.push_back(OrpheusVendorCodec::BuildControlFrame(OrpheusCommand::ClockOutput, op));
        group = OrpheusControlGroup::Settings;
    } else if (id.value == OrpheusControl::SampleRateConverter) {
        const auto* src = std::get_if<int64_t>(&value);
        if (!src) return std::nullopt;
        next.unit.sampleRateConverter = static_cast<uint8_t>(*src);
        const uint8_t op[] = {static_cast<uint8_t>(*src)};
        frames.push_back(OrpheusVendorCodec::BuildControlFrame(OrpheusCommand::SampleRateConverter, op));
        group = OrpheusControlGroup::Settings;
    } else if (id.value == OrpheusControl::ClockSource) {
        const auto* src = std::get_if<int64_t>(&value);
        if (!src) return std::nullopt;
        next.syncSource = static_cast<OrpheusSyncSource>(*src);
        const auto f = OrpheusVendorCodec::BuildSignalSourceControl(next.syncSource, next.unit.HasAdatInput());
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::ClockSource;
    } else if (id.value >= OrpheusControl::Mixer && id.value < OrpheusControl::Mixer + 72) {
        const auto cp = id.value - OrpheusControl::Mixer;
        const auto bus = cp / 12;
        const auto in = cp % 12;
        const auto* db = std::get_if<double>(&value);
        if (!db) return std::nullopt;
        const int16_t raw = static_cast<int16_t>(std::lround(*db * 256.0));
        const size_t pos = Scales::MixGainPositionForRaw(raw);
        const int16_t snapped = Scales::kMixGainValues[pos];
        next.mixer[bus].inputGains[in] = snapped;
        const auto f = OrpheusVendorCodec::BuildMixInputGainControl(bus, in, snapped);
        if (f) frames.push_back(*f);
        group = OrpheusControlGroup::Mixer;
        index = static_cast<uint8_t>(bus);
    } else {
        return std::nullopt;
    }

    if (frames.empty()) return std::nullopt;
    return OrpheusControlWrite{std::move(next), group, index, std::move(frames)};
}

} // namespace ASFW::Audio::Prism::Orpheus
