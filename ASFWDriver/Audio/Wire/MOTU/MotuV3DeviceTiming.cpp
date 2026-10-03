// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Off-hot-path half of MotuV3RxTimingObserver: the servo-stall edge, the
// absolute-phase samples and the once-per-four-seconds RX phase report. Keep
// the log line formats stable: offline analysers read them by regex.

#include "MotuV3DeviceTiming.hpp"

#include "../../../Logging/Logging.hpp"

namespace ASFW::Audio::Wire {

void MotuV3RxTimingObserver::DrainTelemetry(uint32_t maxRecords) noexcept {
    CheckServoStall();

    const uint32_t rate = sampleRateHz_;
    const uint64_t dropped = phaseRecords_.Drain(
        maxRecords, [this, rate](const ::ASFW::Isoch::Rx::ZtsTelemetryRecord& record) {
            // Sampled ahead of the log gate on purpose: the gate fires once per
            // four seconds of device frames, and a single reading per window
            // cannot tell a phase holding its setpoint from one drifting.
            rate_.ObserveAbsolutePhase(record.motuRxSph, record.rxCycleTimer);
            if (!logGate_.ShouldEmit(record, rate)) {
                return;
            }
            EmitWindow(rate);
        });
    if (dropped != 0) {
        ASFW_LOG(Zts, "[RxPhaseAbs] drain overflow: dropped=%llu (capacity=%u)", dropped,
                 ::ASFW::Isoch::Rx::ZtsTelemetryRing::kCapacity);
    }
}

void MotuV3RxTimingObserver::CheckServoStall() noexcept {
    auto* control = control_;
    if (control == nullptr) {
        return;
    }
    ::ASFW::Audio::Runtime::MotuRxSphClockSample bridge{};
    uint64_t bridgeUpdates = 0;
    if (!control->motuRxSphClock.ReadLatest(bridge, bridgeUpdates)) {
        return;
    }
    ::ASFW::Audio::Runtime::MotuServoStallEvent event{};
    if (!stallDetector_.Observe(control->generation.load(std::memory_order_acquire),
                                rate_.CumulativeSnapshot(), bridge, bridgeUpdates, event)) {
        return;
    }
    // Edge-triggered, and bypassing the optional os_log mirror so a passive
    // multi-hour soak preserves the first stall.
    ASFW_LOG_RING_ONLY(
        DirectAudio, ::ASFW::Logging::LogLevel::Error,
        "[MotuServoStall] reason=rx-meter-behind-bridge gen=%llu updates=%llu "
        "bridgeFrames=%llu bridgeTicks=%lld meterFrames=%llu meterTicks=%lld",
        event.streamGeneration, event.bridgeUpdates, event.bridgeFrames,
        static_cast<long long>(event.bridgeTicks), event.meterFrames,
        static_cast<long long>(event.meterTicks));
    os_log_error(
        ::ASFW::Driver::Logging::DirectAudio(),
        "[DirectAudio] [MotuServoStall] reason=rx-meter-behind-bridge "
        "gen=%llu updates=%llu bridgeFrames=%llu bridgeTicks=%lld "
        "meterFrames=%llu meterTicks=%lld",
        event.streamGeneration, event.bridgeUpdates, event.bridgeFrames,
        static_cast<long long>(event.bridgeTicks), event.meterFrames,
        static_cast<long long>(event.meterTicks));
}

void MotuV3RxTimingObserver::EmitWindow(uint32_t rate) noexcept {
    const auto sphRate = rate_.Snapshot(rate);
    if (!sphRate.valid) {
        return;
    }

    // `tpfMicro` is ticks x 1e6 per frame; 512000000 is the nominal 48 kHz
    // step. Deviation in ppb and again as signed ppm with three decimals --
    // whole ppm truncated the sub-ppm reality to "0". The rejection fields
    // go last so that, if the record ever truncates, the fields before them
    // survive.
    const int64_t ppb = sphRate.deviationPpb;
    const int64_t ppbMagnitude = ppb < 0 ? -ppb : ppb;
    ASFW_LOG(Zts,
             "[RxSphRate] tpfMicro=%lld ppb=%lld ppm=%s%lld.%03lld "
             "frames=%llu ticks=%lld steps=%llu rejected=%llu rate=%u "
             "rejNonMono=%llu rejRate=%llu worstRej=%lld/%u",
             sphRate.ticksPerFrameMicro, ppb,
             ppb < 0 ? "-" : "+", ppbMagnitude / 1000, ppbMagnitude % 1000,
             sphRate.frames, sphRate.ticks, sphRate.packets,
             sphRate.rejected, rate,
             sphRate.rejectedNonMonotonic,
             sphRate.rejectedImplausibleRate,
             sphRate.worstRejectedStepTicks,
             sphRate.worstRejectedFrames);

    // Diagnostic-only: the phase error a fixed-but-wrong
    // step would have accumulated since stream start.
    const auto phase = rate_.PhaseSnapshot(rate);
    if (phase.valid) {
        const int64_t milliCycles = phase.phaseErrorMilliCycles;
        const int64_t magnitude = milliCycles < 0 ? -milliCycles : milliCycles;
        ASFW_LOG(Zts,
                 "[RxPhase] cycles=%s%lld.%03lld ticks=%lld "
                 "frames=%llu rate=%u",
                 milliCycles < 0 ? "-" : "+", magnitude / 1000,
                 magnitude % 1000, phase.phaseErrorTicks,
                 phase.frames, rate);
    }

    // Absolute phase on its own short line, in ticks: the official driver's
    // three-cycle setpoint is 9216 ticks and whole cycles would erase it.
    const auto absPhase = rate_.AbsolutePhaseSnapshot();
    auto* control = control_;
    if (absPhase.valid) {
        ASFW_LOG(Zts,
                 "[RxPhaseAbs] min=%lld med=%lld max=%lld n=%u/%u",
                 absPhase.minTicks, absPhase.medianTicks,
                 absPhase.maxTicks, absPhase.stored,
                 absPhase.samples);

        // TX's presentation error (the cadence-period mean of
        // `motuPhaseTrace`) against this window's RX absolute-phase
        // median, as a modular difference. The two terms are
        // neither simultaneous nor of the same statistical kind; a consumer
        // of `rel` must not assume they are aligned in time.
        ::ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample motuPeriod{};
        uint64_t motuUpdates = 0;
        if (control != nullptr &&
            control->motuPhaseTrace.ReadLatest(motuPeriod, motuUpdates)) {
            const auto residual =
                ::ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(motuPeriod);
            if (residual.valid) {
                const int64_t rel = ::ASFW::Audio::Runtime::MotuShortestTickDifference(
                    residual.residualTicks, absPhase.medianTicks);
                ASFW_LOG(Zts,
                         "[RxPhaseRel] rel=%lld residualTicks=%lld "
                         "rxAbsTicks=%lld txUpdates=%llu buckets=%u",
                         rel, residual.residualTicks, absPhase.medianTicks,
                         motuUpdates, motuPeriod.count);

                // The same measurement, conditioned and published
                // for the controller. The line above stays untouched -- the
                // analyser's regex reads it.
                const auto conditioned = relPhase_.Accept(rel);
                if (conditioned.valid) {
                    ::ASFW::Audio::Runtime::MotuRelPhaseSampleRecord sample{};
                    sample.ticks = conditioned.ticks;
                    sample.centerTicks = conditioned.centerTicks;
                    // The block's LIVE generation: a cached copy made every
                    // sample fail the reader's equality check.
                    sample.streamGeneration =
                        control->generation.load(std::memory_order_acquire);
                    control->motuRelPhase.Publish(sample);

                    // One line on a stream's first publish, so a run can tell
                    // "RX published nothing" from "the reader rejected it".
                    if (relPhase_.PublishedSamples() == 1) {
                        ASFW_LOG(Zts,
                                 "[RxPhaseBridge] first publish gen=%llu "
                                 "ticks=%lld center=%lld",
                                 sample.streamGeneration, sample.ticks,
                                 sample.centerTicks);
                    }
                }
                if (conditioned.latticeCorrectionTicks != 0) {
                    ASFW_LOG(Zts,
                             "[RxPhaseLattice] corrected=%lld excess=%lld "
                             "center=%lld corrections=%llu published=%llu",
                             conditioned.latticeCorrectionTicks,
                             conditioned.excessTicks, conditioned.centerTicks,
                             relPhase_.LatticeCorrections(),
                             relPhase_.PublishedSamples());
                }
            }
        }
    }

    rate_.BeginWindow();
}

} // namespace ASFW::Audio::Wire
