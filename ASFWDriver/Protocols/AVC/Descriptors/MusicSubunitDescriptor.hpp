// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicSubunitDescriptor.hpp - Strongly typed parser and data models for
// the AV/C Music Subunit Status Descriptor (specifier 0x80) and nested Info Blocks.
//
// Specification:
// - TA Document 2001007 - AV/C Music Subunit 1.0 (Clauses 5.3 & 6)
// - TA Document 1999045 - AV/C Information Block Types
//

#pragma once

#include "AVCInfoBlock.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Descriptors {

//==============================================================================
// Music Subunit Capabilities (Info Blocks 0x8100 - 0x8105)
//==============================================================================

struct MusicCapabilities {
    // 0x8100: General Music Subunit Status Area (GMSSA)
    bool hasGeneralCapability{false};
    uint8_t transmitCapabilityFlags{0};
    uint8_t receiveCapabilityFlags{0};
    std::optional<uint32_t> latencyCapability{};

    // 0x8101: Audio Capability Status Area
    bool hasAudioCapability{false};
    std::optional<uint16_t> maxAudioInputChannels{};
    std::optional<uint16_t> maxAudioOutputChannels{};

    // 0x8102: MIDI Capability Status Area
    bool hasMidiCapability{false};
    uint8_t midiVersionMajor{0};
    uint8_t midiVersionMinor{0};
    uint8_t midiAdaptationLayerVersion{0};
    std::optional<uint16_t> maxMidiInputPorts{};
    std::optional<uint16_t> maxMidiOutputPorts{};

    // 0x8103: SMPTE Time Code Capability
    bool hasSmpteTimeCodeCapability{false};
    uint8_t smpteTimeCodeCapabilityFlags{0};

    // 0x8104: Sample Count Capability
    bool hasSampleCountCapability{false};
    uint8_t sampleCountCapabilityFlags{0};

    // 0x8105: Audio Sync Capability
    bool hasAudioSyncCapability{false};
    uint8_t audioSyncCapabilityFlags{0};
};

//==============================================================================
// Music Routing & Cluster Models (Info Blocks 0x8108, 0x8109, 0x810A, 0x810B)
//==============================================================================

struct MusicClusterSignal {
    uint16_t musicPlugId{0};
    uint8_t position{0};
};

struct MusicClusterInfo {
    uint8_t streamFormatCode{0};
    uint8_t channelCount{0};
    std::string name;
    std::vector<MusicClusterSignal> signals;
};

struct MusicSubunitPlug {
    uint8_t plugId{0};
    uint8_t usage{0};
    bool isDestination{false}; ///< true = input (dest), false = output (src)
    std::string name;
    std::vector<MusicClusterInfo> clusters;
};

struct MusicPlugDetail {
    uint16_t musicPlugId{0};
    uint8_t portType{0};
    std::string name;
};

//==============================================================================
// Music Subunit Status Descriptor (Top-Level Parsed Model)
//==============================================================================

struct MusicSubunitStatus {
    uint16_t declaredLength{0};
    MusicCapabilities capabilities;
    uint8_t numDestPlugs{0};
    uint8_t numSrcPlugs{0};
    bool hasRoutingStatus{false};

    std::vector<MusicSubunitPlug> plugs;
    std::vector<MusicPlugDetail> musicPlugs;

    [[nodiscard]] const MusicSubunitPlug* FindPlug(uint8_t plugId, bool isDest) const noexcept;
    [[nodiscard]] const MusicPlugDetail* FindMusicPlug(uint16_t musicPlugId) const noexcept;
};

//==============================================================================
// Music Subunit Descriptor Parser
//==============================================================================

class MusicSubunitDescriptorParser {
public:
    [[nodiscard]] static std::optional<MusicSubunitStatus> ParseStatusDescriptor(
        std::span<const uint8_t> data) noexcept;

    [[nodiscard]] static std::string ExtractName(const AVCInfoBlock& block) noexcept;
};

} // namespace ASFW::Protocols::AVC::Descriptors
