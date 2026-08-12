// SPDX-License-Identifier: Apache-2.0

#include "OxfwProfileBuilder.hpp"
#include "../Common/CommonProfileBuilder.hpp"

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
    Common::AddDefaultTiming(profile, 500);
    for (uint8_t i = 0; i < profile.timingCount; ++i) {
        profile.timing[i].inputLatencyFrames = 128;
        profile.timing[i].outputLatencyFrames = 128;
        profile.timing[i].inputSafetyFrames = 128;
        profile.timing[i].outputSafetyFrames = 64;
    }
    return result;
}

} // namespace ASFW::Audio::Families::OXFW
