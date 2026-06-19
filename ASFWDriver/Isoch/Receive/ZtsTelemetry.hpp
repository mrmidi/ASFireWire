#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Isoch::Rx {

// ZTS (zero-timestamp) clock telemetry.
//
// The HAL zero-timestamp anchor ties three clocks together, and diagnosing
// drift/jitter requires seeing all three at the moment each anchor is published:
//   - host monotonic clock (mach ticks): `drainHostTicks` (raw read at batch
//     drain) and `hostTicks` (the value actually published, interpolated onto
//     the P-frame grid),
//   - FireWire bus cycle timer: `drainCycleTimer` (host OHCI IsochronousCycleTimer
//     sampled at drain) and `rxCycleTimer` (the cycle timer recovered from the RX
//     DMA descriptor for the source packet),
//   - sample-frame domain: `sampleFrame`,
// plus the recovered device-SYT cadence and the per-packet SYT/lead.
//
// Capture happens in IsochReceiveContext::Poll(), which runs in the interrupt
// hot path. Logging there would perturb the very timing we are measuring, so the
// hot path only writes a fixed POD record into the ring below (O(1), no IO, no
// allocation). The watchdog drains and formats the records off the hot path.

enum class ZtsEventKind : uint8_t {
    kUpdate = 0,
    kSeed = 1,  // the first accepted publish after (re)establishing the cadence
};

struct ZtsTelemetryRecord final {
    uint64_t publishCount{0};         // rxZtsPublishCount_ at capture
    uint64_t sampleFrame{0};          // gridFrame: HAL zero-timestamp sample frame
    uint64_t hostTicks{0};            // gridHostTicks: published host clock (mach)
    uint64_t rawHostTicks{0};         // same observed packet receive time as hostTicks
    uint64_t drainHostTicks{0};       // mach uptime sampled once per batch drain
    int64_t  ageTicks{0};             // descriptor cycle timer vs drain reference
    int64_t  sytLeadTicks{0};         // device SYT presentation lead
    int64_t  recoveredPhaseTicks{0};  // RxSytCadence recovered device phase
    uint32_t rollingCadenceTicks{0};  // RxSytCadence rolling cadence sum
    uint32_t drainCycleTimer{0};      // host OHCI cycle timer at drain ("host cycle time")
    uint32_t rxCycleTimer{0};         // RX DMA descriptor cycle timer ("dma cycle time from rx")
    uint32_t descriptorIndex{0};
    uint32_t framesDecoded{0};
    uint32_t hostNanosPerSampleQ8{0}; // (1e9 << 8) / sampleRateHz — nominal rate scalar
    uint16_t rawRxTs{0};              // raw 16-bit descriptor SYT-domain timestamp
    uint16_t syt{0};                  // CIP SYT of the source packet
    uint8_t  kind{0};                 // ZtsEventKind
};

// Single-producer / single-consumer overwriting ring.
//
// In this dext the producer (IsochReceiveContext::Poll, reached from the
// interrupt or the watchdog) and the consumer (the watchdog drain) are
// serialized on the single Default dispatch queue, so they never actually run
// concurrently. The release/acquire on the write sequence still makes the
// hand-off well defined and keeps the producer side a plain O(1) store.
class ZtsTelemetryRing final {
public:
    static constexpr uint32_t kCapacity = 256;
    static_assert((kCapacity & (kCapacity - 1)) == 0, "capacity must be a power of two");

    void Reset() noexcept {
        writeSeq_.store(0, std::memory_order_relaxed);
        readSeq_ = 0;
    }

    // Producer (hot path): record one event. No IO, no allocation.
    void Record(const ZtsTelemetryRecord& rec) noexcept {
        const uint64_t seq = writeSeq_.load(std::memory_order_relaxed);
        entries_[seq & (kCapacity - 1)] = rec;
        writeSeq_.store(seq + 1, std::memory_order_release);
    }

    // Consumer (watchdog, off hot path): invoke `emit` for up to `maxEmit`
    // evenly-strided records spanning everything captured since the last drain,
    // plus every seed record (seeds never count against the cap), then mark the
    // whole span consumed. Returns the number of records lost to overflow since
    // the previous drain.
    template <typename EmitFn>
    uint64_t Drain(uint32_t maxEmit, EmitFn&& emit) noexcept {
        const uint64_t writeSeq = writeSeq_.load(std::memory_order_acquire);
        uint64_t dropped = 0;
        if (writeSeq - readSeq_ > kCapacity) {
            dropped = (writeSeq - readSeq_) - kCapacity;
            readSeq_ = writeSeq - kCapacity;  // only the surviving newest entries
        }

        const uint64_t available = writeSeq - readSeq_;
        if (available != 0 && maxEmit != 0) {
            const uint64_t stride = available > maxEmit ? (available / maxEmit) : 1;
            uint32_t emitted = 0;
            for (uint64_t seq = readSeq_; seq < writeSeq; ++seq) {
                const ZtsTelemetryRecord& rec = entries_[seq & (kCapacity - 1)];
                const bool isSeed =
                    rec.kind == static_cast<uint8_t>(ZtsEventKind::kSeed);
                const bool onStride =
                    emitted < maxEmit && ((seq - readSeq_) % stride) == 0;
                if (isSeed || onStride) {
                    emit(rec);
                    if (!isSeed) {
                        ++emitted;
                    }
                }
            }
        }

        readSeq_ = writeSeq;
        return dropped;
    }

private:
    std::array<ZtsTelemetryRecord, kCapacity> entries_{};
    std::atomic<uint64_t> writeSeq_{0};
    uint64_t readSeq_{0};  // consumer-only
};

} // namespace ASFW::Isoch::Rx
