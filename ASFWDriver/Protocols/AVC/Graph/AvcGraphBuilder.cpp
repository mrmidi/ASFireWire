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
    sg.clusters = plug.clusters;

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
            info.musicPlugId = signal.musicPlugId;
            info.clusterName = cluster.name;
            info.formatCode = cluster.streamFormatCode;

            // Name Resolution per AppleFWAudio rules and fixture behavior:
            // 1. Music Plug name (e.g. Duet: "Analog Out 1", "Analog In 1")
            const auto* mp = musicStatus.FindMusicPlug(signal.musicPlugId);
            if (mp && !mp->name.empty()) {
                info.name = mp->name;
            }

            // 2. The music plug's audio stream label (Phase 88: the source plugs'
            //    audio info blocks, TA 2001007 §6.2.3.1). Keyed by music plug, so
            //    a playback channel gets the label of the output it is routed to.
            if (info.name.empty()) {
                const auto it = musicStatus.musicPlugLabels.find(signal.musicPlugId);
                if (it != musicStatus.musicPlugLabels.end()) {
                    info.name = it->second;
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
        sg.slotMapValidation = SlotMapValidation::kRejectedFallback;
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
                sg.slotMapValidation = dataBlockSize > 0 ? SlotMapValidation::kValidated
                                                         : SlotMapValidation::kNoDataBlockSize;
            }
        } else {
            sg.slotMap = {}; // default identity
            sg.slotMapValidation = dataBlockSize > 0 ? SlotMapValidation::kValidated
                                                     : SlotMapValidation::kNoDataBlockSize;
        }
    } else if (dataBlockSize > 0) {
        sg.slotMapValidation = SlotMapValidation::kValidated;
    }

    return sg;
}

DeviceGraph AvcGraphBuilder::BuildGraph(
    const Descriptors::MusicSubunitStatus& musicStatus,
    const Descriptors::AudioSubunitIdentifier* audioIdentifier,
    const Options& options) noexcept {

    DeviceGraph dg;
    dg.supportsBlockingTransmit = (musicStatus.capabilities.transmitCapabilityFlags & 0x02) != 0;
    dg.modelName = options.modelName;

    // 1. Find Playback plug (destination plug)
    const Descriptors::MusicSubunitPlug* playbackPlug = nullptr;
    if (options.playbackSubunitDestPlugId.has_value()) {
        playbackPlug = musicStatus.FindPlug(*options.playbackSubunitDestPlugId, true);
        if (playbackPlug) dg.playback.selectionEvidence = StreamSelectionEvidence::kSignalSourceInquiry;
    } else if (options.allowDefaultPlugSelection) {
        dg.playback.selectionEvidence = StreamSelectionEvidence::kDescriptorDefaultAssumption;
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
        const auto evidence = dg.playback.selectionEvidence;
        dg.playback = BuildStreamGraph(*playbackPlug, musicStatus, options.playbackDataBlockSize);
        dg.playback.selectionEvidence = evidence;
    }

    // 2. Find Capture plug (source plug)
    const Descriptors::MusicSubunitPlug* capturePlug = nullptr;
    if (options.captureSubunitSourcePlugId.has_value()) {
        capturePlug = musicStatus.FindPlug(*options.captureSubunitSourcePlugId, false);
        if (capturePlug) dg.capture.selectionEvidence = StreamSelectionEvidence::kSignalSourceInquiry;
    } else if (options.allowDefaultPlugSelection) {
        dg.capture.selectionEvidence = StreamSelectionEvidence::kDescriptorDefaultAssumption;
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
        const auto evidence = dg.capture.selectionEvidence;
        dg.capture = BuildStreamGraph(*capturePlug, musicStatus, options.captureDataBlockSize);
        dg.capture.selectionEvidence = evidence;
    }

    // Keep sync destinations separate from clock sources. A destination
    // carrying sync content does not establish a selectable source.
    for (const auto& plug : musicStatus.plugs) {
        if (!plug.isDestination) continue;
        for (const auto& cluster : plug.clusters) {
            if (cluster.streamFormatCode == 0x40) {
                dg.syncDestinations.push_back(SyncDestinationInfo{
                    .subunitPlugId = plug.plugId,
                    .name = plug.name,
                });
                break;
            }
        }
    }
    dg.clockSources = options.confirmedClockSources;

    // 4. Function blocks and controls
    if (audioIdentifier) {
        for (const auto& fb : audioIdentifier->functionBlocks) {
            ControlBlockInfo cbi;
            cbi.type = fb.type;
            cbi.id = fb.id;
            cbi.name = fb.name;
            cbi.channelCount = fb.clusterChannels;
            cbi.advertisedMasterControls = fb.masterControls;
            cbi.advertisedChannelControls = fb.channelControls;
            cbi.inputSources = fb.inputSources;

            if (fb.type == Descriptors::AudioFunctionBlockType::kSelector) {
                dg.selectors.push_back(AudioSelectorInfo{
                    .audioSubunitId = options.audioSubunitId,
                    .functionBlockId = fb.id,
                    .name = fb.name,
                    .declaredInputs = fb.inputSources,
                });
            }

            if (fb.type == Descriptors::AudioFunctionBlockType::kFeature) {
                const auto confirmed = std::find_if(
                    options.confirmedFeatureControls.begin(), options.confirmedFeatureControls.end(),
                    [&fb, &options](const auto& item) {
                        return item.audioSubunitId == options.audioSubunitId &&
                               item.functionBlockId == fb.id;
                    });
                if (confirmed != options.confirmedFeatureControls.end()) {
                    cbi.confirmedControls = confirmed->status;
                }
                cbi.isMasterVolume = fb.generalTag == 1 &&
                    cbi.confirmedControls.state == FeatureStatusState::kConfirmed &&
                    std::find(cbi.confirmedControls.master.begin(), cbi.confirmedControls.master.end(),
                              ConfirmedFeatureControl::kVolume) != cbi.confirmedControls.master.end();
            }

            dg.controls.push_back(std::move(cbi));
        }
    }

    return dg;
}

} // namespace ASFW::Protocols::AVC::Graph
