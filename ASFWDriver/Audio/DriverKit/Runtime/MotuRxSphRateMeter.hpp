#pragma once

#include <cstdint>

#include "MotuPhaseTrace.hpp"

namespace ASFW::Audio::Runtime {

// Measured MOTU V3 receive inter-sample period, in the 24.576 MHz SPH domain.
//
// The device stamps every audio block with its own clock, so the slope of the
// RX SPH sequence is the device's sample period. The official driver feeds that
// measurement back into its transmit step. This meter states the device's
// period as a number and exposes its exact cumulative input to the RX-to-TX
// bridge, where the SPH servo uses it as feed-forward; the meter itself never
// influences a transmitted packet.
//
// Integer-only and O(1) per packet, because it observes from the receive path.
// The drain reads it off the hot path on the same dispatch queue, so plain
// members are as safe here as in the ZTS telemetry beside it.

struct MotuRxSphRateWindow final {
    bool valid{false};
    uint64_t packets{0};            // steps accumulated in this window
    uint64_t frames{0};             // frames spanned
    int64_t ticks{0};               // SPH ticks spanned
    int64_t ticksPerFrameMicro{0};  // measured step, ticks x 1e6 per frame
    // Deviation from the exact rational nominal, in parts per billion. Reported
    // at this resolution because the quantity is genuinely sub-ppm: the first
    // hardware measurement was +945 ppb, which an integer ppm field truncated
    // to a flat 0 — indistinguishable from a device in perfect lock.
    int64_t deviationPpb{0};
    uint64_t rejected{0};           // steps discarded, both causes summed
    // The same total, split by cause -- see `MotuRxSphStepVerdict`. `rejected`
    // stays the sum so every existing reader (log analysers, the pinned
    // tests) keeps its meaning.
    uint64_t rejectedNonMonotonic{0};
    uint64_t rejectedImplausibleRate{0};
    // Largest rejected step by magnitude since `Reset()`, signed, with the
    // frame count it spanned. Reported instead of a second threshold on
    // purpose: where the boundary between "noise" and "discontinuity" lies is
    // not known yet, so this exports the distribution's tail and lets a later
    // measurement place the boundary rather than a guess.
    int64_t worstRejectedStepTicks{0};
    uint32_t worstRejectedFrames{0};
};

// Why a step was not accepted. With a single `rejected` counter for both
// causes, a log line could not tell a runt or a noisy step from
// a restart-sized discontinuity -- and "is the hard-resync valve reachable at
// all?" is exactly the question that distinction answers. Splitting the counter
// changes no control flow: a rejected step is still rejected, in both cases.
enum class MotuRxSphStepVerdict : uint8_t {
    kAccepted = 0,
    // The SPH did not advance. The one-second wrap is already folded out by
    // `MotuShortestTickDifference`, so what remains is a genuine regression:
    // a device restart, a re-queued buffer, or a packet delivered out of order.
    kNonMonotonic,
    // It advanced, but at a rate further than `kMaxPlausibleDeviationPercent`
    // from nominal. A real clock cannot do this; a runt packet, a mis-decoded
    // frame count or a discontinuity can.
    kImplausibleRate,
};

// Accumulated MOTU V3 receive phase error since the meter was last `Reset()`
// (i.e. since stream start/restart, not since the last four-second window).
//
// This is the quantity the official driver's SlaveOutputToInput feeds back
// into SetInterSampleTime: the running difference between what
// the device's own clock actually produced and what our fixed nominal step
// would have produced over the same span. `MotuRxSphRateWindow` only reports
// the instantaneous rate per window; a fixed-but-wrong rate integrates into a
// phase error that keeps growing, and that growth is invisible in any single
// window's `deviationPpb`. Diagnostic-only: nothing reads this to steer a
// transmitted packet -- measure before you regulate.
struct MotuRxPhaseWindow final {
    bool valid{false};
    uint64_t frames{0};              // frames accumulated since Reset()
    int64_t phaseErrorTicks{0};      // cumulative measured ticks minus nominal ticks
    // Same units as the official driver's hard-resync threshold (4.0 cycles
    // on the Internal-clock path), scaled by
    // 1000 so the log carries three decimal digits without floating point --
    // the ppb-not-ppm lesson from `deviationPpb` applies here too: a fraction
    // of a cycle is exactly the range this measurement exists to resolve.
    int64_t phaseErrorMilliCycles{0};
};

// Raw accepted measurement since Reset(), before any nominal-rate subtraction
// or presentation scaling. This is the lossless input for the future TX servo,
// including the exact rational geometry of the 44.1 kHz family.
struct MotuRxSphCumulativeMeasurement final {
    bool valid{false};
    uint64_t frames{0};
    int64_t ticks{0};
};

// Absolute receive presentation phase over one window: the device's own SPH
// against the host's receive cycle timer, both folded into the SPH's
// one-second domain.
//
// This is a different physical quantity from `MotuRxPhaseWindow` above, not a
// second view of it. That one integrates rate error since Reset() and has no
// absolute anchor; this one is a difference of two timestamps taken at the same
// instant, which is what the official driver feeds to SlaveOutputToInput
// against a three-cycle setpoint.
// The field only became readable once the SPH domain was fixed to one second --
// before that it differenced against a host clock still carrying whole seconds
// and read as tens of millions of ticks of aliasing.
//
// Reported as a distribution rather than one reading on purpose: the question
// this exists to answer is the *shape* -- whether the phase holds a setpoint,
// drifts, or jumps -- and a single sample per window cannot separate those.
struct MotuRxAbsPhaseWindow final {
    bool valid{false};
    uint32_t samples{0};  // observations offered during this window
    uint32_t stored{0};   // observations that actually fed min/median/max
    int64_t minTicks{0};
    int64_t medianTicks{0};
    int64_t maxTicks{0};
};

class MotuRxSphRateMeter final {
public:
    // Percent of nominal beyond which a step is a wrap, a restart or a
    // discontinuity rather than a clock. The widest legitimate transient seen
    // on the wire from the official driver is a few percent.
    static constexpr int64_t kMaxPlausibleDeviationPercent = 25;

    // Absolute-phase samples retained per window for the order statistics.
    // Hardware offers roughly eleven telemetry records per four-second window
    // at 48 kHz, so this is headroom rather than a working limit -- but the
    // snapshot reports both counts, so a window that ever exceeds it says so
    // instead of quietly reporting a truncated distribution.
    static constexpr uint32_t kAbsPhaseCapacity = 64;

    static constexpr int64_t kMicro = 1000000;
    static constexpr int64_t kNano = 1000000000;

    void Reset() noexcept {
        havePrevious_ = false;
        previousTicks_ = 0;
        previousFrames_ = 0;
        BeginWindow();
        rejected_ = 0;
        rejectedNonMonotonic_ = 0;
        rejectedImplausibleRate_ = 0;
        worstRejectedStepTicks_ = 0;
        worstRejectedFrames_ = 0;
        cumulativeTicks_ = 0;
        cumulativeFrames_ = 0;
    }

    // Starts after an internal receive-context interruption without measuring
    // one synthetic step across the gap. Unlike Reset(), this preserves the
    // cumulative high-water marks consumed by the same-generation TX servo.
    void Reanchor() noexcept {
        havePrevious_ = false;
        previousTicks_ = 0;
        previousFrames_ = 0;
        BeginWindow();
    }

    // Clears the accumulation without breaking the chain: the next packet still
    // measures a step against the last one observed, so windows tile the stream
    // instead of dropping a step at every boundary.
    void BeginWindow() noexcept {
        windowTicks_ = 0;
        windowFrames_ = 0;
        windowPackets_ = 0;
        absPhaseSamples_ = 0;
        absPhaseStored_ = 0;
    }

    // Absolute phase is sampled per telemetry record, deliberately ahead of the
    // once-per-four-seconds log gate: gated sampling would leave exactly one
    // observation per window and no distribution to report at all.
    //
    // Unlike `Observe()` this takes no plausibility gate. A discontinuity here
    // is the signal, not noise to discard -- the whole point of the window is to
    // show whether the phase jumps.
    void ObserveAbsolutePhase(uint32_t sph, uint32_t cycleTimer) noexcept {
        const int64_t ticks = MotuRxSphMinusCycleTimerTicks(sph, cycleTimer);
        ++absPhaseSamples_;
        if (absPhaseStored_ < kAbsPhaseCapacity) {
            absPhase_[absPhaseStored_++] = ticks;
        }
    }

    // `sph` is the raw first-block SPH in wire order; `framesDecoded` is the
    // frame count of the packet carrying it. NO-DATA packets carry no frames
    // and are simply skipped: they advance neither the SPH nor the frame count,
    // so a step measured across them stays correct.
    void Observe(uint32_t sph,
                 uint32_t framesDecoded,
                 uint32_t sampleRateHz) noexcept {
        if (framesDecoded == 0 || sampleRateHz == 0) {
            return;
        }

        const int64_t ticks =
            NormalizeMotuTicks(ASFW::Timing::encodedTstampToOffsets(sph));

        if (havePrevious_) {
            const int64_t step = MotuShortestTickDifference(ticks, previousTicks_);
            const MotuRxSphStepVerdict verdict =
                ClassifyStep(step, previousFrames_, sampleRateHz);
            if (verdict == MotuRxSphStepVerdict::kAccepted) {
                windowTicks_ += step;
                windowFrames_ += previousFrames_;
                ++windowPackets_;
                // Same rejection gate as the window: a step this meter would
                // have discarded as implausible must not enter the cumulative
                // phase either, or one restart-sized discontinuity would look
                // like a permanent multi-cycle phase jump.
                cumulativeTicks_ += step;
                cumulativeFrames_ += previousFrames_;
            } else {
                ++rejected_;
                if (verdict == MotuRxSphStepVerdict::kNonMonotonic) {
                    ++rejectedNonMonotonic_;
                } else {
                    ++rejectedImplausibleRate_;
                }
                // Magnitude, not recency: the tail is what says whether a
                // rejected step was ever large enough to be the discontinuity
                // the valve exists for.
                const int64_t magnitude = step < 0 ? -step : step;
                const int64_t worst = worstRejectedStepTicks_ < 0
                                          ? -worstRejectedStepTicks_
                                          : worstRejectedStepTicks_;
                if (magnitude > worst) {
                    worstRejectedStepTicks_ = step;
                    worstRejectedFrames_ = previousFrames_;
                }
            }
        }

        previousTicks_ = ticks;
        previousFrames_ = framesDecoded;
        havePrevious_ = true;
    }

    [[nodiscard]] MotuRxSphRateWindow Snapshot(uint32_t sampleRateHz) const noexcept {
        MotuRxSphRateWindow out{};
        out.packets = windowPackets_;
        out.frames = windowFrames_;
        out.ticks = windowTicks_;
        out.rejected = rejected_;
        out.rejectedNonMonotonic = rejectedNonMonotonic_;
        out.rejectedImplausibleRate = rejectedImplausibleRate_;
        out.worstRejectedStepTicks = worstRejectedStepTicks_;
        out.worstRejectedFrames = worstRejectedFrames_;
        if (windowFrames_ == 0 || sampleRateHz == 0) {
            return out;
        }

        out.ticksPerFrameMicro =
            (windowTicks_ * kMicro) / static_cast<int64_t>(windowFrames_);

        // The nominal step is kept rational on purpose. Evaluating it as an
        // integer tick count divides evenly only for the 48 kHz family; 44 100
        // would land on 557 instead of 557.278912 and this meter would report a
        // 500 ppm error that the device does not have.
        const int64_t measured = windowTicks_ * static_cast<int64_t>(sampleRateHz);
        const int64_t nominal = static_cast<int64_t>(windowFrames_) *
            static_cast<int64_t>(
                ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerSecond);
        // Widened for the scaling multiply alone. A four-second window at
        // 48 kHz needs 55 bits here; a window left open across a stalled drain
        // would silently wrap a 64-bit intermediate and invert the sign.
        out.deviationPpb = static_cast<int64_t>(
            (static_cast<__int128>(measured - nominal) * kNano) / nominal);
        out.valid = true;
        return out;
    }

    [[nodiscard]] MotuRxSphCumulativeMeasurement CumulativeSnapshot() const noexcept {
        return {
            .valid = cumulativeFrames_ != 0,
            .frames = cumulativeFrames_,
            .ticks = cumulativeTicks_,
        };
    }

    // Cumulative counterpart of `Snapshot()`: never cleared by `BeginWindow()`,
    // only by `Reset()`. Read at the same cadence as `Snapshot()` -- this is a
    // plain read of accumulator state, not a hot-path call.
    [[nodiscard]] MotuRxPhaseWindow PhaseSnapshot(uint32_t sampleRateHz) const noexcept {
        MotuRxPhaseWindow out{};
        out.frames = cumulativeFrames_;
        if (cumulativeFrames_ == 0 || sampleRateHz == 0) {
            return out;
        }

        // Identical discipline to `deviationPpb`: scale before dividing so the
        // 44.1 kHz family's irrational tick/frame ratio does not truncate, and
        // widen for the multiply -- a 40-minute run accumulates tens of
        // billions of ticks, and this intermediate is that value times the
        // sample rate.
        const __int128 measured =
            static_cast<__int128>(cumulativeTicks_) * sampleRateHz;
        const __int128 nominal = static_cast<__int128>(cumulativeFrames_) *
            static_cast<int64_t>(
                ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerSecond);
        out.phaseErrorTicks =
            static_cast<int64_t>((measured - nominal) / sampleRateHz);
        out.phaseErrorMilliCycles = static_cast<int64_t>(
            (static_cast<__int128>(out.phaseErrorTicks) * 1000) /
            static_cast<int64_t>(
                ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerCycle));
        out.valid = true;
        return out;
    }

    // Order statistics over this window's absolute-phase samples. Read at the
    // same cadence as `Snapshot()`, off the hot path, and cleared by the same
    // `BeginWindow()`.
    [[nodiscard]] MotuRxAbsPhaseWindow AbsolutePhaseSnapshot() const noexcept {
        MotuRxAbsPhaseWindow out{};
        out.samples = absPhaseSamples_;
        out.stored = absPhaseStored_;
        if (absPhaseStored_ == 0) {
            return out;
        }

        // Insertion sort over at most kAbsPhaseCapacity samples. Bounded and
        // allocation-free, which matters more here than asymptotics: this runs
        // on the drain path beside the other telemetry snapshots.
        int64_t sorted[kAbsPhaseCapacity];
        for (uint32_t i = 0; i < absPhaseStored_; ++i) {
            const int64_t value = absPhase_[i];
            uint32_t j = i;
            while (j > 0 && sorted[j - 1] > value) {
                sorted[j] = sorted[j - 1];
                --j;
            }
            sorted[j] = value;
        }

        out.minTicks = sorted[0];
        out.maxTicks = sorted[absPhaseStored_ - 1];
        // Upper of the two middle samples on an even count: a real order
        // statistic, rather than an average that would invent a half tick the
        // device never presented.
        out.medianTicks = sorted[absPhaseStored_ / 2];
        out.valid = true;
        return out;
    }

private:
    // Replaces the former `IsPlausibleStep` bool. Same two conditions, in the
    // same order, with the same outcome for the caller -- only the cause now
    // survives the call.
    [[nodiscard]] static MotuRxSphStepVerdict ClassifyStep(
        int64_t step, uint32_t frames, uint32_t sampleRateHz) noexcept {
        if (step <= 0) {
            return MotuRxSphStepVerdict::kNonMonotonic;
        }
        const int64_t measured = step * static_cast<int64_t>(sampleRateHz);
        const int64_t nominal = static_cast<int64_t>(frames) *
            static_cast<int64_t>(
                ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTicksPerSecond);
        const int64_t error = measured > nominal ? measured - nominal
                                                 : nominal - measured;
        if (error * 100 <= nominal * kMaxPlausibleDeviationPercent) {
            return MotuRxSphStepVerdict::kAccepted;
        }
        return MotuRxSphStepVerdict::kImplausibleRate;
    }

    bool havePrevious_{false};
    int64_t previousTicks_{0};
    uint32_t previousFrames_{0};

    int64_t windowTicks_{0};
    uint64_t windowFrames_{0};
    uint64_t windowPackets_{0};
    uint64_t rejected_{0};
    uint64_t rejectedNonMonotonic_{0};
    uint64_t rejectedImplausibleRate_{0};
    int64_t worstRejectedStepTicks_{0};
    uint32_t worstRejectedFrames_{0};

    int64_t cumulativeTicks_{0};
    uint64_t cumulativeFrames_{0};

    // Window-scoped, like windowTicks_ above: cleared by BeginWindow(), which
    // Reanchor() and Reset() both call.
    int64_t absPhase_[kAbsPhaseCapacity]{};
    uint32_t absPhaseSamples_{0};
    uint32_t absPhaseStored_{0};
};

} // namespace ASFW::Audio::Runtime
