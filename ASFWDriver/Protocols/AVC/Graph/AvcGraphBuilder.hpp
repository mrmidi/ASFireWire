// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcGraphBuilder.hpp - AppleFWAudio graph rules implementation.
// Synthesizes device stream geometry, channel mappings, channel names,
// clock sources, and control graphs from AV/C descriptors.
//
// References:
// - Apple AppleFWAudio graph rules (docs/avc-rebuild/applefwaudio-graph-rules.md)
// - Prototype graph_build.py (docs/avc-rebuild/fixtures/graph_build.py)
//

#pragma once

#include "AvcDeviceGraph.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ASFW::Protocols::AVC::Graph {

struct GraphBuildOptions {
    std::string modelName;
    uint32_t playbackDataBlockSize{0};
    uint32_t captureDataBlockSize{0};
    std::optional<uint8_t> playbackSubunitDestPlugId{};  ///< Override from SIGNAL SOURCE (dest plug)
    std::optional<uint8_t> captureSubunitSourcePlugId{}; ///< Override from SIGNAL SOURCE (src plug)
    bool allowDefaultPlugSelection{true}; ///< False for production when SIGNAL SOURCE is unavailable.
    /// Sources returned by completed SPECIFIC INQUIRY probes. A descriptor
    /// sync destination or selector declaration alone must not populate this.
    std::vector<ClockSourceInfo> confirmedClockSources{};
    uint8_t audioSubunitId{0};
    struct ConfirmedFeatureControls {
        uint8_t audioSubunitId{0};
        uint8_t functionBlockId{0};
        ConfirmedFeatureStatus status{};
    };
    /// Feature controls explicitly confirmed by successful STATUS discovery.
    /// Entries are keyed by the typed Audio Feature FB identity.
    std::vector<ConfirmedFeatureControls> confirmedFeatureControls{};
};

class AvcGraphBuilder {
public:
    using Options = GraphBuildOptions;

    /// Build a complete DeviceGraph from parsed Music Subunit Status and optional Audio Subunit Identifier.
    [[nodiscard]] static DeviceGraph BuildGraph(
        const Descriptors::MusicSubunitStatus& musicStatus,
        const Descriptors::AudioSubunitIdentifier* audioIdentifier = nullptr,
        const Options& options = {}) noexcept;

    /// Build a StreamGraph for a single MusicSubunitPlug using AppleFWAudio rules:
    /// - Clusters processed in order.
    /// - Stream positions (AM824 slots) concatenated for audio channels.
    /// - MIDI clusters identified by port type 0x0A or stream format 0x0D.
    /// - Rejection: if any slot >= dataBlockSize, falls back to identity.
    /// - Names: music plug name -> cluster name -> per-plug name list -> fallback.
    [[nodiscard]] static StreamGraph BuildStreamGraph(
        const Descriptors::MusicSubunitPlug& plug,
        const Descriptors::MusicSubunitStatus& musicStatus,
        uint32_t dataBlockSize = 0) noexcept;
};

} // namespace ASFW::Protocols::AVC::Graph
