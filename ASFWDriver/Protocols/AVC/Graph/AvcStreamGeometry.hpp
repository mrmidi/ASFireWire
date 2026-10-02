// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "AvcDeviceGraph.hpp"
#include "../Commands/StreamFormatCommand.hpp"
#include "../Core/RateCodes.hpp"
#include <optional>

namespace ASFW::Protocols::AVC::Graph {

// PCM/MIDI counts follow Linux sound/firewire/oxfw/oxfw-stream.c:552-622.
// A MIDI-conformant entry counts MPX-MIDI data channels (one slot each, eight
// ports per slot: oxfw-stream.c:246); Linux accepts at most one
// (amdtp-am824.h:28 AM824_MAX_CHANNELS_FOR_MIDI, oxfw-stream.c:621).
inline constexpr uint32_t kMaxMidiDataChannels = 1;

// Without descriptor routing, use Linux's PCM-first default slot layout
// (sound/firewire/amdtp-am824.c:99-102); format entries are not a channel map.
[[nodiscard]] inline std::optional<StreamGraph> BuildUnitStreamGeometry(
    const ASFW::AVC::Cmd::StreamFormat& format, bool playback) {
    if (format.kind != ASFW::AVC::Cmd::StreamFormat::Kind::kCompoundAm824 ||
        !format.compound.OnlyPcmAndMidi()) return std::nullopt;
    const auto& compound = format.compound;
    const auto rate = ASFW::AVC::ToHz(compound.rate);
    const auto pcm = compound.PcmChannels();
    const auto dbs = pcm + compound.MidiChannels();
    if (!rate || pcm == 0 || pcm > ASFW::Common::kMaxPcmSlots ||
        compound.MidiChannels() > kMaxMidiDataChannels || dbs > 255)
        return std::nullopt;
    StreamGraph stream;
    stream.selectionEvidence = StreamSelectionEvidence::kUnitPlugFormat;
    stream.isDestination = playback;
    stream.dataBlockSize = dbs;
    stream.currentSampleRate = *rate;
    stream.supportedSampleRates = {*rate};
    stream.channelCount = pcm;
    stream.midiStreamCount = compound.MidiChannels();
    stream.usingFallbackMap = true;
    stream.slotMapValidation = SlotMapValidation::kRejectedFallback;
    for (uint32_t channel = 0; channel < pcm; ++channel) {
        stream.channelNames.push_back(std::string(playback ? "Output " : "Input ") + std::to_string(channel + 1));
    }
    return stream;
}

} // namespace ASFW::Protocols::AVC::Graph
