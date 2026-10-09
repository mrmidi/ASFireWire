// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResolvedAudioConfiguration.hpp"

#ifndef ASFW_AVC_MULTIRATE_VALIDATION
#define ASFW_AVC_MULTIRATE_VALIDATION 0
#endif
#ifndef ASFW_DICE_MULTIRATE_VALIDATION
#define ASFW_DICE_MULTIRATE_VALIDATION 0
#endif

namespace ASFW::Audio::Runtime {
// Xcode builds enable complete AV/C candidates by default. This build policy
// does not turn descriptor support into hardware-validation evidence.
inline constexpr bool kAvcHardwareBatch = ASFW_AVC_MULTIRATE_VALIDATION != 0;
inline constexpr bool kDiceHardwareBatch = ASFW_DICE_MULTIRATE_VALIDATION != 0;
[[nodiscard]] inline bool RateEnabled(const RateFormation& formation, uint32_t baseline,
                                        bool dice = false) noexcept {
    return formation.protocolSupported &&
        (formation.hardwareValidated || formation.packedPcm || formation.sampleRateHz == baseline ||
         (dice ? formation.sampleRateHz <= 48000 || kDiceHardwareBatch : kAvcHardwareBatch));
}
}
