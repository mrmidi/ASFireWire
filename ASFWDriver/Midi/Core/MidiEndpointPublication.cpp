//
// MidiEndpointPublication.cpp
// ASFWDriver
//

#include "MidiEndpointPublication.hpp"

#include "../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../Audio/Protocols/AudioTypes.hpp"
#include "../../Logging/Logging.hpp"
#include "../Capabilities/MidiEndpointCapabilities.hpp"
#include "../Capabilities/MidiStreamProjection.hpp"

namespace ASFW::Midi {

namespace {

constexpr const char* kManufacturerName = "FireWire";

} // namespace

MidiEndpointCapabilities MidiEndpointPublication::Project(
    const uint64_t guid, const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept {
    return ProjectStreamMidi(guid, description);
}

void MidiEndpointPublication::OnEndpointPublished(
    const uint64_t guid, const ASFW::Audio::AudioBackendKind kind,
    const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept {
    // The family adapter decides what MIDI the endpoint carries. One that
    // supplied none gets no service, whatever its streams' midiPorts say.
    (void)kind;
    if (!description.midi.has_value()) return;
    MidiEndpointCapabilities caps = *description.midi;
    caps.guid = guid;
    caps.streamEpoch = 0;  // the endpoint owns the runtime epoch
    if (!caps.AnyUsable()) {
        // A device that reports no MIDI is the ordinary case and says nothing.
        // A device that reports some but whose geometry is rejected is not: say
        // why, so a device that should have MIDI and does not is visible
        // without debug logging.
        const bool reportedNone = caps.deviceToHost.status == MidiCapabilityStatus::kNoMidi &&
                                  caps.hostToDevice.status == MidiCapabilityStatus::kNoMidi;
        if (!reportedNone) {
            ASFW_LOG(Midi,
                     "[MidiPublish] guid=%016llx not published (deviceToHost=%{public}s "
                     "hostToDevice=%{public}s)",
                     guid, MidiCapabilityStatusName(caps.deviceToHost.status),
                     MidiCapabilityStatusName(caps.hostToDevice.status));
        }
        return;
    }

    // The endpoint owns the byte seam and the geometry; set both before the nub
    // exists so the MIDI service never starts against an unconfigured endpoint.
    const auto endpoint = runtime_.EnsureEndpointRuntime(guid);
    if (!endpoint) {
        ASFW_LOG_ERROR(Midi, "[MidiPublish] guid=%016llx no endpoint runtime", guid);
        return;
    }
    const kern_return_t kr = endpoint->ConfigureMidi(caps);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "[MidiPublish] guid=%016llx ConfigureMidi failed kr=0x%x", guid, kr);
        return;
    }
    endpoint->SetMidiReceiveWake(&wake_);

    const char* name = description.deviceName.c_str();
    if (!nubs_.EnsureNub(caps, guid, name, name, kManufacturerName)) {
        ASFW_LOG_ERROR(Midi, "[MidiPublish] guid=%016llx nub publication failed", guid);
    }
}

void MidiEndpointPublication::OnEndpointRetired(const uint64_t guid) noexcept {
    nubs_.TerminateNub(guid);
}

} // namespace ASFW::Midi
