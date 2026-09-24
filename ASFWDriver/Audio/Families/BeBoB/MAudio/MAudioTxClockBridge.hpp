// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "MAudioPresentationObserver.hpp"
#include "../../../Runtime/HardwareSampleTimeline.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Audio::Families::BeBoB::MAudio {

struct TxDataClockObservation final {
    uint64_t completionBusTicks{0};
    uint64_t correlationBusTicks{0};
    uint64_t sampleFrame{0};
    uint32_t frameCount{0};
    uint16_t sytOffsetTicks{0};
};

/// The special cadence is DATA at phases 0/1024/2048, then NO-DATA. Queue
/// indices retain cadence phase because the 912-slot shared ring is divisible
/// by four.
[[nodiscard]] constexpr uint16_t SytOffsetTicksForPacketIndex(
    const uint64_t packetIndex) noexcept {
    const uint64_t phase = packetIndex % 4U;
    return phase < 3U ? static_cast<uint16_t>(phase * 1024U) : 0;
}

struct TxClockBoundaryResult final {
    bool ready{false};
    ASFW::Audio::Runtime::HardwareZeroTimestamp boundary{};
};

/// Qualifies M-Audio TX completion timing and turns data-packet observations
/// into HAL-grid boundaries. All mutation is serialized on the TX preparation
/// queue after Arm() completes before transport start.
class TxClockBridge final {
public:
    [[nodiscard]] bool Arm(uint64_t startEpoch,
                           uint32_t sampleRateHz,
                           uint32_t zeroTimestampPeriodFrames,
                           uint32_t presentationOffsetTicks) noexcept;
    void Disarm() noexcept;

    [[nodiscard]] TxClockBoundaryResult ObserveWake(
        uint64_t transportGeneration,
        uint32_t correlationCycleTime,
        uint64_t correlationBusTicks,
        uint64_t correlationHostTicks,
        uint64_t newestCompletionBusTicks,
        std::span<const TxDataClockObservation> dataPackets) noexcept;

    [[nodiscard]] uint64_t Epoch() const noexcept { return epoch_; }

private:
    ASFW::Audio::Runtime::HardwareSampleTimeline timeline_{};
    PresentationObserver observer_{};
    uint64_t epoch_{0};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
