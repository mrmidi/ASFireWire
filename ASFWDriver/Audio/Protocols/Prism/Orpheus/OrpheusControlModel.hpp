// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "OrpheusTypes.hpp"
#include "../../../Controls/IControlProvider.hpp"
#include <string>

namespace ASFW::Audio::Prism::Orpheus {

namespace OrpheusControl {
inline constexpr uint32_t Gain = 0x4F520010;       // 'OR' (Orpheus)
inline constexpr uint32_t Phantom = 0x4F520020;
inline constexpr uint32_t Polarity = 0x4F520030;
inline constexpr uint32_t HighPassFilter = 0x4F520040;
inline constexpr uint32_t InputLevel = 0x4F520050;
inline constexpr uint32_t Overkiller = 0x4F520060;
inline constexpr uint32_t MidSide = 0x4F520070;

inline constexpr uint32_t OutputVolume = 0x4F520100;
inline constexpr uint32_t OutputMute = 0x4F520101;
inline constexpr uint32_t VolumeSelectMask = 0x4F520102;
inline constexpr uint32_t MasterLock = 0x4F520103;

inline constexpr uint32_t AdatMode = 0x4F520300;
inline constexpr uint32_t ClockSource = 0x4F520301;
inline constexpr uint32_t ClockOutput = 0x4F520302;
inline constexpr uint32_t SampleRateConverter = 0x4F520303;
inline constexpr uint32_t FrontPanelMeterMode = 0x4F520304;
inline constexpr uint32_t MeterBrightness = 0x4F520305;
inline constexpr uint32_t Identify = 0x4F520306;

inline constexpr uint32_t Mixer = 0x4F520400; // Crosspoints: bus * 12 + in
inline constexpr uint32_t MixerPan = 0x4F520500;
inline constexpr uint32_t MixerMute = 0x4F520600;
inline constexpr uint32_t MixerSolo = 0x4F520700;
inline constexpr uint32_t MixerBusGain = 0x4F520800;
inline constexpr uint32_t MixerBusMute = 0x4F520900;
inline constexpr uint32_t MixerDefeat = 0x4F520A00;

inline constexpr uint32_t InputRouter = 0x4F521000;
} // namespace OrpheusControl

struct OrpheusControlsState {
    OrpheusUnitState unit;
    std::array<OrpheusAnalogChannelState, 8> analog{};
    std::array<OrpheusMixBusState, 6> mixer{};
    OrpheusSyncSource syncSource{OrpheusSyncSource::Local};
    OrpheusFirmwareVersion firmware{1, 6};
};

inline Controls::ControlSnapshot BuildOrpheusControls(const OrpheusControlsState& native,
                                                      uint64_t revision,
                                                      uint64_t sequence) {
    namespace AM = ASFW::AudioModel;
    Controls::ControlSnapshot result;
    result.family = "orpheus";
    result.topology.revision = result.state.topologyRevision = revision;
    result.sequence = sequence;

    auto& t = result.topology;
    auto& s = result.state;
    uint32_t nextPort = 1;

    auto node = [&](uint32_t id, std::string name, AM::NodeBody body) {
        t.nodes.push_back({AM::NodeId{id}, std::move(name), std::move(body)});
    };

    auto port = [&](uint32_t owner, AM::PortDirection direction, std::string name, AM::SignalIdentity signal = {}) {
        const AM::PortId id{nextPort++};
        t.ports.push_back({id, AM::NodeId{owner}, direction, 1, std::move(name), signal});
        return id;
    };

    auto param = [&](uint32_t id, AM::ParameterTarget target, AM::ParameterSemantic semantic,
                     AM::ParameterDomain domain, std::string name, AM::ParameterValue value) {
        t.parameters.push_back({AM::ParameterId{id}, target, semantic, std::move(domain), std::move(name)});
        s.parameters.emplace(AM::ParameterId{id}, AM::ParameterState{std::move(value)});
    };

    const AM::EnumDomain xlrLevels{{{0, "Mic"}, {1, "+4 dBu"}, {2, "-10 dBV"}}};
    const AM::EnumDomain lineOnlyLevels{{{1, "+4 dBu"}, {0, "-10 dBV"}}};
    const AM::EnumDomain hpfChoices{{{0, "Flat"}, {1, "40 Hz"}, {2, "75 Hz"}, {3, "160 Hz"}}};

    // Host Endpoints
    node(1, "Host Capture", AM::EndpointNode{AM::EndpointKind::Host});
    node(2, "Host Playback", AM::EndpointNode{AM::EndpointKind::Host});
    node(40, "Device Settings", AM::EndpointNode{AM::EndpointKind::Host});

    std::array<AM::PortId, 8> analogInPorts{};
    std::array<AM::PortId, 8> hostPlaybackPorts{};

    // --- Analog Inputs 1 to 8 ---
    for (uint32_t ch = 0; ch < 8; ++ch) {
        const uint32_t physical = 10 + ch;
        const uint32_t router = OrpheusControl::InputRouter + ch;
        const uint32_t proc = 20 + ch;
        const auto capturePort = port(1, AM::PortDirection::Input, "Capture " + std::to_string(ch + 1),
                                      {AM::SignalKind::HostStream, ch + 1});

        if (ch < 2) {
            // Channels 1-2: Mic (XLR) or Instrument (1/4" Jack)
            node(physical, "Analog Input " + std::to_string(ch + 1), AM::EndpointNode{AM::EndpointKind::Physical});
            const auto xlr = port(physical, AM::PortDirection::Output, "XLR", {AM::SignalKind::AnalogMicXlr, ch + 1});
            const auto inst = port(physical, AM::PortDirection::Output, "Instrument", {AM::SignalKind::AnalogInstrument, ch + 1});

            const auto inA = port(router, AM::PortDirection::Input, "XLR");
            const auto inB = port(router, AM::PortDirection::Input, "Jack");
            const auto selected = port(router, AM::PortDirection::Output, "Selected");

            node(router, "Input " + std::to_string(ch + 1), AM::RouterNode{
                {inA, inB}, {selected},
                {{AM::RouteBundleId{1}, {{inA, selected}}}, {AM::RouteBundleId{2}, {{inB, selected}}}},
                {.maxActiveBundles = 1, .maxSourcesPerOutput = 1}, true
            });
            s.routers.emplace(AM::NodeId{router}, AM::RouterState{{AM::RouteBundleId{native.analog[ch].inputType == 2 ? 2u : 1u}}});

            const auto gi = port(proc, AM::PortDirection::Input, "In");
            const auto go = port(proc, AM::PortDirection::Output, "Out");
            node(proc, "Preamp " + std::to_string(ch + 1), AM::ProcessorNode{{gi}, {go}, true});

            t.fixedLinks.insert(t.fixedLinks.end(), {
                {xlr, inA}, {inst, inB}, {selected, gi}, {go, capturePort}
            });
            analogInPorts[ch] = go;

            // Parameters
            const bool isInst = (native.analog[ch].inputType == 2);
            const double gainVal = isInst ? native.analog[ch].instrumentGain : native.analog[ch].micGain;
            param(OrpheusControl::Gain + ch, go, AM::ParameterSemantic::Level,
                  AM::ScalarDomain{10, 65, 1, AM::ScalarUnit::Decibels},
                  "Gain " + std::to_string(ch + 1), gainVal);
            auto& gainState = s.parameters.at(AM::ParameterId{OrpheusControl::Gain + ch});
            gainState.applicable = (native.analog[ch].inputType == 0 || native.analog[ch].inputType == 2);

            param(OrpheusControl::Phantom + ch, xlr, AM::ParameterSemantic::PhantomPower, AM::BooleanDomain{},
                  "Phantom " + std::to_string(ch + 1), native.analog[ch].phantom);
            s.parameters.at(AM::ParameterId{OrpheusControl::Phantom + ch}).applicable = (native.analog[ch].inputType == 0);

            param(OrpheusControl::Polarity + ch, xlr, AM::ParameterSemantic::PhaseInvert, AM::BooleanDomain{},
                  "Polarity " + std::to_string(ch + 1), native.analog[ch].phase);
            param(OrpheusControl::HighPassFilter + ch, xlr, AM::ParameterSemantic::HighPassFilter, hpfChoices,
                  "Filter " + std::to_string(ch + 1), int64_t(native.analog[ch].filter));
            param(OrpheusControl::Overkiller + ch, xlr, AM::ParameterSemantic::Unknown, AM::BooleanDomain{},
                  "Overkiller " + std::to_string(ch + 1), native.analog[ch].overkiller);
            param(OrpheusControl::InputLevel + ch, xlr, AM::ParameterSemantic::NominalLevel, xlrLevels,
                  "XLR Level " + std::to_string(ch + 1), int64_t(native.analog[ch].lineInputLevel));
        } else if (ch < 4) {
            // Channels 3-4: Mic (XLR) or Line (TRS)
            node(physical, "Analog Input " + std::to_string(ch + 1), AM::EndpointNode{AM::EndpointKind::Physical});
            const auto xlr = port(physical, AM::PortDirection::Output, "XLR", {AM::SignalKind::AnalogMicXlr, ch + 1});
            const auto line = port(physical, AM::PortDirection::Output, "Line", {AM::SignalKind::AnalogLine, ch + 1});

            const auto inA = port(router, AM::PortDirection::Input, "XLR");
            const auto inB = port(router, AM::PortDirection::Input, "Line");
            const auto selected = port(router, AM::PortDirection::Output, "Selected");

            node(router, "Input " + std::to_string(ch + 1), AM::RouterNode{
                {inA, inB}, {selected},
                {{AM::RouteBundleId{1}, {{inA, selected}}}, {AM::RouteBundleId{2}, {{inB, selected}}}},
                {.maxActiveBundles = 1, .maxSourcesPerOutput = 1}, true
            });
            s.routers.emplace(AM::NodeId{router}, AM::RouterState{{AM::RouteBundleId{native.analog[ch].inputType == 1 ? 2u : 1u}}});

            const auto gi = port(proc, AM::PortDirection::Input, "In");
            const auto go = port(proc, AM::PortDirection::Output, "Out");
            node(proc, "Preamp " + std::to_string(ch + 1), AM::ProcessorNode{{gi}, {go}, true});

            t.fixedLinks.insert(t.fixedLinks.end(), {
                {xlr, inA}, {line, inB}, {selected, gi}, {go, capturePort}
            });
            analogInPorts[ch] = go;

            // Parameters
            param(OrpheusControl::Gain + ch, go, AM::ParameterSemantic::Level,
                  AM::ScalarDomain{10, 65, 1, AM::ScalarUnit::Decibels},
                  "Gain " + std::to_string(ch + 1), double(native.analog[ch].micGain));
            s.parameters.at(AM::ParameterId{OrpheusControl::Gain + ch}).applicable = (native.analog[ch].inputType == 0);

            param(OrpheusControl::Phantom + ch, xlr, AM::ParameterSemantic::PhantomPower, AM::BooleanDomain{},
                  "Phantom " + std::to_string(ch + 1), native.analog[ch].phantom);
            s.parameters.at(AM::ParameterId{OrpheusControl::Phantom + ch}).applicable = (native.analog[ch].inputType == 0);

            param(OrpheusControl::Polarity + ch, xlr, AM::ParameterSemantic::PhaseInvert, AM::BooleanDomain{},
                  "Polarity " + std::to_string(ch + 1), native.analog[ch].phase);
            param(OrpheusControl::HighPassFilter + ch, xlr, AM::ParameterSemantic::HighPassFilter, hpfChoices,
                  "Filter " + std::to_string(ch + 1), int64_t(native.analog[ch].filter));
            param(OrpheusControl::Overkiller + ch, xlr, AM::ParameterSemantic::Unknown, AM::BooleanDomain{},
                  "Overkiller " + std::to_string(ch + 1), native.analog[ch].overkiller);
            param(OrpheusControl::InputLevel + ch, xlr, AM::ParameterSemantic::NominalLevel, xlrLevels,
                  "XLR Level " + std::to_string(ch + 1), int64_t(native.analog[ch].lineInputLevel));
        } else {
            // Channels 5-8: Line only (TRS)
            node(physical, "Analog Input " + std::to_string(ch + 1), AM::EndpointNode{AM::EndpointKind::Physical});
            const auto line = port(physical, AM::PortDirection::Output, "Line", {AM::SignalKind::AnalogLine, ch + 1});

            const auto gi = port(proc, AM::PortDirection::Input, "In");
            const auto go = port(proc, AM::PortDirection::Output, "Out");
            node(proc, "Line Input " + std::to_string(ch + 1), AM::ProcessorNode{{gi}, {go}, true});

            t.fixedLinks.insert(t.fixedLinks.end(), {
                {line, gi}, {go, capturePort}
            });
            analogInPorts[ch] = go;

            param(OrpheusControl::InputLevel + ch, line, AM::ParameterSemantic::NominalLevel, lineOnlyLevels,
                  "Line Level " + std::to_string(ch + 1), int64_t(native.analog[ch].lineInputLevel));
            param(OrpheusControl::Overkiller + ch, line, AM::ParameterSemantic::Unknown, AM::BooleanDomain{},
                  "Overkiller " + std::to_string(ch + 1), native.analog[ch].overkiller);
        }
    }

    // --- Master Output Section ---
    node(50, "Master Output", AM::ProcessorNode{});
    auto& outProc = std::get<AM::ProcessorNode>(t.nodes.back().body);
    outProc.channelPreserving = true;

    node(51, "Analog Output", AM::EndpointNode{AM::EndpointKind::Physical});

    for (uint32_t ch = 0; ch < 8; ++ch) {
        hostPlaybackPorts[ch] = port(2, AM::PortDirection::Output, "Playback " + std::to_string(ch + 1),
                                     {AM::SignalKind::HostStream, ch + 1});
        const auto inP = port(50, AM::PortDirection::Input, "In " + std::to_string(ch + 1));
        const auto outP = port(50, AM::PortDirection::Output, "Out " + std::to_string(ch + 1));
        const auto physOut = port(51, AM::PortDirection::Input, "Line " + std::to_string(ch + 1),
                                  {AM::SignalKind::AnalogLine, ch + 1});

        outProc.inputs.push_back(inP);
        outProc.outputs.push_back(outP);
        t.fixedLinks.insert(t.fixedLinks.end(), {
            {hostPlaybackPorts[ch], inP}, {outP, physOut}
        });
    }

    param(OrpheusControl::OutputVolume, AM::NodeId{50}, AM::ParameterSemantic::Level,
          AM::ScalarDomain{-64, 0, 1, AM::ScalarUnit::Decibels}, "Output Volume",
          double(native.unit.masterVolume) / 256.0);
    param(OrpheusControl::OutputMute, AM::NodeId{50}, AM::ParameterSemantic::Mute,
          AM::BooleanDomain{}, "Output Mute", native.unit.masterMute);
    param(OrpheusControl::MasterLock, AM::NodeId{50}, AM::ParameterSemantic::Unknown,
          AM::BooleanDomain{}, "Master Lock", native.unit.masterLock);

    // --- 6-Bus Hardware Mixer ---
    AM::MixerNode mixerNode;
    // 12 Inputs: 8 Analog In + 2 Playback (DAW 1/2) + 2 extra
    for (uint32_t in = 0; in < 12; ++in) {
        std::string name;
        if (in < 8) name = "Analog " + std::to_string(in + 1);
        else if (in < 10) name = "DAW " + std::to_string(in - 7);
        else name = "Input " + std::to_string(in + 1);

        const auto inputP = port(60, AM::PortDirection::Input, name);
        mixerNode.inputs.push_back(inputP);
        if (in < 8) {
            t.fixedLinks.push_back({analogInPorts[in], inputP});
        } else if (in < 10) {
            t.fixedLinks.push_back({hostPlaybackPorts[in - 8], inputP});
        }
    }

    const std::array<std::string, 6> busNames{"AO 1/2", "AO 3/4", "AO 5/6", "AO 7/8", "DO 1/2", "HP 1/2"};
    for (uint32_t bus = 0; bus < 6; ++bus) {
        const auto mixOut = port(60, AM::PortDirection::Output, "Mix " + busNames[bus]);
        mixerNode.outputs.push_back(mixOut);

        for (uint32_t in = 0; in < 12; ++in) {
            const AM::CrosspointId cpId{bus * 12 + in + 1};
            mixerNode.crosspoints.push_back({cpId, mixerNode.inputs[in], mixOut});

            const int16_t gainRaw = native.mixer[bus].inputGains[in];
            param(OrpheusControl::Mixer + bus * 12 + in, cpId, AM::ParameterSemantic::Level,
                  AM::ScalarDomain{-88, 0, 1, AM::ScalarUnit::Decibels},
                  busNames[bus] + " \u2190 " + (in < 8 ? "AI " + std::to_string(in + 1) : "DAW " + std::to_string(in - 7)),
                  double(gainRaw) / 256.0);
        }
    }
    node(60, "Hardware Mixer", std::move(mixerNode));

    // --- Device Settings Node ---
    const AM::EnumDomain adatModes{{{0, "None"}, {1, "Send"}, {2, "Return"}, {3, "Send/Return"}, {4, "Direct"}}};
    const AM::EnumDomain syncSources{{{0, "CSP"}, {1, "FireWire"}, {2, "Local"}, {3, "Digital Input"}, {4, "ADAT"}, {5, "Word Clock"}}};
    const AM::EnumDomain clockOutputs{{{0, "fs (word clock)"}, {1, "Base clock"}, {2, "256x clock"}}};
    const AM::EnumDomain srcModes{{{0, "Disabled"}, {1, "Digital Input"}, {2, "Digital Output"}}};

    param(OrpheusControl::AdatMode, AM::NodeId{40}, AM::ParameterSemantic::Unknown,
          adatModes, "ADAT Mode", int64_t(native.unit.adatMode));
    param(OrpheusControl::ClockSource, AM::NodeId{40}, AM::ParameterSemantic::ClockSource,
          syncSources, "Sync Source", int64_t(native.syncSource));
    param(OrpheusControl::ClockOutput, AM::NodeId{40}, AM::ParameterSemantic::Unknown,
          clockOutputs, "Word Clock Output", int64_t(native.unit.clockOutput));
    param(OrpheusControl::SampleRateConverter, AM::NodeId{40}, AM::ParameterSemantic::Unknown,
          srcModes, "Sample Rate Converter", int64_t(native.unit.sampleRateConverter));
    param(OrpheusControl::MeterBrightness, AM::NodeId{40}, AM::ParameterSemantic::Unknown,
          AM::ScalarDomain{0, 15, 1, AM::ScalarUnit::Generic}, "Meter Brightness", double(native.unit.meterBrightness));

    return result;
}

} // namespace ASFW::Audio::Prism::Orpheus
