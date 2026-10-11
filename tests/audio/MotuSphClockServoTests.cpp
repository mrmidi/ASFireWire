// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The MOTU SPH regulator as pure arithmetic: feed-forward from the device's
// measured period, phase feedback normalised to the elapsed interval, the
// absolute-phase seed, the two-stage gain, and the hard-resync valve.

#include "Audio/Wire/MOTU/MotuSphClockServo.hpp"
#include "Audio/Wire/MOTU/MotuSphHardResyncGate.hpp"
#include "Audio/Wire/MOTU/MotuSphServoPolicy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using ASFW::Encoding::Motu::MotuSphClockServo;
using ASFW::Encoding::Motu::MotuSphHardResyncGate;
using ASFW::Encoding::Motu::MotuSphReferenceCause;
using ASFW::Encoding::Motu::MotuSphServoConfig;
using ASFW::Encoding::Motu::MotuSphServoDecision;
using ASFW::Encoding::Motu::MotuSphServoObservation;

constexpr int64_t kNano = 1000000000;
constexpr int64_t kTicksPerSecond = 24576000;
constexpr std::array<uint32_t, 6> kRates{44100, 48000, 88200, 96000, 176400, 192000};

// The absolute-phase setpoint these tests configure: the 48 kHz value the
// regulator holds against an 828mk3. Spelled out rather than taken from the
// model policy, so a change on either side of the arithmetic fails here.
constexpr int64_t kSetpoint = 510;

int64_t StepAtPpb(uint32_t rate, int64_t ppb) {
    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    return static_cast<int64_t>((static_cast<__int128>(nominal) * (kNano + ppb)) / kNano);
}

// Wide synthetic bounds and a long horizon: these tests are about the loop,
// not about the production clamp (see the two-stage tests further down).
MotuSphServoConfig Config(uint32_t rate, uint64_t horizonSeconds = 8, int64_t boundPpb = 100000) {
    return {
        .sampleRateHz = rate,
        .phaseCorrectionHorizonFrames = rate * horizonSeconds,
        .minimumStepQ32 = StepAtPpb(rate, -boundPpb),
        .maximumStepQ32 = StepAtPpb(rate, boundPpb),
        .absolutePhaseSetpointTicks = kSetpoint,
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

// One closed-loop run at a chosen update interval. `intervalFrames` is the D
// the caller actually delivers, which is what the gain is normalised to; the
// caller only guarantees it is at least the configured horizon.
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
        // The packetizer emits `appliedStep` for the whole interval.
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * intervalFrames);
        rxTicks += TicksForFrames(intervalFrames, config.sampleRateHz, devicePpb);

        // Quantisation is an error on each reading, not something the device
        // carries forward: folding it into `rxTicks` would model a random walk
        // in the timestamp, a much harsher signal than the one injected here.
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

    auto setpointOutOfRange = Config(48000);
    setpointOutOfRange.absolutePhaseSetpointTicks = kTicksPerSecond;
    EXPECT_FALSE(servo.Configure(setpointOutOfRange));
    EXPECT_FALSE(servo.IsConfigured());
    EXPECT_FALSE(servo.Update(Observation(1, 0, 0)).valid);
}

TEST(MotuSphClockServoTests, PhaseGainPerUpdateIsTheSameHoweverLateTheCallerIs) {
    constexpr uint32_t rate = 48000;
    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = 512;

    // D = H, 2H and 4H. Divided by a fixed H the gain per update would be D/H,
    // so 1, 2 and 4; normalised to D every one of them settles at 1/4.
    for (const uint64_t multiple : {uint64_t{1}, uint64_t{2}, uint64_t{4}}) {
        SCOPED_TRACE(multiple);
        const auto trace = RunLoop(config, 512 * multiple, 12, -2400);

        // Each update removes a quarter of the standing error, so three
        // quarters remain. Deadbeat would leave nothing and then overshoot.
        for (size_t i = 1; i + 1 < trace.phaseErrorTicks.size(); ++i) {
            if (std::llabs(trace.phaseErrorTicks[i - 1]) < 8) {
                continue;  // below this the integer division dominates the ratio
            }
            const double previous = static_cast<double>(trace.phaseErrorTicks[i - 1]);
            const double current = static_cast<double>(trace.phaseErrorTicks[i]);
            EXPECT_NEAR(current / previous, 0.75, 0.05);
        }
    }
}

TEST(MotuSphClockServoTests, LoopStillConvergesWhenTheCallerRunsFourHorizonsLate) {
    constexpr uint32_t rate = 48000;
    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = 512;

    // With the gain divided by a fixed H, an interval of 4H gave a loop gain
    // of 4: the error inverted and grew threefold every update until the
    // clamp caught it.
    const auto trace = RunLoop(config, 512 * 4, 16, -2400);

    const int64_t first = std::llabs(trace.phaseErrorTicks.front());
    const int64_t last = std::llabs(trace.phaseErrorTicks.back());
    EXPECT_LT(last, first);
    for (size_t i = 1; i < trace.phaseErrorTicks.size(); ++i) {
        EXPECT_LE(std::llabs(trace.phaseErrorTicks[i]), std::llabs(trace.phaseErrorTicks[i - 1]) + 1);
    }
}

TEST(MotuSphClockServoTests, PhaseGainCutsNoiseAmplificationFourfoldWithoutRemovingIt) {
    constexpr uint32_t rate = 48000;
    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = 512;

    // One tick of quantisation on each reading, alternating sign. The output
    // step is `measured + correction`, and both come from the same noisy
    // reading, so they are positively correlated: the output can never be
    // quieter than the raw measurement. What a quarter of the gain buys is a
    // quarter as much amplification, and that is what this pins.
    const std::vector<int64_t> noise{+1, -1, -1, +1, +1, -1};

    auto amplificationAtShift = [&](uint32_t shift) {
        auto shifted = config;
        shifted.phaseGainShift = shift;
        const auto trace = RunLoop(shifted, 512, 40, -2400, noise);
        // Skip the opening updates: they pull in the standing rate offset,
        // which is signal and would swamp the noise term.
        const std::vector<int64_t> step(trace.stepQ32.begin() + 8, trace.stepQ32.end());
        const std::vector<int64_t> measured(trace.measuredStepQ32.begin() + 8,
                                            trace.measuredStepQ32.end());
        return Variance(step) / Variance(measured);
    };

    const double unityGain = amplificationAtShift(0);
    const double quarterGain = amplificationAtShift(2);

    EXPECT_GT(unityGain, 4.0);
    EXPECT_LT(quarterGain, 2.0);
    // Variance scales with the square of the gain, so a 4x smaller gain must
    // show up as roughly 4x less amplification, not merely "less".
    EXPECT_GT(unityGain / quarterGain, 3.5);

    // The floor is 1, not 0: with no gain the step is the measurement. A
    // quieter output would need a filtered feed-forward term, which this loop
    // deliberately does not have.
    EXPECT_NEAR(amplificationAtShift(MotuSphClockServo::kMaximumPhaseGainShift), 1.0, 0.01);
    EXPECT_GT(quarterGain, 1.0);
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

TEST(MotuSphClockServoTests, ConfigureRejectsAnAcquisitionShiftThatCouldOverflow) {
    MotuSphClockServo servo{};
    auto config = Config(48000);
    config.acquisitionPhaseGainShift = MotuSphClockServo::kMaximumPhaseGainShift + 1;
    EXPECT_FALSE(servo.Configure(config));
    config.acquisitionPhaseGainShift = MotuSphClockServo::kMaximumPhaseGainShift;
    EXPECT_TRUE(servo.Configure(config));
}

TEST(MotuSphClockServoTests, NominalPeriodIsExactForAllSixRates) {
    for (const uint32_t rate : kRates) {
        SCOPED_TRACE(rate);
        MotuSphClockServo servo{};
        ASSERT_TRUE(servo.Configure(Config(rate)));

        const MotuSphServoDecision baseline = servo.Update(Observation(1, 0, 0));
        ASSERT_TRUE(baseline.valid);
        EXPECT_TRUE(baseline.phaseReferenceReset);
        EXPECT_FALSE(baseline.feedbackUpdated);

        // Four exact seconds make both rate families integral in the raw tick
        // domain while still exercising the Q32.32 conversion.
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

    // Joining a stream that has already run: its open-loop phase is far past
    // the valve (-4.8 cycles here, a value recorded on an 828mk3), and that
    // must not fire it.
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
    // -1612 ppb: a device rate offset measured on an 828mk3.
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

    // A rate change recorded on an 828mk3: about -1583 ppb in the first third
    // of a window and -1648 ppb in the last. There is no thermal-step API or
    // alternate branch -- Update() is the only loop.
    for (uint64_t window = 1; window <= 18; ++window) {
        const int64_t devicePpb = window <= 6 ? -1583 : -1648;
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * windowFrames);
        rxTicks += TicksForFrames(windowFrames, rate, devicePpb);
        decision = servo.Update(Observation(1, window * windowFrames, rxTicks, txCorrectionQ32));

        ASSERT_TRUE(decision.feedbackUpdated);
        EXPECT_FALSE(decision.hardResyncRequired);
        EXPECT_LT(std::llabs(decision.phaseErrorTicks), MotuSphClockServo::kHardResyncThresholdTicks);
        appliedStep = decision.stepQ32;
    }

    EXPECT_NEAR(static_cast<double>(decision.stepQ32), static_cast<double>(decision.measuredStepQ32),
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

    const auto decision = servo.Update(Observation(1, frames, TicksForFrames(frames, rate, devicePpb)));
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
    ASSERT_TRUE(servo.Update(Observation(1, frames, TicksForFrames(frames, rate, -1612))).feedbackUpdated);

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
// The absolute seed: with an absolute phase the reference is seeded with the
// distance to the setpoint instead of being zeroed.
// ---------------------------------------------------------------------------

MotuSphServoObservation AbsoluteObservation(uint64_t generation, uint64_t rxFrames, int64_t rxTicks,
                                            int64_t absolutePhaseTicks, int64_t txCorrectionQ32 = 0) {
    return {
        .valid = true,
        .generation = generation,
        .rxFrames = rxFrames,
        .rxTicks = rxTicks,
        .txCorrectionQ32 = txCorrectionQ32,
        .haveAbsolutePhase = true,
        .absolutePhaseTicks = absolutePhaseTicks,
    };
}

MotuSphServoObservation CenteredObservation(uint64_t generation, uint64_t rxFrames, int64_t rxTicks,
                                            int64_t absolutePhaseTicks, int64_t centerTicks) {
    MotuSphServoObservation observation =
        AbsoluteObservation(generation, rxFrames, rxTicks, absolutePhaseTicks);
    observation.haveAbsolutePhaseCenter = true;
    observation.absolutePhaseCenterTicks = centerTicks;
    return observation;
}

// Without a seed a stream that starts a thousand ticks off reports zero error
// forever. The reported error is the distance to the setpoint,
// `setpoint - phase`, not the measurement itself.
TEST(MotuSphClockServoTests, ReferenceSeedsTheMeasuredOffsetInsteadOfZeroingIt) {
    constexpr uint32_t rate = 48000;
    constexpr int64_t kMeasured = -887;        // a median measured on an 828mk3
    constexpr int64_t kErrorToSetpoint = 1397;  // 510 - (-887)

    MotuSphClockServo relative{};
    ASSERT_TRUE(relative.Configure(Config(rate)));
    const auto withoutSeed = relative.Update(Observation(1, 0, 0));
    ASSERT_TRUE(withoutSeed.phaseReferenceReset);
    EXPECT_EQ(withoutSeed.phaseErrorTicks, 0);

    MotuSphClockServo absolute{};
    ASSERT_TRUE(absolute.Configure(Config(rate)));
    const auto withSeed = absolute.Update(AbsoluteObservation(1, 0, 0, kMeasured));
    ASSERT_TRUE(withSeed.phaseReferenceReset);
    EXPECT_EQ(withSeed.phaseErrorTicks, kErrorToSetpoint);

    // A stream sitting exactly on the setpoint has nothing to correct.
    MotuSphClockServo onSetpoint{};
    ASSERT_TRUE(onSetpoint.Configure(Config(rate)));
    EXPECT_EQ(onSetpoint.Update(AbsoluteObservation(1, 0, 0, kSetpoint)).phaseErrorTicks, 0);

    // A perfectly tracking device still reports the offset it actually has.
    constexpr uint64_t frames = rate * 4;
    const auto tracked =
        absolute.Update(AbsoluteObservation(1, frames, TicksForFrames(frames, rate, 0), 0));
    EXPECT_TRUE(tracked.feedbackUpdated);
    EXPECT_EQ(tracked.phaseErrorTicks, kErrorToSetpoint);
}

// The setpoint is a parameter, not a constant of the loop.
TEST(MotuSphClockServoTests, SeedIsTakenAgainstTheConfiguredSetpoint) {
    constexpr uint32_t rate = 48000;
    for (const int64_t setpoint : {int64_t{0}, kSetpoint, int64_t{-700}}) {
        SCOPED_TRACE(setpoint);
        auto config = Config(rate);
        config.absolutePhaseSetpointTicks = setpoint;
        MotuSphClockServo servo{};
        ASSERT_TRUE(servo.Configure(config));
        EXPECT_EQ(servo.Update(AbsoluteObservation(1, 0, 0, -887)).phaseErrorTicks,
                  MotuSphClockServo::FoldAbsolutePhaseErrorTicks(setpoint - (-887)));
    }
}

// OHCI completion stamps have no sub-cycle field, so the phase picks up a
// +/-1 cycle pedestal. The readings below are the same physical phase; a
// branch flip must not read as a 3072-tick step in the error.
TEST(MotuSphClockServoTests, AbsoluteSeedFoldsTheOneCycleStampPedestal) {
    constexpr uint32_t rate = 48000;
    constexpr int64_t kCycle = 3072;
    constexpr int64_t kMeasured = -887;

    for (const int64_t branch : {int64_t{0}, kCycle, -kCycle, 2 * kCycle}) {
        SCOPED_TRACE(branch);
        MotuSphClockServo servo{};
        ASSERT_TRUE(servo.Configure(Config(rate)));
        const auto seeded = servo.Update(AbsoluteObservation(1, 0, 0, kMeasured + branch));
        EXPECT_EQ(seeded.phaseErrorTicks, 1397);  // 510 - (-887)
    }

    // A half-open half-cycle: exactly half a cycle stays positive, one tick
    // past it reads as the shorter negative distance.
    EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(kCycle / 2), kCycle / 2);
    EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(kCycle / 2 + 1), -(kCycle / 2 - 1));
    EXPECT_EQ(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(0), 0);
}

// The device's offset wanders (measured on an 828mk3: -2.12 -> -1.51 ppm over
// 59 min), so the loop is tested against a ramp, not only a constant.
TEST(MotuSphClockServoTests, AbsoluteSeedSurvivesAWanderingOffsetRamp) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t intervalFrames = 512;
    // 0.1 ppm over ten minutes.
    constexpr uint64_t updates = (uint64_t{rate} * 600) / intervalFrames;
    constexpr int64_t kEndPpb = 100;
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

        if (update > updates / 10) {
            worstSettledError = std::max<int64_t>(worstSettledError, std::abs(decision.phaseErrorTicks));
        }
    }

    // The loop corrects the standing offset (1397 ticks, 2.7 frames) and the
    // rate clamp paces it: at the 100 ppm bound of this test that takes about
    // half a second. Expected, but bounded -- a seed that kept the loop pinned
    // at the rate limit would be audible.
    EXPECT_GT(lastClampedUpdate, 0u);
    EXPECT_LT(lastClampedUpdate, updates / 10);

    // Settled, the loop tracks the wandering rate with its internal error near
    // zero. The plant here is open (`rxTicks` never responds to the step), so
    // this holds for either sign of the seed; the sign is pinned by the closed
    // plant of SeedConvergesTheStandingOffsetOnlyWithTheCorrectSign.
    EXPECT_LT(worstSettledError, 200);
}

// ---------------------------------------------------------------------------
// The closed plant. The tests above drive an open object: `rxTicks` ignores
// the applied step and the phase is a constant handed in, so the controller
// always zeroes its internal error and an inverted seed would pass them all.
// Here the measured phase is recomputed from what the actuator has done:
//
//     phase = phase0 + (txDeviation - rxDeviation)
//
// which closes actuator -> measurement and makes the sign observable.
// ---------------------------------------------------------------------------

struct PlantResult final {
    int64_t finalPhase{0};
    int64_t finalDistanceToSetpoint{0};
};

// `mirrorAroundSetpoint` feeds `2*setpoint - phase` instead of `phase`: the
// measurement that makes the loop derive the opposite seed.
PlantResult RunClosedPlant(int64_t initialPhase, int64_t devicePpb, bool mirrorAroundSetpoint) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t intervalFrames = 512;
    constexpr uint64_t updates = 2000;

    auto config = Config(rate);
    config.phaseCorrectionHorizonFrames = intervalFrames;

    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(config));

    const int64_t nominal = MotuSphClockServo::NominalStepQ32(rate);
    int64_t appliedStep = nominal;
    int64_t txCorrectionQ32 = 0;
    int64_t rxTicks = 0;
    int64_t nominalTicks = 0;
    int64_t phase = initialPhase;

    const auto reported = [&](int64_t value) {
        return mirrorAroundSetpoint ? 2 * kSetpoint - value : value;
    };

    EXPECT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, reported(phase))).phaseReferenceReset);

    for (uint64_t update = 1; update <= updates; ++update) {
        txCorrectionQ32 +=
            static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) * intervalFrames);
        rxTicks += TicksForFrames(intervalFrames, rate, devicePpb);
        nominalTicks += TicksForFrames(intervalFrames, rate, 0);

        // The standing phase moves with what our packetizer added on top of
        // nominal, minus what the device's own clock drifted away from it.
        const int64_t txDeviationTicks = static_cast<int64_t>(
            static_cast<__int128>(txCorrectionQ32) / static_cast<__int128>(MotuSphClockServo::kOneQ32));
        phase = initialPhase + txDeviationTicks - (rxTicks - nominalTicks);

        const auto decision = servo.Update(
            AbsoluteObservation(1, update * intervalFrames, rxTicks, reported(phase), txCorrectionQ32));
        EXPECT_TRUE(decision.feedbackUpdated);
        EXPECT_FALSE(decision.hardResyncRequired);
        appliedStep = decision.stepQ32;
    }

    return {phase, phase - kSetpoint};
}

TEST(MotuSphClockServoTests, SeedConvergesTheStandingOffsetOnlyWithTheCorrectSign) {
    constexpr int64_t kInitialPhase = -887;  // a median measured on an 828mk3
    constexpr int64_t kInitialDistance = 1397;

    for (const int64_t devicePpb : {int64_t{0}, int64_t{100}, int64_t{-100}}) {
        SCOPED_TRACE(devicePpb);

        const auto correct = RunClosedPlant(kInitialPhase, devicePpb, false);
        // The standing offset is gone: the stream sits on the setpoint.
        EXPECT_LT(std::abs(correct.finalDistanceToSetpoint), kInitialDistance / 8);
        EXPECT_GT(correct.finalPhase, kSetpoint - 200);
        EXPECT_LT(correct.finalPhase, kSetpoint + 200);

        // With the inverted seed the loop is just as sure it has settled --
        // and has pushed the real phase to 2*phase0 - setpoint, twice as far
        // from the setpoint as it started.
        const auto inverted = RunClosedPlant(kInitialPhase, devicePpb, true);
        EXPECT_GT(std::abs(inverted.finalDistanceToSetpoint), kInitialDistance * 3 / 2);
        EXPECT_LT(inverted.finalPhase, kInitialPhase);
    }
}

// The valve stays reachable through accumulated divergence, and the seed only
// shifts where that happens: the seed alone cannot trip it.
TEST(MotuSphClockServoTests, AbsoluteSeedShiftsTheHardResyncTripWithoutReachingItAlone) {
    constexpr uint32_t rate = 48000;
    constexpr int64_t kCycle = 3072;

    EXPECT_LT(MotuSphClockServo::FoldAbsolutePhaseErrorTicks(kCycle / 2),
              MotuSphClockServo::kHardResyncThresholdTicks);

    // A device phase step large enough to trip the valve does so, and the
    // seed biases the error by exactly its own value. A full minute keeps the
    // same tick step at ~11 ppm of rate, inside the clamp, so this measures
    // the valve and not the clamp.
    constexpr uint64_t frames = rate * 60;
    constexpr int64_t kSeedTicks = 1500;
    const int64_t stepTicks = MotuSphClockServo::kHardResyncThresholdTicks + 4096;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    ASSERT_TRUE(servo.Update(AbsoluteObservation(1, 0, 0, kSeedTicks)).phaseReferenceReset);

    const auto tripped = servo.Update(
        AbsoluteObservation(1, frames, TicksForFrames(frames, rate, 0) + stepTicks, kSeedTicks));
    EXPECT_TRUE(tripped.hardResyncRequired);
    // The bias is the derived seed, -(1500 - 510), not the raw measurement.
    EXPECT_EQ(tripped.phaseErrorTicks, stepTicks - 990);
    // Above the valve the step is the measured rate; the repair is the
    // caller's.
    EXPECT_FALSE(tripped.stepClamped);
    EXPECT_EQ(tripped.stepQ32, tripped.measuredStepQ32);
}

TEST(MotuSphClockServoTests, ClampStillBoundsTheStepWithASeededError) {
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;
    constexpr int64_t kSeedTicks = -1536;

    auto config = Config(rate, 8, 1000);  // deliberately tight bounds
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

// Until the caller supplies an absolute phase, the loop is the relative one.
TEST(MotuSphClockServoTests, ObservationWithoutAbsolutePhaseKeepsTheRelativeLoop) {
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

// --- The seed folds about the operating point --------------------------------

TEST(MotuSphClockServoTests, SeedFoldsAboutTheOperatingPointNotTheSetpoint) {
    // The operating point sits where a setpoint-centred fold cuts: 510+1536.
    // Two samples two ticks apart straddle it. Without a centre they seed the
    // loop a whole cycle apart; with one they seed two ticks apart.
    constexpr uint32_t rate = 48000;
    constexpr int64_t center = kSetpoint + 1536;

    MotuSphClockServo uncentredLow{};
    ASSERT_TRUE(uncentredLow.Configure(Config(rate)));
    const int64_t below = uncentredLow.Update(AbsoluteObservation(1, 0, 0, center - 1)).phaseErrorTicks;
    MotuSphClockServo uncentredHigh{};
    ASSERT_TRUE(uncentredHigh.Configure(Config(rate)));
    const int64_t above = uncentredHigh.Update(AbsoluteObservation(1, 0, 0, center + 1)).phaseErrorTicks;
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

TEST(MotuSphClockServoTests, SeedWithACentreStillAimsAtTheSetpoint) {
    // The centre moves the branch cut, not the target.
    constexpr uint32_t rate = 48000;
    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));
    EXPECT_EQ(servo.Update(CenteredObservation(1, 0, 0, kSetpoint, kSetpoint)).phaseErrorTicks, 0);

    MotuSphClockServo measured{};
    ASSERT_TRUE(measured.Configure(Config(rate)));
    EXPECT_EQ(measured.Update(CenteredObservation(1, 0, 0, -887, -887)).phaseErrorTicks, 1397);
}

TEST(MotuSphClockServoTests, SeedCannotOpenTheHardResyncValve) {
    // Folding about a centre bounds the seed by one cycle; the valve stands at
    // four. Swept over raw values and centres all round the cycle, no seed
    // reaches it.
    constexpr uint32_t rate = 48000;
    for (int64_t center = -6144; center <= 6144; center += 97) {
        for (int64_t offset = -1600; offset <= 1600; offset += 53) {
            MotuSphClockServo servo{};
            ASSERT_TRUE(servo.Configure(Config(rate)));
            const auto seeded = servo.Update(CenteredObservation(1, 0, 0, center + offset, center));
            EXPECT_FALSE(seeded.hardResyncRequired) << "center=" << center << " offset=" << offset;
            EXPECT_LE(std::abs(seeded.phaseErrorTicks), 3072)
                << "center=" << center << " offset=" << offset;
        }
    }
}

TEST(MotuSphClockServoTests, AbsolutePhaseArrivingLateReReferencesTheLoop) {
    // A stream references itself on its first observation, with no absolute
    // phase yet: the conditioner needs three telemetry windows first. If that
    // arrival did not re-reference, the seed would stay zero for the whole
    // stream -- which an 828mk3 run recorded over 32892 decisions.
    constexpr uint32_t rate = 48000;
    constexpr uint64_t frames = rate * 4;
    constexpr int64_t phase = 900;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(rate)));

    const auto bare = servo.Update(Observation(1, 0, 0));
    EXPECT_TRUE(bare.phaseReferenceReset);
    EXPECT_EQ(bare.phaseErrorTicks, 0);

    const auto tracking = servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)));
    EXPECT_TRUE(tracking.feedbackUpdated);
    EXPECT_EQ(tracking.phaseErrorTicks, 0);

    auto acquired = CenteredObservation(1, frames * 2, TicksForFrames(frames * 2, rate, 0), phase, phase);
    acquired.absolutePhaseAcquired = true;
    const auto seeded = servo.Update(acquired);
    EXPECT_TRUE(seeded.phaseReferenceReset);
    EXPECT_FALSE(seeded.feedbackUpdated);
    EXPECT_EQ(seeded.referenceCause, MotuSphReferenceCause::kAbsolutePhaseSeed);
    EXPECT_EQ(seeded.seedReReferenceCount, 1u);
    EXPECT_EQ(seeded.phaseErrorTicks, kSetpoint - phase);

    // Acquisition is an edge: the next observation tracks against the seed.
    const auto after = servo.Update(
        CenteredObservation(1, frames * 3, TicksForFrames(frames * 3, rate, 0), phase + 2, phase));
    EXPECT_TRUE(after.feedbackUpdated);
    EXPECT_FALSE(after.phaseReferenceReset);
}

// --- The two-stage loop ------------------------------------------------------
//
// These pin the servo's thresholds, so they run on the production 48 kHz
// configuration (decision interval and step clamp) rather than on wide
// synthetic bounds. A number here changing means a threshold changed.
constexpr uint64_t kProductionIntervalFrames = 512;
constexpr uint32_t kProductionRate = 48000;

MotuSphServoConfig ProductionConfig(uint32_t acquisitionShift) {
    auto config = *ASFW::Encoding::Motu::MotuSphServoConfigForRate(kProductionRate);
    EXPECT_EQ(config.phaseCorrectionHorizonFrames, kProductionIntervalFrames);
    config.acquisitionPhaseGainShift = acquisitionShift;
    config.absolutePhaseSetpointTicks = kSetpoint;
    return config;
}

MotuSphServoObservation SeededObservation(uint64_t frames, int64_t extraTicks, int64_t phaseTicks,
                                          int64_t centerTicks, int64_t txCorrectionQ32) {
    MotuSphServoObservation observation = AbsoluteObservation(
        1, frames, TicksForFrames(frames, kProductionRate, 0) + extraTicks, phaseTicks, txCorrectionQ32);
    observation.haveAbsolutePhaseCenter = true;
    observation.absolutePhaseCenterTicks = centerTicks;
    return observation;
}

// Corrections the loop needs before it reports a lock, against a device
// running exactly at nominal: the only error in play is the seed.
int CorrectionsToLock(uint32_t acquisitionShift, int64_t phaseTicks, int64_t centerTicks,
                      int64_t& seededErrorTicks) {
    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(ProductionConfig(acquisitionShift)));
    const int64_t nominal = MotuSphClockServo::NominalStepQ32(kProductionRate);

    const auto reference = servo.Update(SeededObservation(0, 0, phaseTicks, centerTicks, 0));
    EXPECT_TRUE(reference.phaseReferenceReset);
    EXPECT_FALSE(reference.locked);
    seededErrorTicks = reference.phaseErrorTicks;

    int64_t appliedStep = reference.stepQ32;
    int64_t txCorrectionQ32 = 0;
    for (int update = 1; update <= 200; ++update) {
        txCorrectionQ32 += static_cast<int64_t>(static_cast<__int128>(appliedStep - nominal) *
                                                kProductionIntervalFrames);
        const uint64_t frames = static_cast<uint64_t>(update) * kProductionIntervalFrames;
        const auto decision =
            servo.Update(SeededObservation(frames, 0, phaseTicks, centerTicks, txCorrectionQ32));
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
    // The seed is bounded by one cycle, so 3072 ticks is the worst case an
    // unlocked loop has to clear. At 1/D the loop is under the lock threshold
    // after one correction (about 11 ms); at the locked quarter gain the same
    // seed takes 14 corrections (156 ms), every one of them muted.
    constexpr int64_t center = kSetpoint + 1536;
    constexpr int64_t phase = kSetpoint + 3072;

    int64_t seeded = 0;
    EXPECT_EQ(CorrectionsToLock(0, phase, center, seeded), 1);
    EXPECT_EQ(seeded, -3072);

    int64_t seededSlow = 0;
    EXPECT_EQ(CorrectionsToLock(2, phase, center, seededSlow), 14);
    EXPECT_EQ(seededSlow, -3072);
}

TEST(MotuSphClockServoTests, LockThresholdIsOneFiftiethOfABusCycle) {
    EXPECT_EQ(MotuSphClockServo::kLockThresholdQ32, (3072 * MotuSphClockServo::kOneQ32) / 50);

    // Seeded 61 ticks off locks on the first feedback decision; 62 ticks off
    // does not, and has to correct first.
    auto FirstFeedbackLocks = [](int64_t seedTicks) {
        MotuSphClockServo servo{};
        EXPECT_TRUE(servo.Configure(ProductionConfig(0)));
        const int64_t phase = kSetpoint - seedTicks;
        EXPECT_EQ(servo.Update(SeededObservation(0, 0, phase, phase, 0)).phaseErrorTicks, seedTicks);
        return servo.Update(SeededObservation(kProductionIntervalFrames, 0, phase, phase, 0)).locked;
    };
    EXPECT_TRUE(FirstFeedbackLocks(61));
    EXPECT_TRUE(FirstFeedbackLocks(-61));
    EXPECT_FALSE(FirstFeedbackLocks(62));
    EXPECT_FALSE(FirstFeedbackLocks(-62));
}

TEST(MotuSphClockServoTests, OnlyTheHardResyncValveDropsTheLock) {
    // 12000 ticks is 195x the lock threshold and still holds the lock; one
    // tick past the valve does not.
    auto LockSurvives = [](int64_t extraTicks) {
        MotuSphClockServo servo{};
        EXPECT_TRUE(servo.Configure(ProductionConfig(0)));
        EXPECT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
        const auto locked = servo.Update(Observation(
            1, kProductionIntervalFrames, TicksForFrames(kProductionIntervalFrames, kProductionRate, 0)));
        EXPECT_TRUE(locked.locked);

        const uint64_t frames = kProductionIntervalFrames * 2;
        const auto disturbed =
            servo.Update(Observation(1, frames, TicksForFrames(frames, kProductionRate, 0) + extraTicks));
        EXPECT_EQ(disturbed.phaseErrorTicks, extraTicks);
        return disturbed.locked;
    };
    EXPECT_TRUE(LockSurvives(12000));
    EXPECT_TRUE(LockSurvives(MotuSphClockServo::kHardResyncThresholdTicks));
    EXPECT_FALSE(LockSurvives(MotuSphClockServo::kHardResyncThresholdTicks + 1));
    EXPECT_FALSE(LockSurvives(-(MotuSphClockServo::kHardResyncThresholdTicks + 1)));
}

TEST(MotuSphClockServoTests, SeedReReferenceKeepsTheLockStanding) {
    // Absolute phase arrives about 12 s into every stream and re-references
    // the loop with a seed of up to a cycle. The caller mutes on `locked`, so
    // dropping it here would put a hole in every stream at a predictable time.
    constexpr int64_t phase = 900;

    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(ProductionConfig(0)));
    ASSERT_TRUE(servo.Update(Observation(1, 0, 0)).phaseReferenceReset);
    ASSERT_TRUE(servo.Update(Observation(1, kProductionIntervalFrames,
                                         TicksForFrames(kProductionIntervalFrames, kProductionRate, 0)))
                    .locked);

    const uint64_t frames = kProductionIntervalFrames * 2;
    auto acquired = SeededObservation(frames, 0, phase, phase, 0);
    acquired.absolutePhaseAcquired = true;
    const auto seeded = servo.Update(acquired);
    EXPECT_TRUE(seeded.phaseReferenceReset);
    EXPECT_EQ(seeded.phaseErrorTicks, kSetpoint - phase);
    EXPECT_TRUE(seeded.locked);

    // The correction that follows is the locked one, a quarter of the error.
    const auto correcting = servo.Update(SeededObservation(kProductionIntervalFrames * 3, 0, phase, phase, 0));
    EXPECT_TRUE(correcting.locked);
    EXPECT_EQ(correcting.phaseErrorTicks, kSetpoint - phase);
    EXPECT_EQ(correcting.stepQ32 - correcting.measuredStepQ32,
              ((kSetpoint - phase) * MotuSphClockServo::kOneQ32) /
                  static_cast<int64_t>(kProductionIntervalFrames * 4));
}

// ---------------------------------------------------------------------------
// Where an event can be swallowed before it reaches the valve: attribution and
// counting only, the behaviour they observe is unchanged by the counting.
// ---------------------------------------------------------------------------

// A servo already tracking a stream, so the next observation exercises a real
// re-reference rather than the bootstrap.
MotuSphClockServo TrackingServo(uint32_t rate = 48000, uint64_t frames = 4096) {
    MotuSphClockServo servo{};
    EXPECT_TRUE(servo.Configure(Config(rate)));
    (void)servo.Update(Observation(1, 0, 0));
    (void)servo.Update(Observation(1, frames, TicksForFrames(frames, rate, 0)));
    return servo;
}

TEST(MotuSphClockServoTests, FirstObservationAttributesToBootstrap) {
    MotuSphClockServo servo{};
    ASSERT_TRUE(servo.Configure(Config(48000)));

    const MotuSphServoDecision first = servo.Update(Observation(1, 0, 0));
    EXPECT_TRUE(first.phaseReferenceReset);
    EXPECT_EQ(first.referenceCause, MotuSphReferenceCause::kBootstrap);
    EXPECT_EQ(first.rxFrameRegressionCount, 0u);
    EXPECT_EQ(first.bridgeStallCount, 0u);
}

// Precedence: an ordinary generation change must not report as a
// discontinuity when both hold at once.
TEST(MotuSphClockServoTests, GenerationChangeOutranksASimultaneousFrameRegression) {
    MotuSphClockServo servo = TrackingServo();
    const MotuSphServoDecision next = servo.Update(Observation(2, 0, 0));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kGenerationChange);
    EXPECT_EQ(next.rxFrameRegressionCount, 0u);
}

TEST(MotuSphClockServoTests, FrameRegressionIsAttributedAndCounted) {
    MotuSphClockServo servo = TrackingServo();
    const MotuSphServoDecision next =
        servo.Update(Observation(1, 1024, TicksForFrames(4096, 48000, 0) + 512));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kRxFrameRegression);
    EXPECT_EQ(next.rxFrameRegressionCount, 1u);
    EXPECT_EQ(next.rxTickRegressionCount, 0u);
}

TEST(MotuSphClockServoTests, TickRegressionIsAttributedAndCounted) {
    MotuSphClockServo servo = TrackingServo();
    const MotuSphServoDecision next = servo.Update(Observation(1, 8192, 1000));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kRxTickRegression);
    EXPECT_EQ(next.rxTickRegressionCount, 1u);
    EXPECT_EQ(next.rxFrameRegressionCount, 0u);
}

// Distinct from a tick regression: the counter failed to move across one
// interval rather than going backwards.
TEST(MotuSphClockServoTests, NonAdvancingTicksAreAttributedAndCounted) {
    MotuSphClockServo servo = TrackingServo();
    const int64_t held = TicksForFrames(4096, 48000, 0);
    const MotuSphServoDecision next = servo.Update(Observation(1, 8192, held));
    ASSERT_TRUE(next.phaseReferenceReset);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kNonAdvancingTicks);
    EXPECT_EQ(next.nonAdvancingTickCount, 1u);
    EXPECT_EQ(next.rxTickRegressionCount, 0u);
}

// A stall is the one swallow point that is not a re-reference: the reference
// survives it, or the loop would re-seed itself every time the bridge is late.
TEST(MotuSphClockServoTests, BridgeStallIsCountedAndLeavesTheReferenceStanding) {
    MotuSphClockServo servo = TrackingServo();
    const uint64_t frames = 4096;
    const int64_t ticks = TicksForFrames(frames, 48000, 0);

    const MotuSphServoDecision stalled = servo.Update(Observation(1, frames, ticks));
    EXPECT_TRUE(stalled.bridgeStalled);
    EXPECT_EQ(stalled.bridgeStallCount, 1u);
    EXPECT_FALSE(stalled.phaseReferenceReset);
    EXPECT_EQ(stalled.referenceCause, MotuSphReferenceCause::kNone);

    const MotuSphServoDecision resumed =
        servo.Update(Observation(1, frames * 2, TicksForFrames(frames * 2, 48000, 0)));
    EXPECT_TRUE(resumed.feedbackUpdated);
    EXPECT_FALSE(resumed.phaseReferenceReset);
    EXPECT_EQ(resumed.bridgeStallCount, 1u);
}

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

TEST(MotuSphClockServoTests, TrackingUpdateReportsNoCauseAndNoStall) {
    MotuSphClockServo servo = TrackingServo();
    const MotuSphServoDecision next = servo.Update(Observation(1, 8192, TicksForFrames(8192, 48000, 0)));
    ASSERT_TRUE(next.feedbackUpdated);
    EXPECT_EQ(next.referenceCause, MotuSphReferenceCause::kNone);
    EXPECT_FALSE(next.bridgeStalled);
    EXPECT_FALSE(next.phaseReferenceReset);
    EXPECT_EQ(next.rxFrameRegressionCount, 0u);
    EXPECT_EQ(next.rxTickRegressionCount, 0u);
    EXPECT_EQ(next.nonAdvancingTickCount, 0u);
    EXPECT_EQ(next.bridgeStallCount, 0u);
}

// ---------------------------------------------------------------------------
// The gate owns no threshold: it turns the servo's lock state into muting and
// the hard-resync edge into the one-shot repair.
// ---------------------------------------------------------------------------

TEST(MotuSphHardResyncGateTests, MutesWhileUnlockedAndRepairsOncePerCrossing) {
    MotuSphHardResyncGate gate{};
    gate.Reset(true);
    EXPECT_TRUE(gate.IsMuted());

    auto action = gate.Update(false, false);
    EXPECT_TRUE(action.muted);
    EXPECT_FALSE(action.unmute);
    EXPECT_FALSE(action.phaseRepairRequired);

    action = gate.Update(false, true);
    EXPECT_TRUE(action.unmute);
    EXPECT_FALSE(action.muted);

    // A crossing: muting is reported together with the repair, ahead of it.
    action = gate.Update(true, false);
    EXPECT_TRUE(action.muteBeforeRepair);
    EXPECT_TRUE(action.phaseRepairRequired);
    EXPECT_TRUE(action.muted);

    // Still beyond the valve is the same crossing, not a second one.
    action = gate.Update(true, false);
    EXPECT_FALSE(action.muteBeforeRepair);
    EXPECT_FALSE(action.phaseRepairRequired);
    EXPECT_TRUE(action.muted);

    // Back under the valve but not yet locked: the repair re-arms, the mute
    // holds.
    action = gate.Update(false, false);
    EXPECT_FALSE(action.unmute);
    EXPECT_TRUE(action.muted);

    action = gate.Update(false, true);
    EXPECT_TRUE(action.unmute);
    EXPECT_FALSE(action.muted);

    action = gate.Update(true, false);
    EXPECT_TRUE(action.muteBeforeRepair);
    EXPECT_TRUE(action.phaseRepairRequired);
}

TEST(MotuSphHardResyncGateTests, TheValveIsReachableDuringAcquisition) {
    // The loop starts muted, so a repair armed on the mute edge would never
    // fire before the first lock. It is armed on the hard-resync edge.
    MotuSphHardResyncGate gate{};
    gate.Reset(true);
    const auto action = gate.Update(true, false);
    EXPECT_TRUE(action.phaseRepairRequired);
    EXPECT_FALSE(action.muteBeforeRepair);  // already muted
    EXPECT_TRUE(action.muted);
}

TEST(MotuSphHardResyncGateTests, AGateNobodyArmedStartsOpen) {
    MotuSphHardResyncGate unarmed{};
    EXPECT_FALSE(unarmed.IsMuted());
    unarmed.Reset();
    EXPECT_FALSE(unarmed.IsMuted());
}

} // namespace
