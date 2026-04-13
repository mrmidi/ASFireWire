#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Isoch::Core {

inline constexpr uint64_t kExternalSyncLiveStaleNanos = 100'000'000ULL;
inline constexpr uint64_t kExternalSyncStartupSeedGraceNanos = 250'000'000ULL;

struct ExternalSyncBridge {
    static constexpr uint8_t kFdf48k = 0x02;
    static constexpr uint16_t kNoInfoSyt = 0xFFFF;

    struct TransportTimingSnapshot {
        bool valid{false};
        uint64_t anchorSampleFrame{0};
        uint64_t anchorHostTicks{0};
        uint32_t hostNanosPerSampleQ8{0};
        uint32_t seq{0};
    };

    // Shared state between IR producer and IT consumer.
    std::atomic<bool> active{false};
    std::atomic<bool> clockEstablished{false};
    std::atomic<bool> startupQualified{false};
    std::atomic<uint32_t> updateSeq{0};
    std::atomic<uint32_t> lastPackedRx{0};      // [SYT:16][FDF:8][DBS:8]
    std::atomic<uint64_t> lastUpdateHostTicks{0};
    std::atomic<bool> transportTimingValid{false};
    std::atomic<uint32_t> transportTimingSeq{0};
    std::atomic<uint64_t> transportAnchorSampleFrame{0};
    std::atomic<uint64_t> transportAnchorHostTicks{0};
    std::atomic<uint32_t> transportHostNanosPerSampleQ8{0};

    static constexpr uint32_t PackRxSample(uint16_t syt, uint8_t fdf, uint8_t dbs) noexcept {
        return (static_cast<uint32_t>(syt) << 16) |
               (static_cast<uint32_t>(fdf) << 8) |
               static_cast<uint32_t>(dbs);
    }

    static constexpr uint16_t UnpackSYT(uint32_t packed) noexcept {
        return static_cast<uint16_t>((packed >> 16) & 0xFFFFu);
    }

    static constexpr uint8_t UnpackFDF(uint32_t packed) noexcept {
        return static_cast<uint8_t>((packed >> 8) & 0xFFu);
    }

    static constexpr uint8_t UnpackDBS(uint32_t packed) noexcept {
        return static_cast<uint8_t>(packed & 0xFFu);
    }

    void PublishTransportTiming(uint64_t sampleFrame,
                                uint64_t hostTicks,
                                uint32_t hostNanosPerSampleQ8) noexcept {
        transportTimingSeq.fetch_add(1, std::memory_order_acq_rel); // odd = writer active
        transportAnchorSampleFrame.store(sampleFrame, std::memory_order_release);
        transportAnchorHostTicks.store(hostTicks, std::memory_order_release);
        transportHostNanosPerSampleQ8.store(hostNanosPerSampleQ8, std::memory_order_release);
        transportTimingValid.store(true, std::memory_order_release);
        transportTimingSeq.fetch_add(1, std::memory_order_acq_rel); // even = stable snapshot
    }

    [[nodiscard]] TransportTimingSnapshot ReadTransportTiming() const noexcept {
        for (;;) {
            const uint32_t seqBefore = transportTimingSeq.load(std::memory_order_acquire);
            if ((seqBefore & 1U) != 0) {
                continue;
            }

            TransportTimingSnapshot snapshot{
                .valid = transportTimingValid.load(std::memory_order_acquire),
                .anchorSampleFrame = transportAnchorSampleFrame.load(std::memory_order_acquire),
                .anchorHostTicks = transportAnchorHostTicks.load(std::memory_order_acquire),
                .hostNanosPerSampleQ8 = transportHostNanosPerSampleQ8.load(std::memory_order_acquire),
                .seq = seqBefore,
            };

            const uint32_t seqAfter = transportTimingSeq.load(std::memory_order_acquire);
            if (seqBefore == seqAfter) {
                return snapshot;
            }
        }
    }

    void Reset() noexcept {
        active.store(false, std::memory_order_release);
        clockEstablished.store(false, std::memory_order_release);
        startupQualified.store(false, std::memory_order_release);
        updateSeq.store(0, std::memory_order_release);
        lastPackedRx.store(0, std::memory_order_release);
        lastUpdateHostTicks.store(0, std::memory_order_release);
        transportTimingValid.store(false, std::memory_order_release);
        transportTimingSeq.store(0, std::memory_order_release);
        transportAnchorSampleFrame.store(0, std::memory_order_release);
        transportAnchorHostTicks.store(0, std::memory_order_release);
        transportHostNanosPerSampleQ8.store(0, std::memory_order_release);
    }
};

class ExternalSyncClockState {
public:
    static constexpr uint32_t kEstablishValidUpdates = 16;

    // Observe one RX CIP sample and publish it when valid for 48k sync tracking.
    // Returns true when establish threshold is reached and caller should log transition.
    // Caller must set bridge.clockEstablished after emitting transition log.
    // These arguments are ordered to match the RX packet fields consumed together.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool ObserveSample(ExternalSyncBridge& bridge,
                       uint64_t nowHostTicks, // NOLINT(bugprone-easily-swappable-parameters)
                       uint16_t syt,
                       uint8_t fdf,
                       uint8_t dbs,
                       uint32_t* outSeq = nullptr) noexcept {
        if (fdf != ExternalSyncBridge::kFdf48k) {
            consecutiveValid_ = 0;
            if (outSeq) *outSeq = 0;
            return false;
        }
        if (syt == ExternalSyncBridge::kNoInfoSyt) {
            // NO-DATA packets should not reset establishment progress.
            if (outSeq) *outSeq = 0;
            return false;
        }

        bridge.lastPackedRx.store(ExternalSyncBridge::PackRxSample(syt, fdf, dbs),
                                  std::memory_order_release);
        bridge.lastUpdateHostTicks.store(nowHostTicks, std::memory_order_release);
        const uint32_t seq = bridge.updateSeq.fetch_add(1, std::memory_order_acq_rel) + 1;

        if (outSeq) {
            *outSeq = seq;
        }

        if (consecutiveValid_ < kEstablishValidUpdates) {
            ++consecutiveValid_;
        }

        return (!bridge.clockEstablished.load(std::memory_order_acquire) &&
                consecutiveValid_ >= kEstablishValidUpdates);
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool HandleStale(ExternalSyncBridge& bridge,
                     uint64_t nowHostTicks, // NOLINT(bugprone-easily-swappable-parameters)
                     uint64_t staleThresholdHostTicks) noexcept {
        if (!bridge.active.load(std::memory_order_acquire)) {
            consecutiveValid_ = 0;
            return bridge.clockEstablished.exchange(false, std::memory_order_acq_rel);
        }

        if (staleThresholdHostTicks == 0) {
            return false;
        }

        const uint64_t last = bridge.lastUpdateHostTicks.load(std::memory_order_acquire);
        if (last == 0) {
            return false;
        }

        const uint64_t delta = nowHostTicks - last;
        if (delta > staleThresholdHostTicks) {
            consecutiveValid_ = 0;
            return bridge.clockEstablished.exchange(false, std::memory_order_acq_rel);
        }

        return false;
    }

    void Reset() noexcept {
        consecutiveValid_ = 0;
    }

    uint32_t ConsecutiveValid() const noexcept {
        return consecutiveValid_;
    }

private:
    uint32_t consecutiveValid_{0};
};

} // namespace ASFW::Isoch::Core
