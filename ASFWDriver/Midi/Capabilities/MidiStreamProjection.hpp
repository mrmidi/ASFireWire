// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiStreamProjection.hpp - MIDI capabilities projected from a description's
// per-stream wire geometry (pcm channels, DBS, MIDI ports), for families whose
// streams report MIDI the IEC 61883-6 way: MPX-MIDI in the slot after the PCM.
// A family adapter that reads its geometry from the device (DICE) calls this
// and supplies the result as the description's `midi`.

#pragma once

#include "MidiEndpointCapabilities.hpp"
#include "../../Audio/Model/ASFWAudioDevice.hpp"

#include <vector>

namespace ASFW::Midi {

namespace Detail {
inline void FillWireStreams(const std::vector<ASFW::Audio::Model::ASFWAudioWireStream>& streams,
                            ASFW::Audio::AudioStreamWireInfo* out, uint32_t& count) noexcept {
    count = 0;
    for (const auto& stream : streams) {
        if (count >= ASFW::Audio::kMaxAudioStreamsPerDirection) break;
        auto& info = out[count++];
        info.pcmChannels = static_cast<uint16_t>(stream.pcmChannels);
        info.am824Slots = static_cast<uint16_t>(stream.am824Slots);
        info.midiPorts = static_cast<uint16_t>(stream.midiPorts);
    }
}
} // namespace Detail

/// The MIDI the description's streams report. The stream epoch is a runtime
/// value the endpoint owns, so it stays zero here.
[[nodiscard]] inline MidiEndpointCapabilities ProjectStreamMidi(
    uint64_t guid, const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept {
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    Detail::FillWireStreams(description.captureStreams, caps.deviceToHostStreams,
                            caps.deviceToHostStreamCount);
    Detail::FillWireStreams(description.playbackStreams, caps.hostToDeviceStreams,
                            caps.hostToDeviceStreamCount);
    return ProjectMidiCapabilities(caps, guid, /*streamEpoch=*/0);
}

} // namespace ASFW::Midi
