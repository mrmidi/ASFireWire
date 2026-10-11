// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuSphQ32Accumulator.hpp - fractional transmit SPH clock for MOTU streams.
//
// The transmit SPH is a free-running clock: seeded once per stream, then
// advanced by one sample period per data block. The period is held in Q32.32
// ticks so the 44.1 kHz family keeps its exact rational value (557.278911...
// ticks per frame at 44.1 kHz) instead of drifting by the integer truncation.
//
// The packetizer owns this state because only it knows which frames were
// actually stamped. The SPH regulator (MotuSphClockServo) changes the step and
// may request a one-shot phase repair; it never owns the phase.

#pragma once

#include "MotuBlockLayout.hpp"
#include "MotuSph.hpp"

#include <cstdint>
#include <limits>

namespace ASFW::Encoding::Motu {

class MotuSphQ32Accumulator final {
public:
    static constexpr uint32_t kFractionBits = 32;
    static constexpr int64_t kOneQ32 = int64_t{1} << kFractionBits;

    // The phase wraps once per second, like the SPH it encodes.
    // observed: the 828mk3's own stream leaves the cycle timer's seconds field
    // (bits 31:25) zero in every data packet (passive bus capture).
    // linux: amdtp-motu.c:383 wraps the tick at TICKS_PER_SECOND; FFADO masks
    // the SPH with 0x1ffffff (libstreaming/motu/MotuReceiveStreamProcessor.cpp:58).
    static constexpr int64_t kTickDomainQ32 = static_cast<int64_t>(kTicksPerSecond) * kOneQ32;

    [[nodiscard]] static constexpr bool SupportsRate(uint32_t sampleRateHz) noexcept {
        return RateToIndex(sampleRateHz) >= 0;
    }

    // floor(kTicksPerSecond / rate) in Q32.32: exact to one Q32 unit per frame.
    [[nodiscard]] static constexpr int64_t NominalStepQ32ForRate(uint32_t sampleRateHz) noexcept {
        if (!SupportsRate(sampleRateHz)) {
            return 0;
        }
        return static_cast<int64_t>(
            (static_cast<__int128>(kTicksPerSecond) << kFractionBits) / sampleRateHz);
    }

    [[nodiscard]] bool Configure(uint32_t sampleRateHz) noexcept {
        const int64_t nominal = NominalStepQ32ForRate(sampleRateHz);
        configured_ = nominal > 0;
        sampleRateHz_ = configured_ ? sampleRateHz : 0;
        nominalStepQ32_ = nominal;
        Reset();
        return configured_;
    }

    void Reset() noexcept {
        phaseQ32_ = 0;
        pendingPhaseCorrectionQ32_ = 0;
        appliedCorrectionQ32_ = 0;
        stepQ32_ = configured_ ? nominalStepQ32_ : 0;
        seeded_ = false;
    }

    // Once per stream: the first stamped block's presentation time, in ticks
    // on any timeline (it is wrapped onto the one-second one here).
    [[nodiscard]] bool SeedOnce(int64_t ticks) noexcept {
        if (!configured_ || seeded_) {
            return false;
        }
        phaseQ32_ = NormalizePhaseQ32(static_cast<__int128>(WrapTick(ticks)) * kOneQ32 +
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

    // A one-shot phase repair chosen by the regulator. The same delta is
    // counted in AppliedCorrectionQ32(): otherwise the regulator would not see
    // its own actuation and would ask for the same repair again. A repair
    // requested before the seed is kept and folded into the seed.
    [[nodiscard]] bool ApplyPhaseCorrectionQ32(int64_t correctionQ32) noexcept {
        if (!configured_) {
            return false;
        }
        appliedCorrectionQ32_ =
            SaturateToInt64(static_cast<__int128>(appliedCorrectionQ32_) + correctionQ32);
        if (seeded_) {
            phaseQ32_ = NormalizePhaseQ32(static_cast<__int128>(phaseQ32_) + correctionQ32);
        } else {
            pendingPhaseCorrectionQ32_ = NormalizePhaseQ32(
                static_cast<__int128>(pendingPhaseCorrectionQ32_) + correctionQ32);
        }
        return true;
    }

    // One stamped block. Before the seed nothing is stamped, so nothing moves.
    void Advance() noexcept {
        if (!seeded_) {
            return;
        }
        phaseQ32_ += stepQ32_;
        if (phaseQ32_ >= kTickDomainQ32) {
            phaseQ32_ %= kTickDomainQ32;
        }
        appliedCorrectionQ32_ = SaturateToInt64(static_cast<__int128>(appliedCorrectionQ32_) +
                                                (static_cast<__int128>(stepQ32_) - nominalStepQ32_));
    }

    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }
    [[nodiscard]] bool IsSeeded() const noexcept { return seeded_; }
    [[nodiscard]] uint32_t SampleRateHz() const noexcept { return sampleRateHz_; }
    [[nodiscard]] int64_t PhaseQ32() const noexcept { return phaseQ32_; }
    [[nodiscard]] int64_t StepQ32() const noexcept { return stepQ32_; }
    [[nodiscard]] int64_t NominalStepQ32() const noexcept { return nominalStepQ32_; }
    // Cumulative movement of the stamped phase relative to the exact nominal
    // step since Reset(), repairs included, in Q32.32 ticks.
    [[nodiscard]] int64_t AppliedCorrectionQ32() const noexcept { return appliedCorrectionQ32_; }
    [[nodiscard]] uint32_t CurrentTicks() const noexcept {
        return seeded_ ? static_cast<uint32_t>(phaseQ32_ / kOneQ32) : 0;
    }
    [[nodiscard]] uint32_t CurrentSph() const noexcept {
        return seeded_ ? SphFromTick(CurrentTicks()) : 0;
    }

private:
    [[nodiscard]] static constexpr int64_t NormalizePhaseQ32(__int128 value) noexcept {
        value %= static_cast<__int128>(kTickDomainQ32);
        if (value < 0) {
            value += kTickDomainQ32;
        }
        return static_cast<int64_t>(value);
    }

    [[nodiscard]] static constexpr int64_t SaturateToInt64(__int128 value) noexcept {
        if (value > std::numeric_limits<int64_t>::max()) {
            return std::numeric_limits<int64_t>::max();
        }
        if (value < std::numeric_limits<int64_t>::min()) {
            return std::numeric_limits<int64_t>::min();
        }
        return static_cast<int64_t>(value);
    }

    static_assert(kTickDomainQ32 > 0);
    static_assert(kTickDomainQ32 < std::numeric_limits<int64_t>::max() / 2,
                  "phase plus one step must not overflow before the wrap");

    uint32_t sampleRateHz_{0};
    int64_t phaseQ32_{0};
    int64_t pendingPhaseCorrectionQ32_{0};
    int64_t stepQ32_{0};
    int64_t nominalStepQ32_{0};
    int64_t appliedCorrectionQ32_{0};
    bool configured_{false};
    bool seeded_{false};
};

} // namespace ASFW::Encoding::Motu
