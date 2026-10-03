// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "MotuV3WireFormat.hpp"

#include <cstdint>
#include <limits>

namespace ASFW::Protocols::Audio::AMDTP {

// Pure fractional clock for MOTU V3 transmit SPH. The packetizer owns this
// state because only it knows which DATA frames were actually accepted for
// transmission. A future controller may change StepQ32(), but it does not own
// phase or the cumulative applied correction.
class MotuSphQ32Accumulator final {
public:
    static constexpr uint32_t kFractionBits = 32;
    static constexpr int64_t kOneQ32 = int64_t{1} << kFractionBits;
    static constexpr int64_t kTickDomainQ32 =
        static_cast<int64_t>(MotuV3Wire::kTickDomain) * kOneQ32;

    [[nodiscard]] static constexpr bool SupportsRate(
        uint32_t sampleRateHz) noexcept {
        switch (sampleRateHz) {
            case 44100:
            case 48000:
            case 88200:
            case 96000:
            case 176400:
            case 192000:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] static constexpr int64_t NominalStepQ32ForRate(
        uint32_t sampleRateHz) noexcept {
        if (!SupportsRate(sampleRateHz)) {
            return 0;
        }
        return static_cast<int64_t>(
            (static_cast<__int128>(MotuV3Wire::kTicksPerSecond)
             << kFractionBits) /
            sampleRateHz);
    }

    [[nodiscard]] bool Configure(uint32_t sampleRateHz) noexcept {
        const int64_t nominal = NominalStepQ32ForRate(sampleRateHz);
        if (nominal <= 0) {
            configured_ = false;
            sampleRateHz_ = 0;
            nominalStepQ32_ = 0;
            Reset();
            return false;
        }

        configured_ = true;
        sampleRateHz_ = sampleRateHz;
        nominalStepQ32_ = nominal;
        Reset();
        return true;
    }

    void Reset() noexcept {
        phaseQ32_ = 0;
        pendingPhaseCorrectionQ32_ = 0;
        appliedCorrectionQ32_ = 0;
        stepQ32_ = configured_ ? nominalStepQ32_ : 0;
        seeded_ = false;
    }

    [[nodiscard]] bool SeedOnce(int64_t ticks) noexcept {
        if (!configured_ || seeded_) {
            return false;
        }
        phaseQ32_ = NormalizePhaseQ32(
            static_cast<__int128>(MotuV3Wire::NormalizeTicks(ticks)) * kOneQ32 +
            pendingPhaseCorrectionQ32_);
        pendingPhaseCorrectionQ32_ = 0;
        seeded_ = true;
        return true;
    }

    [[nodiscard]] bool SetStepQ32(int64_t stepQ32) noexcept {
        if (!configured_ || stepQ32 <= 0 || stepQ32 >= kTickDomainQ32) {
            return false;
        }
        stepQ32_ = stepQ32;
        return true;
    }

    // Apply a one-shot phase repair selected by the owning clock loop. The
    // same delta is included in AppliedCorrectionQ32(), otherwise the servo
    // would not observe its own actuation and would request the same repair
    // again. A pre-seed repair is retained and folded into the first seed.
    [[nodiscard]] bool ApplyPhaseCorrectionQ32(int64_t correctionQ32) noexcept {
        if (!configured_) {
            return false;
        }

        appliedCorrectionQ32_ = SaturateToInt64(
            static_cast<__int128>(appliedCorrectionQ32_) + correctionQ32);
        if (seeded_) {
            phaseQ32_ = NormalizePhaseQ32(
                static_cast<__int128>(phaseQ32_) + correctionQ32);
        } else {
            pendingPhaseCorrectionQ32_ = NormalizePhaseQ32(
                static_cast<__int128>(pendingPhaseCorrectionQ32_) + correctionQ32);
        }
        return true;
    }

    void Advance() noexcept {
        if (!seeded_) {
            return;
        }

        phaseQ32_ += stepQ32_;
        if (phaseQ32_ >= kTickDomainQ32) {
            phaseQ32_ %= kTickDomainQ32;
        }

        appliedCorrectionQ32_ = SaturateToInt64(
            static_cast<__int128>(appliedCorrectionQ32_) +
            (static_cast<__int128>(stepQ32_) - nominalStepQ32_));
    }

    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }
    [[nodiscard]] bool IsSeeded() const noexcept { return seeded_; }
    [[nodiscard]] uint32_t SampleRateHz() const noexcept { return sampleRateHz_; }
    [[nodiscard]] int64_t PhaseQ32() const noexcept { return phaseQ32_; }
    [[nodiscard]] int64_t StepQ32() const noexcept { return stepQ32_; }
    [[nodiscard]] int64_t NominalStepQ32() const noexcept {
        return nominalStepQ32_;
    }
    [[nodiscard]] int64_t AppliedCorrectionQ32() const noexcept {
        return appliedCorrectionQ32_;
    }
    [[nodiscard]] int64_t CurrentTicks() const noexcept {
        return seeded_ ? phaseQ32_ / kOneQ32 : 0;
    }
    [[nodiscard]] uint32_t CurrentSph() const noexcept {
        return seeded_ ? MotuV3Wire::EncodeSph(CurrentTicks()) : 0;
    }

private:
    [[nodiscard]] static constexpr int64_t NormalizePhaseQ32(
        __int128 value) noexcept {
        value %= static_cast<__int128>(kTickDomainQ32);
        if (value < 0) {
            value += kTickDomainQ32;
        }
        return static_cast<int64_t>(value);
    }

    [[nodiscard]] static constexpr int64_t SaturateToInt64(
        __int128 value) noexcept {
        if (value > std::numeric_limits<int64_t>::max()) {
            return std::numeric_limits<int64_t>::max();
        }
        if (value < std::numeric_limits<int64_t>::min()) {
            return std::numeric_limits<int64_t>::min();
        }
        return static_cast<int64_t>(value);
    }

    static_assert(kTickDomainQ32 > 0);
    static_assert(kTickDomainQ32 < std::numeric_limits<int64_t>::max());

    uint32_t sampleRateHz_{0};
    int64_t phaseQ32_{0};
    int64_t pendingPhaseCorrectionQ32_{0};
    int64_t stepQ32_{0};
    int64_t nominalStepQ32_{0};
    int64_t appliedCorrectionQ32_{0};
    bool configured_{false};
    bool seeded_{false};
};

} // namespace ASFW::Protocols::Audio::AMDTP
