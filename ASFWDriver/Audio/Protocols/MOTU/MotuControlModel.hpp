// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuControlModel.hpp - the controls a MOTU unit publishes: the clock source
// (MotuRegisters.hpp) and, on a command-DSP model, everything CueMix drives
// (MotuDspParameters.hpp). Pure: builds the control snapshot and maps a
// parameter write back to a DSP command.
//
// The mixer is a MixerNode of the model's sources by eight mixes: per-source
// parameters sit on the crosspoints, per-mix ones on the mix outputs. Input
// strips, output strips, the monitor and the reverb are port-less nodes. None
// of it reaches a host stream, so the Core Audio projection publishes none of
// it; it is for the ASFW app, as CueMix is. The clock source sits on its own
// port-less node, which the projection publishes as the device's clock source.

#pragma once

#include "MotuCommandDsp.hpp"
#include "MotuDspParameters.hpp"
#include "MotuRegisters.hpp"
#include "../../Controls/IControlProvider.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace ASFW::Audio::Motu {

namespace MotuControl {
/// The clock source ('MCLK'); its values are the 0x0b14 source codes.
inline constexpr uint32_t kClockSource = 0x4d434c4bU;
} // namespace MotuControl

struct MotuControlsState final {
    /// The clock source code 0x0b14 reports, when the model has a source list.
    std::optional<int64_t> clockCode{};
    std::span<const ClockSourceChoice> clockChoices{};
    /// The DSP, on a command-DSP model this code knows.
    std::optional<CommandDsp::ModelDspPorts> dspPorts{};
    CommandDsp::DspState dsp{};
};

[[nodiscard]] inline int64_t ClockSourceCode(ClockSourceV2 source) noexcept {
    return static_cast<int64_t>(EncodeClockSourceV3(0, source).value_or(0));
}

namespace Detail {
/// The items of an enum parameter: its own, or the model's outputs for the
/// two destination parameters whose list depends on the model.
[[nodiscard]] inline std::span<const char* const> ItemsOf(const CommandDsp::ParamSpec& spec,
                                                          const CommandDsp::ModelDspPorts& ports) noexcept {
    return spec.items.empty() ? ports.outputs : spec.items;
}
} // namespace Detail

inline Controls::ControlSnapshot BuildMotuControls(const MotuControlsState& native, uint64_t revision,
                                                   uint64_t sequence) {
    namespace AM = ::ASFW::AudioModel;
    namespace CD = CommandDsp;
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
    if (!native.dspPorts) return result;
    const auto& ports = *native.dspPorts;

    // Mixer ports and crosspoints.
    AM::MixerNode mixer;
    auto port = [&](AM::PortDirection direction, std::string name) {
        const AM::PortId id{nextPort++};
        t.ports.push_back({id, AM::NodeId{10}, direction, 1, std::move(name), {}});
        return id;
    };
    for (const char* source : ports.sources) mixer.inputs.push_back(port(AM::PortDirection::Input, source));
    std::vector<AM::PortId> mixOutputs;
    for (uint32_t mix = 0; mix < CD::kMixerCount; ++mix) {
        mixOutputs.push_back(port(AM::PortDirection::Output, "Mix " + std::to_string(mix + 1)));
        mixer.outputs.push_back(mixOutputs.back());
        for (uint32_t src = 0; src < ports.sources.size(); ++src)
            mixer.crosspoints.push_back({AM::CrosspointId{mix * 64 + src + 1}, mixer.inputs[src], mixOutputs.back()});
    }
    t.nodes.push_back({AM::NodeId{10}, "CueMix", std::move(mixer)});
    t.nodes.push_back({AM::NodeId{11}, "Inputs", AM::EndpointNode{AM::EndpointKind::Host}});
    t.nodes.push_back({AM::NodeId{12}, "Outputs", AM::EndpointNode{AM::EndpointKind::Host}});
    t.nodes.push_back({AM::NodeId{13}, "Monitor", AM::EndpointNode{AM::EndpointKind::Host}});
    t.nodes.push_back({AM::NodeId{14}, "Reverb", AM::EndpointNode{AM::EndpointKind::Host}});

    // The main volume scales the Main outputs, which carry playback 1-2 (the
    // UltraLite mk3's "Main L/R"). Modelled as a channel-preserving stage on
    // that path, it is node-wide over every modelled playback channel, so the
    // Core Audio projection publishes it as the output master volume: the one
    // the Mac's volume keys drive.
    AM::ProcessorNode main{{}, {}, true};
    for (uint32_t ch = 0; ch < 2; ++ch) {
        const AM::PortId playback{nextPort++}, in{nextPort++}, out{nextPort++};
        t.ports.push_back({playback, AM::NodeId{15}, AM::PortDirection::Output, 1, ch ? "Playback 2" : "Playback 1",
                           {AM::SignalKind::HostStream, ch + 1}});
        t.ports.push_back({in, AM::NodeId{16}, AM::PortDirection::Input, 1, ch ? "In R" : "In L", {}});
        t.ports.push_back({out, AM::NodeId{16}, AM::PortDirection::Output, 1, ch ? "Main R" : "Main L", {}});
        main.inputs.push_back(in);
        main.outputs.push_back(out);
        t.fixedLinks.push_back({playback, in});
    }
    t.nodes.push_back({AM::NodeId{15}, "Computer", AM::EndpointNode{AM::EndpointKind::Host}});
    t.nodes.push_back({AM::NodeId{16}, "Main Volume", std::move(main)});

    for (const auto& spec : CD::kParams) {
        const auto items = Detail::ItemsOf(spec, ports);
        AM::ParameterDomain domain = AM::BooleanDomain{};
        if (spec.kind == CD::Kind::Enum) {
            AM::EnumDomain e;
            for (size_t i = 0; i < items.size(); ++i) e.values.push_back({static_cast<int64_t>(i), items[i]});
            domain = std::move(e);
        } else if (spec.kind == CD::Kind::Float) {
            domain = AM::ScalarDomain{spec.min, spec.max,
                                      spec.step > 0 ? std::optional<double>{spec.step} : std::nullopt,
                                      AM::ScalarUnit::Generic};
        }
        const size_t channels = ports.Count(spec.scope);
        const size_t sources = spec.scope == CD::Scope::MixerSource ? ports.sources.size() : 1;
        for (size_t ch = 0; ch < channels; ++ch) {
            for (size_t src = 0; src < sources; ++src) {
                const uint32_t id = CD::ParameterId(spec, static_cast<uint8_t>(ch), static_cast<uint8_t>(src));
                const auto reported = native.dsp.Get(id);
                // A value the unit has not reported, or one outside the domain,
                // shows as the domain's own floor rather than invalidating the
                // whole snapshot.
                AM::ParameterValue value{false};
                if (spec.kind == CD::Kind::Bool) {
                    value = reported && reported->byte != 0;
                } else if (spec.kind == CD::Kind::Enum) {
                    const int64_t raw = reported ? reported->byte : 0;
                    value = raw < static_cast<int64_t>(items.size()) ? raw : int64_t{0};
                } else {
                    double v = reported ? double(reported->value) : 0.0;
                    if (!std::isfinite(v)) v = spec.min;
                    value = std::clamp(v, double(spec.min), double(spec.max));
                }
                AM::ParameterTarget target = AM::NodeId{13};
                std::string name = spec.name;
                AM::ParameterSemantic semantic = AM::ParameterSemantic::Unknown;
                const std::string mixName = "Mix " + std::to_string(ch + 1);
                switch (spec.scope) {
                case CD::Scope::Global:
                    target = AM::NodeId{spec.block == CD::Block::Reverb ? 14u : 13u};
                    if (spec.block == CD::Block::Monitor && spec.third == CD::MonitorThird::kMainVolume) {
                        target = AM::NodeId{16};
                        semantic = AM::ParameterSemantic::Level;
                    }
                    break;
                case CD::Scope::Input:
                    target = AM::NodeId{11};
                    name = std::string(ports.inputs[ch]) + " " + spec.name;
                    break;
                case CD::Scope::Output:
                    target = AM::NodeId{12};
                    name = std::string(ports.outputs[ch]) + " " + spec.name;
                    break;
                case CD::Scope::Mixer:
                    target = mixOutputs[ch];
                    name = mixName + " " + spec.name;
                    if (spec.third == CD::MixerThird::kOutputMute && spec.second == 0) semantic = AM::ParameterSemantic::Mute;
                    if (spec.third == CD::MixerThird::kOutputVolume && spec.second == 0) semantic = AM::ParameterSemantic::Level;
                    if (spec.third == CD::MixerThird::kOutputAssign && spec.second == 0) semantic = AM::ParameterSemantic::Source;
                    break;
                case CD::Scope::MixerSource:
                    target = AM::CrosspointId{static_cast<uint32_t>(ch * 64 + src + 1)};
                    name = mixName + " ← " + ports.sources[src] + " " + spec.name;
                    if (spec.third == CD::MixerThird::kSourceGain) semantic = AM::ParameterSemantic::Level;
                    if (spec.third == CD::MixerThird::kSourceMute) semantic = AM::ParameterSemantic::Mute;
                    break;
                }
                param(id, target, semantic, domain, std::move(name), std::move(value));
            }
        }
    }
    return result;
}

/// The DSP command a parameter write becomes, or nothing for an id that is not
/// a DSP parameter of this model or a value of the wrong kind or range.
[[nodiscard]] inline std::optional<CommandDsp::Command> DspCommandFor(
    const CommandDsp::ModelDspPorts& ports, uint32_t id, const AudioModel::ParameterValue& value) noexcept {
    namespace CD = CommandDsp;
    if (!CD::IsDspParameterId(id)) return std::nullopt;
    auto command = CD::AddressOf(id);
    const auto* flag = std::get_if<bool>(&value);
    const auto* item = std::get_if<int64_t>(&value);
    const auto* scalar = std::get_if<double>(&value);
    // The address must name a parameter on a channel the model has; ask with
    // the width the spec expects.
    for (const bool quadlet : {false, true}) {
        command.quadlet = quadlet;
        const auto at = CD::Lookup(ports, command);
        if (!at || CD::ParameterId(*at->spec, at->channel, at->source) != id) continue;
        const auto& spec = *at->spec;
        switch (spec.kind) {
        case CD::Kind::Bool:
            if (!flag) return std::nullopt;
            command.byte = *flag ? 1 : 0;
            return command;
        case CD::Kind::Enum:
            if (!item || *item < 0 || static_cast<size_t>(*item) >= Detail::ItemsOf(spec, ports).size()) return std::nullopt;
            command.byte = static_cast<uint8_t>(*item);
            return command;
        case CD::Kind::Float:
            if (!scalar || !std::isfinite(*scalar) || *scalar < spec.min || *scalar > spec.max) return std::nullopt;
            command.value = static_cast<float>(*scalar);
            return command;
        }
    }
    return std::nullopt;
}

} // namespace ASFW::Audio::Motu
