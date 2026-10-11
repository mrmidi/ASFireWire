// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPhaseTrace.hpp - the two latest-value bridges that close the MOTU TX
// presentation-phase loop through the transport control block:
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

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// At 48 kHz the blocking cadence is N,D,D,D: three DATA packets of eight
// frames close one period of four bus cycles (24 frames * 512 ticks ==
// 4 * 3072 ticks). One publication carries one whole period, so a reader can
// average over the three DATA packets and get a value that does not depend on
// which of them the stream's frame alignment put first.
inline constexpr uint32_t kMotuPhaseTraceCadenceBuckets = 3;

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
};

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
