// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuControlModel.hpp - the controls a MOTU unit publishes: the clock source
// (MotuRegisters.hpp) and, on a command-DSP model, CueMix's mixer
// (MotuCommandDsp.hpp). Pure: builds the control snapshot and maps a
// parameter write back to a register value or a DSP command.
//
// The mixer is a MixerNode of the model's sources by eight mixes. Gains, mutes,
// solos and pans sit on the crosspoints, each mix's volume, mute and
// destination on its output port. None of them reaches a host stream, so the
// Core Audio projection publishes none of them; they are for the ASFW app, as
// CueMix is. The clock source sits on a port-less node, which the projection
// publishes as the device's clock source.

#pragma once

#include "MotuCommandDsp.hpp"
#include "MotuRegisters.hpp"
#include "../../Controls/IControlProvider.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace ASFW::Audio::Motu {

namespace MotuControl {
/// The clock source ('MCLK'); its values are the 0x0b14 source codes.
inline constexpr uint32_t kClockSource = 0x4d434c4bU;
/// CueMix ('MX'): kMixerBase | mixer << 8 | field.
inline constexpr uint32_t kMixerBase = 0x4d580000U;
inline constexpr uint32_t kMainVolume = kMixerBase | 0xff00U;
namespace Field {
inline constexpr uint32_t kOutputAssign = 0x00;
inline constexpr uint32_t kOutputMute = 0x01;
inline constexpr uint32_t kOutputVolume = 0x02;
/// 0x10 + source * 4 + one of these.
inline constexpr uint32_t kSourceBase = 0x10;
inline constexpr uint32_t kSourceMute = 0;
inline constexpr uint32_t kSourceSolo = 1;
inline constexpr uint32_t kSourceGain = 2;
inline constexpr uint32_t kSourcePan = 3;
} // namespace Field
[[nodiscard]] constexpr uint32_t Mixer(uint32_t mixer, uint32_t field) noexcept {
    return kMixerBase | (mixer << 8) | field;
}
[[nodiscard]] constexpr uint32_t Source(uint32_t mixer, uint32_t source, uint32_t field) noexcept {
    return Mixer(mixer, Field::kSourceBase + source * 4 + field);
}
} // namespace MotuControl

struct MotuControlsState final {
    /// The clock source code 0x0b14 reports, when the model has a source list.
    std::optional<int64_t> clockCode{};
    std::span<const ClockSourceChoice> clockChoices{};
    /// The DSP mixer, on a command-DSP model this code knows.
    std::optional<CommandDsp::ModelMixerPorts> mixerPorts{};
    CommandDsp::MixerState mixer{};
};

[[nodiscard]] inline int64_t ClockSourceCode(ClockSourceV2 source) noexcept {
    return static_cast<int64_t>(EncodeClockSourceV3(0, source).value_or(0));
}

inline Controls::ControlSnapshot BuildMotuControls(const MotuControlsState& native, uint64_t revision,
                                                   uint64_t sequence) {
    namespace AM = ::ASFW::AudioModel;
    Controls::ControlSnapshot result;
    result.family = "motu";
    result.topology.revision = result.state.topologyRevision = revision;
    result.sequence = sequence;
    auto& t = result.topology;
    auto& s = result.state;
    uint32_t nextPort = 1;
    auto param = [&](uint32_t id, AM::ParameterTarget target, AM::ParameterSemantic semantic,
                     AM::ParameterDomain domain, std::string name, AM::ParameterValue value) {
        t.parameters.push_back({AM::ParameterId{id}, target, semantic, std::move(domain), std::move(name)});
        s.parameters.emplace(AM::ParameterId{id}, AM::ParameterState{std::move(value)});
    };

    if (native.clockCode && !native.clockChoices.empty()) {
        t.nodes.push_back({AM::NodeId{1}, "Clock", AM::EndpointNode{AM::EndpointKind::Host}});
        AM::EnumDomain domain;
        for (const auto& choice : native.clockChoices) domain.values.push_back({ClockSourceCode(choice.source), choice.name});
        param(MotuControl::kClockSource, AM::NodeId{1}, AM::ParameterSemantic::ClockSource, std::move(domain),
              "Clock Source", *native.clockCode);
    }

    if (native.mixerPorts) {
        const auto& ports = *native.mixerPorts;
        const auto& m = native.mixer;
        AM::MixerNode mixer;
        auto port = [&](AM::PortDirection direction, std::string name) {
            const AM::PortId id{nextPort++};
            t.ports.push_back({id, AM::NodeId{10}, direction, 1, std::move(name), {}});
            return id;
        };
        for (const char* source : ports.sources) mixer.inputs.push_back(port(AM::PortDirection::Input, source));
        AM::EnumDomain destinations;
        for (size_t i = 0; i < ports.outputs.size(); ++i)
            destinations.values.push_back({static_cast<int64_t>(i), ports.outputs[i]});
        const AM::ScalarDomain unit{0, 1, std::nullopt, AM::ScalarUnit::Generic};
        const AM::ScalarDomain balance{-1, 1, std::nullopt, AM::ScalarUnit::Generic};
        for (uint32_t mix = 0; mix < CommandDsp::kMixerCount; ++mix) {
            const std::string mixName = "Mix " + std::to_string(mix + 1);
            const auto out = port(AM::PortDirection::Output, mixName);
            mixer.outputs.push_back(out);
            const auto& mx = m.mixers[mix];
            param(MotuControl::Mixer(mix, MotuControl::Field::kOutputAssign), out, AM::ParameterSemantic::Source,
                  destinations, mixName + " Destination", int64_t{mx.outputAssign});
            param(MotuControl::Mixer(mix, MotuControl::Field::kOutputMute), out, AM::ParameterSemantic::Mute,
                  AM::BooleanDomain{}, mixName + " Mute", mx.outputMute);
            param(MotuControl::Mixer(mix, MotuControl::Field::kOutputVolume), out, AM::ParameterSemantic::Level,
                  unit, mixName + " Volume", double(mx.outputVolume));
            for (uint32_t src = 0; src < ports.sources.size(); ++src) {
                const AM::CrosspointId cp{mix * 64 + src + 1};
                mixer.crosspoints.push_back({cp, mixer.inputs[src], out});
                const std::string name = mixName + " ← " + ports.sources[src];
                const auto& strip = src < mx.sources.size() ? mx.sources[src] : CommandDsp::MixerSource{};
                param(MotuControl::Source(mix, src, MotuControl::Field::kSourceGain), cp,
                      AM::ParameterSemantic::Level, unit, name, double(strip.gain));
                param(MotuControl::Source(mix, src, MotuControl::Field::kSourcePan), cp,
                      AM::ParameterSemantic::Unknown, balance, name + " Pan", double(strip.pan));
                param(MotuControl::Source(mix, src, MotuControl::Field::kSourceMute), cp,
                      AM::ParameterSemantic::Mute, AM::BooleanDomain{}, name + " Mute", strip.mute);
                param(MotuControl::Source(mix, src, MotuControl::Field::kSourceSolo), cp,
                      AM::ParameterSemantic::Unknown, AM::BooleanDomain{}, name + " Solo", strip.solo);
            }
        }
        t.nodes.push_back({AM::NodeId{10}, "CueMix", std::move(mixer)});
        param(MotuControl::kMainVolume, AM::NodeId{10}, AM::ParameterSemantic::Unknown, unit, "Main Volume",
              double(m.mainVolume));
    }
    return result;
}

/// The DSP command a mixer parameter write becomes, or nothing for an id that
/// is not a mixer parameter of this model or a value of the wrong kind.
[[nodiscard]] inline std::optional<CommandDsp::Command> MixerCommandFor(
    const CommandDsp::ModelMixerPorts& ports, uint32_t id, const AudioModel::ParameterValue& value) noexcept {
    using namespace CommandDsp;
    const auto* scalar = std::get_if<double>(&value);
    const auto* flag = std::get_if<bool>(&value);
    const auto* item = std::get_if<int64_t>(&value);
    if (id == MotuControl::kMainVolume)
        return scalar ? std::optional{Set::MainVolume(static_cast<float>(*scalar))} : std::nullopt;
    if ((id & 0xffff0000U) != MotuControl::kMixerBase) return std::nullopt;
    const uint32_t mix = (id >> 8) & 0xffU;
    const uint32_t field = id & 0xffU;
    if (mix >= kMixerCount) return std::nullopt;
    const auto m = static_cast<uint8_t>(mix);
    switch (field) {
    case MotuControl::Field::kOutputAssign:
        if (!item || *item < 0 || static_cast<size_t>(*item) >= ports.outputs.size()) return std::nullopt;
        return Set::OutputAssign(m, static_cast<uint8_t>(*item));
    case MotuControl::Field::kOutputMute:
        return flag ? std::optional{Set::OutputMute(m, *flag)} : std::nullopt;
    case MotuControl::Field::kOutputVolume:
        return scalar ? std::optional{Set::OutputVolume(m, static_cast<float>(*scalar))} : std::nullopt;
    default: break;
    }
    if (field < MotuControl::Field::kSourceBase) return std::nullopt;
    const uint32_t source = (field - MotuControl::Field::kSourceBase) / 4;
    if (source >= ports.sources.size()) return std::nullopt;
    const auto src = static_cast<uint8_t>(source);
    switch ((field - MotuControl::Field::kSourceBase) % 4) {
    case MotuControl::Field::kSourceMute: return flag ? std::optional{Set::SourceMute(m, src, *flag)} : std::nullopt;
    case MotuControl::Field::kSourceSolo: return flag ? std::optional{Set::SourceSolo(m, src, *flag)} : std::nullopt;
    case MotuControl::Field::kSourceGain:
        return scalar ? std::optional{Set::SourceGain(m, src, static_cast<float>(*scalar))} : std::nullopt;
    case MotuControl::Field::kSourcePan:
        return scalar ? std::optional{Set::SourcePan(m, src, static_cast<float>(*scalar))} : std::nullopt;
    }
    return std::nullopt;
}

} // namespace ASFW::Audio::Motu
