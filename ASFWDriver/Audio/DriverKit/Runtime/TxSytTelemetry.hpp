#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

// Isoch-Transmit SYT-resync telemetry.
//
// Captures, per TX data-packet decision, the full input/state/output of
// TxTimingModel::PeekNextDataSyt + AdjustOutputPhase so the resync can be
// replayed and verified offline (tools/zts_sim.py). The fields are exactly the
// servo's operands:
//   inputs : packetAnchorTicks (OHCI exec cycle), recoveredPhaseTicks +
//            rxPhaseDelayFree (servo target), rollingCadenceTicks,
//            pendingCadenceTicks (the per-entry SYT delta consumed) + readIndex,
//   state  : phaseTicksPre/Post (carried phase across AdjustOutputPhase),
//            phaseError/frameError/correctionTicks, flags (seed/force/reseed/commit),
//   output : syt, leadTicks, wireLeadTicks, health.
//
// Capture happens on the audio driver's TX-preparation dispatch; the record is a
// plain POD store (no IO). The main-driver watchdog drains it off that path via
// the shared AudioTransportControlBlock. Mirrors Isoch/Receive/ZtsTelemetry.hpp.

struct TxSytTelemetryRecord final {
    uint64_t packetIndex{0};
    int64_t  packetAnchorTicks{0};
    int64_t  phaseTicksPre{0};
    int64_t  phaseTicksPost{0};
    int64_t  recoveredPhaseTicks{0};
    int64_t  rxPhaseDelayFree{0};
    int64_t  phaseError{0};
    int64_t  frameError{0};
    int64_t  correctionTicks{0};
    int64_t  leadTicks{0};
    int64_t  wireLeadTicks{0};
    uint32_t rollingCadenceTicks{0};
    uint16_t pendingCadenceTicks{0};
    uint16_t cadenceReadIndex{0};
    uint16_t syt{0};
    uint8_t  health{0};
    uint8_t  flags{0};   // bit0 seeded, bit1 forceAdjust, bit2 reseeded, bit3 committed
    int64_t  targetFromZts{0};    // lastZtsFrame + deltaFrames - contentLead
    int64_t  targetFromDevice{0}; // rxDeviceFrame + deltaFrames - contentLead
    int64_t  targetFrameDiff{0};  // targetFromZts - targetFromDevice
};

namespace TxSytFlags {
inline constexpr uint8_t kSeeded = 0x1;
inline constexpr uint8_t kForceAdjust = 0x2;
inline constexpr uint8_t kReseeded = 0x4;
inline constexpr uint8_t kCommitted = 0x8;
}  // namespace TxSytFlags

// Single-producer (TX prep dispatch) / single-consumer (main-driver watchdog)
// overwriting ring. The two run on different dispatch queues here, so unlike the
// ZTS ring this one genuinely crosses queues; the release/acquire on the write
// sequence makes that hand-off well defined. Capture stays O(1), no IO.
class TxSytTelemetryRing final {
public:
    // TX data-packet decisions arrive ~6000/s (vs the ZTS grid's 250/s), so the
    // ring must hold more than one drain window; 512 covers ~85 ms at 48 kHz.
    static constexpr uint32_t kCapacity = 512;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

    void Reset() noexcept {
        writeSeq_.store(0, std::memory_order_relaxed);
        readSeq_ = 0;
    }

    void Record(const TxSytTelemetryRecord& rec) noexcept {
        const uint64_t seq = writeSeq_.load(std::memory_order_relaxed);
        entries_[seq & (kCapacity - 1)] = rec;
        writeSeq_.store(seq + 1, std::memory_order_release);
    }

    // Emit the most-recent `maxEmit` records as a CONSECUTIVE run (so the resync
    // phase chain phasePost[N] -> phasePre[N+1] is replayable offline), plus
    // every seed/reseed record anywhere in the span (resync events are never
    // sampled out), then consume the whole span. Records between the span start
    // and the recent run are intentionally not emitted (burst sampling, not a
    // loss). Returns only true overflow drops (producer lapped the ring).
    template <typename EmitFn>
    uint64_t Drain(uint32_t maxEmit, EmitFn&& emit) noexcept {
        const uint64_t writeSeq = writeSeq_.load(std::memory_order_acquire);
        uint64_t dropped = 0;
        if (writeSeq - readSeq_ > kCapacity) {
            dropped = (writeSeq - readSeq_) - kCapacity;
            readSeq_ = writeSeq - kCapacity;
        }

        const uint64_t available = writeSeq - readSeq_;
        if (available != 0 && maxEmit != 0) {
            const uint64_t recentStart =
                available > maxEmit ? (writeSeq - maxEmit) : readSeq_;
            for (uint64_t seq = readSeq_; seq < writeSeq; ++seq) {
                const TxSytTelemetryRecord& rec = entries_[seq & (kCapacity - 1)];
                const bool special =
                    (rec.flags & (TxSytFlags::kSeeded | TxSytFlags::kReseeded)) != 0;
                if (special || seq >= recentStart) {
                    emit(rec);
                }
            }
        }

        readSeq_ = writeSeq;
        return dropped;
    }

private:
    std::array<TxSytTelemetryRecord, kCapacity> entries_{};
    std::atomic<uint64_t> writeSeq_{0};
    uint64_t readSeq_{0};
};

} // namespace ASFW::Audio::Runtime
