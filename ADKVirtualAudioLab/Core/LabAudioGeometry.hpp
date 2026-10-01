#pragma once

#include "../../ASFWDriver/Shared/Isoch/AudioHalBufferProfiles.hpp"

#include <cstdint>

namespace ASFW::Lab {

struct LabAudioGeometry final {
    uint32_t sampleRateHz{0};
    uint32_t activeRingFrames{0};
    uint32_t allocatedRingFrames{0};
    uint32_t zeroTimestampPeriodFrames{0};
    uint32_t clientIoBudgetFrames{0};
    uint32_t adkMaxClientIoFrames{0};
};

[[nodiscard]] constexpr LabAudioGeometry AudioGeometryForRate(
    uint32_t sampleRateHz) noexcept {
    const auto hal = ASFW::IsochTransport::HalBufferProfileForRate(sampleRateHz);
    return {
        .sampleRateHz = sampleRateHz,
        .activeRingFrames = hal.frameRingFrames,
        .allocatedRingFrames = ASFW::IsochTransport::kAllocatedFrameRingFrames,
        .zeroTimestampPeriodFrames = hal.zeroTimestampPeriodFrames,
        .clientIoBudgetFrames = hal.clientIoBudgetFrames,
        .adkMaxClientIoFrames =
            ASFW::IsochTransport::AdkMaxClientIoFrames(hal.zeroTimestampPeriodFrames),
    };
}

[[nodiscard]] constexpr bool IsValidAudioGeometry(
    const LabAudioGeometry& geometry) noexcept {
    return geometry.sampleRateHz != 0 &&
           geometry.activeRingFrames != 0 &&
           geometry.allocatedRingFrames >= geometry.activeRingFrames &&
           geometry.zeroTimestampPeriodFrames == geometry.activeRingFrames &&
           geometry.clientIoBudgetFrames != 0 &&
           geometry.adkMaxClientIoFrames >= geometry.clientIoBudgetFrames &&
           geometry.allocatedRingFrames % geometry.activeRingFrames == 0;
}

[[nodiscard]] constexpr uint32_t ActiveRingFrame(
    uint64_t absoluteFrame, const LabAudioGeometry& geometry) noexcept {
    return geometry.activeRingFrames == 0
        ? 0
        : static_cast<uint32_t>(absoluteFrame % geometry.activeRingFrames);
}

} // namespace ASFW::Lab
