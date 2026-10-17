//
// MidiEndpointPublication.hpp
// ASFWDriver
//
// Decides which published audio endpoints also get a MIDI service, and wires
// the endpoint's MIDI seam when they do.
//
// The rule is about the wire, not the device:
//
//   * Only DICE endpoints. The MIDI slot position and port count are read from
//     DICE's own stream-format registers (AudioStreamRuntimeCaps ->
//     ProjectMidiCapabilities); other families have no such discovery wired yet
//     (BridgeCo channel positions, Oxford ports), so they are left exactly as
//     they were.
//   * Only when the projection finds usable MIDI. A device that reports zero
//     ports, an inconsistent geometry, MIDI on a stream this milestone does not
//     route, or more than one MPX slot publishes nothing.
//
// No device is named anywhere in here. A Saffire Pro 24 DSP publishes MIDI
// because its registers report ports; any other DICE device does for the same
// reason, and none is special-cased. Per-model quirks, when one turns up, belong
// in the device profile, not in this path.
//

#pragma once

#include <cstdint>

#include "../../Audio/Host/IAudioEndpointObserver.hpp"
#include "../../Audio/Ports/IMidiReceiveWake.hpp"
#include "IMidiNubPublishing.hpp"

namespace ASFW::Audio {
class AudioRuntimeRegistry;
}

namespace ASFW::Midi {

class MidiEndpointPublication final : public ASFW::Audio::Host::IAudioEndpointObserver {
public:
    MidiEndpointPublication(ASFW::Audio::AudioRuntimeRegistry& runtime,
                            IMidiNubPublishing& nubs,
                            ASFW::Audio::Ports::IMidiReceiveWake& wake) noexcept
        : runtime_(runtime), nubs_(nubs), wake_(wake) {}

    void OnEndpointPublished(uint64_t guid, ASFW::Audio::AudioBackendKind kind,
                             const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept override;
    void OnEndpointRetired(uint64_t guid) noexcept override;

    /// Capabilities the endpoint's published description projects, or none.
    /// Public so the host suite can check the projection without a publisher.
    [[nodiscard]] static MidiEndpointCapabilities Project(
        uint64_t guid, const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept;

private:
    ASFW::Audio::AudioRuntimeRegistry& runtime_;
    IMidiNubPublishing& nubs_;
    ASFW::Audio::Ports::IMidiReceiveWake& wake_;
};

} // namespace ASFW::Midi
