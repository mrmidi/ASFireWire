// SPDX-License-Identifier: Apache-2.0

#include "GenericAvcProfileBuilder.hpp"
#include "../Common/CommonProfileBuilder.hpp"

namespace ASFW::Audio::Families::AVC {

std::expected<Devices::ResolvedAudioEndpointProfile, Devices::ProfileBuildError>
BuildProfile(const Devices::ProfileBuildContext& context) noexcept {
    const auto* facts = std::get_if<Devices::GenericAvcProbeFacts>(&context.probeFacts);
    if (facts == nullptr || (!facts->hasAudioSubunit && !facts->hasMusicSubunit)) {
        return std::unexpected(Devices::ProfileBuildError::InsufficientSafeEvidence);
    }
    auto result = Common::BuildBase(context, facts->streams, facts->supportedRates);
    if (!result) {
        return result;
    }
    result->captureIsoChannelPolicy = Duplex::IsoChannelPolicy::IRMSelectable;
    result->playbackIsoChannelPolicy = Duplex::IsoChannelPolicy::IRMSelectable;
    Common::AddDefaultTiming(*result, 500);
    return result;
}

} // namespace ASFW::Audio::Families::AVC
