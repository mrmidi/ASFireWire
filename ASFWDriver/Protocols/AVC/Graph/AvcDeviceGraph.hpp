// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcDeviceGraph.hpp - Device topology and graph representation derived
// from AV/C Music Subunit Status and Audio Subunit Identifier descriptors.
//
// References:
// - Apple AppleFWAudio graph rules (applefwaudio-graph-rules.md)
// - TA Document 2001007 (AV/C Music Subunit 1.0)
// - TA Document 1999008 (AV/C Audio Subunit 1.0)
//

#pragma once

#include "../../../Audio/Wire/AMDTP/PcmSlotMap.hpp"
#include "../Descriptors/AudioSubunitDescriptor.hpp"
#include "../Descriptors/MusicSubunitDescriptor.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Graph {

/// A single audio channel in a stream
struct StreamChannelInfo {
    uint32_t logicalIndex{0};       ///< 0-based audio channel index
    uint8_t slotIndex{0};           ///< AM824 stream position / slot index
    std::string name;               ///< Resolved channel name
    std::string clusterName;        ///< Name of the containing cluster
    uint8_t formatCode{0};          ///< Stream format code (e.g. 0x06 MBLA)
};

/// A stream direction (playback or capture)
struct StreamGraph {
    uint8_t subunitPlugId{0};
    bool isDestination{false};      ///< true = playback (dest plug), false = capture (src plug)
    uint32_t channelCount{0};       ///< Total PCM audio channels
    uint32_t midiStreamCount{0};    ///< MIDI port/stream count
    Audio::Wire::PcmSlotMap slotMap; ///< Mapped AM824 slots for PCM channels
    std::vector<StreamChannelInfo> channels;
    std::vector<std::string> channelNames;
    bool usingFallbackMap{false};   ///< true if descriptor map was rejected or unavailable
};

/// A discovered clock/synchronization source
struct ClockSourceInfo {
    std::string name;
    uint8_t subunitPlugId{0xFF};
    bool isCurrent{false};
};

/// A discovered function block control
struct ControlBlockInfo {
    Descriptors::AudioFunctionBlockType type{Descriptors::AudioFunctionBlockType::kFeature};
    uint8_t id{0};
    std::string name;
    uint8_t channelCount{0};
    uint16_t masterControls{0};     ///< Bit 0: Mute, Bit 1: Volume
    std::vector<uint16_t> channelControls;
    std::vector<Descriptors::AudioSourceId> inputSources;
    bool isMasterVolume{false};
};

/// Complete device graph built from descriptor discovery
struct DeviceGraph {
    std::string modelName;
    StreamGraph playback;
    StreamGraph capture;
    std::vector<ClockSourceInfo> clockSources;
    std::vector<ControlBlockInfo> controls;
};

} // namespace ASFW::Protocols::AVC::Graph
