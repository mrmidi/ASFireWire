// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AvcControlProjection.hpp"
#include "../../Protocols/AVC/FeatureChangeNotifications.hpp"
#include <bit>

namespace ASFW::Audio::Controls {
/// Pointer-free event shared by the nub and user-client projections.
struct ParameterChanged {
    uint64_t guid, revision, sequence;
    uint32_t parameterId;
    uint64_t kind, bits; // same bool/double/int encoding as selectors 81/82
    uint32_t origin{0}; // StateOrigin::Observed
};
[[nodiscard]] inline ParameterChanged ProjectAvcChange(const Protocols::AVC::FeatureChange& change) {
    const auto kind = change.mute ? AvcBindingKind::kFeatureMute : AvcBindingKind::kFeatureVolume;
    return {change.route.guid, change.revision, change.sequence,
            AvcParameterId(change.subunit, change.block, change.channel, kind).value,
            change.mute ? 0ull : 1ull,
            change.mute ? static_cast<uint64_t>(change.value != 0)
                        : std::bit_cast<uint64_t>(static_cast<double>(change.value) / 256.0)};
}
} // namespace ASFW::Audio::Controls
