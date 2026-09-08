// SPDX-License-Identifier: Apache-2.0

#include "OxfwProfileBuilder.hpp"
#include "../Common/CommonProfileBuilder.hpp"

#include <algorithm>

namespace ASFW::Audio::Families::OXFW {

std::expected<Devices::ResolvedAudioEndpointProfile, Devices::ProfileBuildError>
BuildProfile(const Devices::ProfileBuildContext& context) noexcept {
    const auto* facts = std::get_if<Devices::OxfwProbeFacts>(&context.probeFacts);
    if (facts == nullptr) {
        return std::unexpected(Devices::ProfileBuildError::WrongProbeFacts);
    }
    auto result = Common::BuildBase(context, facts->streams, facts->supportedRates);
    if (!result) {
        return result;
    }
    auto& profile = *result;
    profile.streamMode = Devices::StreamModePolicy::Blocking;
    profile.captureIsoChannelPolicy = Duplex::IsoChannelPolicy::IRMSelectable;
    profile.playbackIsoChannelPolicy = Duplex::IsoChannelPolicy::IRMSelectable;
    profile.startPolicy.startReceiveBeforeDeviceRx = true;
    profile.startPolicy.startTransmitBeforeDeviceTx = true;
    profile.startPolicy.postDeviceEnableDelayMs = 0;
    profile.stopPolicy.disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive = true;
    profile.facets.push_back({Devices::FacetKind::Parameters, 0x44554554});
    // Content availability must not gate packet production: an unavailable PCM
    // range is transmitted as silence rather than withheld. Attested for this
    // device class by Linux (sound/firewire/amdtp-am824.c:358-363, via
    // snd-oxfw/snd-bebob) and by Apple's AppleFWAudio, whose AM824DCLWrite
    // refill has no availability check at all.
    profile.txPacketPolicy.substituteSilenceOnPcmUnavailable = true;
    const bool isApogeeDuet =
        context.staticPlan.profileBuilder ==
        DeviceProfiles::Audio::ProfileBuilderId::ApogeeDuet;
    if (isApogeeDuet) {
        // The Duet's host-selectable formations: 44.1k, 48k, 96k.  Its
        // AV/C control path has no optical selector, so the absent selectors
        // are represented as nullopt rather than a fictitious S/PDIF mode.
        for (const uint32_t rate : {44100U, 48000U, 96000U}) {
            if (std::find(facts->supportedRates.begin(), facts->supportedRates.end(), rate) ==
                facts->supportedRates.end() ||
                profile.configurationCapabilityCount >= profile.configurationCapabilities.size()) {
                continue;
            }
            auto& capability = profile.configurationCapabilities[
                profile.configurationCapabilityCount++];
            capability.configuration = {.sampleRate = rate};
            capability.runtimeCaps = facts->streams;
            capability.runtimeCaps.sampleRateHz = rate;
        }
    }
    Common::AddDefaultTiming(profile, 500);
    for (uint8_t i = 0; i < profile.timingCount; ++i) {
        if (!isApogeeDuet) continue;

        // Match the original AppleFWAudio Duet personality. These are HAL
        // accounting values, not scheduler depths: the profile owns them and
        // graph construction must not replace them with a generic ring or
        // dispatch-jitter floor. The descriptor finality contract is enforced
        // independently by the TX transport.
        auto& timing = profile.timing[i];
        if (timing.sampleRateHz == 44'100) {
            timing.inputLatencyFrames = 46;
            timing.outputLatencyFrames = 55;
            timing.inputSafetyFrames = 46;
            timing.outputSafetyFrames = 46;
        } else if (timing.sampleRateHz == 48'000) {
            timing.inputLatencyFrames = 40;
            timing.outputLatencyFrames = 67;
            timing.inputSafetyFrames = 50;
            timing.outputSafetyFrames = 50;
        } else if (timing.sampleRateHz == 96'000) {
            timing.inputLatencyFrames = 80;
            timing.outputLatencyFrames = 134;
            timing.inputSafetyFrames = 100;
            timing.outputSafetyFrames = 100;
        }
    }
    return result;
}

} // namespace ASFW::Audio::Families::OXFW
