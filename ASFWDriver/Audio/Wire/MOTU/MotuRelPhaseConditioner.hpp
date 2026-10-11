// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRelPhaseConditioner.hpp - conditioning between the measured absolute
// presentation phase of the MOTU transmit stream and the SPH regulator.
//
// It tracks the running operating point, folds each sample about that point
// rather than about zero, and removes the cadence-lattice offset that our own
// measurement geometry puts on the sample. Pure arithmetic and state.
//
// The lattice. The measurement compares the SPH of a transmitted DATA packet
// with a projection of its transmit cycle. The SPH advances 4096 ticks per
// DATA packet in the 48 kHz family and the projection 3072 per cycle, so which
// packet of the cadence the measurement lands on shifts the reading by a
// multiple of gcd(4096, 3072) = 1024 ticks. That is a property of the
// instrument, not of the device: a hop carries no device phase, so it is
// subtracted and never adopted as a new operating point.
// measured: on an 828mk3 at 48 kHz the measured phase stayed constant modulo
// 1024 ticks; a design that adopted hops instead walked its operating point
// by thousands of ticks within minutes.

#pragma once

#include "MotuSphClockServo.hpp"

#include <cstdint>

namespace ASFW::Encoding::Motu {

inline constexpr int64_t kMotuRelCadenceQuantumTicks = 1024;

// Recognition tolerance around a multiple of the quantum. The phase drifts
// about 42 ticks per telemetry window (measured on an 828mk3) and the running
// median trails it by a few windows, so a hop arrives displaced by rather more
// than the sample noise; 64 allows for that and stays tight against the
// 1024-tick spacing.
inline constexpr int64_t kMotuRelBucketHopToleranceTicks = 64;

// Running-median window. Odd, so the median is an observed sample, and short
// enough to follow the ~42 tick/window drift with a bounded lag.
inline constexpr uint32_t kMotuRelMedianWindowSamples = 9;

// Samples before an operating point exists. Until then nothing is published:
// there is no branch to fold about and no level to measure a lattice offset
// from.
inline constexpr uint32_t kMotuRelWarmupSamples = 3;

static_assert(kMotuRelWarmupSamples <= kMotuRelMedianWindowSamples,
              "warmup fills the median window, it cannot exceed it");
static_assert(kMotuRelMedianWindowSamples % 2 == 1, "median window must be odd");

struct MotuRelPhaseSample final {
    // False only during warmup. Afterwards every sample is published,
    // corrected if it needed correcting; nothing is withheld.
    bool valid{false};

    // The sample, folded about the operating point, lattice offset removed.
    int64_t ticks{0};

    // The operating point this sample was folded about. The regulator folds
    // its seed about the same point (MotuSphServoObservation::
    // absolutePhaseCenterTicks), or it would reintroduce the branch cut.
    int64_t centerTicks{0};

    // Signed distance from the operating point after correction.
    int64_t excessTicks{0};

    // What was subtracted as instrument geometry: a signed multiple of the
    // quantum, zero when the sample sat off the lattice.
    int64_t latticeCorrectionTicks{0};
};

class MotuRelPhaseConditioner final {
  public:
    void Reset() noexcept {
        stored_ = 0;
        next_ = 0;
        centerTicks_ = 0;
        haveCenter_ = false;
        corrections_ = 0;
        published_ = 0;
    }

    [[nodiscard]] bool HaveOperatingPoint() const noexcept { return haveCenter_; }
    [[nodiscard]] int64_t OperatingPointTicks() const noexcept { return centerTicks_; }
    [[nodiscard]] uint64_t LatticeCorrections() const noexcept { return corrections_; }
    [[nodiscard]] uint64_t PublishedSamples() const noexcept { return published_; }

    MotuRelPhaseSample Accept(int64_t phaseTicks) noexcept {
        MotuRelPhaseSample out{};

        if (!haveCenter_) {
            // Warmup: fold and correct against the first sample, so the window
            // cannot straddle a wrap while it fills and a hop arriving now
            // cannot drag the first operating point a whole quantum off.
            const int64_t reference = stored_ == 0 ? phaseTicks : window_[0];
            const int64_t folded =
                MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(phaseTicks, reference);
            const int64_t correction = LatticeOffsetTicks(folded - reference);
            const int64_t corrected = folded - correction;
            if (correction != 0) {
                ++corrections_;
            }
            Push(corrected);
            if (stored_ < kMotuRelWarmupSamples) {
                out.centerTicks = corrected;
                out.latticeCorrectionTicks = correction;
                return out;
            }
            centerTicks_ = Median();
            haveCenter_ = true;
            ++published_;
            out.valid = true;
            out.ticks = corrected;
            out.centerTicks = centerTicks_;
            out.excessTicks = corrected - centerTicks_;
            out.latticeCorrectionTicks = correction;
            return out;
        }

        const int64_t folded =
            MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(phaseTicks, centerTicks_);
        const int64_t correction = LatticeOffsetTicks(folded - centerTicks_);
        const int64_t corrected = folded - correction;
        if (correction != 0) {
            ++corrections_;
        }

        Push(corrected);
        centerTicks_ = Median();
        ++published_;
        out.valid = true;
        out.ticks = corrected;
        out.centerTicks = centerTicks_;
        out.excessTicks = corrected - centerTicks_;
        out.latticeCorrectionTicks = correction;
        return out;
    }

  private:
    // The lattice offset an excess carries: a signed multiple of the quantum
    // when it sits within tolerance of one, otherwise zero.
    // assumed: real device phase never moves more than half a quantum (512
    // ticks) from the running operating point between two windows. A genuine
    // excursion that large would be mis-corrected; the published correction
    // count makes such a regime visible.
    [[nodiscard]] static constexpr int64_t LatticeOffsetTicks(int64_t excess) noexcept {
        const int64_t magnitude = excess < 0 ? -excess : excess;
        const int64_t buckets =
            (magnitude + kMotuRelCadenceQuantumTicks / 2) / kMotuRelCadenceQuantumTicks;
        if (buckets == 0) {
            return 0;
        }
        const int64_t residue = magnitude - buckets * kMotuRelCadenceQuantumTicks;
        const int64_t distance = residue < 0 ? -residue : residue;
        if (distance > kMotuRelBucketHopToleranceTicks) {
            return 0;
        }
        const int64_t offset = buckets * kMotuRelCadenceQuantumTicks;
        return excess < 0 ? -offset : offset;
    }

    void Push(int64_t value) noexcept {
        window_[next_] = value;
        next_ = (next_ + 1) % kMotuRelMedianWindowSamples;
        if (stored_ < kMotuRelMedianWindowSamples) {
            ++stored_;
        }
    }

    // Insertion sort on a fixed copy of at most nine entries, once per
    // telemetry window: allocation-free. While the window is filling the upper
    // of the two middles is taken, so the operating point is always an
    // observed sample.
    [[nodiscard]] int64_t Median() const noexcept {
        int64_t sorted[kMotuRelMedianWindowSamples]{};
        for (uint32_t index = 0; index < stored_; ++index) {
            const int64_t value = window_[index];
            uint32_t position = index;
            while (position > 0 && sorted[position - 1] > value) {
                sorted[position] = sorted[position - 1];
                --position;
            }
            sorted[position] = value;
        }
        return sorted[stored_ / 2];
    }

    int64_t window_[kMotuRelMedianWindowSamples]{};
    uint32_t stored_{0};
    uint32_t next_{0};
    int64_t centerTicks_{0};
    bool haveCenter_{false};
    uint64_t corrections_{0};
    uint64_t published_{0};
};

} // namespace ASFW::Encoding::Motu
