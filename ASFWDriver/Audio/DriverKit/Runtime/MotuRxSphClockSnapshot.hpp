// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRxSphClockSnapshot.hpp - latest-value bridge carrying the MOTU device
// clock, as measured from received SPH, from the RX side to the TX SPH stamper.
// Common to every MOTU protocol version: the SPH format is the same.

#pragma once

#include "../../Runtime/Seqlock.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// Cumulative RX clock observation in the MOTU 24.576 MHz SPH domain: rxTicks
// device ticks elapsed over rxFrames received frames since the stream start.
// `streamGeneration` is the AudioTransportControlBlock start generation, not a
// publication counter; a consumer must rebase when it changes. firstAudioFrame
// identifies the frame carrying sphTicks. replayEpoch protects cursor rebinding;
// TX projects by frame index, never by a guessed number of cadence packets.
struct MotuRxSphClockSample final {
    uint64_t streamGeneration{0};
    uint64_t rxFrames{0};
    int64_t rxTicks{0};
    uint64_t firstAudioFrame{0};
    uint32_t sphTicks{0}, replayEpoch{0};
};

// Single writer (the master RX stream's timing observer), single reader (the
// serialized TX producer). Readers may miss intermediate publications but never
// accept fields from two different observations (Seqlock.hpp).
//
// Reset() runs from AudioTransportControlBlock::ResetForStart while the isoch
// contexts are quiesced, so it has no concurrent writer. It arms the bridge for
// one start generation; a publication stamped with any other generation (a
// late writer from the previous stream) is refused.
class MotuRxSphClockLatest final {
public:
    void Reset(uint64_t streamGeneration) noexcept {
        SeqlockWriteBegin(sequence_);
        streamGeneration_.store(streamGeneration, std::memory_order_relaxed);
        rxFrames_.store(0, std::memory_order_relaxed);
        rxTicks_.store(0, std::memory_order_relaxed);
        firstAudioFrame_.store(0, std::memory_order_relaxed);
        sphTicks_.store(0, std::memory_order_relaxed);
        replayEpoch_.store(0, std::memory_order_relaxed);
        updates_.store(0, std::memory_order_relaxed);
        SeqlockWriteEnd(sequence_);
    }

    // Accepts only a sample of the armed generation that moves both counters
    // strictly forward: the measurement is cumulative, so anything else is a
    // stale or reordered observation.
    [[nodiscard]] bool Publish(const MotuRxSphClockSample& sample) noexcept {
        if (sample.streamGeneration == 0 || sample.rxFrames == 0 || sample.rxTicks <= 0 ||
            streamGeneration_.load(std::memory_order_relaxed) != sample.streamGeneration) {
            return false;
        }
        const uint64_t updates = updates_.load(std::memory_order_relaxed);
        if (updates != 0 && (sample.rxFrames <= rxFrames_.load(std::memory_order_relaxed) ||
                             sample.rxTicks <= rxTicks_.load(std::memory_order_relaxed))) {
            return false;
        }
        SeqlockWriteBegin(sequence_);
        rxFrames_.store(sample.rxFrames, std::memory_order_relaxed);
        rxTicks_.store(sample.rxTicks, std::memory_order_relaxed);
        firstAudioFrame_.store(sample.firstAudioFrame, std::memory_order_relaxed);
        sphTicks_.store(sample.sphTicks, std::memory_order_relaxed);
        replayEpoch_.store(sample.replayEpoch, std::memory_order_relaxed);
        updates_.store(updates + 1, std::memory_order_relaxed);
        SeqlockWriteEnd(sequence_);
        return true;
    }

    // False until the armed generation has its first publication, or on a torn
    // read. outUpdates counts publications since the last Reset().
    [[nodiscard]] bool ReadLatest(MotuRxSphClockSample& out,
                                  uint64_t& outUpdates) const noexcept {
        MotuRxSphClockSample sample{};
        uint64_t updates = 0;
        const auto accepted = SeqlockTryRead(sequence_, [&] {
            sample.streamGeneration = streamGeneration_.load(std::memory_order_relaxed);
            sample.rxFrames = rxFrames_.load(std::memory_order_relaxed);
            sample.rxTicks = rxTicks_.load(std::memory_order_relaxed);
            sample.firstAudioFrame = firstAudioFrame_.load(std::memory_order_relaxed);
            sample.sphTicks = sphTicks_.load(std::memory_order_relaxed);
            sample.replayEpoch = replayEpoch_.load(std::memory_order_relaxed);
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
    std::atomic<uint64_t> rxFrames_{0};
    std::atomic<int64_t> rxTicks_{0};
    std::atomic<uint64_t> firstAudioFrame_{0};
    std::atomic<uint32_t> sphTicks_{0}, replayEpoch_{0};
    std::atomic<uint64_t> updates_{0};
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the RX-to-TX SPH clock bridge must stay lock-free");
static_assert(std::atomic<int64_t>::is_always_lock_free,
              "the RX-to-TX SPH clock bridge must stay lock-free");

} // namespace ASFW::Audio::Runtime
