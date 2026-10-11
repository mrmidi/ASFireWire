// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuSphClockServo.hpp - the MOTU transmit SPH regulator, as pure arithmetic.
//
// One continuous loop per stream. The feed-forward term is the device's own
// sample period, measured from received SPH; the feedback term is the phase
// between what the device's clock produced and what our transmit clock
// (MotuSphQ32Accumulator) has already followed. Above a four-cycle error the
// loop asks its caller for a one-shot phase repair instead (the hard-resync
// valve). There is no second controller and no thermal-step detector: slow
// drift and faster change go through the same expression.
//
// No wire, no runtime, no DriverKit: the host tests drive all of it.

#pragma once

#include "MotuSph.hpp"
#include "MotuSphQ32Accumulator.hpp"

#include <cstdint>
#include <limits>

namespace ASFW::Encoding::Motu {

// Per-rate production values, with where each came from: MotuSphServoPolicy.hpp.
struct MotuSphServoConfig final {
    uint32_t sampleRateHz{0};

    // The caller's minimum interval between decisions, in frames. The loop
    // does not divide by it -- it normalises to the interval that actually
    // elapsed -- but the caller gates its updates on it, so it is the one
    // source of truth for the decision cadence and must not be zero.
    uint64_t phaseCorrectionHorizonFrames{0};

    // Locked phase gain as a right shift: the correction per frame is
    // `error / (D << shift)` over an elapsed interval of D frames, so the gain
    // per decision is 1 / (1 << shift) whatever D is. 2 gives 1/4.
    uint32_t phaseGainShift{2};

    // The same gain before the loop has first locked. 0 gives 1/D: the whole
    // measured error is asked for over the next interval.
    uint32_t acquisitionPhaseGainShift{0};

    // Step clamp, Q32.32 ticks per frame. The smallest step is the highest
    // rate the transmit clock may run at, the largest the lowest.
    int64_t minimumStepQ32{0};
    int64_t maximumStepQ32{0};

    // The absolute presentation phase the loop holds when the caller supplies
    // one (MotuSphServoObservation::absolutePhaseTicks), in ticks. A per-model,
    // per-rate value; unused by the relative loop.
    int64_t absolutePhaseSetpointTicks{0};
};

struct MotuSphServoObservation final {
    bool valid{false};
    uint64_t generation{0};

    // Cumulative accepted RX measurement since the stream started: `rxTicks`
    // device ticks over `rxFrames` frames. Raw, so the 44.1 kHz family keeps
    // its exact rational period until the single Q32.32 division below.
    uint64_t rxFrames{0};
    int64_t rxTicks{0};

    // The transmit clock's cumulative movement relative to the exact nominal
    // step (MotuSphQ32Accumulator::AppliedCorrectionQ32), Q32.32 ticks.
    int64_t txCorrectionQ32{0};

    // Optional absolute presentation phase of our transmit stream against the
    // device, in ticks -- a measurement, not an error; the loop forms the
    // error against MotuSphServoConfig::absolutePhaseSetpointTicks. Without it
    // the loop is relative: it holds whatever phase existed at the reference.
    bool haveAbsolutePhase{false};
    int64_t absolutePhaseTicks{0};

    // The running operating point the phase above was folded about
    // (MotuRelPhaseConditioner). Without it the seed folds about the setpoint.
    bool haveAbsolutePhaseCenter{false};
    int64_t absolutePhaseCenterTicks{0};

    // Absolute phase has just become available while the standing reference
    // was built without it: re-reference on this observation. This is the
    // normal case, not an edge case -- the reference is taken on the first
    // observation of a stream, long before the phase measurement has an
    // operating point.
    bool absolutePhaseAcquired{false};
};

// Why Update() re-referenced, or why it returned without measuring. Evaluated
// in this order; the first condition that holds is the one reported.
enum class MotuSphReferenceCause : uint8_t {
    kNone = 0,
    // Ordinary.
    kBootstrap,          // no reference yet: the first observation of a stream
    kGenerationChange,   // a new stream generation owns a new reference
    kAbsolutePhaseSeed,  // absolute phase arrived; re-read to seed the offset
    // Discontinuities.
    kRxFrameRegression,  // the device's frame count went backwards
    kRxTickRegression,   // the device's tick count went backwards
    kNonAdvancingTicks,  // frames advanced but ticks did not
};

struct MotuSphServoDecision final {
    bool valid{false};
    bool feedbackUpdated{false};
    bool phaseReferenceReset{false};
    bool stepClamped{false};
    bool hardResyncRequired{false};

    // Which stage produced this decision, and the single source of truth for
    // muting: the output is silent exactly while this is false
    // (MotuSphHardResyncGate).
    bool locked{false};

    int64_t stepQ32{0};
    int64_t measuredStepQ32{0};
    int64_t phaseErrorQ32{0};
    int64_t phaseErrorTicks{0};

    // Why this update re-referenced; kNone when it did not.
    MotuSphReferenceCause referenceCause{MotuSphReferenceCause::kNone};

    // The update returned without measuring because the frame count did not
    // advance. Not a re-reference: the reference survives.
    bool bridgeStalled{false};

    // Cumulative counts since Configure(). Deliberately not cleared by
    // Reset(): a cause that fires exactly at a generation boundary must not be
    // erased by that boundary. Bootstrap and generation change are not
    // counted; the generation is visible on its own.
    uint32_t seedReReferenceCount{0};
    uint32_t rxFrameRegressionCount{0};
    uint32_t rxTickRegressionCount{0};
    uint32_t nonAdvancingTickCount{0};
    uint32_t bridgeStallCount{0};
};

class MotuSphClockServo final {
  public:
    static constexpr uint32_t kFractionBits = MotuSphQ32Accumulator::kFractionBits;
    static constexpr int64_t kOneQ32 = MotuSphQ32Accumulator::kOneQ32;
    // `interval << shift` must stay well inside the 128-bit divisor.
    static constexpr uint32_t kMaximumPhaseGainShift = 16;

    // Lock threshold: one fiftieth of a bus cycle, 61.44 ticks.
    // measured: on an 828mk3 at 48 kHz, over eight runs, the settled error
    // stayed at or under 5.39 ticks at the largest residual rate offset
    // (4935 ppb) and the feed-forward dither floor was about 4 ticks, so the
    // threshold stands 11.4x and 15.4x above them.
    //
    // One decision under it locks; only the hard-resync valve unlocks. An
    // intermediate unlock threshold would turn drift into mute flicker: open
    // loop crosses 61.44 ticks in 0.51-6.8 s at the measured residuals.
    static constexpr int64_t kLockThresholdQ32 =
        (static_cast<int64_t>(kTicksPerCycle) * kOneQ32) / 50;

    // assumed: four cycles. It must stand well above the largest seed the
    // loop can give itself (one cycle, see SeedErrorTicks) so that a
    // re-reference can never open the valve on its own. Not measured: the
    // valve has never fired on hardware.
    static constexpr int64_t kHardResyncThresholdCycles = 4;
    static constexpr int64_t kHardResyncThresholdTicks =
        kHardResyncThresholdCycles * static_cast<int64_t>(kTicksPerCycle);

    [[nodiscard]] static constexpr int64_t NominalStepQ32(uint32_t sampleRateHz) noexcept {
        return MotuSphQ32Accumulator::NominalStepQ32ForRate(sampleRateHz);
    }

    // The absolute phase is known only modulo one bus cycle: OHCI completion
    // stamps carry no sub-cycle offset, so the measurement picks up a +/-1
    // cycle pedestal (on an 828mk3 it flipped branch in 27% of windows).
    // Unwrapping that would integrate the aliasing into invented whole cycles,
    // so the phase is folded to (-half, +half] a cycle instead.
    [[nodiscard]] static constexpr int64_t FoldAbsolutePhaseErrorTicks(int64_t ticks) noexcept {
        constexpr int64_t kCycle = kTicksPerCycle;
        int64_t folded = ticks % kCycle;
        if (folded < 0) {
            folded += kCycle;
        }
        return folded > kCycle / 2 ? folded - kCycle : folded;
    }

    // The same fold with the branch cut half a cycle from `centerTicks`
    // instead of from zero. The operating point is arbitrary per stream, so a
    // cut fixed at zero eventually lands on it and two neighbouring samples
    // fold a whole cycle apart; folding about the operating point keeps the
    // cut ~1536 ticks away wherever the stream sits. Same phase, named from
    // the branch nearest the operating point.
    [[nodiscard]] static constexpr int64_t FoldAbsolutePhaseErrorTicksAbout(
        int64_t ticks, int64_t centerTicks) noexcept {
        return centerTicks + FoldAbsolutePhaseErrorTicks(ticks - centerTicks);
    }

    [[nodiscard]] bool Configure(const MotuSphServoConfig& config) noexcept {
        const int64_t nominal = NominalStepQ32(config.sampleRateHz);
        constexpr int64_t kSetpointBound = kTicksPerSecond / 2;
        if (nominal <= 0 || config.phaseCorrectionHorizonFrames == 0 ||
            config.phaseGainShift > kMaximumPhaseGainShift ||
            config.acquisitionPhaseGainShift > kMaximumPhaseGainShift ||
            config.minimumStepQ32 <= 0 || config.minimumStepQ32 > nominal ||
            config.maximumStepQ32 < nominal ||
            config.absolutePhaseSetpointTicks > kSetpointBound ||
            config.absolutePhaseSetpointTicks < -kSetpointBound) {
            configured_ = false;
            decision_ = {};
            return false;
        }

        config_ = config;
        nominalStepQ32_ = nominal;
        configured_ = true;
        Reset();
        return true;
    }

    // A new stream: acquire from scratch. The caller mutes until the loop
    // locks, so starting locked would publish audio at an unverified phase.
    void Reset() noexcept {
        haveReference_ = false;
        locked_ = false;
        referenceGeneration_ = 0;
        referenceRxFrames_ = 0;
        referenceRxTicks_ = 0;
        referenceTxCorrectionQ32_ = 0;
        referenceAbsoluteErrorQ32_ = 0;
        previousRxFrames_ = 0;
        previousRxTicks_ = 0;

        decision_ = {};
        decision_.valid = configured_;
        decision_.stepQ32 = configured_ ? nominalStepQ32_ : 0;
        decision_.measuredStepQ32 = decision_.stepQ32;
    }

    [[nodiscard]] MotuSphServoDecision Update(const MotuSphServoObservation& observation) noexcept {
        if (!configured_) {
            return {};
        }

        // `next` starts as a copy of the last decision, so every per-update
        // flag is cleared here; only the cumulative counts carry over.
        MotuSphServoDecision next = decision_;
        next.feedbackUpdated = false;
        next.phaseReferenceReset = false;
        next.stepClamped = false;
        next.hardResyncRequired = false;
        next.locked = locked_;
        next.referenceCause = MotuSphReferenceCause::kNone;
        next.bridgeStalled = false;

        if (!observation.valid) {
            decision_ = next;
            return decision_;
        }

        const MotuSphReferenceCause cause = ClassifyReference(observation);
        if (cause != MotuSphReferenceCause::kNone) {
            CountReferenceCause(cause);
            EstablishReference(observation, next);
            next.referenceCause = cause;
            PublishCounts(next);
            decision_ = next;
            return decision_;
        }

        if (observation.rxFrames == previousRxFrames_) {
            ++bridgeStallCount_;
            next.bridgeStalled = true;
            PublishCounts(next);
            decision_ = next;
            return decision_;
        }

        const uint64_t intervalFrames = observation.rxFrames - previousRxFrames_;
        const int64_t intervalTicks = observation.rxTicks - previousRxTicks_;
        if (intervalTicks <= 0) {
            // Frames advanced, ticks did not move across this interval.
            ++nonAdvancingTickCount_;
            EstablishReference(observation, next);
            next.referenceCause = MotuSphReferenceCause::kNonAdvancingTicks;
            PublishCounts(next);
            decision_ = next;
            return decision_;
        }

        // Feed-forward: the device's own period over this interval, the raw
        // quotient with no filter (the missing filter is our choice).
        // vendor driver: after its start-up manoeuvre the official driver
        // transmits at the device's rate (two bus captures, 48 kHz).
        // linux: FFADO spaces each transmitted SPH by the receive sync source's
        // ticks per frame (libstreaming/motu/MotuTransmitStreamProcessor.cpp:299,322).
        // observed: with this loop our transmit stream ran at +13.7 ppm while
        // the 828mk3 ran at +14.3 ppm (bus capture of our stream, 48 kHz).
        const __int128 measuredStep =
            (static_cast<__int128>(intervalTicks) << kFractionBits) / intervalFrames;

        const uint64_t referenceFrames = observation.rxFrames - referenceRxFrames_;
        const int64_t referenceTicks = observation.rxTicks - referenceRxTicks_;

        // Feedback, in one domain: how far the device moved from nominal since
        // the reference, minus how much of that the transmit clock has already
        // followed, plus the seed (zero for the relative loop).
        const __int128 rxDeviationQ32 = (static_cast<__int128>(referenceTicks) << kFractionBits) -
                                        static_cast<__int128>(nominalStepQ32_) * referenceFrames;
        const __int128 txDeviationQ32 =
            static_cast<__int128>(observation.txCorrectionQ32) - referenceTxCorrectionQ32_;
        const __int128 phaseErrorQ32 = rxDeviationQ32 - txDeviationQ32 +
                                       static_cast<__int128>(referenceAbsoluteErrorQ32_);

        const __int128 hardThresholdQ32 = static_cast<__int128>(kHardResyncThresholdTicks)
                                          << kFractionBits;
        const bool hardResync = Abs(phaseErrorQ32) > hardThresholdQ32;

        // Above the valve the step returns to the measured rate and the caller
        // repairs the phase. Below it the correction is spread over the
        // interval that actually elapsed, D, not over the configured one: the
        // correction then lives for about D frames, so dividing by a fixed
        // horizon H made the gain per decision D/H. On an 828mk3 at 48 kHz D
        // was 533-543 against H = 512, a gain of about 1.04 with overshoot,
        // and a caller late by 2H or 4H would oscillate or diverge.
        __int128 requestedStep = measuredStep;
        if (!hardResync) {
            const uint32_t gainShift =
                locked_ ? config_.phaseGainShift : config_.acquisitionPhaseGainShift;
            const __int128 gainDivisor = static_cast<__int128>(intervalFrames) << gainShift;
            requestedStep += phaseErrorQ32 / gainDivisor;
        }

        const int64_t requested = SaturateToInt64(requestedStep);
        const int64_t clamped = ClampStep(requested);

        next.valid = true;
        next.feedbackUpdated = true;
        next.stepClamped = clamped != requested;
        next.hardResyncRequired = hardResync;
        next.stepQ32 = clamped;
        next.measuredStepQ32 = SaturateToInt64(measuredStep);
        next.phaseErrorQ32 = SaturateToInt64(phaseErrorQ32);
        next.phaseErrorTicks = SaturateToInt64(phaseErrorQ32 / static_cast<__int128>(kOneQ32));

        // Lock state is updated after the gain above, so each decision is
        // actuated at the gain of the stage it was taken in. No dwell counter:
        // the threshold already stands 11-15x above the noise. The seed
        // re-reference does not unlock either (see EstablishReference).
        if (hardResync) {
            locked_ = false;
        } else if (Abs(phaseErrorQ32) < static_cast<__int128>(kLockThresholdQ32)) {
            locked_ = true;
        }
        next.locked = locked_;

        previousRxFrames_ = observation.rxFrames;
        previousRxTicks_ = observation.rxTicks;
        PublishCounts(next);
        decision_ = next;
        return decision_;
    }

    [[nodiscard]] MotuSphServoDecision Current() const noexcept { return decision_; }
    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }
    [[nodiscard]] int64_t NominalStep() const noexcept { return nominalStepQ32_; }

  private:
    static constexpr __int128 Abs(__int128 value) noexcept { return value < 0 ? -value : value; }

    static constexpr int64_t SaturateToInt64(__int128 value) noexcept {
        if (value > std::numeric_limits<int64_t>::max()) {
            return std::numeric_limits<int64_t>::max();
        }
        if (value < std::numeric_limits<int64_t>::min()) {
            return std::numeric_limits<int64_t>::min();
        }
        return static_cast<int64_t>(value);
    }

    [[nodiscard]] int64_t ClampStep(int64_t value) const noexcept {
        if (value < config_.minimumStepQ32) {
            return config_.minimumStepQ32;
        }
        if (value > config_.maximumStepQ32) {
            return config_.maximumStepQ32;
        }
        return value;
    }

    // The loop's error at the reference, `setpoint - phase`, named from the
    // branch nearest the operating point. The operating point is folded to at
    // most half a cycle from the setpoint and the sample to at most half a
    // cycle from the operating point, so the seed is bounded by one cycle --
    // a quarter of the hard-resync valve.
    [[nodiscard]] int64_t SeedErrorTicks(const MotuSphServoObservation& observation) const noexcept {
        const int64_t setpoint = config_.absolutePhaseSetpointTicks;
        const int64_t error = observation.absolutePhaseTicks - setpoint;
        if (!observation.haveAbsolutePhaseCenter) {
            return FoldAbsolutePhaseErrorTicks(error);
        }
        const int64_t centerError =
            FoldAbsolutePhaseErrorTicks(observation.absolutePhaseCenterTicks - setpoint);
        return FoldAbsolutePhaseErrorTicksAbout(error, centerError);
    }

    [[nodiscard]] MotuSphReferenceCause ClassifyReference(
        const MotuSphServoObservation& observation) const noexcept {
        if (!haveReference_) {
            return MotuSphReferenceCause::kBootstrap;
        }
        if (observation.generation != referenceGeneration_) {
            return MotuSphReferenceCause::kGenerationChange;
        }
        if (observation.absolutePhaseAcquired) {
            return MotuSphReferenceCause::kAbsolutePhaseSeed;
        }
        if (observation.rxFrames < previousRxFrames_) {
            return MotuSphReferenceCause::kRxFrameRegression;
        }
        if (observation.rxTicks < previousRxTicks_) {
            return MotuSphReferenceCause::kRxTickRegression;
        }
        return MotuSphReferenceCause::kNone;
    }

    void CountReferenceCause(MotuSphReferenceCause cause) noexcept {
        switch (cause) {
            case MotuSphReferenceCause::kAbsolutePhaseSeed:
                ++seedReReferenceCount_;
                break;
            case MotuSphReferenceCause::kRxFrameRegression:
                ++rxFrameRegressionCount_;
                break;
            case MotuSphReferenceCause::kRxTickRegression:
                ++rxTickRegressionCount_;
                break;
            default:
                break;
        }
    }

    void PublishCounts(MotuSphServoDecision& next) const noexcept {
        next.seedReReferenceCount = seedReReferenceCount_;
        next.rxFrameRegressionCount = rxFrameRegressionCount_;
        next.rxTickRegressionCount = rxTickRegressionCount_;
        next.nonAdvancingTickCount = nonAdvancingTickCount_;
        next.bridgeStallCount = bridgeStallCount_;
    }

    void EstablishReference(const MotuSphServoObservation& observation,
                            MotuSphServoDecision& next) noexcept {
        haveReference_ = true;
        referenceGeneration_ = observation.generation;
        referenceRxFrames_ = observation.rxFrames;
        referenceRxTicks_ = observation.rxTicks;
        referenceTxCorrectionQ32_ = observation.txCorrectionQ32;

        // With an absolute phase the reference is seeded, not zeroed, so a
        // re-reference re-reads where the stream really sits instead of
        // declaring it correct. The loop settles at
        // `txDeviation - rxDeviation == seed` and the standing phase moves by
        // exactly that, so the seed that lands on the setpoint is
        // `-(phase - setpoint)`; `+phase` would double the offset.
        // vendor driver: the official driver holds a constant presentation
        // phase against the device (one bus capture, 48 kHz, 87.5 s).
        // observed: on an 828mk3 at 48 kHz the seed matched the measured
        // offset to the tick; on a bus capture of our stream the seed moved
        // the transmit phase once, by about +68 ticks, and nothing else did.
        referenceAbsoluteErrorQ32_ =
            observation.haveAbsolutePhase ? -SeedErrorTicks(observation) * kOneQ32 : 0;
        previousRxFrames_ = observation.rxFrames;
        previousRxTicks_ = observation.rxTicks;

        // The lock is left standing: a seed is an offset to remove at the
        // locked gain, not a discontinuity, and dropping the lock here would
        // put a predictable hole in the audio of every stream. The seed is
        // bounded by one cycle, so it cannot trip the four-cycle valve.
        next.valid = true;
        next.feedbackUpdated = false;
        next.phaseReferenceReset = true;
        next.stepClamped = false;
        next.hardResyncRequired = false;
        next.stepQ32 = nominalStepQ32_;
        next.measuredStepQ32 = nominalStepQ32_;
        next.phaseErrorQ32 = referenceAbsoluteErrorQ32_;
        next.phaseErrorTicks = referenceAbsoluteErrorQ32_ / kOneQ32;
    }

    MotuSphServoConfig config_{};
    int64_t nominalStepQ32_{0};
    bool configured_{false};
    bool haveReference_{false};
    bool locked_{false};

    uint64_t referenceGeneration_{0};
    uint64_t referenceRxFrames_{0};
    int64_t referenceRxTicks_{0};
    int64_t referenceTxCorrectionQ32_{0};
    // Bounded by one cycle in ticks, so the Q32.32 form cannot overflow.
    int64_t referenceAbsoluteErrorQ32_{0};

    uint32_t seedReReferenceCount_{0};
    uint32_t rxFrameRegressionCount_{0};
    uint32_t rxTickRegressionCount_{0};
    uint32_t nonAdvancingTickCount_{0};
    uint32_t bridgeStallCount_{0};

    uint64_t previousRxFrames_{0};
    int64_t previousRxTicks_{0};

    MotuSphServoDecision decision_{};
};

static_assert(MotuSphClockServo::kHardResyncThresholdTicks == 4 * 3072,
              "four FireWire cycles in the 24.576 MHz SPH domain");
static_assert(MotuSphClockServo::kLockThresholdQ32 > 61 * MotuSphClockServo::kOneQ32 &&
                  MotuSphClockServo::kLockThresholdQ32 < 62 * MotuSphClockServo::kOneQ32,
              "one fiftieth of a cycle is 61.44 ticks");

} // namespace ASFW::Encoding::Motu
