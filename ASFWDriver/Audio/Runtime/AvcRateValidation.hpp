// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ResolvedAudioConfiguration.hpp"

#ifndef ASFW_AVC_MULTIRATE_VALIDATION
#define ASFW_AVC_MULTIRATE_VALIDATION 0
#endif

namespace ASFW::Audio::Runtime {
// Explicit hardware-batch build opt-in. Shipping builds do not turn descriptor
// support into a claim of hardware validation.
inline constexpr bool kAvcHardwareBatch = ASFW_AVC_MULTIRATE_VALIDATION != 0;
[[nodiscard]] inline bool AvcRateEnabled(const RateFormation& formation, uint32_t baseline) noexcept {
    return formation.protocolSupported &&
        (formation.hardwareValidated || formation.sampleRateHz == baseline || kAvcHardwareBatch);
}
}
