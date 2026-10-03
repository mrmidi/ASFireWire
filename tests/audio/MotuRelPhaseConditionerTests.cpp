// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The conditioning stage between `[RxPhaseRel]` and the controller: folding
// about the running operating point instead of about zero, and removing the
// cadence-lattice offset our own geometry puts on the measurement.
//
// A hardware run showed an earlier design adopting a lattice hop as a new
// operating point 12 times in six minutes; these tests pin the correction that
// replaced it, using the numbers that run produced.

#include "Audio/Protocols/MOTU/MotuRelPhaseConditioner.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::MOTU::kMotuRelCadenceQuantumTicks;
using ASFW::Audio::MOTU::kMotuRelWarmupSamples;
using ASFW::Audio::MOTU::MotuRelPhaseConditioner;
using ASFW::Audio::MOTU::MotuRelPhaseSample;
using ASFW::Audio::MOTU::MotuSphClockServo;

constexpr int64_t kCycle = 3072;
constexpr int64_t kHalfCycle = kCycle / 2;

// An operating point measured on hardware: 140 ticks from the +/-1536 fold cap,
// with a MAD of 1 tick.
constexpr int64_t kNearCapOperatingPoint = kHalfCycle - 140;

// Bring the stage past warmup at a stated level, and assert it got there.
void Warmup(MotuRelPhaseConditioner& stage, int64_t level) {
    for (uint32_t sample = 0; sample < kMotuRelWarmupSamples; ++sample) {
        stage.Accept(level);
    }
    ASSERT_TRUE(stage.HaveOperatingPoint());
    ASSERT_EQ(stage.OperatingPointTicks(), level);
}

// --- Correction 2: fold about the operating point ---------------------------

TEST(MotuRelPhaseFold, FoldingAboutZeroSplitsAnOperatingPointNearTheCap) {
    // The failure this correction exists for, stated as an assertion rather
    // than as prose: two samples 2 ticks apart, folded about zero, are reported
    // 3070 ticks apart because the branch cut runs between them.
    const int64_t low = kHalfCycle - 1;
    const int64_t high = kHalfCycle + 1;

    const int64_t foldedLow = MotuSphClockServo::FoldAbsolutePhaseErrorTicks(low);
    const int64_t foldedHigh = MotuSphClockServo::FoldAbsolutePhaseErrorTicks(high);

    EXPECT_EQ(foldedLow, 1535);
    EXPECT_EQ(foldedHigh, -1535);
    EXPECT_EQ(foldedHigh - foldedLow, -3070);
}

TEST(MotuRelPhaseFold, FoldingAboutTheOperatingPointKeepsNeighboursTogether) {
    const int64_t low = kHalfCycle - 1;
    const int64_t high = kHalfCycle + 1;
    const int64_t center = kHalfCycle;

    const int64_t foldedLow =
        MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(low, center);
    const int64_t foldedHigh =
        MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(high, center);

    EXPECT_EQ(foldedLow, low);
    EXPECT_EQ(foldedHigh, high);
    EXPECT_EQ(foldedHigh - foldedLow, 2);
}

TEST(MotuRelPhaseFold, FoldingAboutZeroIsTheSpecialCaseOfFoldingAboutAPoint) {
    for (int64_t ticks = -4 * kCycle; ticks <= 4 * kCycle; ++ticks) {
        EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(ticks, 0),
                  MotuSphClockServo::FoldAbsolutePhaseErrorTicks(ticks))
            << "ticks=" << ticks;
    }
}

TEST(MotuRelPhaseFold, EveryFoldAboutAPointIsCongruentAndWithinHalfACycle) {
    // The two properties that make this a fold and not a rewrite: same phase
    // modulo a cycle, and never further than half a cycle from the point.
    for (int64_t center = -2000; center <= 2000; center += 137) {
        for (int64_t ticks = -3 * kCycle; ticks <= 3 * kCycle; ticks += 7) {
            const int64_t folded =
                MotuSphClockServo::FoldAbsolutePhaseErrorTicksAbout(ticks, center);
            EXPECT_EQ(((folded - ticks) % kCycle + kCycle) % kCycle, 0)
                << "center=" << center << " ticks=" << ticks;
            const int64_t distance = folded - center;
            EXPECT_GT(distance, -kHalfCycle) << "center=" << center << " ticks=" << ticks;
            EXPECT_LE(distance, kHalfCycle) << "center=" << center << " ticks=" << ticks;
        }
    }
}

// --- The stage: warmup ------------------------------------------------------

TEST(MotuRelPhaseConditionerTest, PublishesNothingUntilAnOperatingPointExists) {
    MotuRelPhaseConditioner stage{};
    for (uint32_t sample = 0; sample + 1 < kMotuRelWarmupSamples; ++sample) {
        const MotuRelPhaseSample out = stage.Accept(700);
        EXPECT_FALSE(out.valid) << "sample=" << sample;
        EXPECT_FALSE(stage.HaveOperatingPoint());
    }
    const MotuRelPhaseSample out = stage.Accept(700);
    EXPECT_TRUE(out.valid);
    EXPECT_TRUE(stage.HaveOperatingPoint());
    EXPECT_EQ(out.centerTicks, 700);
    EXPECT_EQ(out.ticks, 700);
    EXPECT_EQ(stage.PublishedSamples(), 1u);
}

TEST(MotuRelPhaseConditionerTest, WarmupDoesNotStraddleAWrap) {
    MotuRelPhaseConditioner stage{};
    stage.Accept(kHalfCycle - 2);
    stage.Accept(kHalfCycle + 1);
    const MotuRelPhaseSample out = stage.Accept(kHalfCycle);

    ASSERT_TRUE(out.valid);
    EXPECT_EQ(out.centerTicks, kHalfCycle);
    EXPECT_LE(out.excessTicks < 0 ? -out.excessTicks : out.excessTicks, 2);
}

TEST(MotuRelPhaseConditionerTest, AHopDuringWarmupCannotDragTheFirstOperatingPoint) {
    // A lattice hop arriving before the operating point exists would otherwise
    // land in the median and put the whole stream a quantum off where it sits.
    MotuRelPhaseConditioner stage{};
    stage.Accept(900);
    stage.Accept(900 - kMotuRelCadenceQuantumTicks);
    const MotuRelPhaseSample out = stage.Accept(902);

    ASSERT_TRUE(out.valid);
    EXPECT_EQ(out.centerTicks, 900);
    EXPECT_GE(stage.LatticeCorrections(), 1u);
}

// --- The stage: the lattice is corrected, never adopted ---------------------

TEST(MotuRelPhaseConditionerTest, ALatticeHopIsSubtractedAndTheSampleStillPublishes) {
    // The hardware lesson. 1024 is gcd(4096, 3072) -- our projection against our
    // packetizer -- so a hop carries no device phase. Subtract it and publish;
    // withholding the sample was the old design, adopting it was worse.
    MotuRelPhaseConditioner stage{};
    Warmup(stage, 900);

    const MotuRelPhaseSample out = stage.Accept(900 - kMotuRelCadenceQuantumTicks);
    EXPECT_TRUE(out.valid);
    EXPECT_EQ(out.latticeCorrectionTicks, -kMotuRelCadenceQuantumTicks);
    EXPECT_EQ(out.ticks, 900);
    EXPECT_EQ(out.excessTicks, 0);
    EXPECT_EQ(stage.OperatingPointTicks(), 900);
    EXPECT_EQ(stage.LatticeCorrections(), 1u);
}

TEST(MotuRelPhaseConditionerTest, ThePointOfWorkNeverWandersDownTheLattice) {
    // The exact failure hardware recorded: hops alternating either side of the
    // operating point, sustained for many windows at a time. The old stage
    // walked -3542 -> -6657 -> -5633 and re-referenced the loop 12 times. The
    // operating point must not move at all.
    MotuRelPhaseConditioner stage{};
    constexpr int64_t level = -3542;
    Warmup(stage, level);

    for (int block = 0; block < 6; ++block) {
        const int64_t offset =
            (block % 2 == 0) ? -kMotuRelCadenceQuantumTicks : kMotuRelCadenceQuantumTicks;
        for (int repeat = 0; repeat < 5; ++repeat) {
            const MotuRelPhaseSample out = stage.Accept(level + offset);
            ASSERT_TRUE(out.valid) << "block=" << block << " repeat=" << repeat;
            EXPECT_EQ(out.ticks, level) << "block=" << block;
            EXPECT_EQ(out.latticeCorrectionTicks, offset) << "block=" << block;
        }
    }

    EXPECT_EQ(stage.OperatingPointTicks(), level);
    EXPECT_EQ(stage.LatticeCorrections(), 30u);
    EXPECT_EQ(stage.PublishedSamples(), 31u);
}

TEST(MotuRelPhaseConditionerTest, APersistentCadenceShiftBecomesASteadyCorrection) {
    // If the packetizer's cadence phase moves for good, every later sample
    // carries the same offset. That must stay a constant correction, not
    // accumulate and not re-reference anything.
    MotuRelPhaseConditioner stage{};
    Warmup(stage, 900);

    for (int sample = 0; sample < 40; ++sample) {
        const MotuRelPhaseSample out = stage.Accept(900 + kMotuRelCadenceQuantumTicks);
        ASSERT_TRUE(out.valid) << "sample=" << sample;
        EXPECT_EQ(out.latticeCorrectionTicks, kMotuRelCadenceQuantumTicks);
        EXPECT_EQ(out.ticks, 900);
    }
    EXPECT_EQ(stage.OperatingPointTicks(), 900);
}

TEST(MotuRelPhaseConditionerTest, NoiseAtTheCapNeverReportsACycleStep) {
    MotuRelPhaseConditioner stage{};
    const int64_t center = kNearCapOperatingPoint;
    Warmup(stage, center);

    int64_t previous = center;
    for (int64_t offset = 0; offset <= 280; ++offset) {
        const MotuRelPhaseSample out = stage.Accept(center + offset);
        ASSERT_TRUE(out.valid) << "offset=" << offset;
        const int64_t step = out.ticks - previous;
        EXPECT_LT(step < 0 ? -step : step, kMotuRelCadenceQuantumTicks / 2)
            << "offset=" << offset << " ticks=" << out.ticks;
        previous = out.ticks;
    }

    EXPECT_EQ(previous, center + 280);
    EXPECT_EQ(stage.LatticeCorrections(), 0u);
}

TEST(MotuRelPhaseConditionerTest, TheSweepRampIsNotMistakenForALatticeHop) {
    // The measured sweep is ~42 ticks per window and runs for as
    // long as the stream does, straight through bucket boundaries and past
    // whole cycles. None of it is a lattice offset.
    MotuRelPhaseConditioner stage{};
    Warmup(stage, 0);

    int64_t level = 0;
    for (int window = 0; window < 200; ++window) {
        level += 42;
        const MotuRelPhaseSample out = stage.Accept(level);
        ASSERT_TRUE(out.valid) << "window=" << window << " level=" << level;
        EXPECT_EQ(out.latticeCorrectionTicks, 0) << "window=" << window;
    }
    EXPECT_EQ(stage.LatticeCorrections(), 0u);
}

TEST(MotuRelPhaseConditionerTest, ADisplacementOffTheLatticeIsSignalAndSurvives) {
    // The correction is narrow on purpose: only within 64 ticks of a whole
    // quantum. Anything else is signal and reaches the controller untouched.
    MotuRelPhaseConditioner stage{};
    Warmup(stage, 900);

    const MotuRelPhaseSample out = stage.Accept(900 + 512);
    EXPECT_TRUE(out.valid);
    EXPECT_EQ(out.latticeCorrectionTicks, 0);
    EXPECT_EQ(out.ticks, 900 + 512);

    MotuRelPhaseConditioner edge{};
    Warmup(edge, 900);
    EXPECT_EQ(edge.Accept(900 + kMotuRelCadenceQuantumTicks - 65).latticeCorrectionTicks, 0);

    MotuRelPhaseConditioner inside{};
    Warmup(inside, 900);
    EXPECT_EQ(inside.Accept(900 + kMotuRelCadenceQuantumTicks - 64).latticeCorrectionTicks,
              kMotuRelCadenceQuantumTicks);
}

TEST(MotuRelPhaseConditionerTest, ResetClearsTheOperatingPointAndTheCounters) {
    MotuRelPhaseConditioner stage{};
    Warmup(stage, 900);
    stage.Accept(900 - kMotuRelCadenceQuantumTicks);
    ASSERT_EQ(stage.LatticeCorrections(), 1u);

    stage.Reset();
    EXPECT_FALSE(stage.HaveOperatingPoint());
    EXPECT_EQ(stage.LatticeCorrections(), 0u);
    EXPECT_EQ(stage.PublishedSamples(), 0u);
    EXPECT_FALSE(stage.Accept(900).valid);
}

} // namespace
