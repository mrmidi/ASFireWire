// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "MAudioDuplexChoreography.hpp"
#include "../../../Runtime/HardwareSampleTimeline.hpp"

#include <cstdint>

namespace ASFW::Audio::Families::BeBoB::MAudio {

struct PresentationObservationResult final {
    bool captureReferencePlanted{false};
    TxClockAnchor captureReference{};
    bool observationReady{false};
    uint32_t groupCount{0};
    ASFW::Audio::Runtime::HardwarePresentationObservation observation{};
};

// Converts the special firmware's qualified TX callback into the same hardware
// observation contract used by every backend. It performs no nominal-rate
// projection and owns no HAL sample cursor.
class PresentationObserver final {
public:
    [[nodiscard]] bool Arm(StartEpoch startEpoch,
                           uint64_t timelineEpoch,
                           uint32_t sampleRateHz,
                           uint32_t presentationOffsetTicks) noexcept;
    void Disarm() noexcept;

    [[nodiscard]] PresentationObservationResult ObserveHardwareWake(
        uint64_t transportGeneration,
        uint64_t completionBusTicks,
        uint64_t correlationBusTicks,
        TxClockAnchor correlation,
        uint64_t sampleFrame,
        uint32_t frameCount) noexcept;

    [[nodiscard]] bool IsArmed() const noexcept { return armed_; }
    [[nodiscard]] const ChoreographyState& State() const noexcept {
        return state_;
    }

private:
    [[nodiscard]] ChoreographyStep Advance(ChoreographyEvent event) noexcept;
    StartEpoch startEpoch_{};
    ChoreographyState state_{Stopped{}};
    uint64_t timelineEpoch_{0};
    uint64_t lastTransportGeneration_{0};
    uint32_t observedGroupCount_{0};
    uint32_t presentationOffsetTicks_{0};
    bool armed_{false};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
