// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RmeFamilyAdapter.hpp - RME Fireface on the audio device host (E2).
//
// documentation/AUDIO_DEVICE_HOST.md §4.2. Replaces RmeAudioBackend. The
// Fireface publishes a fixed description: 48 kHz only, one stream per
// direction, 28 channels on the FF800 and 18 on the FF400 (the endpoint the
// backend published, unchanged). Every runtime fault restarts: the family
// reads no health evidence of its own. It raises no device events: no
// notification mechanism is known for it.

#pragma once

#include "FamilyAdapter.hpp"
#include "../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

namespace ASFW::Audio::Host {

class RmeFamilyAdapter final : public FamilyAdapter {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "RME"; }

    void Describe(const DescribeInput& in, DescribeDone done) override;

    [[nodiscard]] bool ActsOn(DuplexRestartReason reason) const noexcept override {
        // RmeAudioBackend never acted on cycle inconsistent (IAudioBackend's
        // default no-op); everything else restarts.
        return reason != DuplexRestartReason::kRecoverAfterCycleInconsistent;
    }

    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override {
        // RmeAudioBackend restarted on every timing loss without a health read.
        (void)guid;
        (void)reason;
        (void)context;
        return FaultVerdict::kRestart;
    }

    void SetEventSink(DeviceEventSink* sink) noexcept override {
        // No device notifications are known for the Fireface; nothing to raise.
        (void)sink;
    }

    /// The endpoint the RME backend published: 48 kHz, one blocking stream per
    /// direction, FF800 28 channels, otherwise 18.
    [[nodiscard]] static Model::ASFWAudioDevice BuildNubConfig(
        const Discovery::DeviceRecord& record,
        DeviceProfiles::Audio::ProfileBuilderId builder,
        const char* name) {
        const uint32_t channels = builder == DeviceProfiles::Audio::ProfileBuilderId::RmeFireface800
                                      ? 28U : 18U;
        Model::ASFWAudioDevice config{};
        config.guid = record.guid;
        config.vendorId = record.vendorId;
        config.modelId = record.modelId;
        config.profileBuilderId = static_cast<uint32_t>(builder);
        config.deviceName = name ? name : "RME Fireface";
        config.inputPlugName = "Input";
        config.outputPlugName = "Output";
        config.inputChannelCount = channels;
        config.outputChannelCount = channels;
        config.channelCount = channels;
        config.sampleRates = {48000U};
        config.currentSampleRate = 48000U;
        config.streamMode = Model::StreamMode::kBlocking;
        config.captureStreams.push_back({.pcmChannels = channels, .am824Slots = channels,
                                         .midiPorts = 0, .channelOffset = 0});
        config.playbackStreams.push_back({.pcmChannels = channels, .am824Slots = channels,
                                          .midiPorts = 0, .channelOffset = 0});
        config.resolvedGeometryRequired = true;
        return config;
    }
};

} // namespace ASFW::Audio::Host
