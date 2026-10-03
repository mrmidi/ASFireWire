// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pure MOTU V3 SPH clock controller.  This models the observable shape of the
// original driver's Internal-clock loop without copying its implementation:
// measured RX period is the feed-forward term, closed-loop phase is the
// proportional term, and a four-cycle error selects the hard-resync outcome.

#pragma once

#include "../../Wire/AMDTP/MotuSphQ32Accumulator.hpp"
#include "../../Wire/AMDTP/MotuV3WireFormat.hpp"

#include <cstdint>
#include <limits>

namespace ASFW::Audio::MOTU {

struct MotuSphServoConfig final {
    uint32_t sampleRateHz{0};

    // The caller's minimum update interval, in frames.  The controller no
    // longer divides the phase error by this: it divides by the interval that
    // actually elapsed, so the loop gain per update stops depending on how
    // late the caller was.  The field stays in the config because the wiring
    // needs one source of truth for the cadence -- DiceTxStreamEngine gates its
    // updates on this same value -- and because a caller that leaves it unset
    // has no defined update cadence at all, which Configure still rejects.
    uint64_t phaseCorrectionHorizonFrames{0};

    // Phase gain as a right shift on the elapsed interval: the correction per
    // frame is `phaseError / (D << phaseGainShift)`, so the gain per update is
    // `1 / (1 << phaseGainShift)` for any D.  The default 2 gives 0.25, the
    // original's locked-state `measured + correction/4`.  Inferred from the
    // official driver's behaviour, not measured on the wire -- named here so the
    // value is a stated policy rather than a constant nobody can question.
    uint32_t phaseGainShift{2};

    // The same gain while the loop is still acquiring, before it has ever
    // reported an error under kLockThresholdQ32. Zero means 1/D, i.e. the whole
    // measured error is asked for over the next interval instead of a quarter
    // of it: from a full-cycle seed (3072 ticks, the bound on SeedErrorTicks)
    // that reaches the lock threshold in ONE update -- about 11 ms -- against
    // 14 updates and 156 ms at the locked gain. The output is muted for
    // exactly that window, so
    // the choice is between 11 ms and 156 ms of silence, not between two
    // settling curves.
    //
    // This is the acquisition stage of the two-stage loop, not a second
    // controller: same error, same actuator, same clamp, one shift different.
    uint32_t acquisitionPhaseGainShift{0};

    // Equivalent of the original SetInterSampleTime clamp.  Production wiring
    // must choose these bounds from an explicit policy; the pure controller
    // merely enforces them.
    int64_t minimumStepQ32{0};
    int64_t maximumStepQ32{0};

    // Plant identification. A one-shot phase step, injected MID-STREAM,
    // so the actuator can be checked against a known input before it is ever
    // fed a measured one: does `rel` move, by how much, and in which direction.
    //
    // Mid-stream and not at the reference, deliberately. The operating point is
    // arbitrary per stream start (measured on hardware), so a before/after
    // comparison across two starts compares two unrelated points; inside one
    // continuous Running it compares the same one. That is also why this is a
    // step on the EXISTING reference rather than a re-reference: re-referencing
    // would reset the very loop state the comparison is about.
    //
    // Zero disables it, and zero is what every production build ships. A run
    // with this set is an instrument build, not a driver build.
    int64_t phaseInjectionTicks{0};
    // How many frames after the reference the step fires. Must be non-zero when
    // an injection is armed -- firing at the first observation would be a seed,
    // not a mid-stream step, and would measure nothing the seed path does not.
    uint64_t phaseInjectionAfterFrames{0};
};

struct MotuSphServoObservation final {
    bool valid{false};
    uint64_t generation{0};

    // Cumulative accepted RX measurement.  `rxTicks` is the raw measured span,
    // not a formatted micro/ppb value, so the 44.1 kHz family keeps its exact
    // rational period until the single Q32.32 conversion below.
    uint64_t rxFrames{0};
    int64_t rxTicks{0};

    // Cumulative TX phase movement relative to the exact nominal step, Q32.32
    // ticks.  The future packetizer integration owns this accumulator because
    // only it knows which DATA frames were actually emitted.
    int64_t txCorrectionQ32{0};

    // Absolute presentation phase in 24.576 MHz ticks -- the `rel` the
    // [RxPhaseRel] bridge measures, which already has kPresentationLeadTicks
    // subtracted. This is a MEASUREMENT, not an error: the value it should hold
    // is kOracleRelSetpointTicks (+510, measured off the official driver on
    // the wire), and the controller forms the error against that setpoint
    // in EstablishReference. The earlier reading of this field -- "zero when we
    // sit exactly three cycles ahead of the device" -- was an assumption that
    // the same measurement disproved.
    //
    // Optional on purpose. Without it the loop keeps its historical behaviour:
    // the error is whatever has accumulated since the reference, so a stream
    // that started with a real offset is declared perfect forever. With it the
    // reference is seeded instead of zeroed, which is what makes the error
    // absolute.
    bool haveAbsolutePhaseError{false};
    int64_t absolutePhaseErrorTicks{0};

    // The operating point the measurement above was folded about, from
    // MotuRelPhaseConditioner. Optional and separate from the value on purpose:
    // without it the seed folds about the setpoint, which is where the branch
    // cut sat originally, so every caller that does not supply one keeps
    // exactly that behaviour.
    bool haveAbsolutePhaseCenter{false};
    int64_t absolutePhaseCenterTicks{0};

    // Absolute phase has just become available, and the standing reference was
    // built without it. Forces a re-reference on this observation.
    //
    // Not an edge case -- the normal case. The reference is established on the
    // first observation of a stream, about 30 ms in, while the conditioner
    // needs three telemetry windows (~12 s) before it has an operating point.
    // Without this the loop would keep a zero seed for the whole stream and the
    // measured phase would never reach it, which is exactly what an earlier
    // hardware run recorded: phaseTicks=0 across 32892 decisions.
    bool absolutePhaseAcquired{false};
};

// Why `Update` abandoned the running reference and re-read the stream, or why
// it returned without measuring at all. Previously all of these took the same
// silent path: one condition with five causes ORed together, plus two early
// returns, none of them reported. Two of the causes are ordinary (a new stream,
// the ~12 s seed re-reference) and three are discontinuities -- exactly the
// events the hard-resync valve exists for. A counter that cannot tell them
// apart cannot answer whether those events ever reach the servo.
//
// Order matters: these are evaluated with the same short-circuit precedence as
// the condition they replace, so the reported cause is the first one that held,
// not an arbitrary one among several.
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

    // Which stage of the loop produced this decision, and the single source of
    // truth for muting: the output is silent exactly while this is false. See
    // MotuSphHardResyncGate, which owns no threshold of its own any more.
    bool locked{false};

    int64_t stepQ32{0};
    int64_t measuredStepQ32{0};
    int64_t phaseErrorQ32{0};
    int64_t phaseErrorTicks{0};

    // True on the single update where the injected step fired, so the run
    // that measures `rel` before and after can mark the boundary from the log
    // instead of guessing it from wall time.
    bool phaseInjectionApplied{false};

    // Why this update re-referenced, `kNone` when it did not. Makes a
    // single event attributable; `phaseReferenceReset` alone says only that
    // something happened, and it is set for all six causes.
    MotuSphReferenceCause referenceCause{MotuSphReferenceCause::kNone};

    // True on an update that returned without measuring because the bridge did
    // not advance the frame count. Not a re-reference -- the reference survives
    // -- but it is the third place an event could be swallowed, so it is
    // reported alongside them.
    bool bridgeStalled{false};

    // Cumulative counts, carried on the decision so the servo stays the single
    // classifier and the caller only has to copy numbers. Deliberately NOT
    // cleared by `Reset()`: a cause that fires exactly at a generation
    // boundary must not be erased by that boundary. Counted since `Configure`.
    uint32_t seedReReferenceCount{0};
    uint32_t rxFrameRegressionCount{0};
    uint32_t rxTickRegressionCount{0};
    uint32_t nonAdvancingTickCount{0};
    uint32_t bridgeStallCount{0};
};

class MotuSphClockServo final {
  public:
    static constexpr uint32_t kFractionBits =
        ASFW::Protocols::Audio::AMDTP::MotuSphQ32Accumulator::kFractionBits;
    static constexpr int64_t kOneQ32 =
        ASFW::Protocols::Audio::AMDTP::MotuSphQ32Accumulator::kOneQ32;
    static constexpr uint32_t kMaximumPhaseGainShift = 16;

    // Lock threshold, Q32.32 ticks: one fiftieth of a bus cycle, 61.44 ticks.
    // Taken from the official driver's unmute hysteresis (inferred, not
    // measured on the wire) and
    // kept because the measured margins are wide, not because it was inherited:
    // at the worst residual of the eight retained runs (4935 ppb) the settled
    // error is 5.39 ticks and the feed-forward dither floor about 4, so the
    // threshold stands 11.4x and 15.4x above them.
    //
    // The hard-resync valve is the only way back out. An
    // intermediate unlock threshold was considered and rejected: open loop
    // crosses 61.44 ticks in 0.51-6.8 s at the observed residuals, so any
    // threshold below the valve would turn ordinary drift into mute flicker.
    static constexpr int64_t kLockThresholdQ32 =
        (static_cast<int64_t>(ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerCycle) *
         kOneQ32) /
        50;

    static constexpr int64_t kHardResyncThresholdCycles = 4;
    static constexpr int64_t kHardResyncThresholdTicks =
        kHardResyncThresholdCycles *
        static_cast<int64_t>(ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerCycle);

    [[nodiscard]] static constexpr int64_t NominalStepQ32(uint32_t sampleRateHz) noexcept {
        return ASFW::Protocols::Audio::AMDTP::MotuSphQ32Accumulator::
            NominalStepQ32ForRate(sampleRateHz);
    }

    // The absolute error is known only modulo one bus cycle, and folding is the
    // honest representation of that -- not a loss of information we ever had.
    // OHCI completion stamps carry no sub-cycle offset field, so `rel` picks up
    // a +/-1 cycle pedestal; on hardware it flipped branch in 27% of windows,
    // correlated with the RX sawtooth phase rather than with anything the
    // device did. Unwrapping that sequence would integrate the
    // aliasing into invented whole cycles, so we fold to +/-half a cycle and
    // state the resulting accuracy rather than pretending to more.
    [[nodiscard]] static constexpr int64_t FoldAbsolutePhaseErrorTicks(int64_t ticks) noexcept {
        constexpr int64_t kCycle =
            static_cast<int64_t>(ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerCycle);
        int64_t folded = ticks % kCycle;
        if (folded < 0) {
            folded += kCycle;
        }
        return folded > kCycle / 2 ? folded - kCycle : folded;
    }

    // The same fold, with the branch cut moved half a cycle away from a stated
    // operating point instead of away from zero.
    //
    // Folding about zero puts the cut at a fixed place while the operating
    // point is arbitrary per stream start, so sooner or later the two
    // coincide: one hardware run landed 140 ticks from the +/-1536 cap with a MAD
    // of 1, close enough that neighbouring samples of an 8-tick-noise signal
    // fold to opposite ends and report a 3060-tick step that never happened.
    // Folding about the running operating point keeps the cut ~1536 ticks away
    // from it wherever the stream started. This fixes an instability of the
    // wrap, not an amplitude: the returned value is the same phase, named from
    // the branch nearest the operating point.
    [[nodiscard]] static constexpr int64_t FoldAbsolutePhaseErrorTicksAbout(
        int64_t ticks, int64_t centerTicks) noexcept {
        return centerTicks + FoldAbsolutePhaseErrorTicks(ticks - centerTicks);
    }

    [[nodiscard]] bool Configure(const MotuSphServoConfig& config) noexcept {
        const int64_t nominal = NominalStepQ32(config.sampleRateHz);
        // The shift bound is not decoration: `interval << shift` is undefined
        // once the shift reaches the width of the type, and the interval is a
        // runtime value, so an unchecked config turns a wiring mistake into
        // undefined behaviour on the audio path rather than a failed Configure.
        // An armed injection has two bounds of its own. It must fire mid-stream,
        // not at the reference, or it is a seed wearing another name; and it
        // must stay under the hard-resync threshold, or the identification run
        // measures the valve firing rather than the plant responding.
        const bool injectionArmed = config.phaseInjectionTicks != 0;
        const bool injectionRejected =
            injectionArmed && (config.phaseInjectionAfterFrames == 0 ||
                               config.phaseInjectionTicks >= kHardResyncThresholdTicks ||
                               config.phaseInjectionTicks <= -kHardResyncThresholdTicks);

        if (nominal <= 0 || config.phaseCorrectionHorizonFrames == 0 ||
            config.phaseGainShift > kMaximumPhaseGainShift ||
            config.acquisitionPhaseGainShift > kMaximumPhaseGainShift ||
            config.minimumStepQ32 <= 0 || config.minimumStepQ32 > nominal ||
            config.maximumStepQ32 < nominal || config.minimumStepQ32 > config.maximumStepQ32 ||
            injectionRejected) {
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

    void Reset() noexcept {
        haveReference_ = false;
        // A new stream acquires from scratch: the caller mutes until the loop
        // locks, so starting locked would publish audio at an unverified phase.
        locked_ = false;
        // Re-armed per stream generation, NOT per reference: a re-reference
        // mid-run already voids the before/after comparison, and injecting a
        // second step into the same run would void it twice over.
        phaseInjectionFired_ = false;
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

        MotuSphServoDecision next = decision_;
        next.feedbackUpdated = false;
        next.phaseReferenceReset = false;
        next.stepClamped = false;
        next.hardResyncRequired = false;
        next.phaseInjectionApplied = false;
        next.locked = locked_;
        // Per-update, like every flag above it: `next` starts as a copy of the
        // last decision, so a cause left set would report the previous event
        // forever. The cumulative counts are the fields that persist.
        next.referenceCause = MotuSphReferenceCause::kNone;
        next.bridgeStalled = false;

        if (!observation.valid) {
            decision_ = next;
            return decision_;
        }

        // Same five conditions, same precedence, same outcome -- only the cause
        // now survives the branch. `ClassifyReference` short-circuits in the
        // order the original `||` chain did, so which cause is reported when
        // several hold is unchanged.
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
            // The bridge published no new frames. The reference stands, so this
            // is not a re-reference -- but it is an update that measured
            // nothing, so it is reported too.
            ++bridgeStallCount_;
            next.bridgeStalled = true;
            PublishCounts(next);
            decision_ = next;
            return decision_;
        }

        const uint64_t intervalFrames = observation.rxFrames - previousRxFrames_;
        const int64_t intervalTicks = observation.rxTicks - previousRxTicks_;
        if (intervalTicks <= 0) {
            // Frames advanced but ticks did not. Distinct from the tick
            // regression above: there the counter went backwards across the
            // reference, here it failed to move across one interval.
            ++nonAdvancingTickCount_;
            EstablishReference(observation, next);
            next.referenceCause = MotuSphReferenceCause::kNonAdvancingTicks;
            PublishCounts(next);
            decision_ = next;
            return decision_;
        }

        const __int128 measuredStep =
            (static_cast<__int128>(intervalTicks) << kFractionBits) / intervalFrames;

        const uint64_t referenceFrames = observation.rxFrames - referenceRxFrames_;
        const int64_t referenceTicks = observation.rxTicks - referenceRxTicks_;

        // Step the reference, not the phase. Steady state of this loop is
        // `txDeviation - rxDeviation == seed`, so adding X to the seed asks the
        // loop to move the standing phase by exactly +X -- through its own
        // actuator, at its own clamped rate. A direct phase jump would test the
        // packetizer's repair path instead, which is not the question: the
        // question is whether the SERVO moves `rel`, and which way.
        if (config_.phaseInjectionTicks != 0 && !phaseInjectionFired_ &&
            referenceFrames >= config_.phaseInjectionAfterFrames) {
            referenceAbsoluteErrorQ32_ += config_.phaseInjectionTicks * kOneQ32;
            phaseInjectionFired_ = true;
            next.phaseInjectionApplied = true;
        }

        // Close the loop in one domain. RX deviation says how far the device
        // moved from nominal; TX deviation says how much of that movement the
        // packetizer has already followed. Feeding raw PhaseSnapshot directly
        // would omit the second term and integrate forever after actuation.
        const __int128 rxDeviationQ32 = (static_cast<__int128>(referenceTicks) << kFractionBits) -
                                        static_cast<__int128>(nominalStepQ32_) * referenceFrames;
        const __int128 txDeviationQ32 =
            static_cast<__int128>(observation.txCorrectionQ32) - referenceTxCorrectionQ32_;
        // The seed is what makes this error absolute. Without it both deviation
        // terms are zero at the reference, so the loop holds whatever phase
        // happened to exist then -- including a wrong one -- and reports zero
        // error while doing it.
        const __int128 phaseErrorQ32 = rxDeviationQ32 - txDeviationQ32 +
                                       static_cast<__int128>(referenceAbsoluteErrorQ32_);

        const __int128 hardThresholdQ32 = static_cast<__int128>(kHardResyncThresholdTicks)
                                          << kFractionBits;
        const bool hardResync = Abs(phaseErrorQ32) > hardThresholdQ32;

        // This is one continuous loop for both slow drift and faster change:
        // no thermal-step detector or alternate controller exists.  Above the
        // safety threshold the same loop returns to measured rate and asks its
        // caller to perform the separately-owned hard phase repair.
        // Normalise the gain to the interval that actually elapsed, not to the
        // configured one. The correction is applied per frame and then lives
        // for D frames, so dividing by a fixed H makes the gain per update D/H
        // -- and D is not H. In production D is about 533.5 against H = 512, so
        // the loop ran at gain 1.04: deadbeat with overshoot. Worse, the gain
        // scaled with lateness, so a caller running at D >= 2H oscillated and
        // at D >= 4H diverged, with nothing in the loop to stop it. Dividing by
        // `D << shift` makes the gain per update exactly 1/(1 << shift) for
        // every D, which is the property the original has in its locked state.
        __int128 requestedStep = measuredStep;
        if (!hardResync) {
            // The two-stage part of P5: the same expression, one shift apart.
            // Acquisition asks for the whole measured error over the next
            // interval so the muted window stays at one update; once locked the
            // loop drops to the original's quarter gain, where its noise
            // amplification is four times lower (P3).
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

        // One decision with feedback below the threshold
        // locks: there is no dwell counter, because the threshold already
        // stands 11-15x above the noise, so confirming it a second time buys
        // nothing and costs another update of silence. Only the hard-resync
        // valve unlocks -- in particular the ~12 s seed re-reference does not,
        // which is what keeps a predictable audio hole out of every stream.
        // Evaluated after the gain above, so a
        // decision is actuated at the gain of the stage it was taken in.
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

    // The loop's reported error at the reference: `setpoint - rel`, named from
    // the branch nearest the operating point.
    //
    // Bound, because it is no longer the old half-cycle one and silence about
    // that would be a trap: the operating point is folded to at most half a
    // cycle from the setpoint, and the sample to at most half a cycle from the
    // operating point, so the seed is bounded by ONE cycle rather than half.
    // The hard-resync valve stands at four, so a reference still cannot open it
    // on its own -- pinned by SeedCannotOpenTheHardResyncValve.
    [[nodiscard]] static constexpr int64_t SeedErrorTicks(
        const MotuSphServoObservation& observation) noexcept {
        constexpr int64_t kSetpoint =
            ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kOracleRelSetpointTicks;
        const int64_t error = observation.absolutePhaseErrorTicks - kSetpoint;
        if (!observation.haveAbsolutePhaseCenter) {
            return FoldAbsolutePhaseErrorTicks(error);
        }
        const int64_t centerError =
            FoldAbsolutePhaseErrorTicks(observation.absolutePhaseCenterTicks - kSetpoint);
        return FoldAbsolutePhaseErrorTicksAbout(error, centerError);
    }

    // The former `||` chain, unrolled. Returning the FIRST condition that
    // holds is what keeps this behaviourally identical: `||` stopped at the
    // first true operand too, so a stream generation change that also carries
    // a frame regression still reports as a generation change, exactly as the
    // old branch treated it.
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

    // Bootstrap and generation change are deliberately not counted: the first
    // happens once per stream and the second is already legible from `gen` on
    // the telemetry line, so a counter for either would only add a number that
    // says what is visible elsewhere.
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
        // Seed rather than zero: a re-reference must not erase a real offset.
        // Every re-reference re-reads where
        // the stream actually sits instead of declaring it correct by fiat.
        //
        // Both the sign and the setpoint are fixes, not decoration. This loop
        // settles at
        // `txDeviation - rxDeviation == seed`, and the standing phase moves by
        // exactly that difference, so the phase it converges on is `rel + seed`.
        // Seeding `+rel` therefore DOUBLES the offset it was meant to remove,
        // and seeding against zero aims at a setpoint the device does not hold.
        // The seed that lands on the measured setpoint is `-(rel - setpoint)`;
        // equivalently the loop's reported error becomes `setpoint - rel`.
        //
        // The fold's branch cut goes half a cycle from the
        // OPERATING POINT, not from the setpoint. The operating point is
        // arbitrary per stream start, so a cut fixed at the setpoint eventually
        // lands on it and neighbouring samples of a 1-tick-MAD signal seed the
        // loop 3060 ticks apart. Folding the operating point itself about the
        // setpoint first keeps the seed bounded whatever raw value arrives.
        referenceAbsoluteErrorQ32_ =
            observation.haveAbsolutePhaseError
                ? -SeedErrorTicks(observation) * kOneQ32
                : 0;
        previousRxFrames_ = observation.rxFrames;
        previousRxTicks_ = observation.rxTicks;

        next.valid = true;
        next.feedbackUpdated = false;
        next.phaseReferenceReset = true;
        next.stepClamped = false;
        // The seed is bounded by one cycle (see SeedErrorTicks) and the gate
        // trips at four, so a reference can never trip it on its own.
        // Accumulated divergence still can, now measured from the true offset
        // rather than from zero.
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
    // Bounded by half a cycle in ticks, so the Q32.32 form cannot overflow.
    // An armed injection adds at most one bounded step on top of that.
    int64_t referenceAbsoluteErrorQ32_{0};
    bool phaseInjectionFired_{false};
    // Counted since `Configure`, NOT cleared by `Reset()` -- see the decision
    // fields these feed for why a generation boundary must not erase a cause
    // that fires at one.
    uint32_t seedReReferenceCount_{0};
    uint32_t rxFrameRegressionCount_{0};
    uint32_t rxTickRegressionCount_{0};
    uint32_t nonAdvancingTickCount_{0};
    uint32_t bridgeStallCount_{0};

    uint64_t previousRxFrames_{0};
    int64_t previousRxTicks_{0};

    MotuSphServoDecision decision_{};
};

// Written as 4 * 3072 rather than the product: these are SPH ticks, and the
// timing-geometry guard reserves the bare product for the HAL period.
static_assert(MotuSphClockServo::kHardResyncThresholdTicks == 4 * 3072,
              "four FireWire cycles in the 24.576 MHz SPH domain");

} // namespace ASFW::Audio::MOTU
