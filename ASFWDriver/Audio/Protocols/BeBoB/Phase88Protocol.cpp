// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Phase88Protocol.cpp — Exact TerraTec PHASE 88 Rack FW BeBoB/CMP adapter.
//
// Inherits the general BeBoB/CMP lifecycle from BeBoBProtocol. Only Phase88-specific
// behavior lives here: stream geometry (10 PCM + 1 MIDI at 48 kHz), the ASFW-specific
// mixer-unmute workaround, and clock-health reporting.
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c:400-465, 500-523, 593-674;
// no reference source is copied.

#include "Phase88Protocol.hpp"
#include "Phase88MixerData.hpp"

#include "../../../Logging/Logging.hpp"

#include <memory>
#include <vector>

namespace ASFW::Audio::BeBoB {
[[nodiscard]] AudioStreamRuntimeCaps Phase88Protocol::Phase88Caps() noexcept {
    // The exact PHASE 88 model map is ten PCM channels plus one AM824 MIDI
    // conformant-data block in each direction (DBS=11 at 48 kHz). The one
    // data block can multiplex the unit's two physical MIDI ports.
    // Cross-validated with alsa-userspace-control-protocols-impl/
    // protocols/bebob/src/terratec/phase88.rs:11-46.
    constexpr uint32_t kPhase88PcmChannels = 10;
    constexpr uint32_t kPhase88MidiDataBlocks = 1;
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = kPhase88PcmChannels,
        .hostOutputPcmChannels = kPhase88PcmChannels,
        .deviceToHostAm824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks,
        .hostToDeviceAm824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks,
        .sampleRateHz = 48000,
        .deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    caps.deviceToHostStreams[0] = {.pcmChannels = kPhase88PcmChannels,
                                   .am824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks};
    caps.hostToDeviceStreams[0] = {.pcmChannels = kPhase88PcmChannels,
                                   .am824Slots = kPhase88PcmChannels + kPhase88MidiDataBlocks};
    return caps;
}

void Phase88Protocol::RunMixerSteps(const MixerMap& map,
                                    MixerFailurePolicy policy, MixerCompletion finalCompletion) {
    struct Step {
        std::function<void(MixerCompletion)> submit;
    };
    std::vector<Step> steps;
    steps.reserve(map.selectors.size() + map.mutes.size() + map.volumes.size());

    for (const auto& sel : map.selectors) {
        steps.push_back({[this, fbId = sel.fbId, value = sel.value](MixerCompletion cb) {
            SetSelectorBlock(fbId, value, std::move(cb));
        }});
    }
    for (const auto& mute : map.mutes) {
        steps.push_back({[this, fbId = mute.fbId, ch = mute.channel, unmute = mute.unmute](MixerCompletion cb) {
            SetFeatureMute(fbId, ch, unmute, std::move(cb));
        }});
    }
    for (const auto& vol : map.volumes) {
        steps.push_back({[this, fbId = vol.fbId, ch = vol.channel, value = vol.value](MixerCompletion cb) {
            SetFeatureVolume(fbId, ch, value, std::move(cb));
        }});
    }

    auto runNext = std::make_shared<std::function<void(size_t, IOReturn)>>();
    *runNext = [steps = std::move(steps), runNext, finalCompletion = std::move(finalCompletion), policy](size_t idx, IOReturn lastStatus) mutable {
        if (lastStatus != kIOReturnSuccess && policy == MixerFailurePolicy::kRequired) {
            finalCompletion(lastStatus);
            return;
        }
        if (idx >= steps.size()) {
            finalCompletion(kIOReturnSuccess);
            return;
        }
        steps[idx].submit([runNext, idx](IOReturn status) {
            (*runNext)(idx + 1, status);
        });
    };
    (*runNext)(0, kIOReturnSuccess);
}

bool Phase88Protocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = Phase88Caps();
    return true;
}

AudioStreamRuntimeCaps Phase88Protocol::DeviceCaps() const {
    return Phase88Caps();
}

std::vector<uint32_t> Phase88Protocol::SupportedRates() const {
    return {kPhase88SampleRateHz};
}

void Phase88Protocol::ReadClockHealth(HealthCallback callback) {
    callback(kIOReturnSuccess, DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                                   .appliedClock = appliedClock_,
                                                   .runtimeCaps = Phase88Caps(),
                                                   .sourceLocked = inputConnected_ && outputConnected_,
                                                   .clockReferenceHealthy = true,
                                                   .nominalRateHz = kPhase88SampleRateHz});
}

void Phase88Protocol::ConfigureMixer(MixerFailurePolicy policy, MixerCompletion completion) {
    ASFW_LOG(Audio, "[BeBoB] Phase88: configuring hardware mixer (unmute + max volume)");
    RunMixerSteps(kPhase88MixerMap, policy, std::move(completion));
}

} // namespace ASFW::Audio::BeBoB
