//
// MidiEndpointCapabilities.cpp
// ASFWDriver
//

#include "MidiEndpointCapabilities.hpp"

namespace ASFW::Midi {

namespace {

using ASFW::Audio::AudioStreamWireInfo;

/// Project one direction from its per-stream wire geometry.
///
/// `streams` is the direction's array and `streamCount` the count discovery
/// reported -- which includes streams the device reports disabled (iso = -1)
/// but that the host must still arm, so it is not a count of active streams.
[[nodiscard]] MidiDirectionCapabilities ProjectDirection(
    const AudioStreamWireInfo* streams, uint32_t streamCount) noexcept {
    MidiDirectionCapabilities out{};

    if (streams == nullptr || streamCount == 0) {
        out.status = MidiCapabilityStatus::kNoStream;
        return out;
    }

    // Any MIDI on a stream this milestone does not route is unreachable. Linux
    // takes the maximum port count across streams and drives stream 0 only
    // (dice/dice-midi.c); taking that maximum without routing the other stream
    // would publish ports that can never carry a byte. Reject instead.
    for (uint32_t i = 1; i < streamCount; ++i) {
        if (streams[i].midiPorts > 0) {
            out.status = MidiCapabilityStatus::kMidiOnUnroutedStream;
            return out;
        }
    }

    const AudioStreamWireInfo& stream = streams[kMidiStreamIndex];
    const uint32_t ports = stream.midiPorts;
    if (ports == 0) {
        out.status = MidiCapabilityStatus::kNoMidi;
        return out;
    }

    // One MPX slot multiplexes 8 ports, and IEC 61883-6 allows exactly one such
    // slot. These are two distinct limits and a device can trip either.
    if (ports > kMidiPortsPerMpxSlot) {
        out.status = MidiCapabilityStatus::kPortCountUnsupported;
        return out;
    }
    const uint32_t midiSlots =
        (ports + kMidiPortsPerMpxSlot - 1u) / kMidiPortsPerMpxSlot;
    if (midiSlots > kMaxMpxMidiSlots) {
        out.status = MidiCapabilityStatus::kMultipleMpxSlots;
        return out;
    }

    const uint32_t pcmChannels = stream.pcmChannels;
    const uint32_t dbs = stream.am824Slots;

    // The reported DBS must equal the slots the reported contents need.
    //
    // With a trailing MIDI slot this single check is also the PCM-overlap
    // check, and that is worth being explicit about. The slot index is
    // pcmChannels, so the MIDI slot is inside the PCM range exactly when
    // dbs <= pcmChannels, and past the end of the data block exactly when
    // dbs < pcmChannels + midiSlots. Both are dbs != pcmChannels + midiSlots.
    // A device claiming 8 PCM channels, 1 MIDI port and DBS 8 is telling us
    // MIDI shares a PCM slot; writing MIDI would overwrite an audio channel in
    // every data block, so reject the formation rather than pick a victim.
    //
    // kSlotOutOfRange and kSlotOverlapsPcm stay in the status enum because a
    // family that reports its own MIDI position -- BridgeCo section type 0x0a,
    // where Linux notes M-Audio firmware puts the location where others put a
    // channel index -- can violate either independently of DBS. Whoever adds
    // that discovery must make those two checks live again; they are not
    // reachable while the index is derived as the trailing slot.
    if (dbs != pcmChannels + midiSlots) {
        out.status = MidiCapabilityStatus::kGeometryInconsistent;
        return out;
    }

    // Linux defaults midi_position to pcm_channels (amdtp-am824.c) and
    // Focusrite hardcodes the last slot. DICE reports no position of its own.
    const uint32_t midiSlotIndex = pcmChannels;

    // Everything below is only reachable once the checks above bound each value
    // to a byte.
    out.status = MidiCapabilityStatus::kOk;
    out.portCount = static_cast<uint8_t>(ports);
    out.streamIndex = static_cast<uint8_t>(kMidiStreamIndex);
    out.midiSlotIndex = static_cast<uint8_t>(midiSlotIndex);
    out.dbs = static_cast<uint8_t>(dbs);
    out.pcmChannels = static_cast<uint8_t>(pcmChannels);
    out.dbcAligned = true;
    return out;
}

} // namespace

MidiEndpointCapabilities ProjectMidiCapabilities(
    const ASFW::Audio::AudioStreamRuntimeCaps& caps,
    const uint64_t guid,
    const uint64_t streamEpoch) noexcept {
    MidiEndpointCapabilities out{};
    out.guid = guid;
    out.streamEpoch = streamEpoch;
    out.deviceToHost =
        ProjectDirection(caps.deviceToHostStreams, caps.deviceToHostStreamCount);
    out.hostToDevice =
        ProjectDirection(caps.hostToDeviceStreams, caps.hostToDeviceStreamCount);
    return out;
}

} // namespace ASFW::Midi
