#pragma once

#include <atomic>
#include <cstdint>

#include "../../../Common/TimingUtils.hpp"
#include "../../Wire/AMDTP/MotuV3WireFormat.hpp"

namespace ASFW::Audio::Runtime {

// Latest MOTU V3 TX presentation-time observation.  `firstSph` is the raw
// big-endian wire value emitted for the first audio block of `packetIndex`.
// The OUTPUT_LAST pair is a deliberately separate reference: it identifies
// the latest packet that hardware has actually completed when this packet was
// prepared.  Consumers must correlate by packet index, never assume they are
// the same packet.
struct MotuPhaseTraceSample final {
    uint64_t packetIndex{0};
    uint64_t outputLastPacketIndex{0};
    uint32_t firstSph{0};
    uint32_t outputLastCycleTimer{0};
    bool hasOutputLast{false};
};

// Diagnostic-only reconstruction of the TX presentation phase. The expected
// SPH follows the production seed projection: OUTPUT_LAST plus one nominal
// cycle per prepared packet and MOTU's captured three-cycle presentation lead.
struct MotuTxPhaseResidual final {
    bool valid{false};
    uint64_t packetLead{0};
    int64_t expectedSphTicks{0};
    int64_t actualSphTicks{0};
    int64_t residualTicks{0};
};

[[nodiscard]] constexpr int64_t NormalizeMotuTicks(int64_t ticks) noexcept {
    return static_cast<int64_t>(
        ASFW::Protocols::Audio::AMDTP::MotuV3Wire::NormalizeTicks(ticks));
}

// Shortest signed distance in MOTU's one-second SPH domain, so a step across a
// second rollover reads as the few thousand ticks it really is. While this was
// computed modulo eight seconds it produced a ~-24.6 M step once per second,
// which every plausibility gate downstream discarded.
[[nodiscard]] constexpr int64_t MotuShortestTickDifference(
    int64_t a, int64_t b) noexcept {
    constexpr int64_t kDomain = static_cast<int64_t>(
        ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kTickDomain);
    int64_t difference = (a - b) % kDomain;
    if (difference < 0) {
        difference += kDomain;
    }
    return difference > kDomain / 2 ? difference - kDomain : difference;
}

[[nodiscard]] inline MotuTxPhaseResidual ComputeMotuTxPhaseResidual(
    const MotuPhaseTraceSample& sample) noexcept {
    MotuTxPhaseResidual out{};
    if (!sample.hasOutputLast || sample.packetIndex < sample.outputLastPacketIndex) {
        return out;
    }
    out.packetLead = sample.packetIndex - sample.outputLastPacketIndex;
    const int64_t completedTicks = NormalizeMotuTicks(
        ASFW::Timing::encodedTstampToOffsets(sample.outputLastCycleTimer));
    out.expectedSphTicks = NormalizeMotuTicks(
        completedTicks + static_cast<int64_t>(out.packetLead) *
            static_cast<int64_t>(ASFW::Timing::kTicksPerCycle) +
        ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kPresentationLeadTicks);
    out.actualSphTicks = NormalizeMotuTicks(
        ASFW::Timing::encodedTstampToOffsets(sample.firstSph));
    out.residualTicks = MotuShortestTickDifference(
        out.actualSphTicks, out.expectedSphTicks);
    out.valid = true;
    return out;
}

// 48 kHz blocking cadence is N,D,D,D: three DATA packets close one period of
// four bus cycles (24 frames * 512 ticks == 4 * 3072 ticks). MOTU V3 is
// 48 kHz-only -- AmdtpTxPacketizer::Configure rejects every other rate for the
// packed layout -- so this is the only geometry that can reach the trace
// today. Any other rate needs its own value and its own measurement.
inline constexpr uint32_t kMotuPhaseTraceDataPacketStride48k = 3;

// Same number, different job, so it gets its own name: the stride above thins
// the publication rate, this one is how many DATA packets a cadence period
// holds and therefore how many residuals one mean is taken over.
inline constexpr uint32_t kMotuPhaseTraceCadenceBuckets = 3;

// The three DATA packets of one cadence period, sampled together.
//
// Publishing a single packet leaves the residual on one of the three
// cadence buckets, 1024 ticks apart, and WHICH bucket is set by where
// AlignFrameCursorOnce happened to leave the frame cursor -- arbitrary per
// stream start. Measured, not argued: driving the production packetizer with
// alignment cursors 0..1747 moves the gated level across -3072/-2048/-1024
// while the mean over any three CONSECUTIVE DATA packets stays -2048 for every
// one of them (MotuTxPhaseResidualQuantizationTests). The mean is therefore the
// alignment-invariant statistic, and it is also the convention the analysis of
// the official driver's bus capture uses -- it averages the same triple, so the
// two numbers are in one domain.
//
// One OUTPUT_LAST reference is shared by all three: the projection subtracts the
// cycles it adds, which is what ResidualIsInvariantToPacketLeadJitter pins, so
// reading the completion stamp once per publication rather than once per packet
// changes no residual and keeps the publisher's hot path at one stamp read.
struct MotuPhaseTraceCadenceSample final {
    uint64_t packetIndex[kMotuPhaseTraceCadenceBuckets]{};
    uint32_t firstSph[kMotuPhaseTraceCadenceBuckets]{};
    uint64_t outputLastPacketIndex{0};
    uint32_t outputLastCycleTimer{0};
    bool hasOutputLast{false};
    // How many of the buckets carry a packet. Below kMotuPhaseTraceCadenceBuckets
    // only at stream start, and the mean is not formed until all three are in.
    uint32_t count{0};
};

// Slide one DATA packet into the window, dropping the oldest. Lives here rather
// than in the publisher so the host tests pin the production shifting itself:
// the DriverKit slot provider that calls it cannot be built host-side.
inline void PushMotuPhaseTraceCadencePacket(MotuPhaseTraceCadenceSample& window,
                                            uint64_t packetIndex,
                                            uint32_t firstSph) noexcept {
    for (uint32_t bucket = 1; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
        window.packetIndex[bucket - 1] = window.packetIndex[bucket];
        window.firstSph[bucket - 1] = window.firstSph[bucket];
    }
    constexpr uint32_t newest = kMotuPhaseTraceCadenceBuckets - 1;
    window.packetIndex[newest] = packetIndex;
    window.firstSph[newest] = firstSph;
    if (window.count < kMotuPhaseTraceCadenceBuckets) {
        ++window.count;
    }
}

// The newest bucket as a plain sample, for the per-packet `[MotuPhase]` line
// that wants one packet's detail rather than a period statistic.
[[nodiscard]] inline MotuPhaseTraceSample NewestMotuPhaseTraceSample(
    const MotuPhaseTraceCadenceSample& period) noexcept {
    MotuPhaseTraceSample out{};
    if (period.count == 0) {
        return out;
    }
    const uint32_t newest = period.count - 1;
    out.packetIndex = period.packetIndex[newest];
    out.firstSph = period.firstSph[newest];
    out.outputLastPacketIndex = period.outputLastPacketIndex;
    out.outputLastCycleTimer = period.outputLastCycleTimer;
    out.hasOutputLast = period.hasOutputLast;
    return out;
}

[[nodiscard]] constexpr int64_t MotuDivideRoundedToNearest(int64_t numerator,
                                                           int64_t denominator) noexcept {
    const int64_t half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator
                          : (numerator - half) / denominator;
}

// The cadence-period mean residual: one number that does not depend on which
// bucket this stream's alignment landed on. Invalid unless every bucket is
// filled and every one of them yields a valid residual -- a partial mean would
// carry exactly the bias this exists to remove.
[[nodiscard]] inline MotuTxPhaseResidual ComputeMotuTxPhaseResidualCadenceMean(
    const MotuPhaseTraceCadenceSample& period) noexcept {
    MotuTxPhaseResidual out{};
    if (period.count != kMotuPhaseTraceCadenceBuckets) {
        return out;
    }

    int64_t residualSum = 0;
    uint64_t leadSum = 0;
    MotuTxPhaseResidual newest{};
    for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
        MotuPhaseTraceSample one{};
        one.packetIndex = period.packetIndex[bucket];
        one.firstSph = period.firstSph[bucket];
        one.outputLastPacketIndex = period.outputLastPacketIndex;
        one.outputLastCycleTimer = period.outputLastCycleTimer;
        one.hasOutputLast = period.hasOutputLast;

        const MotuTxPhaseResidual single = ComputeMotuTxPhaseResidual(one);
        if (!single.valid) {
            return {};
        }
        residualSum += single.residualTicks;
        leadSum += single.packetLead;
        newest = single;
    }

    constexpr int64_t kBuckets = static_cast<int64_t>(kMotuPhaseTraceCadenceBuckets);
    out.packetLead = leadSum / kMotuPhaseTraceCadenceBuckets;
    // Only the residual is averaged. `expected`/`actual` are absolute positions
    // in the one-second SPH domain, so a mean taken across the second rollover
    // would land half a domain away from every value it averages; the residual
    // is already a shortest signed difference of a few thousand ticks and
    // averages safely. These two therefore stay the NEWEST bucket's values and
    // remain what they always were: context for the diagnostic line, not a
    // period statistic.
    out.expectedSphTicks = newest.expectedSphTicks;
    out.actualSphTicks = newest.actualSphTicks;
    out.residualTicks = MotuDivideRoundedToNearest(residualSum, kBuckets);
    out.valid = true;
    return out;
}

// Two successive residuals are only comparable to each other once a whole
// blocking cadence period has closed. ComputeMotuTxPhaseResidual advances its
// EXPECTED SPH by kTicksPerCycle for every PREPARED packet -- packetLead counts
// the no-data ones too -- while the ACTUAL SPH advances only on DATA packets,
// by framesPerDataPacket * ticksPerFrame. In between, the residual walks the
// gcd of the two steps: 1024 ticks at 48 kHz, two audio frames. That lattice is
// what put the plateaus into the first hardware [RxPhaseRel] run;
// it is instrument geometry, not device behaviour, and both the lattice and
// this gate are pinned in MotuTxPhaseResidualQuantizationTests.
//
// Dividing the frame cursor by the packet's own frame count yields a DATA
// packet ordinal that increments by exactly one per DATA packet even when
// AlignFrameCursorOnce left the cursor off a packet multiple, so the gate keeps
// firing at a fixed cadence phase wherever alignment landed. Gating on the
// frame cursor directly (`% 24`) does not survive that case.
[[nodiscard]] constexpr bool MotuPhaseTraceSamplesThisPacket(
    uint64_t firstAudioFrame,
    uint32_t framesInPacket,
    uint32_t dataPacketStride) noexcept {
    if (dataPacketStride <= 1 || framesInPacket == 0) {
        return true;
    }
    return (firstAudioFrame / framesInPacket) % dataPacketStride == 0;
}

// RX SPH and its OHCI receive timestamp share the 24.576 MHz tick rate but not
// the same wrap: the SPH carries no seconds, the cycle timer counts to 128 s.
// Both NormalizeMotuTicks() calls below fold their argument into the SPH's
// one-second domain, which is what makes the subtraction mean anything -- read
// straight, the pair differs by whole seconds of the host clock. This is an
// observation, not a target: hardware data establishes its normal range.
[[nodiscard]] inline int64_t MotuRxSphMinusCycleTimerTicks(
    uint32_t sph, uint32_t cycleTimer) noexcept {
    return MotuShortestTickDifference(
        NormalizeMotuTicks(ASFW::Timing::encodedTstampToOffsets(sph)),
        NormalizeMotuTicks(
            ASFW::Timing::encodedTstampToOffsets(cycleTimer)));
}

class MotuPhaseTraceLatest final {
public:
    void Reset() noexcept {
        sequence_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
    }

    // One publication carries a whole cadence period, so a reader can form the
    // alignment-invariant mean from a single consistent snapshot rather than
    // stitching three independent reads of a latest-value bridge -- which it
    // could not do anyway, since it samples once per telemetry window.
    void Publish(const MotuPhaseTraceCadenceSample& period) noexcept {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
            packetIndex_[bucket].store(period.packetIndex[bucket],
                                       std::memory_order_relaxed);
            firstSph_[bucket].store(period.firstSph[bucket], std::memory_order_relaxed);
        }
        outputLastPacketIndex_.store(period.outputLastPacketIndex,
                                     std::memory_order_relaxed);
        outputLastCycleTimer_.store(period.outputLastCycleTimer,
                                    std::memory_order_relaxed);
        hasOutputLast_.store(period.hasOutputLast, std::memory_order_relaxed);
        count_.store(period.count, std::memory_order_relaxed);
        updates_.fetch_add(1, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] bool ReadLatest(MotuPhaseTraceCadenceSample& out,
                                  uint64_t& outUpdates) const noexcept {
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                continue;
            }
            MotuPhaseTraceCadenceSample period{};
            for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
                period.packetIndex[bucket] =
                    packetIndex_[bucket].load(std::memory_order_relaxed);
                period.firstSph[bucket] = firstSph_[bucket].load(std::memory_order_relaxed);
            }
            period.outputLastPacketIndex =
                outputLastPacketIndex_.load(std::memory_order_relaxed);
            period.outputLastCycleTimer =
                outputLastCycleTimer_.load(std::memory_order_relaxed);
            period.hasOutputLast = hasOutputLast_.load(std::memory_order_relaxed);
            period.count = count_.load(std::memory_order_relaxed);
            const uint64_t updates = updates_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                if (updates == 0) {
                    return false;
                }
                out = period;
                outUpdates = updates;
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> packetIndex_[kMotuPhaseTraceCadenceBuckets]{};
    std::atomic<uint32_t> firstSph_[kMotuPhaseTraceCadenceBuckets]{};
    std::atomic<uint64_t> outputLastPacketIndex_{0};
    std::atomic<uint32_t> outputLastCycleTimer_{0};
    std::atomic<bool> hasOutputLast_{false};
    std::atomic<uint32_t> count_{0};
    std::atomic<uint64_t> updates_{0};
};

// One conditioned `rel` sample, carried from the RX telemetry gate that forms
// it to the serialized TX producer that owns the controller.
struct MotuRelPhaseSampleRecord final {
    int64_t ticks{0};
    int64_t centerTicks{0};
    // The control block's LIVE generation at publication time. The reader
    // requires equality with its own, so a cached or stale value silently
    // disables the bridge for the whole stream.
    uint64_t streamGeneration{0};
};

// Same seqlock shape as MotuPhaseTraceLatest above, for the same reason: one
// publication is one consistent sample, and a torn read reports failure rather
// than a value stitched from two.
class MotuRelPhaseLatest final {
public:
    void Reset() noexcept {
        sequence_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
    }

    void Publish(const MotuRelPhaseSampleRecord& sample) noexcept {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
        ticks_.store(sample.ticks, std::memory_order_relaxed);
        centerTicks_.store(sample.centerTicks, std::memory_order_relaxed);
        streamGeneration_.store(sample.streamGeneration, std::memory_order_relaxed);
        updates_.fetch_add(1, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] bool ReadLatest(MotuRelPhaseSampleRecord& out,
                                  uint64_t& outUpdates) const noexcept {
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) {
                continue;
            }
            MotuRelPhaseSampleRecord sample{};
            sample.ticks = ticks_.load(std::memory_order_relaxed);
            sample.centerTicks = centerTicks_.load(std::memory_order_relaxed);
            sample.streamGeneration = streamGeneration_.load(std::memory_order_relaxed);
            const uint64_t updates = updates_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                if (updates == 0) {
                    return false;
                }
                out = sample;
                outUpdates = updates;
                return true;
            }
        }
        return false;
    }

private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<int64_t> ticks_{0};
    std::atomic<int64_t> centerTicks_{0};
    std::atomic<uint64_t> streamGeneration_{0};
    std::atomic<uint64_t> updates_{0};
};

} // namespace ASFW::Audio::Runtime
