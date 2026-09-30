// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcGraphBuilder.cpp - AppleFWAudio graph rules implementation.
//

#include "AvcGraphBuilder.hpp"

#include <algorithm>

namespace ASFW::Protocols::AVC::Graph {

StreamGraph AvcGraphBuilder::BuildStreamGraph(
    const Descriptors::MusicSubunitPlug& plug,
    const Descriptors::MusicSubunitStatus& musicStatus,
    uint32_t dataBlockSize) noexcept {

    StreamGraph sg;
    sg.subunitPlugId = plug.plugId;
    sg.isDestination = plug.isDestination;

    std::vector<uint8_t> slots;

    for (const auto& cluster : plug.clusters) {
        // Detect MIDI: port type 0x0A (MIDI) or stream format 0x0D (AM824 MIDI)
        const bool isMidi = (cluster.portType == 0x0A || cluster.streamFormatCode == 0x0D);
        if (isMidi) {
            sg.midiStreamCount += cluster.channelCount;
            continue;
        }

        // Process audio signals
        for (const auto& signal : cluster.signals) {
            StreamChannelInfo info;
            info.logicalIndex = static_cast<uint32_t>(sg.channels.size());
            info.slotIndex = signal.position;
            info.clusterName = cluster.name;
            info.formatCode = cluster.streamFormatCode;

            // Name Resolution per AppleFWAudio rules and fixture behavior:
            // 1. Music Plug name (e.g. Duet: "Analog Out 1", "Analog In 1")
            const auto* mp = musicStatus.FindMusicPlug(signal.musicPlugId);
            if (mp && !mp->name.empty()) {
                info.name = mp->name;
            }

            // 2. Per-plug channel name list (e.g. Phase 88 from 0x8101 -> 0x8102 -> 0x8103)
            if (info.name.empty()) {
                const auto it = musicStatus.perPlugChannelNames.find(plug.plugId);
                if (it != musicStatus.perPlugChannelNames.end() &&
                    info.logicalIndex < it->second.size()) {
                    info.name = it->second[info.logicalIndex];
                }
            }

            // 3. Cluster name (e.g. "Line In 1/2", "Analog Out")
            if (info.name.empty() && !cluster.name.empty()) {
                info.name = cluster.name;
            }

            // 4. Default fallback name
            if (info.name.empty()) {
                info.name = "Channel " + std::to_string(info.logicalIndex + 1);
            }

            slots.push_back(signal.position);
            sg.channelNames.push_back(info.name);
            sg.channels.push_back(std::move(info));
        }
    }

    sg.channelCount = static_cast<uint32_t>(sg.channels.size());

    // Slot validation per Apple rules:
    // If dataBlockSize > 0 and any mapped slot >= dataBlockSize, reject descriptor map.
    bool rejectMap = false;
    if (dataBlockSize > 0) {
        for (uint8_t slot : slots) {
            if (slot >= dataBlockSize) {
                rejectMap = true;
                break;
            }
        }
    }

    if (rejectMap) {
        sg.usingFallbackMap = true;
        sg.slotMap = {}; // default identity
    } else if (!slots.empty()) {
        bool isIdentity = true;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i] != static_cast<uint8_t>(i)) {
                isIdentity = false;
                break;
            }
        }
        if (!isIdentity) {
            if (!sg.slotMap.SetSlots(slots)) {
                sg.usingFallbackMap = true;
                sg.slotMap = {};
            } else {
                sg.slotMap.channelCount = sg.channelCount;
            }
        } else {
            sg.slotMap = {}; // default identity
        }
    }

    return sg;
}

DeviceGraph AvcGraphBuilder::BuildGraph(
    const Descriptors::MusicSubunitStatus& musicStatus,
    const Descriptors::AudioSubunitIdentifier* audioIdentifier,
    const Options& options) noexcept {

    DeviceGraph dg;
    dg.modelName = options.modelName;

    // 1. Find Playback plug (destination plug)
    const Descriptors::MusicSubunitPlug* playbackPlug = nullptr;
    if (options.playbackSubunitDestPlugId.has_value()) {
        playbackPlug = musicStatus.FindPlug(*options.playbackSubunitDestPlugId, true);
    } else {
        // Default: find plug 0 or first destination plug
        playbackPlug = musicStatus.FindPlug(0, true);
        if (!playbackPlug) {
            for (const auto& plug : musicStatus.plugs) {
                if (plug.isDestination) {
                    playbackPlug = &plug;
                    break;
                }
            }
        }
    }

    if (playbackPlug) {
        dg.playback = BuildStreamGraph(*playbackPlug, musicStatus, options.playbackDataBlockSize);
    }

    // 2. Find Capture plug (source plug)
    const Descriptors::MusicSubunitPlug* capturePlug = nullptr;
    if (options.captureSubunitSourcePlugId.has_value()) {
        capturePlug = musicStatus.FindPlug(*options.captureSubunitSourcePlugId, false);
    } else {
        // Default: find plug 0 or first source plug
        capturePlug = musicStatus.FindPlug(0, false);
        if (!capturePlug) {
            for (const auto& plug : musicStatus.plugs) {
                if (!plug.isDestination) {
                    capturePlug = &plug;
                    break;
                }
            }
        }
    }

    if (capturePlug) {
        dg.capture = BuildStreamGraph(*capturePlug, musicStatus, options.captureDataBlockSize);
    }

    // 3. Discover clock sources
    for (const auto& plug : musicStatus.plugs) {
        if (plug.isDestination) {
            for (const auto& cluster : plug.clusters) {
                if (cluster.streamFormatCode == 0x40 || cluster.portType == 0x09) {
                    ClockSourceInfo clk;
                    clk.name = plug.name.empty() ? ("Sync Dest Plug " + std::to_string(plug.plugId))
                                                 : plug.name;
                    clk.subunitPlugId = plug.plugId;
                    dg.clockSources.push_back(std::move(clk));
                    break;
                }
            }
        }
    }

    // 4. Function blocks and controls
    if (audioIdentifier) {
        for (const auto& fb : audioIdentifier->functionBlocks) {
            ControlBlockInfo cbi;
            cbi.type = fb.type;
            cbi.id = fb.id;
            cbi.name = fb.name;
            cbi.channelCount = fb.clusterChannels;
            cbi.masterControls = fb.masterControls;
            cbi.channelControls = fb.channelControls;
            cbi.inputSources = fb.inputSources;

            if (fb.type == Descriptors::AudioFunctionBlockType::kFeature) {
                if (fb.generalTag == 1 || fb.name == "Mixer Output Level" || fb.name == "Master") {
                    cbi.isMasterVolume = true;
                }
            }

            // Register clock selectors as sync source options
            if (fb.type == Descriptors::AudioFunctionBlockType::kSelector &&
                (fb.name.find("Clock") != std::string::npos || fb.name.find("clock") != std::string::npos)) {
                ClockSourceInfo clk;
                clk.name = fb.name;
                clk.subunitPlugId = 0xFF;
                dg.clockSources.push_back(std::move(clk));
            }

            dg.controls.push_back(std::move(cbi));
        }
    }

    return dg;
}

} // namespace ASFW::Protocols::AVC::Graph
