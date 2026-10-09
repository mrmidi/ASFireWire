#include "../../Model/RateConfiguration.hpp"
#include "../BeBoB/MAudioSpecialFormation.hpp"
#include "../../Runtime/RateValidation.hpp"
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcAudioConfig.cpp - The audio endpoint an AV/C unit publishes.

#include "AvcAudioConfig.hpp"
#include "AvcControlMapping.hpp"
#include "../../../Protocols/AVC/Descriptors/DescriptorTypeCodes.hpp"
#include "../Duplex/AudioClockConfig.hpp"

#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <algorithm>
#include <cstdio>

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
        const bool canReconfigure = plan.protocolImplementation == DeviceProfiles::Audio::ProtocolImplementationId::ApogeeDuet ||
                                    plan.protocolImplementation == DeviceProfiles::Audio::ProtocolImplementationId::BeBoBPhase88;
        if (!canReconfigure) config.sampleRates = {config.currentSampleRate};
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
                                                     const Graph::DeviceGraph& graph) {
    const auto& playback = graph.playback;
    const auto& capture = graph.capture;
    // Initial plug rates can differ. Configure writes OUTPUT then INPUT before
    // arming transport (Linux bebob_stream.c:96-115); publication must allow
    // that synchronization when the graph has a common same-shape rate.
    if (playback.currentSampleRate == 0 || capture.currentSampleRate == 0 ||
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
    ApplyRatePolicy(config, plan);
    if (config.sampleRates.empty()) {
        return std::nullopt;
    }
    PreferDefaultStartRate(config);
    // Advertisement below comes from complete, transaction-resolvable formations.
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
    // Descriptor purpose resolves competing output stages. It identifies the
    // intended master, never invents mute/volume support or its limits.
    const bool declaredOutputMaster = std::ranges::any_of(graph.featureChannels, [&](const auto& channel) {
        if (channel.channel != 0 || (!channel.volume && !channel.mute)) return false;
        return std::ranges::any_of(graph.controls, [&](const auto& block) {
            return block.audioSubunitId == channel.subunit && block.id == channel.block &&
                block.volumePurpose == 1 && IsAvcOutputMaster(graph, block);
        });
    });
    for (const auto& channel : graph.featureChannels) {
        const auto block = std::ranges::find_if(graph.controls, [&](const auto& item) { return item.type == Descriptors::AudioFunctionBlockType::kFeature && item.audioSubunitId == channel.subunit && item.id == channel.block; });
        if (block == graph.controls.end() || channel.channel != 0) continue;
        if (!IsAvcOutputMaster(graph, *block) || (declaredOutputMaster && block->volumePurpose != 1)) continue;
        ::ASFW::Audio::Model::AvcPublishedControl control;
        control.token = ::ASFW::Audio::Model::AvcControlToken(channel.subunit, channel.block, channel.channel);
        control.scope = static_cast<uint32_t>('outp');
        control.element = 0;
        snprintf(control.name, sizeof(control.name), "%s %s %u",
                 block->name.empty() ? ("Feature " + std::to_string(block->id)).c_str() : block->name.c_str(),
                 channel.channel == 0 ? "Master" : "Channel", channel.channel);
        control.hasMute = channel.mute.has_value(); control.muted = channel.mute.value_or(false);
        if (channel.volume && channel.minimum && channel.maximum && channel.resolution) {
            control.range = {*channel.minimum, *channel.maximum, *channel.resolution};
            control.current = *channel.volume;
            control.hasVolume = control.range.Valid() && control.current != INT16_MIN &&
                control.current >= control.range.minimum && control.current <= control.range.maximum;
        }
        if (control.hasMute || control.hasVolume) config.avcControls.push_back(control);
    }

    // One output master per control class. Competing volume/mute candidates
    // are omitted rather than publishing duplicate HAL main-element controls.
    const auto volumeCount = std::ranges::count_if(config.avcControls, [](const auto& c) { return c.hasVolume; });
    const auto muteCount = std::ranges::count_if(config.avcControls, [](const auto& c) { return c.hasMute; });
    for (auto& control : config.avcControls) {
        if (volumeCount > 1) control.hasVolume = false;
        if (muteCount > 1) control.hasMute = false;
    }
    std::erase_if(config.avcControls, [](const auto& c) { return !c.hasVolume && !c.hasMute; });

    const auto forced = plan.streamTraits.wire.forcedStreamMode;
    const bool blocking = forced == ForcedStreamMode::Blocking ||
                          (forced == ForcedStreamMode::Unspecified && graph.supportsBlockingTransmit);
    config.streamMode = blocking ? StreamMode::kBlocking : StreamMode::kNonBlocking;
    if (graph.transmitModes && graph.receiveModes) {
        const uint8_t common = *graph.transmitModes & *graph.receiveModes;
        // Validated device quirks override advertised flags (Duet: Linux oxfw.c:164-167).
        if (forced == ForcedStreamMode::Unspecified) {
            if (common & Descriptors::kMusicCapabilityBlockingBit) config.streamMode = StreamMode::kBlocking;
            else if (common & Descriptors::kMusicCapabilityNonBlockingBit) config.streamMode = StreamMode::kNonBlocking;
            else return std::nullopt;
        }
    }
    const auto inventory = [](const Graph::StreamGraph& stream) {
        auto formations = stream.formations;
        if (formations.empty()) {
            // Legacy documents only offer same-shape rates. Do not extrapolate
            // their channel count to rates absent from that explicit list.
            for (uint32_t rate : stream.supportedSampleRates)
                formations.push_back({rate, stream.channelCount, stream.dataBlockSize,
                    stream.midiStreamCount, stream.slotMap, false});
        }
        return formations;
    };
    const auto playbackInventory = inventory(playback);
    const auto captureInventory = inventory(capture);
    for (const auto& output : playbackInventory) {
        if (!Encoding::AmdtpRateGeometryForSampleRate(output.sampleRateHz)) continue;
        // One complete formation per direction is required. Ambiguous same-rate
        // alternatives need explicit protocol selection, not a first-match guess.
        if (std::ranges::count_if(playbackInventory, [&](const auto& f) {
                return f.sampleRateHz == output.sampleRateHz;
            }) != 1 || std::ranges::count_if(captureInventory, [&](const auto& f) {
                return f.sampleRateHz == output.sampleRateHz;
            }) != 1) continue;
        const auto input = std::ranges::find_if(captureInventory, [&](const auto& f) {
            return f.sampleRateHz == output.sampleRateHz;
        });
        config.rateFormationCandidates.push_back({
            .sampleRateHz = output.sampleRateHz,
            .mode = config.streamMode == StreamMode::kBlocking
                ? Encoding::StreamMode::kBlocking : Encoding::StreamMode::kNonBlocking,
            .playback = {{output.pcmChannels, output.dataBlockSize, output.midiSlots, output.pcmSlots}},
            .capture = {{input->pcmChannels, input->dataBlockSize, input->midiSlots, input->pcmSlots}},
            .protocolSupported = true,
            .hardwareValidated = false,
        });
    }
    std::ranges::sort(config.rateFormationCandidates, {},
        &::ASFW::Audio::Runtime::RateFormation::sampleRateHz);
    // Pick the startup shape from the complete inventory too. The device may
    // have been discovered in a different rate tier with a narrower ADAT shape.
    // Merely changing currentSampleRate would publish the wrong buffer stride.
    if (plan.streamTraits.start.startRatePinHz == 0 &&
        std::ranges::find(config.rateFormationCandidates, kDefaultStartRateHz,
        &::ASFW::Audio::Runtime::RateFormation::sampleRateHz) != config.rateFormationCandidates.end())
        config.currentSampleRate = kDefaultStartRateHz;
    config.sampleRates.clear();
    for (const auto& formation : config.rateFormationCandidates)
        if (::ASFW::Audio::Runtime::RateEnabled(formation, config.currentSampleRate))
            config.sampleRates.push_back(formation.sampleRateHz);
    if (config.sampleRates.empty()) return std::nullopt;
    const auto initial = ::ASFW::Audio::Model::WithRateFormation(config, config.currentSampleRate);
    if (!initial) return std::nullopt;
    return *initial;
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
    // The special firmware's existing initialization explicitly selects SPDIF
    // independently in both directions. Resolve that exact vendor state from
    // its tables, without sending generic discovery to the hazardous firmware.
    if (plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814 ||
        plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioProjectMix) {
        const auto count = plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814
            ? ::ASFW::Audio::BeBoB::kMAudioFireWire1814RateCount : ::ASFW::Audio::BeBoB::kMAudioProjectMixRateCount;
        for (size_t i = 0; i < count; ++i) {
            const auto rate = ::ASFW::Audio::BeBoB::kMAudioSpecialRatesHz[i];
            const auto formation = ::ASFW::Audio::BeBoB::MAudioFormationFor(::ASFW::Audio::BeBoB::MAudioDigitalFormat::SPDIF,
                ::ASFW::Audio::BeBoB::MAudioDigitalFormat::SPDIF, rate);
            if (!formation) continue;
            config.rateFormationCandidates.push_back({rate, Encoding::StreamMode::kBlocking,
                {{formation->playbackPcmChannels, formation->playbackPcmChannels + formation->midiDataBlocks,
                    formation->midiDataBlocks, {}}},
                {{formation->capturePcmChannels, formation->capturePcmChannels + formation->midiDataBlocks,
                    formation->midiDataBlocks, {}}}, true, false});
        }
        config.sampleRates.clear();
        for (const auto& formation : config.rateFormationCandidates)
            if (::ASFW::Audio::Runtime::RateEnabled(formation, config.currentSampleRate))
                config.sampleRates.push_back(formation.sampleRateHz);
    }
    return config;
}

} // namespace ASFW::Protocols::AVC
