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
#include "../Wire/Digi00x/Digi00xSlotLayout.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"


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
        return config;
    }
};

} // namespace ASFW::Audio::Host
