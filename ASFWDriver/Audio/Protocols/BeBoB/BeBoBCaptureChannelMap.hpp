// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBCaptureChannelMap.hpp — BridgeCo plug-0 capture permutation.

#pragma once

#include "../../Engine/Direct/Rx/RxCaptureChannelMap.hpp"
#include "../../Wire/AMDTP/PcmSlotMap.hpp"
#include "BeBoBPlug0StreamDiscovery.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ASFW::Audio::BeBoBProbe {

/// Builds the logical PCM-channel -> AM824-slot map described by BridgeCo's
/// channel-position and section-type replies for the device-to-host plug.
///
/// Cross-validated with Linux `firewire/bebob/bebob_stream.c:254-370`: PCM
/// section locations establish the logical channel, stream positions select the
/// AM824 data block, and MIDI sections do not consume a PCM channel. Unlike
/// Linux, malformed or duplicate positions fail closed to identity rather than
/// allowing an accidental overwrite to relabel audio.
[[nodiscard]] inline Wire::PcmSlotMap ChannelMapFromProbe(
    const BeBoB::IsochronousPlugModel& capturePlug,
    uint32_t pcmChannels, uint32_t dataBlockSize) noexcept {
    using Map = Wire::PcmSlotMap;
    constexpr uint8_t kMidiSectionType = 0x0a;
    if (pcmChannels == 0 || pcmChannels > Encoding::kMaxPcmChannels ||
        dataBlockSize < pcmChannels || capturePlug.channelSections.empty()) {
        return {};
    }

    std::array<uint8_t, Encoding::kMaxPcmChannels> slots{};
    std::array<bool, Encoding::kMaxPcmChannels> assigned{};
    uint32_t pcmOffset = 0;
    for (const auto& section : capturePlug.channelSections) {
        if (!section.type.has_value()) return {};
        const uint32_t sectionChannels = static_cast<uint32_t>(section.positions.size());
        if (*section.type == kMidiSectionType) continue;
        if (sectionChannels == 0 || pcmOffset + sectionChannels > pcmChannels) return {};

        for (uint32_t source = 0; source < sectionChannels; ++source) {
            const auto& position = section.positions[source];
            // Linux treats an out-of-section location as the source position;
            // keep that compatibility rule while retaining full permutation
            // validation below.
            const uint32_t location = position.sectionLocation < sectionChannels
                ? position.sectionLocation : source;
            const uint32_t channel = pcmOffset + location;
            if (channel >= pcmChannels || assigned[channel] ||
                position.streamPosition >= dataBlockSize) {
                return {};
            }
            slots[channel] = position.streamPosition;
            assigned[channel] = true;
        }
        pcmOffset += sectionChannels;
    }

    if (pcmOffset != pcmChannels) return {};
    bool identity = true;
    for (uint32_t channel = 0; channel < pcmChannels; ++channel) {
        if (!assigned[channel]) return {};
        identity = identity && slots[channel] == channel;
    }
    if (identity) return {};

    Map map{};
    if (!map.SetSlots(std::span<const uint8_t>{slots.data(), pcmChannels})) return {};
    map.channelCount = pcmChannels;
    return map;
}

[[nodiscard]] inline AudioEngine::Direct::Rx::RxCaptureChannelMap
CaptureChannelMapFromProbe(
    const BeBoB::IsochronousPlugModel& capturePlug,
    uint32_t pcmChannels, uint32_t dataBlockSize) noexcept {
    AudioEngine::Direct::Rx::RxCaptureChannelMap captureMap{};
    static_cast<Wire::PcmSlotMap&>(captureMap) =
        ChannelMapFromProbe(capturePlug, pcmChannels, dataBlockSize);
    return captureMap;
}

[[nodiscard]] inline Wire::PcmSlotMap PlaybackChannelMapFromProbe(
    const BeBoB::IsochronousPlugModel& playbackPlug,
    uint32_t pcmChannels, uint32_t dataBlockSize) noexcept {
    return ChannelMapFromProbe(playbackPlug, pcmChannels, dataBlockSize);
}

} // namespace ASFW::Audio::BeBoBProbe
