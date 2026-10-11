// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPhaseTrace.hpp - the two latest-value bridges that close the MOTU TX
// presentation-phase loop through the transport control block, and the
// arithmetic that turns what they carry into a phase:
//
//   motuPhaseTrace  TX -> RX telemetry: the SPH the host released for the DATA
//                   packets of one cadence period, with the latest completed
//                   (OUTPUT_LAST) packet as the bus-time reference.
//   motuRelPhase    RX telemetry -> TX: the conditioned phase of that TX
//                   presentation against the device's own RX phase.
//
// Both are generation-stamped like motuRxSphClock: ResetForStart arms them with
// the start generation, and a publication carrying any other generation is
// refused. The SPH format is common to every MOTU protocol version.

#pragma once

#include "../../Runtime/Seqlock.hpp"
#include "../../Wire/MOTU/MotuSph.hpp"
#include "../../../Common/TimingUtils.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// At 48 kHz the blocking cadence is N,D,D,D: three DATA packets of eight
// frames close one period of four bus cycles (24 frames * 512 ticks ==
// 4 * 3072 ticks). One publication carries one whole period, so a reader can
// average over the three DATA packets and get a value that does not depend on
// which of them the stream's frame alignment put first.
inline constexpr uint32_t kMotuPhaseTraceCadenceBuckets = 3;

// How often the publisher publishes, in DATA packets: once per cadence period.
// The same number as the buckets, with a different job -- this one thins the
// publication rate, the buckets are what one mean is taken over.
inline constexpr uint32_t kMotuPhaseTraceDataPacketStride = kMotuPhaseTraceCadenceBuckets;

// Where the phase arithmetic below holds: the 48 kHz family. There every DATA
// packet carries 8, 16 or 32 frames of 512, 256 or 128 ticks -- 4096 ticks of
// SPH -- against 3072 ticks per bus cycle, and the blocking cadence closes one
// period in three DATA packets over four cycles. The 44.1 kHz family has no
// integer ticks per frame and 32 kHz another cadence; no `rel` is formed there.
[[nodiscard]] constexpr bool MotuRelPhaseGeometryHolds(uint32_t sampleRateHz) noexcept {
    return sampleRateHz == 48'000 || sampleRateHz == 96'000 || sampleRateHz == 192'000;
}

// The DATA packets of one cadence period, oldest first, as released. firstSph
// is the raw big-endian wire SPH of each packet's first block. The OUTPUT_LAST
// pair is a deliberately separate reference: the newest packet the hardware had
// completed when the period was published. Consumers correlate by packet
// index; they must not assume it is one of the period's packets.
struct MotuPhaseTraceCadenceSample final {
    uint64_t streamGeneration{0};
    uint64_t packetIndex[kMotuPhaseTraceCadenceBuckets]{};
    uint32_t firstSph[kMotuPhaseTraceCadenceBuckets]{};
    uint64_t outputLastPacketIndex{0};
    uint32_t outputLastCycleTimer{0};
    bool hasOutputLast{false};
    // How many buckets carry a packet; below kMotuPhaseTraceCadenceBuckets only
    // at stream start.
    uint32_t count{0};
    // The model's presentation lead the stream was stamped with
    // (RuntimePolicy::presentationLeadTicks): the projection below adds it, so
    // the residual reads zero for a packet stamped exactly at its transmit
    // cycle plus the lead.
    uint32_t presentationLeadTicks{0};
};

// One released DATA packet against the same OUTPUT_LAST reference.
struct MotuPhaseTraceSample final {
    uint64_t packetIndex{0};
    uint64_t outputLastPacketIndex{0};
    uint32_t firstSph{0};
    uint32_t outputLastCycleTimer{0};
    uint32_t presentationLeadTicks{0};
    bool hasOutputLast{false};
};

// The transmit presentation phase of a released packet: its SPH against the
// SPH it would carry if it were stamped exactly at its transmit cycle plus the
// lead. The transmit cycle is projected from the newest completed packet, one
// cycle per prepared packet after it.
struct MotuTxPhaseResidual final {
    bool valid{false};
    uint64_t packetLead{0};
    int64_t expectedSphTicks{0};
    int64_t actualSphTicks{0};
    int64_t residualTicks{0};
};

// A cycle timer on the SPH's one-second timeline: the seconds dropped, so the
// two can be subtracted.
[[nodiscard]] inline int64_t MotuTickFromCycleTimer(uint32_t cycleTimer) noexcept {
    return ::ASFW::Encoding::Motu::WrapTick(::ASFW::Timing::encodedTstampToOffsets(cycleTimer));
}

[[nodiscard]] inline int64_t MotuTickFromSphQuadlet(uint32_t sph) noexcept {
    return ::ASFW::Encoding::Motu::WrapTick(::ASFW::Encoding::Motu::TickFromSph(sph));
}

[[nodiscard]] inline MotuTxPhaseResidual ComputeMotuTxPhaseResidual(
    const MotuPhaseTraceSample& sample) noexcept {
    MotuTxPhaseResidual out{};
    if (!sample.hasOutputLast || sample.packetIndex < sample.outputLastPacketIndex) {
        return out;
    }
    out.packetLead = sample.packetIndex - sample.outputLastPacketIndex;
    const int64_t completedTicks = MotuTickFromCycleTimer(sample.outputLastCycleTimer);
    out.expectedSphTicks = ::ASFW::Encoding::Motu::WrapTick(
        completedTicks +
        static_cast<int64_t>(out.packetLead) * ::ASFW::Encoding::Motu::kTicksPerCycle +
        static_cast<int64_t>(sample.presentationLeadTicks));
    out.actualSphTicks = MotuTickFromSphQuadlet(sample.firstSph);
    out.residualTicks =
        ::ASFW::Encoding::Motu::TickDifference(out.actualSphTicks, out.expectedSphTicks);
    out.valid = true;
    return out;
}

// Slide one DATA packet into the window, dropping the oldest. Here rather than
// in the publisher so the host tests pin the production shifting itself.
inline void PushMotuPhaseTraceCadencePacket(MotuPhaseTraceCadenceSample& window,
                                            uint64_t packetIndex,
                                            uint32_t firstSph) noexcept {
    for (uint32_t bucket = 1; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
        window.packetIndex[bucket - 1] = window.packetIndex[bucket];
        window.firstSph[bucket - 1] = window.firstSph[bucket];
    }
    constexpr uint32_t kNewest = kMotuPhaseTraceCadenceBuckets - 1;
    window.packetIndex[kNewest] = packetIndex;
    window.firstSph[kNewest] = firstSph;
    if (window.count < kMotuPhaseTraceCadenceBuckets) {
        ++window.count;
    }
}

[[nodiscard]] constexpr int64_t MotuDivideRoundedToNearest(int64_t numerator,
                                                           int64_t denominator) noexcept {
    const int64_t half = denominator / 2;
    return numerator >= 0 ? (numerator + half) / denominator : (numerator - half) / denominator;
}

// The residual of one cadence period, averaged over its three DATA packets.
//
// A single packet's residual sits on one of three levels 1024 ticks apart
// (two frames at 48 kHz): the projection advances 3072 ticks per prepared
// packet, NO-DATA included, while the SPH advances 4096 per DATA packet, so
// between period boundaries the two walk the lattice gcd(4096, 3072). Which
// level a fixed-stride sample lands on depends on where the stream's frame
// alignment put the cursor, which is arbitrary per start. The mean over any
// three consecutive DATA packets does not depend on it
// (MotuTxPhaseResidualQuantizationTests).
//
// Invalid unless every bucket is filled and yields a residual: a partial mean
// carries exactly the bias this removes. `expected`/`actual` stay the newest
// bucket's, since a mean of absolute positions across the second rollover
// would land half a second away; only the residual, a short signed
// difference, is averaged.
[[nodiscard]] inline MotuTxPhaseResidual ComputeMotuTxPhaseResidualCadenceMean(
    const MotuPhaseTraceCadenceSample& period) noexcept {
    if (period.count != kMotuPhaseTraceCadenceBuckets) {
        return {};
    }

    int64_t residualSum = 0;
    uint64_t leadSum = 0;
    MotuTxPhaseResidual newest{};
    for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
        const MotuTxPhaseResidual single = ComputeMotuTxPhaseResidual({
            .packetIndex = period.packetIndex[bucket],
            .outputLastPacketIndex = period.outputLastPacketIndex,
            .firstSph = period.firstSph[bucket],
            .outputLastCycleTimer = period.outputLastCycleTimer,
            .presentationLeadTicks = period.presentationLeadTicks,
            .hasOutputLast = period.hasOutputLast,
        });
        if (!single.valid) {
            return {};
        }
        residualSum += single.residualTicks;
        leadSum += single.packetLead;
        newest = single;
    }

    MotuTxPhaseResidual out = newest;
    out.packetLead = leadSum / kMotuPhaseTraceCadenceBuckets;
    out.residualTicks = MotuDivideRoundedToNearest(
        residualSum, static_cast<int64_t>(kMotuPhaseTraceCadenceBuckets));
    return out;
}

// The publication gate: true on one DATA packet per `dataPacketStride`.
// Dividing the frame cursor by the packet's own frame count gives a DATA packet
// ordinal that steps by exactly one per DATA packet even where the start's
// alignment left the cursor off a packet multiple; gating on the frame cursor
// itself (`% 24`) would then never fire.
[[nodiscard]] constexpr bool MotuPhaseTraceSamplesThisPacket(uint64_t firstAudioFrame,
                                                             uint32_t framesInPacket,
                                                             uint32_t dataPacketStride) noexcept {
    if (dataPacketStride <= 1 || framesInPacket == 0) {
        return true;
    }
    return (firstAudioFrame / framesInPacket) % dataPacketStride == 0;
}

// The device's absolute receive phase: its SPH against the cycle the packet
// was received in. Both run at 24.576 MHz but wrap differently -- the SPH
// carries no seconds, the cycle timer counts to 128 s -- so both are folded
// into the SPH's one-second timeline before the shortest signed difference.
[[nodiscard]] inline int64_t MotuRxSphMinusCycleTimerTicks(uint32_t sph,
                                                           uint32_t cycleTimer) noexcept {
    return ::ASFW::Encoding::Motu::TickDifference(MotuTickFromSphQuadlet(sph),
                                                  MotuTickFromCycleTimer(cycleTimer));
}

// Single writer (the TX preparation queue, at packet release), single reader
// (the RX telemetry drain). A torn read reports failure rather than a period
// stitched from two publications (Seqlock.hpp).
class MotuPhaseTraceLatest final {
public:
    void Reset(uint64_t streamGeneration) noexcept {
        SeqlockWriteBegin(sequence_);
        streamGeneration_.store(streamGeneration, std::memory_order_relaxed);
        for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
            packetIndex_[bucket].store(0, std::memory_order_relaxed);
            firstSph_[bucket].store(0, std::memory_order_relaxed);
        }
        outputLastPacketIndex_.store(0, std::memory_order_relaxed);
        outputLastCycleTimer_.store(0, std::memory_order_relaxed);
        hasOutputLast_.store(false, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        presentationLeadTicks_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
        SeqlockWriteEnd(sequence_);
    }

    [[nodiscard]] bool Publish(const MotuPhaseTraceCadenceSample& period) noexcept {
        if (period.streamGeneration == 0 || period.count > kMotuPhaseTraceCadenceBuckets ||
            streamGeneration_.load(std::memory_order_relaxed) != period.streamGeneration) {
            return false;
        }
        SeqlockWriteBegin(sequence_);
        for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
            packetIndex_[bucket].store(period.packetIndex[bucket], std::memory_order_relaxed);
            firstSph_[bucket].store(period.firstSph[bucket], std::memory_order_relaxed);
        }
        outputLastPacketIndex_.store(period.outputLastPacketIndex, std::memory_order_relaxed);
        outputLastCycleTimer_.store(period.outputLastCycleTimer, std::memory_order_relaxed);
        hasOutputLast_.store(period.hasOutputLast, std::memory_order_relaxed);
        count_.store(period.count, std::memory_order_relaxed);
        presentationLeadTicks_.store(period.presentationLeadTicks, std::memory_order_relaxed);
        updates_.store(updates_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        SeqlockWriteEnd(sequence_);
        return true;
    }

    // False until the armed generation has its first publication, or on a torn
    // read. outUpdates counts publications since the last Reset().
    [[nodiscard]] bool ReadLatest(MotuPhaseTraceCadenceSample& out,
                                  uint64_t& outUpdates) const noexcept {
        MotuPhaseTraceCadenceSample period{};
        uint64_t updates = 0;
        const auto accepted = SeqlockTryRead(sequence_, [&] {
            period.streamGeneration = streamGeneration_.load(std::memory_order_relaxed);
            for (uint32_t bucket = 0; bucket < kMotuPhaseTraceCadenceBuckets; ++bucket) {
                period.packetIndex[bucket] = packetIndex_[bucket].load(std::memory_order_relaxed);
                period.firstSph[bucket] = firstSph_[bucket].load(std::memory_order_relaxed);
            }
            period.outputLastPacketIndex = outputLastPacketIndex_.load(std::memory_order_relaxed);
            period.outputLastCycleTimer = outputLastCycleTimer_.load(std::memory_order_relaxed);
            period.hasOutputLast = hasOutputLast_.load(std::memory_order_relaxed);
            period.count = count_.load(std::memory_order_relaxed);
            period.presentationLeadTicks = presentationLeadTicks_.load(std::memory_order_relaxed);
            updates = updates_.load(std::memory_order_relaxed);
        });
        if (!accepted || updates == 0) {
            return false;
        }
        out = period;
        outUpdates = updates;
        return true;
    }

private:
    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> streamGeneration_{0};
    std::atomic<uint64_t> packetIndex_[kMotuPhaseTraceCadenceBuckets]{};
    std::atomic<uint32_t> firstSph_[kMotuPhaseTraceCadenceBuckets]{};
    std::atomic<uint64_t> outputLastPacketIndex_{0};
    std::atomic<uint32_t> outputLastCycleTimer_{0};
    std::atomic<bool> hasOutputLast_{false};
    std::atomic<uint32_t> count_{0};
    std::atomic<uint32_t> presentationLeadTicks_{0};
    std::atomic<uint64_t> updates_{0};
};

// One conditioned `rel` sample: the TX presentation phase relative to the
// device's RX phase, in 24.576 MHz ticks, and the conditioner's operating
// point at the time (the center a consumer folds the sample around).
struct MotuRelPhaseSampleRecord final {
    uint64_t streamGeneration{0};
    int64_t ticks{0};
    int64_t centerTicks{0};
};

// Single writer (the RX telemetry drain), single reader (the serialized TX
// producer). A separate bridge rather than extra fields on motuRxSphClock:
// `rel` is formed once per telemetry window while the clock sample is published
// far more often, and merging them would either throttle the clock or stamp
// `rel` onto windows that never measured it.
class MotuRelPhaseLatest final {
public:
    void Reset(uint64_t streamGeneration) noexcept {
        SeqlockWriteBegin(sequence_);
        streamGeneration_.store(streamGeneration, std::memory_order_relaxed);
        ticks_.store(0, std::memory_order_relaxed);
        centerTicks_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
        SeqlockWriteEnd(sequence_);
    }

    [[nodiscard]] bool Publish(const MotuRelPhaseSampleRecord& sample) noexcept {
        if (sample.streamGeneration == 0 ||
            streamGeneration_.load(std::memory_order_relaxed) != sample.streamGeneration) {
            return false;
        }
        SeqlockWriteBegin(sequence_);
        ticks_.store(sample.ticks, std::memory_order_relaxed);
        centerTicks_.store(sample.centerTicks, std::memory_order_relaxed);
        updates_.store(updates_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        SeqlockWriteEnd(sequence_);
        return true;
    }

    // False until the armed generation has its first publication, or on a torn
    // read. outUpdates counts publications since the last Reset().
    [[nodiscard]] bool ReadLatest(MotuRelPhaseSampleRecord& out,
                                  uint64_t& outUpdates) const noexcept {
        MotuRelPhaseSampleRecord sample{};
        uint64_t updates = 0;
        const auto accepted = SeqlockTryRead(sequence_, [&] {
            sample.streamGeneration = streamGeneration_.load(std::memory_order_relaxed);
            sample.ticks = ticks_.load(std::memory_order_relaxed);
            sample.centerTicks = centerTicks_.load(std::memory_order_relaxed);
            updates = updates_.load(std::memory_order_relaxed);
        });
        if (!accepted || updates == 0) {
            return false;
        }
        out = sample;
        outUpdates = updates;
        return true;
    }

private:
    std::atomic<uint64_t> sequence_{0};
    std::atomic<uint64_t> streamGeneration_{0};
    std::atomic<int64_t> ticks_{0};
    std::atomic<int64_t> centerTicks_{0};
    std::atomic<uint64_t> updates_{0};
};

} // namespace ASFW::Audio::Runtime
