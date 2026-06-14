#pragma once

#include <algorithm>
#include <cstdint>

namespace ASFW::Audio {

[[nodiscard]] constexpr uint32_t RequiredInputSafetyFrames(
    uint32_t profileInputSafety,
    uint32_t outputSafety,
    uint32_t maximumClientIoFrames,
    uint32_t maximumFramesPerInterrupt,
    uint32_t schedulingJitterFrames) noexcept {
    return std::max(
        profileInputSafety,
        std::max(
            outputSafety + maximumClientIoFrames + schedulingJitterFrames,
            maximumFramesPerInterrupt + schedulingJitterFrames));
}

} // namespace ASFW::Audio
