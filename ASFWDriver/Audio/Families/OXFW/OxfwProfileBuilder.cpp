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
        // Output: measured, not assumed. [TxLead] reports the presentation
        // lead this driver actually stamps -- 53876 ticks, 105 frames at
        // 48 kHz, min 105 / max 111 on the Duet.
        //
        // It is NOT the 25 frames of txTransferDelayTicks alone. The duplex
        // path stamps transmitBusTicks + replayEntry.sytOffset + transfer, and
        // the recovered sytOffset carries a full 16-cycle modulus whenever the
        // device's raw SYT offset falls below the transfer delay. Linux does
        // the identical thing (amdtp-stream.c:483-487, "Subtract transfer delay
        // so that the synchronization offset is not so large at transmission"),
        // so this is ordinary AMDTP behaviour rather than a defect, and our
        // 12800-tick transfer delay matches its blocking-mode derivation at
        // :288-292 exactly.
        //
        // It has to be reported because HardwareSampleTimeline anchors on TX
        // completion -- transmit time -- so everything after transmission falls
        // outside the safety offset. The device's own analogue delay sits on
        // top and is not claimed here; Apple reports 67 total for this
        // hardware, which their anchor must place differently, so their split
        // is not transferable to ours. Moving our anchor to presentation time
        // would fold these 105 frames into it and is the real lever.
        profile.timing[i].outputLatencyFrames = 105;
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
