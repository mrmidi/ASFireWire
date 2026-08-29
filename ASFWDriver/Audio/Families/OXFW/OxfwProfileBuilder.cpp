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
    if (context.staticPlan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::ApogeeDuet) {
        // The Duet's host-selectable formations are the two base rates.  Its
        // AV/C control path has no optical selector, so the absent selectors
        // are represented as nullopt rather than a fictitious S/PDIF mode.
        for (const uint32_t rate : {44100U, 48000U}) {
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
        // Reported latency is the delay CoreAudio must account for BEYOND the
        // safety offset -- the part of the path the client cannot shorten by
        // writing earlier. It is not a scheduling budget, and it was 128 in
        // both directions purely as a placeholder.
        //
        // Output: the presentation lead this driver stamps is
        // txTransferDelayTicks = 12800 ticks = 25 frames at 48 kHz (see
        // ASFWAudioDriverZts.cpp, presentationBusTicks = transmitBusTicks +
        // transfer). The rest is the device's own analogue delay, which no
        // host-side measurement can reach. Apple's shipping override for this
        // exact hardware reports 67 total, so ~42 frames belong to the device.
        // Both terms are checked on hardware: [TxLead] reports the stamped lead
        // so the 25 is measured rather than assumed.
        profile.timing[i].outputLatencyFrames = 67;
        // Input: acquisition-to-delivery is device ADC plus wire; our own
        // decode latency lives inside the interrupt batch that inputSafety
        // already covers, so it must not be counted twice. Reference value for
        // this hardware, same source.
        profile.timing[i].inputLatencyFrames = 40;
        // Safety stays 128. AudioTimingGeometry derives a floor of 104 -- one
        // maximum interrupt batch (40 frames; observed acquisition age ran
        // 15-30) plus a 64-frame scheduling cushion -- which the 32-frame ring
        // alignment rounds back up to 128 anyway. Going lower means shrinking
        // that cushion, and the only measurement we have of this queue's tail
        // is [TxPrep]'s 8 ms outliers. Not without RX-side numbers.
        profile.timing[i].inputSafetyFrames = 128;
        profile.timing[i].outputSafetyFrames = 64;
    }
    return result;
}

} // namespace ASFW::Audio::Families::OXFW
