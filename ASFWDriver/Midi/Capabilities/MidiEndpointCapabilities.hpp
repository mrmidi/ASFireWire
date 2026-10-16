//
// MidiEndpointCapabilities.hpp
// ASFWDriver
//
// What MIDI a discovered endpoint can actually carry, projected from the wire
// geometry the audio discovery path already parsed.
//
// Directions are named for the host throughout. The wire names invert twice and
// the inversion is easy to get backwards:
//
//   DICE "tx"  = device -> host = capture  = CoreMIDI SOURCE
//   DICE "rx"  = host -> device = playback = CoreMIDI DESTINATION
//   ASFW's TX packetizer is the host -> device direction, i.e. DICE rx.
//
// So "tx" means opposite things on the two sides of the seam. Nothing in this
// header uses tx/rx for that reason.
//
// Pure logic: no DriverKit dependency, no allocation, no logging.
//

#pragma once

#include <cstdint>

#include "../../Audio/Protocols/AudioTypes.hpp"

namespace ASFW::Midi {

/// One MPX-MIDI AM824 slot multiplexes up to 8 ports.
/// Linux amdtp-am824.c: port = (DBC + f) % 8.
inline constexpr uint8_t kMidiPortsPerMpxSlot = 8;

/// IEC 61883-6 allows exactly one MPX-MIDI data channel.
/// Linux amdtp-am824.h AM824_MAX_CHANNELS_FOR_MIDI = 1.
inline constexpr uint8_t kMaxMpxMidiSlots = 1;

/// Only stream 0 carries MIDI in this milestone.
/// Linux dice/dice-midi.c drives MIDI on stream 0 only.
inline constexpr uint32_t kMidiStreamIndex = 0;

/// Why a direction cannot carry MIDI.
///
/// Every one of these rejects the direction rather than normalising it. A
/// guessed port count publishes an endpoint that silently drops bytes, which is
/// worse than publishing nothing: the user blames their cable.
enum class MidiCapabilityStatus : uint8_t {
    kOk = 0,
    /// The device reports no MIDI in this direction. Not an error.
    kNoMidi,
    /// More ports than one MPX slot can multiplex.
    kPortCountUnsupported,
    /// The formation needs more than one MIDI slot; the spec allows one.
    kMultipleMpxSlots,
    /// Ports reported on a stream this milestone does not route. Publishing
    /// only stream 0's share would expose ports that can never carry bytes.
    kMidiOnUnroutedStream,
    /// The MIDI slot index falls outside the data block.
    kSlotOutOfRange,
    /// The MIDI slot collides with a PCM slot; writing MIDI would overwrite an
    /// audio channel.
    kSlotOverlapsPcm,
    /// DBS does not equal the PCM slots plus the MIDI slots the port count
    /// implies, so the reported geometry is not self-consistent.
    kGeometryInconsistent,
    /// No stream reported for this direction at all.
    kNoStream,
};

[[nodiscard]] constexpr const char* MidiCapabilityStatusName(
    MidiCapabilityStatus status) noexcept {
    switch (status) {
        case MidiCapabilityStatus::kOk:                   return "ok";
        case MidiCapabilityStatus::kNoMidi:               return "no-midi";
        case MidiCapabilityStatus::kPortCountUnsupported: return "port-count-unsupported";
        case MidiCapabilityStatus::kMultipleMpxSlots:     return "multiple-mpx-slots";
        case MidiCapabilityStatus::kMidiOnUnroutedStream: return "midi-on-unrouted-stream";
        case MidiCapabilityStatus::kSlotOutOfRange:       return "slot-out-of-range";
        case MidiCapabilityStatus::kSlotOverlapsPcm:      return "slot-overlaps-pcm";
        case MidiCapabilityStatus::kGeometryInconsistent: return "geometry-inconsistent";
        case MidiCapabilityStatus::kNoStream:             return "no-stream";
    }
    return "unknown";
}

/// MIDI capability for one host-facing direction.
struct MidiDirectionCapabilities final {
    MidiCapabilityStatus status{MidiCapabilityStatus::kNoMidi};

    /// Physical MIDI ports, 1..8. Zero whenever status is not kOk.
    uint8_t portCount{0};
    /// Isochronous stream carrying them. Always kMidiStreamIndex here.
    uint8_t streamIndex{kMidiStreamIndex};
    /// AM824 slot index of the MPX-MIDI channel within each data block.
    uint8_t midiSlotIndex{0};
    /// Data block size in AM824 slots (the CIP DBS).
    uint8_t dbs{0};
    /// PCM slots preceding the MIDI slot.
    uint8_t pcmChannels{0};
    /// Whether the port rotation counts from the packet's DBC.
    ///
    /// Default true: Focusrite implements no unaligned path and Linux applies
    /// the CIP_UNALIGHED_DBC quirk only to specific families. Make it false
    /// only with a reference that names the device.
    bool dbcAligned{true};

    [[nodiscard]] constexpr bool Usable() const noexcept {
        return status == MidiCapabilityStatus::kOk && portCount > 0;
    }
};

/// MIDI capability of one endpoint, both directions.
struct MidiEndpointCapabilities final {
    /// Device -> host. Published to CoreMIDI as sources.
    MidiDirectionCapabilities deviceToHost{};
    /// Host -> device. Published to CoreMIDI as destinations. Carried by the
    /// ASFW TX packetizer.
    MidiDirectionCapabilities hostToDevice{};

    /// Persistent device identity. Survives replug; never a runtime handle.
    uint64_t guid{0};
    /// Invalidates outstanding reservations and parser state on restart.
    uint64_t streamEpoch{0};

    [[nodiscard]] constexpr bool AnyUsable() const noexcept {
        return deviceToHost.Usable() || hostToDevice.Usable();
    }
};

/// Which end of the link a port belongs to, for identity derivation.
enum class MidiDirection : uint8_t {
    kDeviceToHost = 0,  ///< CoreMIDI source
    kHostToDevice = 1,  ///< CoreMIDI destination
};

/// A stable key for one physical port.
///
/// Derived from persistent device identity, direction and port index, so a
/// replug or a stream restart reproduces the same key and CoreMIDI keeps the
/// user's routing. Deliberately excludes the stream epoch and every runtime
/// handle -- anything that changes across a restart would defeat the purpose.
[[nodiscard]] constexpr uint64_t MidiPortKey(uint64_t guid,
                                             MidiDirection direction,
                                             uint8_t portIndex) noexcept {
    // GUID is the whole identity; direction and port index are folded into the
    // low bits of a mixed value so two ports of one device cannot collide with
    // a neighbouring device's GUID.
    uint64_t key = guid;
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDULL;
    key ^= key >> 33;
    key += (static_cast<uint64_t>(direction) << 8) | portIndex;
    return key;
}

/// Project MIDI capability from the wire geometry audio discovery already read.
///
/// Pure: reads the caps, never mutates them. A direction rejected here has no
/// effect on audio capability publication -- an endpoint whose MIDI geometry is
/// unusable is still a perfectly good audio device.
[[nodiscard]] MidiEndpointCapabilities ProjectMidiCapabilities(
    const ASFW::Audio::AudioStreamRuntimeCaps& caps,
    uint64_t guid,
    uint64_t streamEpoch) noexcept;

} // namespace ASFW::Midi
