// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Protocols/MOTU/MotuSphClockServo.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using ASFW::Audio::MOTU::MotuSphClockServo;
using ASFW::Audio::MOTU::MotuSphServoConfig;
using ASFW::Audio::MOTU::MotuSphServoDecision;
using ASFW::Audio::MOTU::MotuSphServoObservation;

constexpr int64_t kNano = 1000000000;
constexpr int64_t kTicksPerSecond = 24576000;
constexpr std::array<uint32_t, 6> kRates{44100, 48000, 88200, 96000, 176400, 192000};

int64_t StepAtPpb(uint32_t rate, int64_t ppb) {
    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    return static_cast<int64_t>((static_cast<__int128>(nominal) * (kNano + ppb)) / kNano);
}

MotuSphServoConfig Config(uint32_t rate, uint64_t horizonSeconds = 8, int64_t boundPpb = 100000) {
    return {
        .sampleRateHz = rate,
        .phaseCorrectionHorizonFrames = rate * horizonSeconds,
        .minimumStepQ32 = StepAtPpb(rate, -boundPpb),
        .maximumStepQ32 = StepAtPpb(rate, boundPpb),
    };
}

int64_t TicksForFrames(uint64_t frames, uint32_t rate, int64_t ppb) {
    return static_cast<int64_t>((static_cast<__int128>(frames) * kTicksPerSecond * (kNano + ppb)) /
                                (static_cast<__int128>(rate) * kNano));
}

MotuSphServoObservation Observation(uint64_t generation, uint64_t rxFrames, int64_t rxTicks,
                                    int64_t txCorrectionQ32 = 0) {
    return {
        .valid = true,
        .generation = generation,
        .rxFrames = rxFrames,
        .rxTicks = rxTicks,
        .txCorrectionQ32 = txCorrectionQ32,
    };
}

// One closed-loop run at a chosen update interval.  `intervalFrames` is the D
// the caller actually delivers, which is the quantity P3 normalises the gain
// to; the production wiring only guarantees it is at least the configured
// horizon, never that it equals it.
struct LoopTrace final {
    std::vector<int64_t> phaseErrorTicks;
    std::vector<int64_t> stepQ32;
    std::vector<int64_t> measuredStepQ32;
};

LoopTrace RunLoop(const MotuSphServoConfig& config, uint64_t intervalFrames, uint64_t updates,
                  int64_t devicePpb, const std::vector<int64_t>& noiseTicks = {}) {
    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(config));
    EXPECT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);

    const int64_t nominal = MotuSphClockServo::NominalStepQ32(config.sampleRateHz);
    int64_t appliedStep = nominal;
    int64_t txCorrectionQ32 = 0;
    int64_t rxTicks = 0;
    LoopTrace trace{};

    for (uint64_t update = 1; update <= updates; ++update) {
        // The packetizer emits `appliedStep` for the whole interval, so the
        // correction the loop has already actuated grows by the deviation from
        // nominal times the frames it covered.
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * intervalFrames);
        rxTicks += TicksForFrames(intervalFrames, config.sampleRateHz, devicePpb);

        // Quantisation is an error on each reading, not something the device
        // carries forward: the next SPH is quantised against the true edge, not
        // against the previous rounding. Folding the noise into `rxTicks` would
        // model a random walk in the timestamp instead, which is a different
        // and much harsher signal than the one this test claims to inject.
        const int64_t reportedTicks =
            rxTicks + (noiseTicks.empty() ? 0 : noiseTicks[(update - 1) % noiseTicks.size()]);

        const auto decision =
            servo.Update(Observation(1, update * intervalFrames, reportedTicks, txCorrectionQ32));
        EXPECT_TRUE(decision.feedbackUpdated);
        trace.phaseErrorTicks.push_back(decision.phaseErrorTicks);
        trace.stepQ32.push_back(decision.stepQ32);
        trace.measuredStepQ32.push_back(decision.measuredStepQ32);
        appliedStep = decision.stepQ32;
    }
    return trace;
}

// Variance, not its root: the comparison P3 asks for is monotone in either, and
// staying in the squared domain keeps the ratio exact for the assertion below.
double Variance(const std::vector<int64_t>& values) {
    double mean = 0.0;
    for (const int64_t value : values) {
        mean += static_cast<double>(value);
    }
    mean /= static_cast<double>(values.size());

    double sum = 0.0;
    for (const int64_t value : values) {
        const double delta = static_cast<double>(value) - mean;
        sum += delta * delta;
    }
    return sum / static_cast<double>(values.size() - 1);
}

TEST(MotuSphClockServoTests, RejectsInvalidOrUnsupportedConfiguration) {
    MotuSphClockServo servo{};
    EXPECT_FALSE(servo.Configure(Config(32000)));

    auto zeroHorizon = Config(48000);
    zeroHorizon.phaseCorrectionHorizonFrames = 0;
    EXPECT_FALSE(servo.Configure(zeroHorizon));

    auto invertedBounds = Config(48000);
    const int64_t minimum = invertedBounds.minimumStepQ32;
    invertedBounds.minimumStepQ32 = invertedBounds.maximumStepQ32;
    invertedBounds.maximumStepQ32 = minimum;
    EXPECT_FALSE(servo.Configure(invertedBounds));
    EXPECT_FALSE(servo.IsConfigured());
}

TEST(MotuSphClockServoTests, PhaseGainPerUpdateIsTheSameHoweverLateTheCallerIs) {
    constexpr uint32_t rate = 48000;
    // The production horizon is the HAL IO period, not a multiple of a second.
    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = 512;

    // D = H, 2H and 4H. Before P3 the gain per update was D/H, so these three
    // ran at 1, 2 and 4; now every one of them has to settle at 1/4.
    for (const uint64_t multiple : {uint64_t{1}, uint64_t{2}, uint64_t{4}}) {
        SCOPED_TRACE(multiple);
        const auto trace = RunLoop(config, 512 * multiple, 12, -2400);

        // Each update removes a quarter of the standing error, so what is left
        // is three quarters of it. Deadbeat would leave nothing and then
        // overshoot -- that is the failure this pins, not slow convergence.
        for (size_t i = 1; i + 1 < trace.phaseErrorTicks.size(); ++i) {
            const double previous = static_cast<double>(trace.phaseErrorTicks[i - 1]);
            const double current = static_cast<double>(trace.phaseErrorTicks[i]);
            if (std::llabs(trace.phaseErrorTicks[i - 1]) < 8) {
                continue; // Below this the integer division dominates the ratio.
            }
            EXPECT_NEAR(current / previous, 0.75, 0.05);
        }
    }
}

TEST(MotuSphClockServoTests, LoopStillConvergesWhenTheCallerRunsFourHorizonsLate) {
    constexpr uint32_t rate = 48000;
    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = 512;

    // Regression on a latent instability, not a hypothetical: with the gain
    // divided by a fixed H, an interval of 4H gave a loop gain of 4, so the
    // error inverted and grew by 3x every update until the clamp caught it.
    const auto trace = RunLoop(config, 512 * 4, 16, -2400);

    const int64_t first = std::llabs(trace.phaseErrorTicks.front());
    const int64_t last = std::llabs(trace.phaseErrorTicks.back());
    EXPECT_LT(last, first);
    for (size_t i = 1; i < trace.phaseErrorTicks.size(); ++i) {
        EXPECT_LE(std::llabs(trace.phaseErrorTicks[i]),
                  std::llabs(trace.phaseErrorTicks[i - 1]) + 1);
    }
}

TEST(MotuSphClockServoTests, PhaseGainCutsNoiseAmplificationFourfoldWithoutRemovingIt) {
    constexpr uint32_t rate = 48000;
    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = 512;

    // One tick of quantisation on each reading, alternating sign.
    //
    // The output step is `measured + correction`, and the correction is derived
    // from the same reading whose noise the measurement already carries, so the
    // two are positively correlated and the output can never be quieter than
    // the raw measurement. This loop does not filter its feed-forward term, so
    // "output variance below measurement variance" is not a property it can
    // have -- the honest claim is that a quarter of the gain amplifies a
    // quarter as much, and that is what this pins.
    const std::vector<int64_t> noise{+1, -1, -1, +1, +1, -1};

    auto amplificationAtShift = [&](uint32_t shift) {
        auto shifted = config;
        shifted.phaseGainShift = shift;
        const auto trace = RunLoop(shifted, 512, 40, -2400, noise);
        // Skip the opening updates: the loop is still pulling in the standing
        // rate offset there, which is signal and would swamp the noise term.
        const std::vector<int64_t> step(trace.stepQ32.begin() + 8, trace.stepQ32.end());
        const std::vector<int64_t> measured(trace.measuredStepQ32.begin() + 8,
                                            trace.measuredStepQ32.end());
        return Variance(step) / Variance(measured);
    };

    const double legacyGain = amplificationAtShift(0); // the fixed-H loop at D = H
    const double p3Gain = amplificationAtShift(2);

    EXPECT_GT(legacyGain, 4.0);
    EXPECT_LT(p3Gain, 2.0);
    // Variance scales with the square of the loop gain, so a 4x smaller gain
    // has to show up as roughly 4x less amplification, not merely "less".
    EXPECT_GT(legacyGain / p3Gain, 3.5);

    // The floor is 1, not 0, and this pins why. Drive the gain to nothing and
    // the step becomes the measurement, ratio 1 exactly. Any real gain adds a
    // correction built from the same reading the measurement differenced, so
    // the two are positively correlated and the sum is always the noisier of
    // the two. "Output quieter than input" is therefore unreachable for this
    // loop at any gain -- it needs a filtered feed-forward term, which is not
    // what P3 changes. Recorded so the criterion is not mistaken for a target.
    EXPECT_NEAR(amplificationAtShift(MotuSphClockServo::kMaximumPhaseGainShift), 1.0, 0.01);
    EXPECT_GT(p3Gain, 1.0);
}

TEST(MotuSphClockServoTests, RejectsAPhaseGainShiftThatCouldOverflowTheDivisor) {
    MotuSphClockServo servo{};
    auto tooLarge = Config(48000);
    tooLarge.phaseGainShift = MotuSphClockServo::kMaximumPhaseGainShift + 1;
    EXPECT_FALSE(servo.Configure(tooLarge));

    auto atBound = Config(48000);
    atBound.phaseGainShift = MotuSphClockServo::kMaximumPhaseGainShift;
    EXPECT_TRUE(servo.Configure(atBound));
}

TEST(MotuSphClockServoTests, NominalPeriodIsExactForAllSixCapturedRates) {
    for (const uint32_t rate : kRates) {
        SCOPED_TRACE(rate);
        MotuSphClockServo servo{};
        ASSERT_TRUE(servo.Configure(Config(rate)));

        const MotuSphServoDecision baseline = servo.Update(Observation(1, 0, 0));
        ASSERT_TRUE(baseline.valid);
        EXPECT_TRUE(baseline.phaseReferenceReset);
        EXPECT_FALSE(baseline.feedbackUpdated);

        // Four exact seconds make both rational rate families integral in the
        // raw tick domain while still exercising the Q32.32 conversion.
        const uint64_t frames = static_cast<uint64_t>(rate) * 4;
        const MotuSphServoDecision decision =
            servo.Update(Observation(1, frames, kTicksPerSecond * 4));

        ASSERT_TRUE(decision.feedbackUpdated);
        EXPECT_EQ(decision.measuredStepQ32, MotuSphClockServo::NominalStepQ32(rate));
        EXPECT_EQ(decision.stepQ32, decision.measuredStepQ32);
        EXPECT_EQ(decision.phaseErrorTicks, 0);
        EXPECT_FALSE(decision.stepClamped);
        EXPECT_FALSE(decision.hardResyncRequired);
    }
}

TEST(MotuSphClockServoTests, FirstSnapshotOnlyEstablishesACommonPhaseReference) {
    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(48000)));

    // The hardware export's first retained snapshot was already at -4.823
    // cycles. An absolute open-loop value must not fire the safety valve when
    // the controller first joins an existing stream.
    constexpr int64_t retainedPhaseTicks = -14818;
    constexpr uint64_t retainedFrames = 18429144;
    const int64_t retainedMeasuredTicks =
        TicksForFrames(retainedFrames, 48000, 0) + retainedPhaseTicks;

    const auto decision = servo.Update(Observation(7, retainedFrames, retainedMeasuredTicks));
    EXPECT_TRUE(decision.phaseReferenceReset);
    EXPECT_FALSE(decision.feedbackUpdated);
    EXPECT_FALSE(decision.hardResyncRequired);
    EXPECT_EQ(decision.phaseErrorTicks, 0);
    EXPECT_EQ(decision.stepQ32, MotuSphClockServo::NominalStepQ32(48000));
}

TEST(MotuSphClockServoTests, MeasuredHardwareDriftConvergesAtAllSixRates) {
    constexpr int64_t measuredPpb = -1612;
    for (const uint32_t rate : kRates) {
        SCOPED_TRACE(rate);
        const uint64_t windowFrames = static_cast<uint64_t>(rate) * 4;

        MotuSphClockServo servo{};
        ASSERT_TRUE(servo.Configure(Config(rate)));
        ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);

        const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
        int64_t appliedStep = nominal;
        int64_t txCorrectionQ32 = 0;
        MotuSphServoDecision first{};
        MotuSphServoDecision last{};

        for (uint64_t window = 1; window <= 12; ++window) {
            txCorrectionQ32 +=
                static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * windowFrames);
            const uint64_t frames = window * windowFrames;
            last = servo.Update(
                Observation(1, frames, TicksForFrames(frames, rate, measuredPpb), txCorrectionQ32));
            if (window == 1) {
                first = last;
            }
            ASSERT_TRUE(last.feedbackUpdated);
            EXPECT_FALSE(last.hardResyncRequired);
            appliedStep = last.stepQ32;
        }

        EXPECT_LT(std::llabs(last.phaseErrorTicks), std::llabs(first.phaseErrorTicks));
        EXPECT_NEAR(static_cast<double>(last.stepQ32), static_cast<double>(last.measuredStepQ32),
                    static_cast<double>(StepAtPpb(rate, 20) - nominal));
    }
}

TEST(MotuSphClockServoTests, SameLoopTracksSlowThermalChangeWithoutASecondMechanism) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t windowFrames = rate * 4;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);

    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    int64_t appliedStep = nominal;
    int64_t txCorrectionQ32 = 0;
    int64_t rxTicks = 0;
    MotuSphServoDecision decision{};

    // Distilled from the retained hardware window: approximately -1583 ppb in
    // its first third and -1648 ppb in its last third.  There is deliberately
    // no thermal-step API or alternate branch -- Update() is the only loop.
    for (uint64_t window = 1; window <= 18; ++window) {
        const int64_t devicePpb = window <= 6 ? -1583 : -1648;
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * windowFrames);
        rxTicks += TicksForFrames(windowFrames, rate, devicePpb);
        decision = servo.Update(Observation(1, window * windowFrames, rxTicks, txCorrectionQ32));

        ASSERT_TRUE(decision.feedbackUpdated);
        EXPECT_FALSE(decision.hardResyncRequired);
        EXPECT_LT(std::llabs(decision.phaseErrorTicks),
                  MotuSphClockServo::kHardResyncThresholdTicks);
        appliedStep = decision.stepQ32;
    }

    EXPECT_NEAR(static_cast<double>(decision.stepQ32),
                static_cast<double>(decision.measuredStepQ32),
                static_cast<double>(StepAtPpb(rate, 20) - nominal));
}

TEST(MotuSphClockServoTests, HardResyncBoundaryIsStrictlyGreaterThanFourCycles) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = static_cast<uint64_t>(rate) * 100;
    constexpr int64_t nominalTicks = kTicksPerSecond * 100;
    constexpr int64_t threshold = MotuSphClockServo::kHardResyncThresholdTicks;

    auto Evaluate = [&](int64_t phaseTicks) {
        MotuSphClockServo servo{};
        EXPECT_TRUE(servo.Configure(Config(rate)));
        EXPECT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
        return servo.Update(Observation(1, frames, nominalTicks + phaseTicks));
    };

    EXPECT_FALSE(Evaluate(threshold).hardResyncRequired);
    EXPECT_FALSE(Evaluate(-threshold).hardResyncRequired);

    const auto positive = Evaluate(threshold + 1);
    EXPECT_TRUE(positive.hardResyncRequired);
    EXPECT_EQ(positive.stepQ32, positive.measuredStepQ32);

    const auto negative = Evaluate(-threshold - 1);
    EXPECT_TRUE(negative.hardResyncRequired);
    EXPECT_EQ(negative.stepQ32, negative.measuredStepQ32);
}

TEST(MotuSphClockServoTests, StepClampContainsRateAndPhaseCorrection) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;
    constexpr int64_t devicePpb = 5000;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate, 8, 1000)));
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);

    const auto decision =
        servo.Update(Observation(1, frames, TicksForFrames(frames, rate, devicePpb)));
    EXPECT_TRUE(decision.feedbackUpdated);
    EXPECT_TRUE(decision.stepClamped);
    EXPECT_EQ(decision.stepQ32, StepAtPpb(rate, 1000));
    EXPECT_FALSE(decision.hardResyncRequired);
}

TEST(MotuSphClockServoTests, GenerationChangeRebasesWithoutCarryingOldPhase) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
    ASSERT_TRUE(
        servo.Update(Observation(1, frames, TicksForFrames(frames, rate, -1612))).feedbackUpdated);

    const auto reset = servo.Update(Observation(2, 0, 0));
    EXPECT_TRUE(reset.phaseReferenceReset);
    EXPECT_FALSE(reset.feedbackUpdated);
    EXPECT_FALSE(reset.hardResyncRequired);
    EXPECT_EQ(reset.phaseErrorTicks, 0);
    EXPECT_EQ(reset.stepQ32, MotuSphClockServo::NominalStepQ32(rate));

    const auto nominal = servo.Update(Observation(2, frames, TicksForFrames(frames, rate, 0)));
    EXPECT_TRUE(nominal.feedbackUpdated);
    EXPECT_EQ(nominal.phaseErrorTicks, 0);
    EXPECT_EQ(nominal.stepQ32, MotuSphClockServo::NominalStepQ32(rate));
}

// ---------------------------------------------------------------------------
// Absolute seed: the reference is seeded with the measured absolute error
// instead of being zeroed.
// ---------------------------------------------------------------------------

MotuSphServoObservation AbsoluteObservation(uint64_t generation, uint64_t rxFrames, int64_t rxTicks,
                                            int64_t absoluteErrorTicks,
                                            int64_t txCorrectionQ32 = 0) {
    return {
        .valid = true,
        .generation = generation,
        .rxFrames = rxFrames,
        .rxTicks = rxTicks,
        .txCorrectionQ32 = txCorrectionQ32,
        .haveAbsolutePhaseError = true,
        .absolutePhaseErrorTicks = absoluteErrorTicks,
    };
}

// The defect the seed exists to fix: without a seed a stream that starts a thousand
// ticks off reports zero error forever, so nothing downstream can act on it.
//
// The reported error is the DISTANCE TO THE SETPOINT, `setpoint - rel`, not
// the measurement itself. Pinning `phaseErrorTicks == rel` instead is what once
// let an inverted seed go unnoticed.
TEST(MotuSphClockServoTests, ReferenceSeedsTheMeasuredOffsetInsteadOfZeroingIt) {
    constexpr uint32_t rate = 48000;
    constexpr int64_t kMeasuredRel = -887; // a measured hardware median
    // 510 - (-887). Spelled out rather than derived from the production
    // constant, so a change to either side of the arithmetic fails here.
    constexpr int64_t kErrorToSetpoint = 1397;

    ASSERT_EQ(ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kOracleRelSetpointTicks, 510);

    MotuSphClockServo relative{};
    ASSERT_TRUE(relative.Configure(Config(rate)));
    const auto withoutSeed = relative.Update(Observation(1, 0, 0));
    ASSERT_TRUE(withoutSeed.phaseReferenceReset);
    EXPECT_EQ(withoutSeed.phaseErrorTicks, 0);

    MotuSphClockServo absolute{};
    ASSERT_TRUE(absolute.Configure(Config(rate)));
    const auto withSeed = absolute.Update(AbsoluteObservation(1, 0, 0, kMeasuredRel));
    ASSERT_TRUE(withSeed.phaseReferenceReset);
    EXPECT_EQ(withSeed.phaseErrorTicks, kErrorToSetpoint);

    // A stream sitting exactly on the measured setpoint has nothing to correct.
    MotuSphClockServo onSetpoint{};
    ASSERT_TRUE(onSetpoint.Configure(Config(rate)));
    EXPECT_EQ(onSetpoint.Update(AbsoluteObservation(1, 0, 0, 510)).phaseErrorTicks, 0);

    // A perfectly tracking device still reports the offset it actually has.
    constexpr uint64_t frames = rate * 4;
    const auto tracked =
        absolute.Update(AbsoluteObservation(1, frames, TicksForFrames(frames, rate, 0), 0));
    EXPECT_TRUE(tracked.feedbackUpdated);
    EXPECT_EQ(tracked.phaseErrorTicks, kErrorToSetpoint);
}

// OHCI completion stamps have no sub-cycle field, so `rel` picks
// up a +/-1 cycle pedestal that flipped branch in 27% of hardware windows. The
// three readings below are the same physical phase; the loop must not treat a
// stamp-quantisation branch flip as a 3072-tick step in the error.
TEST(MotuSphClockServoTests, AbsoluteSeedFoldsTheOneCycleStampPedestal) {
    constexpr uint32_t rate = 48000;
    constexpr int64_t kCycle = 3072;
    constexpr int64_t kMeasuredRel = -887;

    for (const int64_t branch : {int64_t{0}, kCycle, -kCycle, 2 * kCycle}) {
        SCOPED_TRACE(branch);
        MotuSphClockServo servo{};
        ASSERT_TRUE(servo.Configure(Config(rate)));
        const auto seeded = servo.Update(AbsoluteObservation(1, 0, 0, kMeasuredRel + branch));
        EXPECT_EQ(seeded.phaseErrorTicks, 1397); // 510 - (-887)
    }

    // The fold is a half-open half-cycle: exactly half a cycle stays positive,
    // one tick past it reads as the shorter negative distance.
    EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(kCycle / 2), kCycle / 2);
    EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(kCycle / 2 + 1), -(kCycle / 2 - 1));
    EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(0), 0);
}

// Mandatory rather than optional: hardware measured the offset WANDERING (-2.12 -> -1.51 ppm over 59 min), so a loop that only
// survives a constant offset has not been tested against reality.
TEST(MotuSphClockServoTests, AbsoluteSeedSurvivesAWanderingOffsetRamp) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t intervalFrames = 512;
    // 0.1 ppm over ten minutes, delivered at the HAL IO period.
    constexpr uint64_t updates = (uint64_t{rate} * 600) / intervalFrames;
    constexpr int64_t kEndPpb = 100; // 0.1 ppm
    constexpr int64_t kSeedTicks = -887;

    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = intervalFrames;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(config));
    ASSERT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, kSeedTicks)).phaseReferenceReset);

    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    int64_t appliedStep = nominal;
    int64_t txCorrectionQ32 = 0;
    int64_t rxTicks = 0;
    int64_t worstSettledError = 0;
    uint64_t lastClampedUpdate = 0;

    for (uint64_t update = 1; update <= updates; ++update) {
        const int64_t ppb = (kEndPpb * static_cast<int64_t>(update)) / static_cast<int64_t>(updates);
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * intervalFrames);
        rxTicks += TicksForFrames(intervalFrames, rate, ppb);

        const auto decision = servo.Update(
            AbsoluteObservation(1, update * intervalFrames, rxTicks, kSeedTicks, txCorrectionQ32));
        ASSERT_TRUE(decision.feedbackUpdated);
        EXPECT_FALSE(decision.hardResyncRequired);
        if (decision.stepClamped) {
            lastClampedUpdate = update;
        }
        appliedStep = decision.stepQ32;

        // Ignore the settling transient; the claim is about the tracking state.
        if (update > updates / 10) {
            worstSettledError = std::max<int64_t>(worstSettledError, std::abs(decision.phaseErrorTicks));
        }
    }

    // A1 makes the loop actually CORRECT the standing offset instead of
    // freezing it, and the rate clamp is what paces that correction. The seed
    // is now the distance to the setpoint, 510 - (-887) = 1397 ticks = 2.73
    // frames; at the 100 ppm bound it takes ~54 updates of 512 frames, i.e.
    // about half a second at a 0.01% pitch excursion. Recording the clamp as
    // expected here, not as a defect -- but also bounding it, because a seed
    // that kept the loop pinned at the rate limit would be audible.
    EXPECT_GT(lastClampedUpdate, 0u);
    EXPECT_LT(lastClampedUpdate, updates / 10);

    // Once settled the loop tracks the wandering rate with its INTERNAL error
    // near zero -- not parked at the seed, and nowhere near the hard-resync
    // gate. Note what this does and does not show: the object model here is
    // open (`rxTicks` never responds to `appliedStep`), so this assertion holds
    // for either sign of the seed. Whether the loop moves the real phase toward
    // the setpoint or away from it is a separate claim, and it needs the closed
    // plant of SeedConvergesTheStandingOffsetOnlyWithTheCorrectSign below.
    EXPECT_LT(worstSettledError, 200);
    EXPECT_LT(worstSettledError, MotuSphClockServo::kHardResyncThresholdTicks);
}

// ---------------------------------------------------------------------------
// The closed plant. Every absolute-seed test above drives an OPEN object
// -- `rxTicks` is synthesised independently of the step the loop applied, and
// `absolutePhaseErrorTicks` is a constant handed in from outside -- so the
// controller always zeroes its INTERNAL error and all six passed with the seed
// inverted. Here the measured `rel` is recomputed from
// the phase the actuator has really accumulated:
//
//     rel = rel0 + (txDeviation - rxDeviation)
//
// which closes actuator -> measurement and is what makes the sign observable.
// ---------------------------------------------------------------------------

struct PlantResult final {
    int64_t finalRel{0};
    int64_t finalDistanceToSetpoint{0};
};

// `mirrorAroundSetpoint` feeds the servo `2*setpoint - rel` instead of `rel`,
// which is exactly the measurement that makes production derive the OPPOSITE
// seed. It is how this test proves it can tell the two signs apart without
// keeping a second copy of the controller.
PlantResult RunClosedPlant(int64_t initialRel, int64_t devicePpb, bool mirrorAroundSetpoint) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t intervalFrames = 512;
    constexpr uint64_t updates = 2000;
    constexpr int64_t setpoint = ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kOracleRelSetpointTicks;

    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = intervalFrames;

    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(config));

    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    int64_t appliedStep = nominal;
    int64_t txCorrectionQ32 = 0;
    int64_t rxTicks = 0;
    int64_t nominalTicks = 0;
    int64_t rel = initialRel;

    const auto reported = [&](int64_t phase) {
        return mirrorAroundSetpoint ? 2 * setpoint - phase : phase;
    };

    EXPECT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, reported(rel))).phaseReferenceReset);

    for (uint64_t update = 1; update <= updates; ++update) {
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * intervalFrames);
        rxTicks += TicksForFrames(intervalFrames, rate, devicePpb);
        nominalTicks += TicksForFrames(intervalFrames, rate, 0);

        // The standing phase moves with the difference of the two deviations:
        // what our packetizer has added on top of nominal, minus what the
        // device's own clock drifted away from it.
        const int64_t txDeviationTicks =
            static_cast<int64_t>(static_cast<__int128>(txCorrectionQ32) /
                                 static_cast<__int128>(MotuSphClockServo::kOneQ32));
        rel = initialRel + txDeviationTicks - (rxTicks - nominalTicks);

        const auto decision = servo.Update(AbsoluteObservation(
            1, update * intervalFrames, rxTicks, reported(rel), txCorrectionQ32));
        EXPECT_TRUE(decision.feedbackUpdated);
        EXPECT_FALSE(decision.hardResyncRequired);
        appliedStep = decision.stepQ32;
    }

    return {rel, rel - setpoint};
}

// ---------------------------------------------------------------------------
// The injected step, on the same closed plant.
//
// Before the loop is ever handed a MEASURED `rel`, the hardware run asks a
// smaller question: does the actuator move the standing phase at all, by how
// much, and in which direction? These tests fix what that run should see, so a
// disagreement on hardware is a finding about the device rather than an
// argument about what the code was supposed to do.
// ---------------------------------------------------------------------------

struct InjectionPlant final {
    int64_t relBeforeInjection{0};
    int64_t relAfterSettling{0};
    uint32_t injectionUpdates{0};
    bool hardResyncSeen{false};
};

InjectionPlant RunInjectionPlant(int64_t initialRel, int64_t injectionTicks,
                                 uint64_t injectAfterFrames) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t intervalFrames = 512;
    constexpr uint64_t updates = 4000;

    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = intervalFrames;
    config.phaseInjectionTicks = injectionTicks;
    config.phaseInjectionAfterFrames = injectAfterFrames;

    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(config));

    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    int64_t appliedStep = nominal;
    int64_t txCorrectionQ32 = 0;
    int64_t rxTicks = 0;
    int64_t nominalTicks = 0;
    int64_t rel = initialRel;
    InjectionPlant out{};

    EXPECT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, rel)).phaseReferenceReset);

    for (uint64_t update = 1; update <= updates; ++update) {
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * intervalFrames);
        rxTicks += TicksForFrames(intervalFrames, rate, 0);
        nominalTicks += TicksForFrames(intervalFrames, rate, 0);

        const int64_t txDeviationTicks =
            static_cast<int64_t>(static_cast<__int128>(txCorrectionQ32) /
                                 static_cast<__int128>(MotuSphClockServo::kOneQ32));
        rel = initialRel + txDeviationTicks - (rxTicks - nominalTicks);

        const auto decision = servo.Update(
            AbsoluteObservation(1, update * intervalFrames, rxTicks, rel, txCorrectionQ32));
        if (decision.phaseInjectionApplied) {
            ++out.injectionUpdates;
            out.relBeforeInjection = rel;
        }
        out.hardResyncSeen = out.hardResyncSeen || decision.hardResyncRequired;
        appliedStep = decision.stepQ32;
    }
    out.relAfterSettling = rel;
    return out;
}

// The measurement a hardware run makes, made here against a known plant:
// `rel` moves by the injected amount, in the injected direction.
TEST(MotuSphClockServoTests, InjectedStepMovesTheStandingPhaseByTheInjectedAmount) {
    constexpr int64_t kInitialRel = -887;
    constexpr uint64_t kAfterFrames = 48000; // one second in, well past settling

    for (const int64_t injection : {int64_t{1024}, int64_t{-1024}}) {
        SCOPED_TRACE(injection);
        const auto plant = RunInjectionPlant(kInitialRel, injection, kAfterFrames);

        EXPECT_EQ(plant.injectionUpdates, 1u); // one shot, not one per update
        EXPECT_FALSE(plant.hardResyncSeen);    // 1024 is far under the 12288 valve

        // Before the step the loop has already pulled the standing offset onto
        // the measured setpoint -- that is the closed-plant result, and it is what makes
        // "before" a defined quantity at all.
        EXPECT_NEAR(static_cast<double>(plant.relBeforeInjection),
                    static_cast<double>(
                        ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kOracleRelSetpointTicks),
                    32.0);

        // And after it, the phase sits one injection away from where it was.
        const int64_t moved = plant.relAfterSettling - plant.relBeforeInjection;
        EXPECT_NEAR(static_cast<double>(moved), static_cast<double>(injection), 32.0);
    }
}

// Mid-stream, not at the reference: an injection that fired on the first
// observation would be a seed by another name, and would compare two stream
// starts -- which hardware showed are unrelated operating points.
TEST(MotuSphClockServoTests, InjectionWaitsForItsFrameThresholdAndFiresOnlyOnce) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t intervalFrames = rate; // one second per update
    constexpr uint64_t kAfterFrames = rate * 3;

    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = intervalFrames;
    config.phaseInjectionTicks = 1024;
    config.phaseInjectionAfterFrames = kAfterFrames;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(config));
    const auto reference = servo.Update(Observation(1, 0, 0));
    ASSERT_TRUE(reference.phaseReferenceReset);
    EXPECT_FALSE(reference.phaseInjectionApplied); // never at the reference

    uint32_t fired = 0;
    uint64_t firedAtFrames = 0;
    for (uint64_t update = 1; update <= 8; ++update) {
        const uint64_t frames = update * intervalFrames;
        const auto decision =
            servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)));
        if (decision.phaseInjectionApplied) {
            ++fired;
            firedAtFrames = frames;
        }
    }
    EXPECT_EQ(fired, 1u);
    EXPECT_EQ(firedAtFrames, kAfterFrames);
}

// A new stream generation re-arms it, because that is a different run of the
// experiment -- but a re-reference inside one run does not, since the run's
// before/after comparison is already void once the loop re-references.
TEST(MotuSphClockServoTests, InjectionReArmsPerStreamGenerationNotPerReference) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate;

    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = frames;
    config.phaseInjectionTicks = 1024;
    config.phaseInjectionAfterFrames = frames;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(config));
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
    EXPECT_TRUE(servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)))
                    .phaseInjectionApplied);

    // Re-reference inside the same generation: rxFrames going backwards.
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
    EXPECT_FALSE(servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)))
                     .phaseInjectionApplied);

    servo.Reset();
    ASSERT_TRUE(servo.Update(Observation(2, 0, 0)).phaseReferenceReset);
    EXPECT_TRUE(servo.Update(Observation(2, frames, TicksForFrames(frames, rate, 0)))
                    .phaseInjectionApplied);
}

// Two ways to arm an injection that would measure something other than the
// plant, both refused at Configure rather than at the first hardware run.
TEST(MotuSphClockServoTests, ConfigureRefusesAnInjectionThatCannotIdentifyThePlant) {
    constexpr uint32_t rate = 48000;

    auto atReference = Config(rate);
    atReference.phaseInjectionTicks = 1024;
    atReference.phaseInjectionAfterFrames = 0; // would fire as a seed
    MotuSphClockServo seedLike{};
    EXPECT_FALSE(seedLike.Configure(atReference));

    for (const int64_t tooLarge : {MotuSphClockServo::kHardResyncThresholdTicks,
                                   -MotuSphClockServo::kHardResyncThresholdTicks}) {
        SCOPED_TRACE(tooLarge);
        auto opensValve = Config(rate);
        opensValve.phaseInjectionTicks = tooLarge;
        opensValve.phaseInjectionAfterFrames = rate;
        MotuSphClockServo servo{};
        EXPECT_FALSE(servo.Configure(opensValve));
    }

    // The shipping configuration -- no injection at all -- still configures.
    MotuSphClockServo production{};
    EXPECT_TRUE(production.Configure(Config(rate)));
}

TEST(MotuSphClockServoTests, SeedConvergesTheStandingOffsetOnlyWithTheCorrectSign) {
    constexpr int64_t setpoint = ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kOracleRelSetpointTicks;
    constexpr int64_t kInitialRel = -887;         // a measured hardware median
    constexpr int64_t kInitialDistance = 1397;    // 510 - (-887)

    for (const int64_t devicePpb : {int64_t{0}, int64_t{100}, int64_t{-100}}) {
        SCOPED_TRACE(devicePpb);

        const auto correct = RunClosedPlant(kInitialRel, devicePpb, false);
        // The whole point of an absolute error: the standing offset is GONE,
        // and what is left is the phase the original driver holds.
        EXPECT_LT(std::abs(correct.finalDistanceToSetpoint), kInitialDistance / 8);
        EXPECT_GT(correct.finalRel, setpoint - 200);
        EXPECT_LT(correct.finalRel, setpoint + 200);

        // With the inverted seed the loop is just as convinced it has settled
        // -- and has pushed the real phase to 2*rel0 - setpoint, twice as far
        // from the setpoint as it started. This is the assertion the six
        // earlier tests structurally could not make.
        const auto inverted = RunClosedPlant(kInitialRel, devicePpb, true);
        EXPECT_GT(std::abs(inverted.finalDistanceToSetpoint), kInitialDistance * 3 / 2);
        EXPECT_LT(inverted.finalRel, kInitialRel);
    }
}

// P6 asks whether the hard-resync valve is reachable at all. It is -- through
// accumulated divergence, and the seed shifts where that happens. What it does
// NOT do is make the seed alone able to trip the gate: the fold caps the seed
// at half a cycle against a four-cycle threshold. Both halves are pinned here
// so P6 inherits the real limit rather than an assumption.
TEST(MotuSphClockServoTests, AbsoluteSeedShiftsTheHardResyncTripWithoutReachingItAlone) {
    constexpr uint32_t rate = 48000;
    constexpr int64_t kCycle = 3072;

    // Half a cycle is the largest seed the fold can produce, and it is well
    // under the four-cycle gate.
    EXPECT_LT(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(kCycle / 2),
              MotuSphClockServo::kHardResyncThresholdTicks);

    // A step in device phase large enough to trip the gate does so, and the
    // seed biases the error by exactly its own value. The interval is a full
    // minute so that the same absolute tick step is only ~11 ppm of rate and
    // therefore stays inside the clamp -- otherwise this would measure the
    // clamp rather than the gate.
    constexpr uint64_t frames = rate * 60;
    constexpr int64_t kSeedTicks = 1500;
    const int64_t stepTicks = MotuSphClockServo::kHardResyncThresholdTicks + 4096;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    ASSERT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, kSeedTicks)).phaseReferenceReset);

    const auto tripped = servo.Update(AbsoluteObservation(
        1, frames, TicksForFrames(frames, rate, 0) + stepTicks, kSeedTicks));
    EXPECT_TRUE(tripped.hardResyncRequired);
    // The bias is the seed the servo derived, -(1500 - 510), not the raw
    // measurement: both sign and origin differ.
    EXPECT_EQ(tripped.phaseErrorTicks, stepTicks - 990);
    // Above the threshold the loop drops the proportional term and returns the
    // measured rate, handing the phase repair to its owner -- exactly as it
    // does without a seed.
    EXPECT_FALSE(tripped.stepClamped);
    EXPECT_EQ(tripped.stepQ32, tripped.measuredStepQ32);
}

// The clamp is the SetInterSampleTime equivalent; a seeded error must not push
// the requested step outside it, nor change what happens when it would.
TEST(MotuSphClockServoTests, ClampStillBoundsTheStepWithASeededError) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;
    constexpr int64_t kSeedTicks = -1536;

    auto config = Config(rate, 8, 1000); // deliberately tight bounds
    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(config));
    ASSERT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, kSeedTicks)).phaseReferenceReset);

    const auto decision = servo.Update(
        AbsoluteObservation(1, frames, TicksForFrames(frames, rate, 50000), kSeedTicks));
    ASSERT_TRUE(decision.feedbackUpdated);
    EXPECT_TRUE(decision.stepClamped);
    EXPECT_GE(decision.stepQ32, config.minimumStepQ32);
    EXPECT_LE(decision.stepQ32, config.maximumStepQ32);
}

// Regression guard for the wiring gap: until the runtime supplies `rel`, every
// observation arrives without it and the loop must behave exactly as before.
TEST(MotuSphClockServoTests, ObservationWithoutAbsoluteErrorKeepsTheRelativeLoop) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    const auto reference = servo.Update(Observation(1, 0, 0));
    EXPECT_TRUE(reference.phaseReferenceReset);
    EXPECT_EQ(reference.phaseErrorTicks, 0);

    const auto nominal = servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)));
    EXPECT_TRUE(nominal.feedbackUpdated);
    EXPECT_EQ(nominal.phaseErrorTicks, 0);
    EXPECT_EQ(nominal.stepQ32, MotuSphClockServo::NominalStepQ32(rate));
}

// --- The seed folds about the operating point ------------------------------

MotuSphServoObservation CenteredObservation(uint64_t generation, uint64_t rxFrames,
                                            int64_t rxTicks, int64_t absoluteErrorTicks,
                                            int64_t centerTicks) {
    MotuSphServoObservation observation =
        AbsoluteObservation(generation, rxFrames, rxTicks, absoluteErrorTicks);
    observation.haveAbsolutePhaseCenter = true;
    observation.absolutePhaseCenterTicks = centerTicks;
    return observation;
}

TEST(MotuSphClockServoTests, SeedFoldsAboutTheOperatingPointNotTheSetpoint) {
    // The operating point sits where the setpoint-centred fold cuts: 510+1536.
    // Two samples two ticks apart straddle it. Without a centre they seed the
    // loop a whole cycle apart; with one they seed two ticks apart.
    constexpr uint32_t rate = 48000;
    constexpr int64_t center = 510 + 1536;

    MotuSphClockServo uncentred{};
    ASSERT_TRUE(uncentred.Configure(Config(rate)));
    const int64_t below =
        uncentred.Update(AbsoluteObservation(1, 0, 0, center - 1)).phaseErrorTicks;
    MotuSphClockServo uncentredHigh{};
    ASSERT_TRUE(uncentredHigh.Configure(Config(rate)));
    const int64_t above =
        uncentredHigh.Update(AbsoluteObservation(1, 0, 0, center + 1)).phaseErrorTicks;
    EXPECT_EQ(std::abs(above - below), 3070);

    MotuSphClockServo centredLow{};
    ASSERT_TRUE(centredLow.Configure(Config(rate)));
    const int64_t seededLow =
        centredLow.Update(CenteredObservation(1, 0, 0, center - 1, center)).phaseErrorTicks;
    MotuSphClockServo centredHigh{};
    ASSERT_TRUE(centredHigh.Configure(Config(rate)));
    const int64_t seededHigh =
        centredHigh.Update(CenteredObservation(1, 0, 0, center + 1, center)).phaseErrorTicks;
    EXPECT_EQ(std::abs(seededHigh - seededLow), 2);
}

TEST(MotuSphClockServoTests, SeedWithACentreStillAimsAtTheMeasuredSetpoint) {
    // Correction 2 moves the branch cut, not the target: a stream sitting
    // exactly on the +510 setpoint is still declared to have no error.
    constexpr uint32_t rate = 48000;
    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    EXPECT_EQ(servo.Update(CenteredObservation(1, 0, 0, 510, 510)).phaseErrorTicks, 0);

    // And the closed-plant number is unchanged when the centre equals the sample.
    MotuSphClockServo measured{};
    ASSERT_TRUE(measured.Configure(Config(rate)));
    EXPECT_EQ(measured.Update(CenteredObservation(1, 0, 0, -887, -887)).phaseErrorTicks, 1397);
}

TEST(MotuSphClockServoTests, SeedCannotOpenTheHardResyncValve) {
    // The old bound was half a cycle; folding about a centre makes it one
    // cycle, and the valve stands at four. Swept over raw values a whole domain
    // wide and centres all round the cycle, no seed may reach the threshold.
    constexpr uint32_t rate = 48000;
    for (int64_t center = -6144; center <= 6144; center += 97) {
        for (int64_t offset = -1600; offset <= 1600; offset += 53) {
            MotuSphClockServo servo{};
            ASSERT_TRUE(servo.Configure(Config(rate)));
            const auto seeded =
                servo.Update(CenteredObservation(1, 0, 0, center + offset, center));
            EXPECT_FALSE(seeded.hardResyncRequired)
                << "center=" << center << " offset=" << offset;
            EXPECT_LT(std::abs(seeded.phaseErrorTicks),
                      MotuSphClockServo::kHardResyncThresholdTicks)
                << "center=" << center << " offset=" << offset;
            EXPECT_LE(std::abs(seeded.phaseErrorTicks), 3072)
                << "center=" << center << " offset=" << offset;
        }
    }
}

TEST(MotuSphClockServoTests, AbsolutePhaseArrivingLateReReferencesTheLoop) {
    // A failure seen on hardware, as a unit test. A real stream references
    // itself ~30 ms in, with no absolute phase available: the conditioner needs
    // three telemetry windows first. If that arrival does not re-reference, the
    // seed stays zero for the whole stream -- 32892 decisions of it on hardware.
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;
    constexpr int64_t rel = 900;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));

    const auto bare = servo.Update(Observation(1, 0, 0));
    EXPECT_TRUE(bare.phaseReferenceReset);
    EXPECT_EQ(bare.phaseErrorTicks, 0);

    const auto tracking = servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)));
    EXPECT_TRUE(tracking.feedbackUpdated);
    EXPECT_EQ(tracking.phaseErrorTicks, 0);

    auto acquired = CenteredObservation(1, frames * 2, TicksForFrames(frames * 2, rate, 0),
                                        rel, rel);
    acquired.absolutePhaseAcquired = true;
    const auto seeded = servo.Update(acquired);
    EXPECT_TRUE(seeded.phaseReferenceReset);
    EXPECT_FALSE(seeded.feedbackUpdated);
    EXPECT_EQ(seeded.phaseErrorTicks, 510 - rel);

    // Acquisition is an edge: the next observation tracks against the new seed.
    const auto after = servo.Update(
        CenteredObservation(1, frames * 3, TicksForFrames(frames * 3, rate, 0), rel + 2, rel));
    EXPECT_TRUE(after.feedbackUpdated);
    EXPECT_FALSE(after.phaseReferenceReset);
}

// --- The two-stage loop -----------------------------------------------------
//
// These pin the servo's thresholds, so unlike
// the tests above they use the production wiring rather than wide synthetic
// bounds: the 512-frame decision interval DiceTxStreamEngine gates on, and the
// one-rate-family clamp. A number here changing means a threshold changed.
constexpr uint64_t kProductionIntervalFrames = 512;
constexpr uint32_t kProductionRate = 48000;

// The clamp is expressed at rates the wire format does not carry, so it cannot
// go through NominalStepQ32 -- which answers only for the six captured rates.
// This is the same arithmetic DiceTxStreamEngine uses to build the bound.
constexpr int64_t StepQ32AtRate(uint32_t rateHz) {
    return static_cast<int64_t>((static_cast<__int128>(kTicksPerSecond)
                                 << MotuSphClockServo::kFractionBits) /
                                rateHz);
}

MotuSphServoConfig ProductionConfig(uint32_t acquisitionShift) {
    return {
        .sampleRateHz = kProductionRate,
        .phaseCorrectionHorizonFrames = kProductionIntervalFrames,
        .phaseGainShift = 2,
        .acquisitionPhaseGainShift = acquisitionShift,
        .minimumStepQ32 = StepQ32AtRate(50160),
        .maximumStepQ32 = StepQ32AtRate(41895),
    };
}

MotuSphServoObservation SeededObservation(uint64_t frames, int64_t extraTicks, int64_t relTicks,
                                          int64_t centerTicks, int64_t txCorrectionQ32) {
    MotuSphServoObservation observation = AbsoluteObservation(
        1, frames, TicksForFrames(frames, kProductionRate, 0) + extraTicks, relTicks,
        txCorrectionQ32);
    observation.haveAbsolutePhaseCenter = true;
    observation.absolutePhaseCenterTicks = centerTicks;
    return observation;
}

// How many corrections the loop needs before it reports a lock, against a device
// running exactly at nominal: the only error in play is the seed, and the only
// thing that removes it is the loop's own actuator.
int CorrectionsToLock(uint32_t acquisitionShift, int64_t relTicks, int64_t centerTicks,
                      int64_t& seededErrorTicks) {
    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(ProductionConfig(acquisitionShift)));
    const int64_t nominal = MotuSphClockServo::NominalStepQ32(kProductionRate);

    const auto reference = servo.Update(SeededObservation(0, 0, relTicks, centerTicks, 0));
    EXPECT_TRUE(reference.phaseReferenceReset);
    EXPECT_FALSE(reference.locked);
    seededErrorTicks = reference.phaseErrorTicks;

    int64_t appliedStep = reference.stepQ32;
    int64_t txCorrectionQ32 = 0;
    for (int update = 1; update <= 200; ++update) {
        // The packetizer emits the decided step for the whole interval.
        txCorrectionQ32 += static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) *
                                                kProductionIntervalFrames);
        const uint64_t frames = static_cast<uint64_t>(update) * kProductionIntervalFrames;
        const auto decision =
            servo.Update(SeededObservation(frames, 0, relTicks, centerTicks, txCorrectionQ32));
        EXPECT_TRUE(decision.feedbackUpdated);
        EXPECT_FALSE(decision.stepClamped);
        if (decision.locked) {
            return update - 1;
        }
        appliedStep = decision.stepQ32;
    }
    return -1;
}

TEST(MotuSphClockServoTests, AcquisitionGainClearsAFullCycleSeedInOneCorrection) {
    // The reason the acquisition stage exists at all. The seed
    // is bounded by one cycle (SeedCannotOpenTheHardResyncValve), so 3072 ticks
    // is the worst case the muted window has to cover. At 1/D the loop asks for
    // the whole error over the next interval and is under the lock threshold
    // after one correction -- about 11 ms. At the locked quarter gain the same
    // seed takes 14 corrections and 156 ms, every one of them silent.
    constexpr int64_t center = 510 + 1536;
    constexpr int64_t rel = 510 + 3072;

    int64_t seeded = 0;
    EXPECT_EQ(CorrectionsToLock(0, rel, center, seeded), 1);
    EXPECT_EQ(seeded, -3072);

    int64_t seededSlow = 0;
    EXPECT_EQ(CorrectionsToLock(2, rel, center, seededSlow), 14);
    EXPECT_EQ(seededSlow, -3072);
}

TEST(MotuSphClockServoTests, LockThresholdIsOneFiftiethOfABusCycle) {
    // 3072/50 = 61.44 ticks, the official driver's unmute hysteresis. The
    // margin it is kept for is measured, not inherited: at the worst residual of
    // the eight retained runs the settled error is 5.39 ticks and the
    // feed-forward dither floor about 4.
    EXPECT_EQ(MotuSphClockServo::kLockThresholdQ32,
              (3072 * MotuSphClockServo::kOneQ32) / 50);

    // A stream seeded 61 ticks off locks on its first feedback decision; one
    // seeded 62 ticks off does not, and has to correct first.
    auto FirstFeedbackLocks = [](int64_t seedTicks) {
        MotuSphClockServo servo{};
        EXPECT_TRUE(servo.Configure(ProductionConfig(0)));
        const int64_t rel = 510 - seedTicks;
        EXPECT_EQ(servo.Update(SeededObservation(0, 0, rel, rel, 0)).phaseErrorTicks, seedTicks);
        return servo.Update(SeededObservation(kProductionIntervalFrames, 0, rel, rel, 0)).locked;
    };
    EXPECT_TRUE(FirstFeedbackLocks(61));
    EXPECT_TRUE(FirstFeedbackLocks(-61));
    EXPECT_FALSE(FirstFeedbackLocks(62));
    EXPECT_FALSE(FirstFeedbackLocks(-62));
}

TEST(MotuSphClockServoTests, OnlyTheHardResyncValveDropsTheLock) {
    // There is no intermediate unlock threshold, deliberately: open
    // loop crosses the lock threshold in 0.51-6.8 s at the residuals actually
    // measured, so any threshold below the valve would turn ordinary drift into
    // mute flicker. 12000 ticks is 195 times the lock threshold and still holds
    // the lock; one tick past the valve does not.
    auto LockSurvives = [](int64_t extraTicks) {
        MotuSphClockServo servo{};
        EXPECT_TRUE(servo.Configure(ProductionConfig(0)));
        EXPECT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
        const auto locked = servo.Update(
            Observation(1, kProductionIntervalFrames,
                        TicksForFrames(kProductionIntervalFrames, kProductionRate, 0)));
        EXPECT_TRUE(locked.locked);

        const uint64_t frames = kProductionIntervalFrames * 2;
        const auto disturbed = servo.Update(Observation(
            1, frames, TicksForFrames(frames, kProductionRate, 0) + extraTicks));
        EXPECT_EQ(disturbed.phaseErrorTicks, extraTicks);
        return disturbed.locked;
    };
    EXPECT_TRUE(LockSurvives(12000));
    EXPECT_TRUE(LockSurvives(MotuSphClockServo::kHardResyncThresholdTicks));
    EXPECT_FALSE(LockSurvives(MotuSphClockServo::kHardResyncThresholdTicks + 1));
    EXPECT_FALSE(LockSurvives(-(MotuSphClockServo::kHardResyncThresholdTicks + 1)));
}

TEST(MotuSphClockServoTests, SeedReReferenceKeepsTheLockStanding) {
    // Absolute phase arrives about 12 s into EVERY
    // stream and re-references the loop with a seed of up to a full cycle. The
    // caller mutes on exactly this flag, so dropping the lock here would put a
    // predictable hole in the audio of every stream, at a predictable moment.
    constexpr int64_t rel = 900;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(ProductionConfig(0)));
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
    ASSERT_TRUE(servo.Update(Observation(1, kProductionIntervalFrames,
                                         TicksForFrames(kProductionIntervalFrames,
                                                        kProductionRate, 0)))
                    .locked);

    const uint64_t frames = kProductionIntervalFrames * 2;
    auto acquired = SeededObservation(frames, 0, rel, rel, 0);
    acquired.absolutePhaseAcquired = true;
    const auto seeded = servo.Update(acquired);
    EXPECT_TRUE(seeded.phaseReferenceReset);
    EXPECT_EQ(seeded.phaseErrorTicks, 510 - rel);
    EXPECT_TRUE(seeded.locked);

    // And the correction that follows is the locked one, a quarter of the
    // standing error: the seed is an offset to remove, not a discontinuity.
    const auto correcting =
        servo.Update(SeededObservation(kProductionIntervalFrames * 3, 0, rel, rel, 0));
    EXPECT_TRUE(correcting.locked);
    EXPECT_EQ(correcting.phaseErrorTicks, 510 - rel);
    EXPECT_EQ(correcting.stepQ32 - correcting.measuredStepQ32,
              ((510 - rel) * MotuSphClockServo::kOneQ32) /
                  static_cast<int64_t>(kProductionIntervalFrames * 4));
}

TEST(MotuSphClockServoTests, ConfigureRejectsAnAcquisitionShiftThatCouldOverflow) {
    MotuSphClockServo servo{};
    auto config = ProductionConfig(MotuSphClockServo::kMaximumPhaseGainShift + 1);
    EXPECT_FALSE(servo.Configure(config));
    config.acquisitionPhaseGainShift = MotuSphClockServo::kMaximumPhaseGainShift;
    EXPECT_TRUE(servo.Configure(config));
}

} // namespace

// ---------------------------------------------------------------------------
// Where an event can be swallowed before it reaches the hard-resync valve.
// These pin attribution and counting only -- the behaviour they observe is
// unchanged by the counting.
// ---------------------------------------------------------------------------

namespace {

using ASFW::Audio::MOTU::MotuSphReferenceCause;

// A servo already tracking a stream, so the next observation exercises a real
// re-reference rather than bootstrap.
MotuSphClockServo TrackingServo(uint32_t rate = 48000, uint64_t frames = 4096) {
    MotuSphClockServo servo{};
    servo.Configure(Config(rate));
    servo.Reset();
    servo.Update(Observation(1, 0, 0));
    servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)));
    return servo;
}

}  // namespace

TEST(MotuSphClockServoTests, FirstObservationAttributesToBootstrap) {
    MotuSphClockServo servo{};
    servo.Configure(Config(48000));
    servo.Reset();

    const MotuSphServoDecision first = servo.Update(Observation(1, 0, 0));
    EXPECT_TRUE(first.phaseReferenceReset);
    EXPECT_EQ(first.referenceCause, MotuSphReferenceCause::kBootstrap);
    // Bootstrap is not an anomaly and gets no counter of its own.
    EXPECT_EQ(first.rxFrameRegressionCount, 0u);
    EXPECT_EQ(first.bridgeStallCount, 0u);
}

// The old `||` chain stopped at its first true operand. Precedence has to
// survive the split, or an ordinary generation change starts reporting as a
// discontinuity the moment both hold at once.
TEST(MotuSphClockServoTests, GenerationChangeOutranksASimultaneousFrameRegression) {
    MotuSphClockServo servo = TrackingServo();

    // New generation AND a frame count below the previous one: both conditions
    // hold on the same observation.
    const MotuSphServoDecision next = servo.Update(Observation(2, 0, 0));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kGenerationChange);
    EXPECT_EQ(next.rxFrameRegressionCount, 0u);
}

TEST(MotuSphClockServoTests, FrameRegressionIsAttributedAndCounted) {
    MotuSphClockServo servo = TrackingServo();

    // Same generation, frames go backwards.
    const MotuSphServoDecision next =
        servo.Update(Observation(1, 1024, TicksForFrames(4096, 48000, 0) + 512));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kRxFrameRegression);
    EXPECT_EQ(next.rxFrameRegressionCount, 1u);
    EXPECT_EQ(next.rxTickRegressionCount, 0u);
}

TEST(MotuSphClockServoTests, TickRegressionIsAttributedAndCounted) {
    MotuSphClockServo servo = TrackingServo();

    // Frames advance, ticks go backwards: a tick regression, not a frame one.
    const MotuSphServoDecision next = servo.Update(Observation(1, 8192, 1000));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kRxTickRegression);
    EXPECT_EQ(next.rxTickRegressionCount, 1u);
    EXPECT_EQ(next.rxFrameRegressionCount, 0u);
}

// Distinct from a tick regression: the counter did not go backwards, it simply
// failed to move across one interval.
TEST(MotuSphClockServoTests, NonAdvancingTicksAreAttributedAndCounted) {
    MotuSphClockServo servo = TrackingServo();
    const int64_t held = TicksForFrames(4096, 48000, 0);

    const MotuSphServoDecision next = servo.Update(Observation(1, 8192, held));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kNonAdvancingTicks);
    EXPECT_EQ(next.nonAdvancingTickCount, 1u);
    EXPECT_EQ(next.rxTickRegressionCount, 0u);
}

// A stall is the one swallow point that is NOT a re-reference: the reference
// has to survive it, or the loop would be re-seeding itself every time the
// bridge was late.
TEST(MotuSphClockServoTests, BridgeStallIsCountedAndLeavesTheReferenceStanding) {
    MotuSphClockServo servo = TrackingServo();
    const uint64_t frames = 4096;
    const int64_t ticks = TicksForFrames(frames, 48000, 0);

    const MotuSphServoDecision stalled = servo.Update(Observation(1, frames, ticks));
    EXPECT_TRUE(stalled.bridgeStalled);
    EXPECT_EQ(stalled.bridgeStallCount, 1u);
    EXPECT_FALSE(stalled.phaseReferenceReset);
    EXPECT_EQ(stalled.referenceCause, MotuSphReferenceCause::kNone);

    // And the next real advance measures from the surviving reference.
    const MotuSphServoDecision resumed =
        servo.Update(Observation(1, frames * 2, TicksForFrames(frames * 2, 48000, 0)));
    EXPECT_TRUE(resumed.feedbackUpdated);
    EXPECT_FALSE(resumed.phaseReferenceReset);
    EXPECT_EQ(resumed.bridgeStallCount, 1u);
}

// The counts are cumulative since Configure on purpose. A cause that fires
// exactly at a generation boundary must not be erased by that boundary, which
// is precisely what per-generation counters would do.
TEST(MotuSphClockServoTests, CountsSurviveReset) {
    MotuSphClockServo servo = TrackingServo();
    const MotuSphServoDecision regressed =
        servo.Update(Observation(1, 1024, TicksForFrames(4096, 48000, 0) + 512));
    ASSERT_EQ(regressed.rxFrameRegressionCount, 1u);

    servo.Reset();
    const MotuSphServoDecision afterReset = servo.Update(Observation(2, 0, 0));
    EXPECT_EQ(afterReset.referenceCause, MotuSphReferenceCause::kBootstrap);
    EXPECT_EQ(afterReset.rxFrameRegressionCount, 1u);
}

// A healthy tracking update reports no cause at all. Without this the line
// would read as a permanent anomaly on every stream.
TEST(MotuSphClockServoTests, TrackingUpdateReportsNoCauseAndNoStall) {
    MotuSphClockServo servo = TrackingServo();

    const MotuSphServoDecision next =
        servo.Update(Observation(1, 8192, TicksForFrames(8192, 48000, 0)));
    ASSERT_TRUE(next.feedbackUpdated);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kNone);
    EXPECT_FALSE(next.bridgeStalled);
    EXPECT_FALSE(next.phaseReferenceReset);
    EXPECT_EQ(next.rxFrameRegressionCount, 0u);
    EXPECT_EQ(next.rxTickRegressionCount, 0u);
    EXPECT_EQ(next.nonAdvancingTickCount, 0u);
    EXPECT_EQ(next.bridgeStallCount, 0u);
}
