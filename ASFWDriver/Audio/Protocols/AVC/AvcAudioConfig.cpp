// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcAudioConfig.cpp - The audio endpoint an AV/C unit publishes.

#include "AvcAudioConfig.hpp"

#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <algorithm>

namespace ASFW::Protocols::AVC {

namespace {

using ::ASFW::Audio::Model::ASFWAudioDevice;
using ::ASFW::Audio::Model::StreamMode;
using DeviceProfiles::Audio::ForcedStreamMode;
using DeviceProfiles::Audio::StaticAudioEndpointPlan;

constexpr uint32_t kMaxDataBlockSize = 255;

void FillIdentity(ASFWAudioDevice& config, const AvcEndpointIdentity& identity,
                  const StaticAudioEndpointPlan& plan, std::string deviceName) {
    config.guid = identity.guid;
    config.vendorId = identity.vendorId;
    config.modelId = identity.modelId;
    config.profileBuilderId = static_cast<uint32_t>(plan.profileBuilder);
    config.deviceName = std::move(deviceName);
    config.inputPlugName = config.deviceName + " Inputs";
    config.outputPlugName = config.deviceName + " Outputs";
}

// The catalog's start-rate traits narrow what the device offers: a pinned rate
// is the only rate its runtime supports; an observed-rate device has geometry
// for its current rate only.
void ApplyRatePolicy(ASFWAudioDevice& config, const StaticAudioEndpointPlan& plan) {
    const auto& start = plan.streamTraits.start;
    if (start.startRatePinHz != 0) {
        config.sampleRates = {start.startRatePinHz};
        config.currentSampleRate = start.startRatePinHz;
    } else if (start.startAtObservedRate) {
        config.sampleRates = {config.currentSampleRate};
    }
}

// Every AV/C device starts at 48 kHz when it can run it, whatever rate it was
// left at: 48 kHz is the rate the runtimes are validated at, and DICE, MOTU and
// RME already start there (DiceInitialRate, SessionScheduler DefaultStartRate).
// The first start writes the rate to the device (BeBoBProtocol::
// ApplyClockConfig). A device that cannot run 48 kHz starts at the rate it
// reported, or else at the first rate offered.
constexpr uint32_t kDefaultStartRateHz = 48000;

void PreferDefaultStartRate(ASFWAudioDevice& config) {
    const auto offered = [&config](uint32_t hz) {
        return std::ranges::find(config.sampleRates, hz) != config.sampleRates.end();
    };
    if (offered(kDefaultStartRateHz)) {
        config.currentSampleRate = kDefaultStartRateHz;
    } else if (!offered(config.currentSampleRate)) {
        config.currentSampleRate = config.sampleRates.front();
    }
}

} // namespace

std::optional<ASFWAudioDevice> BuildGraphAudioConfig(const AvcEndpointIdentity& identity,
                                                     const StaticAudioEndpointPlan& plan,
                                                     const Graph::DeviceGraph& graph,
                                                     const std::vector<uint32_t>& runtimeRates) {
    const auto& playback = graph.playback;
    const auto& capture = graph.capture;
    if (playback.currentSampleRate == 0 || playback.currentSampleRate != capture.currentSampleRate ||
        playback.dataBlockSize == 0 || capture.dataBlockSize == 0 ||
        playback.channelCount == 0 || capture.channelCount == 0 ||
        playback.channelCount > Encoding::kMaxPcmChannels ||
        capture.channelCount > Encoding::kMaxPcmChannels ||
        playback.dataBlockSize > kMaxDataBlockSize || capture.dataBlockSize > kMaxDataBlockSize) {
        return std::nullopt;
    }

    ASFWAudioDevice config;
    FillIdentity(config, identity, plan, identity.modelName);
    config.inputChannelCount = capture.channelCount;
    config.outputChannelCount = playback.channelCount;
    config.channelCount = std::max(config.inputChannelCount, config.outputChannelCount);
    config.currentSampleRate = playback.currentSampleRate;
    for (const auto hz : playback.supportedSampleRates) {
        if (std::ranges::find(capture.supportedSampleRates, hz) != capture.supportedSampleRates.end()) {
            config.sampleRates.push_back(hz);
        }
    }
    if (!runtimeRates.empty()) {
        std::erase_if(config.sampleRates, [&runtimeRates](uint32_t hz) {
            return std::ranges::find(runtimeRates, hz) == runtimeRates.end();
        });
    }
    ApplyRatePolicy(config, plan);
    if (config.sampleRates.empty()) {
        return std::nullopt;
    }
    PreferDefaultStartRate(config);
    config.inputChannelNames = capture.channelNames;
    config.outputChannelNames = playback.channelNames;
    config.playbackStreams = {{.pcmChannels = playback.channelCount,
                               .am824Slots = playback.dataBlockSize,
                               .midiPorts = playback.midiStreamCount,
                               .pcmSlotMap = playback.slotMap}};
    config.captureStreams = {{.pcmChannels = capture.channelCount,
                              .am824Slots = capture.dataBlockSize,
                              .midiPorts = capture.midiStreamCount,
                              .pcmSlotMap = capture.slotMap}};
    config.resolvedGeometryRequired = true;
    config.deviceSampleRates = true;
    config.graphResolved = true;
    const auto forced = plan.streamTraits.wire.forcedStreamMode;
    const bool blocking = forced == ForcedStreamMode::Blocking ||
                          (forced == ForcedStreamMode::Unspecified && graph.supportsBlockingTransmit);
    config.streamMode = blocking ? StreamMode::kBlocking : StreamMode::kNonBlocking;
    return config;
}

std::optional<ASFWAudioDevice> BuildProfileOwnedAudioConfig(
    const AvcEndpointIdentity& identity, const StaticAudioEndpointPlan& plan,
    const ::ASFW::Isoch::Audio::IAudioDeviceProfile& profile) {
    const uint32_t input = profile.RxChannelCount();   // device -> host
    const uint32_t output = profile.TxChannelCount();  // host -> device
    const auto rates = profile.SupportedSampleRates();
    if (input == 0 || output == 0 || rates.empty() || profile.RxDbs() < input || profile.TxDbs() < output ||
        profile.RxDbs() > kMaxDataBlockSize || profile.TxDbs() > kMaxDataBlockSize) {
        return std::nullopt;
    }

    ASFWAudioDevice config;
    FillIdentity(config, identity, plan, profile.Name());
    config.inputChannelCount = input;
    config.outputChannelCount = output;
    config.channelCount = std::max(input, output);
    config.sampleRates = rates;
    config.currentSampleRate = rates.front();
    ApplyRatePolicy(config, plan);
    PreferDefaultStartRate(config);
    config.captureStreams = {{.pcmChannels = input,
                              .am824Slots = profile.RxDbs(),
                              .midiPorts = profile.RxMidiPorts()}};
    config.playbackStreams = {{.pcmChannels = output,
                               .am824Slots = profile.TxDbs(),
                               .midiPorts = profile.TxMidiPorts()}};
    config.resolvedGeometryRequired = true;
    // Every AV/C family that is never probed transmits blocking (Linux
    // bebob_stream.c:433, fireworks_stream.c:32); the catalog may say otherwise.
    config.streamMode = plan.streamTraits.wire.forcedStreamMode == ForcedStreamMode::NonBlocking
                            ? StreamMode::kNonBlocking
                            : StreamMode::kBlocking;
    return config;
}

} // namespace ASFW::Protocols::AVC
