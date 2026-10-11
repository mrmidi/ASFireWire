// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRxSphRateMeter.hpp - the MOTU device's sample period, measured from the
// SPH it stamps on every received data block.
//
// The device stamps each block with its own clock, so the slope of the RX SPH
// sequence is its sample period. The meter reports that as a windowed rate
// (telemetry) and exposes the exact cumulative input -- frames and ticks since
// the stream started -- that the transmit SPH regulator uses as its
// feed-forward term, through the motuRxSphClock bridge. The meter itself
// never touches a transmitted packet.
//
// Integer-only and O(1) per packet: it runs on the receive path. The
// telemetry drain reads it on the same dispatch queue, so plain members are
// safe.

#pragma once

#include "../../Wire/MOTU/MotuSph.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

struct MotuRxSphRateWindow final {
    bool valid{false};
    uint64_t packets{0};            // steps accumulated in this window
    uint64_t frames{0};             // frames spanned
    int64_t ticks{0};               // SPH ticks spanned
    int64_t ticksPerFrameMicro{0};  // measured step, ticks x 1e6 per frame
    // Deviation from the exact rational nominal, in parts per billion: the
    // quantity is genuinely sub-ppm (an 828mk3 measured +945 ppb), which an
    // integer ppm field would truncate to a device in perfect lock.
    int64_t deviationPpb{0};
    uint64_t rejected{0};  // steps discarded, both causes summed
    uint64_t rejectedNonMonotonic{0};
    uint64_t rejectedImplausibleRate{0};
    // Largest rejected step by magnitude since Reset(), signed, with the
    // frame count it spanned: the tail of the distribution, so a later
    // measurement can tell noise from a discontinuity.
    int64_t worstRejectedStepTicks{0};
    uint32_t worstRejectedFrames{0};
};

// Why a step was not accepted.
enum class MotuRxSphStepVerdict : uint8_t {
    kAccepted = 0,
    // The SPH did not advance. The one-second wrap is already folded out, so
    // this is a genuine regression: a device restart, a re-queued buffer, or a
    // packet delivered out of order.
    kNonMonotonic,
    // It advanced, but further from nominal than kMaxPlausibleDeviationPercent.
    // A clock cannot do that; a runt packet, a mis-decoded frame count or a
    // discontinuity can.
    kImplausibleRate,
};

// Phase error accumulated since Reset(): what the device's clock produced
// minus what the exact nominal step would have produced over the same frames.
// A fixed-but-wrong rate looks the same in every window's deviationPpb but
// grows here. Telemetry only.
struct MotuRxPhaseWindow final {
    bool valid{false};
    uint64_t frames{0};
    int64_t phaseErrorTicks{0};
    // In bus cycles x 1000, so a fraction of a cycle survives integer logging.
    int64_t phaseErrorMilliCycles{0};
};

// Raw accepted measurement since Reset(), before any nominal subtraction: the
// lossless feed-forward input of the transmit regulator, exact for the
// 44.1 kHz family too.
struct MotuRxSphCumulativeMeasurement final {
    bool valid{false};
    uint64_t frames{0};
    int64_t ticks{0};
};

class MotuRxSphRateMeter final {
public:
    // Percent of nominal beyond which a step is a wrap, a restart or a
    // discontinuity rather than a clock.
    // assumed: a real device clock stays within a fraction of a percent of
    // nominal; 25 % only has to separate a clock from those events.
    static constexpr int64_t kMaxPlausibleDeviationPercent = 25;

    static constexpr int64_t kMicro = 1000000;
    static constexpr int64_t kNano = 1000000000;

    // A new stream: forget everything.
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

    // Resume after an interruption of the receive context within the same
    // stream, without measuring one synthetic step across the gap. Unlike
    // Reset() the cumulative totals are kept, so what the regulator reads
    // stays monotonic for the whole stream.
    void Reanchor() noexcept {
        havePrevious_ = false;
        previousTicks_ = 0;
        previousFrames_ = 0;
        BeginWindow();
    }

    // Clear the window without breaking the chain: the next packet still
    // measures a step against the last one, so windows tile the stream.
    void BeginWindow() noexcept {
        windowTicks_ = 0;
        windowFrames_ = 0;
        windowPackets_ = 0;
    }

    // `sph` is the first block's SPH quadlet in host order, `framesDecoded`
    // the packet's frame count. NO-DATA packets carry no frames and are
    // skipped: they advance neither the SPH nor the frame count, so a step
    // measured across them is still one packet's worth.
    void Observe(uint32_t sph, uint32_t framesDecoded, uint32_t sampleRateHz) noexcept {
        if (framesDecoded == 0 || sampleRateHz == 0) {
            return;
        }

        const int64_t ticks = ::ASFW::Encoding::Motu::TickFromSph(sph);

        if (havePrevious_) {
            const int64_t step = ::ASFW::Encoding::Motu::TickDifference(ticks, previousTicks_);
            const MotuRxSphStepVerdict verdict = ClassifyStep(step, previousFrames_, sampleRateHz);
            if (verdict == MotuRxSphStepVerdict::kAccepted) {
                windowTicks_ += step;
                windowFrames_ += previousFrames_;
                ++windowPackets_;
                // The same gate guards the cumulative total: a restart-sized
                // step must not become a permanent multi-cycle phase jump.
                cumulativeTicks_ += step;
                cumulativeFrames_ += previousFrames_;
            } else {
                ++rejected_;
                if (verdict == MotuRxSphStepVerdict::kNonMonotonic) {
                    ++rejectedNonMonotonic_;
                } else {
                    ++rejectedImplausibleRate_;
                }
                const int64_t magnitude = step < 0 ? -step : step;
                const int64_t worst =
                    worstRejectedStepTicks_ < 0 ? -worstRejectedStepTicks_ : worstRejectedStepTicks_;
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

        out.ticksPerFrameMicro = (windowTicks_ * kMicro) / static_cast<int64_t>(windowFrames_);

        // The nominal stays rational: as an integer tick count it divides
        // evenly only for the 48 kHz family, and 44.1 kHz would read 500 ppm
        // slow on a device that is exactly on rate. Widened for the multiply:
        // a window left open across a stalled drain must not wrap.
        const __int128 measured = static_cast<__int128>(windowTicks_) * sampleRateHz;
        const __int128 nominal = static_cast<__int128>(windowFrames_) * kTicksPerSecond;
        out.deviationPpb = static_cast<int64_t>(((measured - nominal) * kNano) / nominal);
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

    // Cleared only by Reset(), never by BeginWindow().
    [[nodiscard]] MotuRxPhaseWindow PhaseSnapshot(uint32_t sampleRateHz) const noexcept {
        MotuRxPhaseWindow out{};
        out.frames = cumulativeFrames_;
        if (cumulativeFrames_ == 0 || sampleRateHz == 0) {
            return out;
        }

        // Scale before dividing, so the 44.1 kHz family does not truncate,
        // and widen: a long run accumulates tens of billions of ticks.
        const __int128 measured = static_cast<__int128>(cumulativeTicks_) * sampleRateHz;
        const __int128 nominal = static_cast<__int128>(cumulativeFrames_) * kTicksPerSecond;
        out.phaseErrorTicks = static_cast<int64_t>((measured - nominal) / sampleRateHz);
        out.phaseErrorMilliCycles = static_cast<int64_t>(
            (static_cast<__int128>(out.phaseErrorTicks) * 1000) / kTicksPerCycle);
        out.valid = true;
        return out;
    }

private:
    static constexpr int64_t kTicksPerSecond = ::ASFW::Encoding::Motu::kTicksPerSecond;
    static constexpr int64_t kTicksPerCycle = ::ASFW::Encoding::Motu::kTicksPerCycle;

    [[nodiscard]] static MotuRxSphStepVerdict ClassifyStep(int64_t step,
                                                           uint32_t frames,
                                                           uint32_t sampleRateHz) noexcept {
        if (step <= 0) {
            return MotuRxSphStepVerdict::kNonMonotonic;
        }
        const int64_t measured = step * static_cast<int64_t>(sampleRateHz);
        const int64_t nominal = static_cast<int64_t>(frames) * kTicksPerSecond;
        const int64_t error = measured > nominal ? measured - nominal : nominal - measured;
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
};

} // namespace ASFW::Audio::Runtime
