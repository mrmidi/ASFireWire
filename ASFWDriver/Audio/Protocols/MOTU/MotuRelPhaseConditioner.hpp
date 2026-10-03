// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Conditioning stage between the `[RxPhaseRel]` measurement and the SPH clock
// controller: it tracks the running operating point, folds each sample about
// that point rather than about zero, and removes the cadence-lattice offset our
// own geometry puts on the measurement. Pure arithmetic and pure state -- no
// wire, no runtime, no DriverKit -- so the host tests pin the whole of it.
//
// Redesigned after a hardware run showed the first design's failure mode.
//
// WHY A CORRECTION AND NOT A GATE. The first design rejected a lattice hop and,
// if it persisted, ADOPTED it as a new operating point. Hardware showed what
// that costs: 24 rejections and 12 adoptions in six minutes, the operating
// point wandering -3542 -> -6657 -> -5633, and -- had the bridge been
// delivering -- a forced re-reference every ~30 s. The error was a category
// mistake. The quantum is `gcd(4096, 3072) = 1024`: 4096 ticks is what the SPH
// advances per DATA packet, 3072 what the projection adds per prepared cycle
// It is a property of OUR instrument, not of the device,
// so a hop carries no device phase whatsoever -- it must be subtracted, never
// promoted to a state. Subtracting it also keeps the signal continuous: no
// withheld samples, no re-references, and a persistent cadence shift simply
// becomes the same correction applied every window.

#pragma once

#include "MotuSphClockServo.hpp"

#include <cstdint>

namespace ASFW::Audio::MOTU {

// The cadence quantum. See the file comment for where 1024 comes from and why
// it belongs to the instrument rather than to the device.
inline constexpr int64_t kMotuRelCadenceQuantumTicks = 1024;

// Recognition tolerance around a multiple of the quantum. The sweep ramps the
// series ~42 ticks per window (measured on hardware) and the running median
// trails it by a few windows, so a hop arrives displaced by rather more than
// the sample noise -- 64 is generous towards that and still tight against the
// 1024-tick spacing. Offline analysis of `[RxPhaseRel]` must use the same
// tolerance: the instrument and the driver must classify a hop identically or
// a run cannot be read against what the loop saw.
inline constexpr int64_t kMotuRelBucketHopToleranceTicks = 64;

// Running-median window over published samples. Odd on purpose (a true median,
// no averaging of the two middles) and short enough that the ~42 tick/window
// sweep is tracked with a bounded lag rather than integrated.
inline constexpr uint32_t kMotuRelMedianWindowSamples = 9;

// Samples required before an operating point exists at all. Until then nothing
// is published: with no operating point there is no branch to fold about and no
// level to measure a lattice offset from, and the honest report is "not yet"
// rather than a guess seeded off one sample.
inline constexpr uint32_t kMotuRelWarmupSamples = 3;

static_assert(kMotuRelWarmupSamples <= kMotuRelMedianWindowSamples,
              "warmup fills the median window, it cannot exceed it");
static_assert(kMotuRelMedianWindowSamples % 2 == 1, "median window must be odd");

struct MotuRelPhaseSample final {
    // True when this sample may be handed to the controller. False only during
    // warmup -- after that every sample is published, corrected if it needed
    // correcting. Nothing is withheld: a withheld sample was the old design.
    bool valid{false};

    // The measurement, folded about the operating point and with any lattice
    // offset removed.
    int64_t ticks{0};

    // The running operating point this sample was folded about. The consumer
    // needs it too: seeding against the setpoint has to fold about the same
    // point, or it reintroduces the branch cut this stage moved away.
    int64_t centerTicks{0};

    // Signed distance from the operating point AFTER correction.
    int64_t excessTicks{0};

    // What was subtracted as instrument geometry: a signed multiple of the
    // quantum, zero when the sample sat off the lattice and was left alone.
    // Reported rather than silently applied -- a run whose corrections stop
    // being occasional is telling us something about the packetizer.
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

    MotuRelPhaseSample Accept(int64_t relTicks) noexcept {
        MotuRelPhaseSample out{};

        if (!haveCenter_) {
            // Warmup. Fold about the first sample so the window cannot straddle
            // a wrap while it fills, and correct against that same reference so
            // a hop arriving mid-warmup cannot drag the first operating point
            // a whole quantum off where the stream actually sits.
            const int64_t reference = stored_ == 0 ? relTicks : window_[0];
            const int64_t folded =
                MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(relTicks, reference);
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
            MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(relTicks, centerTicks_);
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
    // The lattice offset this excess carries: a signed multiple of the quantum
    // when it sits within tolerance of one, otherwise zero.
    //
    // The assumption, stated rather than buried: real device phase never sits
    // more than half a quantum (512 ticks) from the running operating point
    // between two windows. Measured dynamics support it by a wide margin -- the
    // sweep moves ~42 ticks per window and the median trails by a few of them,
    // so the honest excess stays in the low hundreds. A genuine
    // device excursion past 512 ticks in one window WOULD be mis-corrected, and
    // that is the price of removing an artefact that is otherwise
    // indistinguishable from signal. The correction count is published so such
    // a regime announces itself instead of hiding.
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

    // Insertion sort on a fixed copy: the window is nine entries and this runs
    // once per four-second telemetry gate, so the simple form is the right one
    // and it pulls in no allocation and no <algorithm> on the DriverKit side.
    // While the window is still filling `stored_` can be even, and the upper of
    // the two middles is taken rather than their mean -- a published sample is
    // an observed value and the operating point stays one of them.
    [[nodiscard]] int64_t Median() const noexcept {
        int64_t sorted[kMotuRelMedianWindowSamples]{};
        for (uint32_t index = 0; index < stored_; ++index) {
            int64_t value = window_[index];
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

} // namespace ASFW::Audio::MOTU
