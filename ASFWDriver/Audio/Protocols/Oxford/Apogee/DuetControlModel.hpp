// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ApogeeTypes.hpp"
#include "../../../Controls/IControlProvider.hpp"
namespace ASFW::Audio::Oxford::Apogee {
namespace DuetControl {
inline constexpr uint32_t Gain = 0x44550010, Phantom = 0x44550020, Polarity = 0x44550030, InputLevel = 0x44550040;
inline constexpr uint32_t OutputVolume = 0x44550100, OutputMute = 0x44550101, OutputSource = 0x44550102,
    OutputLevel = 0x44550103, LineMuteMode = 0x44550104, HeadphoneMuteMode = 0x44550105,
    Clickless = 0x44550106, KnobTarget = 0x44550107, DisplayTarget = 0x44550108,
    DisplayMode = 0x44550109, DisplayOverhold = 0x4455010a, ClearDisplay = 0x4455010b, Mixer = 0x44550200;
inline constexpr uint32_t InputRouter = 0x44551000;
}
struct DuetControlsState {
    InputParams input;
    OutputParams output;
    MixerParams mixer;
    DisplayParams display;
    KnobState knob;
};
/// Behavioral ranges/connector choices cross-checked with ALSA apogee.rs
/// input/output parameter definitions and command conversion (lines 280–590).
/// The jack's Hi-Z characterization remains a hardware acceptance check.
inline Controls::ControlSnapshot BuildDuetControls(const DuetControlsState& native, uint64_t revision, uint64_t sequence) {
    namespace AM = ASFW::AudioModel;
    Controls::ControlSnapshot result;
    result.family = "duet"; result.topology.revision = result.state.topologyRevision = revision; result.sequence = sequence;
    auto& t = result.topology; auto& s = result.state;
    uint32_t nextPort = 1;
    auto node = [&](uint32_t id, std::string name, AM::NodeBody body) { t.nodes.push_back({AM::NodeId{id}, std::move(name), std::move(body)}); };
    auto port = [&](uint32_t owner, AM::PortDirection direction, std::string name, AM::SignalIdentity signal = {}) {
        const AM::PortId id{nextPort++};
        t.ports.push_back({id, AM::NodeId{owner}, direction, 1, std::move(name), signal}); return id;
    };
    auto param = [&](uint32_t id, AM::ParameterTarget target, AM::ParameterSemantic semantic, AM::ParameterDomain domain,
                     std::string name, AM::ParameterValue value) {
        t.parameters.push_back({AM::ParameterId{id}, target, semantic, std::move(domain), std::move(name)});
        s.parameters.emplace(AM::ParameterId{id}, AM::ParameterState{std::move(value)});
    };
    const AM::EnumDomain xlrLevels{{{0, "Mic"}, {1, "+4 dBu"}, {2, "-10 dBV"}}};
    std::array<AM::PortId, 2> analog, hostPlayback, mixed;
    node(1, "Host Capture", AM::EndpointNode{AM::EndpointKind::Host});
    node(2, "Host Playback", AM::EndpointNode{AM::EndpointKind::Host});
    for (uint32_t ch = 0; ch < 2; ++ch) {
        const uint32_t physical = 10 + ch, router = DuetControl::InputRouter + ch, gain = 20 + ch;
        node(physical, "Analog Input " + std::to_string(ch + 1), AM::EndpointNode{AM::EndpointKind::Physical});
        const auto xlr = port(physical, AM::PortDirection::Output, "XLR", {AM::SignalKind::AnalogMicXlr, ch + 1});
        const auto jack = port(physical, AM::PortDirection::Output, "Instrument", {AM::SignalKind::AnalogInstrument, ch + 1});
        const auto a = port(router, AM::PortDirection::Input, "XLR"), b = port(router, AM::PortDirection::Input, "Jack");
        const auto selected = port(router, AM::PortDirection::Output, "Selected");
        node(router, "Input " + std::to_string(ch + 1), AM::RouterNode{{a, b}, {selected},
            {{AM::RouteBundleId{1}, {{a, selected}}}, {AM::RouteBundleId{2}, {{b, selected}}}},
            {.maxActiveBundles = 1, .maxSourcesPerOutput = 1}, true});
        s.routers.emplace(AM::NodeId{router}, AM::RouterState{{AM::RouteBundleId{native.input.sources[ch] == InputSource::Phone ? 2u : 1u}}});
        const auto gi = port(gain, AM::PortDirection::Input, "In"), go = port(gain, AM::PortDirection::Output, "Out");
        node(gain, "Gain " + std::to_string(ch + 1), AM::ProcessorNode{{gi}, {go}, true});
        const auto capture = port(1, AM::PortDirection::Input, "Capture", {AM::SignalKind::HostStream, ch + 1});
        t.fixedLinks.insert(t.fixedLinks.end(), {{xlr, a}, {jack, b}, {selected, gi}, {go, capture}});
        analog[ch] = go;
        hostPlayback[ch] = port(2, AM::PortDirection::Output, "Playback", {AM::SignalKind::HostStream, ch + 1});
        const bool instrument = native.input.sources[ch] == InputSource::Phone;
        const double offset = instrument ? 10 : 0;
        param(DuetControl::Gain + ch, go, AM::ParameterSemantic::Level, AM::ScalarDomain{10, 75, 1, AM::ScalarUnit::Decibels},
              "Gain " + std::to_string(ch + 1), double(native.input.gains[ch]) - offset);
        auto& gainState = s.parameters.at(AM::ParameterId{DuetControl::Gain + ch});
        gainState.domainOverride = AM::ScalarDomain{10 - offset, 75 - offset, 1, AM::ScalarUnit::Decibels};
        gainState.applicable = instrument || native.input.xlrNominalLevels[ch] == InputXlrNominalLevel::Microphone;
        param(DuetControl::Phantom + ch, xlr, AM::ParameterSemantic::PhantomPower, AM::BooleanDomain{}, "Phantom " + std::to_string(ch + 1), native.input.phantomPowerings[ch]);
        param(DuetControl::Polarity + ch, xlr, AM::ParameterSemantic::PhaseInvert, AM::BooleanDomain{}, "Polarity " + std::to_string(ch + 1), native.input.polarities[ch]);
        param(DuetControl::InputLevel + ch, xlr, AM::ParameterSemantic::NominalLevel, xlrLevels, "XLR Level " + std::to_string(ch + 1), int64_t(native.input.xlrNominalLevels[ch]));
    }
    AM::MixerNode mixer;
    for (uint32_t k = 0; k < 4; ++k) {
        const auto input = port(30, AM::PortDirection::Input, k < 2 ? "Analog" : "Playback");
        mixer.inputs.push_back(input);
        t.fixedLinks.push_back({k < 2 ? analog[k] : hostPlayback[k - 2], input});
    }
    for (uint32_t out = 0; out < 2; ++out) {
        mixed[out] = port(30, AM::PortDirection::Output, "Mix");
        mixer.outputs.push_back(mixed[out]);
        for (uint32_t in = 0; in < 4; ++in) {
            const AM::CrosspointId id{out * 4 + in + 1};
            mixer.crosspoints.push_back({id, mixer.inputs[in], mixed[out]});
            const auto coefficient = in < 2 ? native.mixer.outputs[out].analogInputs[in] : native.mixer.outputs[out].streamInputs[in - 2];
            param(DuetControl::Mixer + out * 4 + in, id, AM::ParameterSemantic::Level,
                  AM::ScalarDomain{double(ApogeeDuetSpec::kMixerSourceGain.min), double(ApogeeDuetSpec::kMixerSourceGain.max), 1, AM::ScalarUnit::Generic}, "Mixer " + std::to_string(in + 1) + " → " + std::to_string(out + 1), double(coefficient));
        }
    }
    node(30, "Monitor Mixer", std::move(mixer));
    AM::RouterNode source; AM::ProcessorNode output; output.channelPreserving = true;
    AM::RouteBundle stream{AM::RouteBundleId{1}}, mix{AM::RouteBundleId{2}};
    node(42, "Analog Output", AM::EndpointNode{AM::EndpointKind::Physical});
    for (uint32_t ch = 0; ch < 2; ++ch) {
        const auto a = port(40, AM::PortDirection::Input, "Stream"), b = port(40, AM::PortDirection::Input, "Mixer");
        const auto selected = port(40, AM::PortDirection::Output, "Selected");
        source.inputs.insert(source.inputs.end(), {a, b}); source.outputs.push_back(selected);
        stream.routes.push_back({a, selected}); mix.routes.push_back({b, selected});
        const auto in = port(41, AM::PortDirection::Input, "In"), out = port(41, AM::PortDirection::Output, "Out");
        output.inputs.push_back(in); output.outputs.push_back(out);
        const auto physical = port(42, AM::PortDirection::Input, "Line", {AM::SignalKind::AnalogLine, ch + 1});
        t.fixedLinks.insert(t.fixedLinks.end(), {{hostPlayback[ch], a}, {mixed[ch], b}, {selected, in}, {out, physical}});
    }
    source.legalBundles = {stream, mix}; source.constraints.maxActiveBundles = 1; source.constraints.maxSourcesPerOutput = 1;
    node(40, "Output Source", std::move(source)); node(41, "Output", std::move(output));
    s.routers.emplace(AM::NodeId{40}, AM::RouterState{{AM::RouteBundleId{native.output.source == OutputSource::MixerOutputPair0 ? 2u : 1u}}});
    param(DuetControl::OutputVolume, AM::NodeId{41}, AM::ParameterSemantic::Level, AM::ScalarDomain{-64, 0, 1, AM::ScalarUnit::Decibels}, "Output Volume", -double(native.output.volume));
    param(DuetControl::OutputMute, AM::NodeId{41}, AM::ParameterSemantic::Mute, AM::BooleanDomain{}, "Output Mute", native.output.mute);
    param(DuetControl::OutputSource, AM::NodeId{40}, AM::ParameterSemantic::Source, AM::EnumDomain{{{0, "Stream"}, {1, "Mixer"}}}, "Output Source", int64_t(native.output.source));
    param(DuetControl::OutputLevel, AM::NodeId{41}, AM::ParameterSemantic::NominalLevel, AM::EnumDomain{{{0, "Instrument"}, {1, "-10 dBV"}}}, "Output Level", int64_t(native.output.nominalLevel));
    const AM::EnumDomain muteModes{{{0, "Never"}, {1, "Normal"}, {2, "Swapped"}}};
    param(DuetControl::LineMuteMode, AM::NodeId{41}, AM::ParameterSemantic::Unknown, muteModes, "Line Mute Mode", int64_t(native.output.lineMuteMode));
    param(DuetControl::HeadphoneMuteMode, AM::NodeId{41}, AM::ParameterSemantic::Unknown, muteModes, "Headphone Mute Mode", int64_t(native.output.hpMuteMode));
    param(DuetControl::Clickless, AM::NodeId{1}, AM::ParameterSemantic::Unknown, AM::BooleanDomain{}, "Clickless", native.input.clickless);
    param(DuetControl::KnobTarget, AM::NodeId{1}, AM::ParameterSemantic::Unknown, AM::EnumDomain{{{0, "Output"}, {1, "Input 1"}, {2, "Input 2"}}}, "Knob Target", int64_t(native.knob.target));
    param(DuetControl::DisplayTarget, AM::NodeId{1}, AM::ParameterSemantic::Unknown, AM::EnumDomain{{{0, "Output"}, {1, "Input"}}}, "Display Target", int64_t(native.display.target));
    param(DuetControl::DisplayMode, AM::NodeId{1}, AM::ParameterSemantic::Unknown, AM::EnumDomain{{{0, "Independent"}, {1, "Follow Knob"}}}, "Display Mode", int64_t(native.display.mode));
    param(DuetControl::DisplayOverhold, AM::NodeId{1}, AM::ParameterSemantic::Unknown, AM::EnumDomain{{{0, "Infinite"}, {1, "Two Seconds"}}}, "Display Overhold", int64_t(native.display.overhold));
    t.parameters.push_back({AM::ParameterId{DuetControl::ClearDisplay}, AM::NodeId{1}, AM::ParameterSemantic::Unknown, AM::ActionDomain{}, "Clear Display"});
    return result;
}
} // namespace ASFW::Audio::Oxford::Apogee
