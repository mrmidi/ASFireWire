// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xFamilyAdapter.hpp - Digidesign Digi 002 / 003 on the audio device
// host (issue #249).
//
// A fixed description: 44.1 and 48 kHz (ASFW streams 1x only), one
// non-blocking stream per direction, 18 PCM channels plus the MIDI data channel
// (19 AM824 slots; PCM in slots 1-18 after the MIDI slot). Every runtime fault restarts: the
// family reads no fault evidence of its own. It raises no device events yet.
//
// Channel names are left to the host's defaults: the order of the analog,
// ADAT and S/PDIF channels inside a data block is still to be confirmed on
// hardware.

#pragma once

#include "FamilyAdapter.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../Protocols/Digi00x/Digi00xFamilyDriver.hpp"
#include "../Protocols/Digi00x/Digi00xRegisters.hpp"
#include "../Wire/Digi00x/Digi00xMidi.hpp"
#include "../Wire/Digi00x/Digi00xSlotLayout.hpp"
#include "../../Midi/Capabilities/MidiEndpointCapabilities.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceIds.hpp"


namespace ASFW::Audio::Host {

class Digi00xFamilyAdapter final : public FamilyAdapter {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Digi00x"; }

    void Describe(const DescribeInput& in, DescribeDone done) override {
        if (!in.policy ||
            in.policy->plan.family != DeviceProfiles::Audio::AudioFamilyProviderId::Digi00xRegister) {
            done(DescribeRefusal{kIOReturnUnsupported, "not-a-digi00x-policy"});
            return;
        }
        if (!in.protocol) {
            done(DescribeRefusal{kIOReturnNotReady, "no-protocol"});
            return;
        }
        done(BuildNubConfig(in.record, in.policy->plan.profileBuilder, in.protocol->GetName()));
    }

    [[nodiscard]] bool ActsOn(DuplexRestartReason reason) const noexcept override {
        // As RME: every reason but a cycle inconsistency restarts.
        return reason != DuplexRestartReason::kRecoverAfterCycleInconsistent;
    }

    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override {
        (void)guid;
        (void)reason;
        (void)context;
        return FaultVerdict::kRestart;
    }

    void SetEventSink(DeviceEventSink* sink) noexcept override {
        // Device messages are not decoded yet; nothing to raise.
        (void)sink;
    }

    [[nodiscard]] static Model::ASFWAudioDevice BuildNubConfig(
        const Discovery::DeviceRecord& record,
        DeviceProfiles::Audio::ProfileBuilderId builder,
        const char* name) {
        constexpr uint32_t kRate = Digi00x::kDefaultStreamedRateHz;
        const uint32_t pcm = Digi00x::PcmChannelsForRate(kRate).value_or(0);
        const uint32_t slots = Digi00x::DataBlockQuadletsForRate(kRate).value_or(0);
        Model::ASFWAudioDevice config{};
        config.guid = record.guid;
        config.vendorId = record.vendorId;
        config.modelId = record.modelId;
        config.profileBuilderId = static_cast<uint32_t>(builder);
        config.deviceName = name ? name : "Digi 002/003";
        config.inputPlugName = "Input";
        config.outputPlugName = "Output";
        config.inputChannelCount = pcm;
        config.outputChannelCount = pcm;
        config.channelCount = pcm;
        config.sampleRates = {Digi00x::kStreamedRatesHz.begin(), Digi00x::kStreamedRatesHz.end()};
        config.currentSampleRate = kRate;
        config.streamMode = Model::StreamMode::kNonBlocking;
        // PCM sits after the MIDI data channel in both directions.
        const auto pcmAfterMidi = Encoding::Digi00x::PcmAfterMidiSlotMap(pcm);
        config.captureStreams.push_back({.pcmChannels = pcm, .am824Slots = slots, .midiPorts = 0,
                                         .channelOffset = 0, .pcmSlotMap = pcmAfterMidi});
        config.playbackStreams.push_back({.pcmChannels = pcm, .am824Slots = slots, .midiPorts = 0,
                                          .channelOffset = 0, .pcmSlotMap = pcmAfterMidi});
        config.resolvedGeometryRequired = true;
        config.midi = DeclareMidi(record.guid, pcm, slots, IsConsole(record));
        return config;
    }

    /// Whether the unit is a Console (002 / 003), the models with a control
    /// surface. The unit specifier names the product (AudioDeviceIds.hpp); the
    /// GUID's product field carries the same value when the record lacks it.
    [[nodiscard]] static bool IsConsole(const Discovery::DeviceRecord& record) noexcept {
        const uint32_t spec = record.unitSpecId.value_or(
            static_cast<uint32_t>((static_cast<uint64_t>(record.guid) >> 16) & 0xFFu));
        return spec == DeviceProfiles::Audio::kDigi002ConsoleSpecifierId ||
               spec == DeviceProfiles::Audio::kDigi003ConsoleSpecifierId;
    }

    /// Every model has MIDI In 1 and MIDI Out 1 and 2, in data channel 0 in
    /// the Digi's own format. A Rack publishes MIDI 1 (in and out) and MIDI 2
    /// (out only).
    ///
    /// A Console also has its control port (port code 0xE, host port 2): the
    /// faders, touch, switches and encoders arrive on it, and fader positions,
    /// LEDs, encoder rings and the scribble strips are sent to it. It is
    /// published as a third port pair, MIDI 3, so a control-surface bridge can
    /// drive the desk the way the vendor driver's "Port 3 (Control)" did. The
    /// device has no MIDI In 2, so on a Console the MIDI 2 source stays silent.
    [[nodiscard]] static ::ASFW::Midi::MidiEndpointCapabilities DeclareMidi(
        uint64_t guid, uint32_t pcm, uint32_t slots, bool console = false) noexcept {
        using Encoding::MidiWireFormat;
        const uint64_t pcmSlots = pcm >= 64 ? 0
            : ((uint64_t{1} << pcm) - 1U) << Encoding::Digi00x::kFirstPcmDataChannel;
        const auto dbs = static_cast<uint8_t>(slots);
        const auto pcmChannels = static_cast<uint8_t>(pcm);
        constexpr auto kSlot = static_cast<uint8_t>(Encoding::Digi00x::kMidiDataChannel);
        constexpr auto kWithControl = static_cast<uint8_t>(Encoding::Digi00x::kControlPort + 1u);
        const uint8_t inPorts = console ? kWithControl : uint8_t{1};
        const uint8_t outPorts = console ? kWithControl : uint8_t{2};
        return ::ASFW::Midi::MidiEndpointCapabilities{
            .deviceToHost = ::ASFW::Midi::DeclareMidiDirection(MidiWireFormat::kDigi00x, inPorts, kSlot,
                                                       dbs, pcmChannels, pcmSlots),
            .hostToDevice = ::ASFW::Midi::DeclareMidiDirection(MidiWireFormat::kDigi00x, outPorts, kSlot,
                                                       dbs, pcmChannels, pcmSlots),
            .guid = guid,
        };
    }
};

} // namespace ASFW::Audio::Host
