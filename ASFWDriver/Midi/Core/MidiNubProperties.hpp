//
// MidiNubProperties.hpp
// ASFWDriver
//
// The registry-property contract between the core driver, which publishes
// ASFWMidiNub, and ASFWMIDIDriver, which matches it.
//
// Properties are set on the nub before it starts, the way AudioNubPublisher
// already populates ASFWAudioNub. The MIDI service therefore reads everything
// it needs from its provider in Start() and never calls back across the seam
// to discover its own shape.
//
// Pure header: no DriverKit dependency, no allocation. Both sides include it so
// a key can never be spelled two ways.
//

#pragma once

#include <cstdint>

#include "../Capabilities/MidiEndpointCapabilities.hpp"

namespace ASFW::Midi::NubKeys {

/// Distinguishes this nub from the audio and SBP-2 nubs the same driver
/// publishes. Matched by ASFWMIDIDriverService's IOPropertyMatch.
inline constexpr const char* kNubType = "ASFWNubType";
inline constexpr const char* kNubTypeValue = "MIDI";

/// Persistent device identity; the CoreMIDI endpoint identity derives from it.
inline constexpr const char* kGuid = "ASFWMidiGuid";
/// Invalidates parser and reservation state across a restart.
inline constexpr const char* kStreamEpoch = "ASFWMidiStreamEpoch";
/// The runtime endpoint this nub belongs to, so teardown can find it again.
inline constexpr const char* kEndpointId = "ASFWMidiEndpointId";

/// Names shown in CoreMIDI.
inline constexpr const char* kDeviceName = "ASFWMidiDeviceName";
inline constexpr const char* kModel = "ASFWMidiModel";
inline constexpr const char* kManufacturer = "ASFWMidiManufacturer";

// Per-direction geometry. "Source" and "Destination" are the CoreMIDI words,
// which is what the MIDI service deals in:
//
//   source      = device -> host = capture  = DICE tx
//   destination = host -> device = playback = DICE rx = ASFW's TX packetizer
//
// Spelling them this way at the seam is deliberate: "tx" means opposite things
// on the two sides of it.
inline constexpr const char* kSourcePorts = "ASFWMidiSourcePorts";
inline constexpr const char* kSourceSlotIndex = "ASFWMidiSourceSlotIndex";
inline constexpr const char* kSourceDbs = "ASFWMidiSourceDbs";
inline constexpr const char* kSourceStreamIndex = "ASFWMidiSourceStreamIndex";

inline constexpr const char* kDestinationPorts = "ASFWMidiDestinationPorts";
inline constexpr const char* kDestinationSlotIndex = "ASFWMidiDestinationSlotIndex";
inline constexpr const char* kDestinationDbs = "ASFWMidiDestinationDbs";
inline constexpr const char* kDestinationStreamIndex = "ASFWMidiDestinationStreamIndex";

/// Whether the port rotation counts from the packet's DBC. One flag for the
/// endpoint: it is a device-family property, not a per-direction one.
inline constexpr const char* kDbcAligned = "ASFWMidiDbcAligned";

/// Entities CoreMIDI should publish for these capabilities.
///
/// One entity per physical jack pair, which is the shape Linux gives ALSA. A
/// device with 2 sources and 1 destination publishes 2 entities: the second has
/// a source and no destination, rather than a phantom destination that would
/// accept bytes with nowhere to send them.
[[nodiscard]] constexpr uint32_t EntityCount(
    const MidiEndpointCapabilities& caps) noexcept {
    const uint32_t sources = caps.deviceToHost.Usable() ? caps.deviceToHost.portCount : 0;
    const uint32_t destinations =
        caps.hostToDevice.Usable() ? caps.hostToDevice.portCount : 0;
    return sources > destinations ? sources : destinations;
}

/// Whether entity `index` carries a source.
[[nodiscard]] constexpr bool EntityHasSource(
    const MidiEndpointCapabilities& caps, uint32_t index) noexcept {
    return caps.deviceToHost.Usable() && index < caps.deviceToHost.portCount;
}

/// Whether entity `index` carries a destination.
[[nodiscard]] constexpr bool EntityHasDestination(
    const MidiEndpointCapabilities& caps, uint32_t index) noexcept {
    return caps.hostToDevice.Usable() && index < caps.hostToDevice.portCount;
}

} // namespace ASFW::Midi::NubKeys
