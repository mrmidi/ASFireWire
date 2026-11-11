//
// MidiEndpointPublication.hpp
// ASFWDriver
//
// Decides which published audio endpoints also get a MIDI service, and wires
// the endpoint's MIDI seam when they do.
//
// The family adapter decides, not this path:
//
//   * An endpoint gets MIDI only when its adapter supplied MidiEndpointCapabilities
//     in the published description (ASFWAudioDevice::midi): the wire format, the
//     MIDI slot and the ports per direction. DICE projects them from its
//     stream-format registers (MidiStreamProjection.hpp); the Digi 002 / 003
//     declares its own format and slot. A family that supplies none (AV/C, MOTU,
//     RME today) gets none, whatever its streams' midiPorts say.
//   * Only when the capabilities are usable. A device that reports zero ports,
//     an inconsistent geometry, MIDI on a stream this milestone does not route,
//     or a slot that would overwrite PCM publishes nothing.
//
// No device is named anywhere in here. Per-model quirks belong in the family
// adapter or the device profile, not in this path.
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

    /// Capabilities the description's streams project the IEC 61883-6 way
    /// (ProjectStreamMidi). Public so the host suite can check the projection
    /// without a publisher.
    [[nodiscard]] static MidiEndpointCapabilities Project(
        uint64_t guid, const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept;

private:
    ASFW::Audio::AudioRuntimeRegistry& runtime_;
    IMidiNubPublishing& nubs_;
    ASFW::Audio::Ports::IMidiReceiveWake& wake_;
};

} // namespace ASFW::Midi
