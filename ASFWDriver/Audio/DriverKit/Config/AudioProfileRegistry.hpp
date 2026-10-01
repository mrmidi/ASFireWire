// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.hpp
// Global profile registry dispatcher.
//
// Profiles are static singletons chosen by the catalog's profile builder. A
// device's discovered geometry travels in its published config, not here.

#pragma once

#include "DICE/DiceProfile.hpp"
#include "IAudioDeviceProfile.hpp"

#include <cstdint>

namespace ASFW::Isoch::Audio {

class AudioProfileRegistry {
public:
    /// Resolve the profile that owns this device's wire geometry.
    ///
    /// `profileBuilderId` is the device catalog's answer, carried across the
    /// nub by the side that actually identified the device (it holds the
    /// Config-ROM evidence; this side only ever sees scalars). When it is set,
    /// it decides, and no identity matching happens here at all.
    ///
    /// Zero means the publisher did not resolve one. The (vendorId, modelId)
    /// fallback below then runs, and says so in the log: that pair cannot
    /// express a published model_id of 0, so it cannot identify a MOTU device,
    /// and silently falling through to the generic DICE profile is how a device
    /// ends up with a geometry that rejects every packet it receives.
    [[nodiscard]] static const IAudioDeviceProfile* FindProfile(
        uint32_t vendorId,
        uint32_t modelId,
        uint64_t guid,
        uint32_t profileBuilderId = 0) noexcept;

    /// The profile a resolved ProfileBuilderId names, or nullptr when this
    /// branch carries no profile object for it. Exposed so the publishing side
    /// can resolve the same profile it is about to tell the nub about, without
    /// a second matcher.
    [[nodiscard]] static const IAudioDeviceProfile* ProfileForBuilderId(
        uint32_t profileBuilderId) noexcept;

    /// The DICE half of the above, typed so DICE callers get the DICE profile
    /// without a downcast. nullptr for every non-DICE builder.
    [[nodiscard]] static const DICE::DiceProfile* DiceProfileForBuilderId(
        uint32_t profileBuilderId) noexcept;
};

} // namespace ASFW::Isoch::Audio
