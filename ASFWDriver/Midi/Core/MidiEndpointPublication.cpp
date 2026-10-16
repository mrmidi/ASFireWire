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

namespace ASFW::Midi {

namespace {

constexpr const char* kManufacturerName = "FireWire";

// The published description carries the per-stream wire geometry the audio side
// was given (pcm channels, DBS, MIDI ports). The capability projection takes the
// shape discovery produced, so rebuild exactly that shape from it.
void FillStreams(const std::vector<ASFW::Audio::Model::ASFWAudioWireStream>& streams,
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

} // namespace

MidiEndpointCapabilities MidiEndpointPublication::Project(
    const uint64_t guid, const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept {
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    FillStreams(description.captureStreams, caps.deviceToHostStreams, caps.deviceToHostStreamCount);
    FillStreams(description.playbackStreams, caps.hostToDeviceStreams, caps.hostToDeviceStreamCount);
    // The stream epoch is a runtime value the endpoint owns; the projection's
    // copy of it is not used for publication.
    return ProjectMidiCapabilities(caps, guid, /*streamEpoch=*/0);
}

void MidiEndpointPublication::OnEndpointPublished(
    const uint64_t guid, const ASFW::Audio::AudioBackendKind kind,
    const ASFW::Audio::Model::ASFWAudioDevice& description) noexcept {
    // Family gate: the MIDI geometry comes from DICE's stream-format registers.
    if (kind != ASFW::Audio::AudioBackendKind::Dice) return;

    const MidiEndpointCapabilities caps = Project(guid, description);
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
