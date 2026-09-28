// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "IAudioBackend.hpp"
#include "PublicationGate.hpp"
#include "../../Session/AudioSessions.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"
#include "../../../Audio/Model/ASFWAudioDevice.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <unordered_set>

namespace ASFW::Audio {
class AudioNubPublisher;
class AudioRuntimeRegistry;

class RmeAudioBackend final : public IAudioBackend {
public:
    RmeAudioBackend(AudioNubPublisher&, Discovery::DeviceRegistry&, AudioRuntimeRegistry&,
                    Session::AudioSessions&) noexcept;
    ~RmeAudioBackend() noexcept override;
    const char* Name() const noexcept override { return "RME Fireface"; }
    IOReturn StartStreaming(uint64_t guid) noexcept override;
    IOReturn StopStreaming(uint64_t guid) noexcept override;
    void OnDeviceRecordUpdated(uint64_t guid) noexcept override;
    void CancelRemoteDeviceWork(uint64_t guid) noexcept override;
    void HandleHostTimingLoss(uint64_t guid) noexcept override;
    void OnStreamsRestarted(uint64_t guid) noexcept override { EnsureNubForGuid(guid); }
    void BeginTeardown() noexcept override;
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

private:
    void EnsureNubForGuid(uint64_t guid) noexcept;
    AudioNubPublisher& publisher_;
    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    Session::AudioSessions& sessions_;
    OSSharedPtr<IODispatchQueue> workQueue_{};
    PublicationGate recoveryAdmission_{};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> recoveryInFlight_{false};
    std::atomic<bool> teardownStarted_{false};
    std::atomic<bool> teardownComplete_{false};
    IOLock* lock_{nullptr};
    std::unordered_set<uint64_t> active_{};
};
} // namespace ASFW::Audio
