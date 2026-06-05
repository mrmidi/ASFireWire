#pragma once

#include "../../../AudioWire/AMDTP/TimingUtils.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::AudioEngine::DirectIsoch {

inline constexpr uint64_t kExternalSyncLiveStaleNanos = 100'000'000ULL;
inline constexpr uint64_t kExternalSyncStartupSeedGraceNanos = 250'000'000ULL;

struct RxCadenceSnapshot final {
    bool established{false};
    uint32_t writeIndex{0};
    uint32_t warmupCount{0};
    int64_t recoveredDeviceOffsetTicks{0};
    uint32_t seq{0};
};

struct ExternalSyncBridge {
    static constexpr uint8_t kFdf48k = 0x02;
    static constexpr uint16_t kNoInfoSyt = 0xFFFF;
    static constexpr uint32_t kCadenceRingSize = 512;
    static constexpr uint32_t kCadenceRingMask = kCadenceRingSize - 1;
    static constexpr uint32_t kCadenceEstablishedUpdates = kCadenceRingSize;

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
    std::array<std::atomic<uint16_t>, kCadenceRingSize> cadenceRing{};
    std::atomic<uint32_t> cadenceWriteIndex{0};
    std::atomic<uint32_t> cadenceWarmupCount{0};
    std::atomic<bool> cadenceEstablished{false};
    std::atomic<int64_t> recoveredDeviceOffsetTicks{0};
    std::atomic<uint32_t> recoveredDeviceSeq{0};

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

    void PublishCadenceDelta(uint16_t deltaTicks, int64_t recoveredOffsetTicks) noexcept {
        const uint32_t writeIndex = cadenceWriteIndex.load(std::memory_order_relaxed) & kCadenceRingMask;
        cadenceRing[writeIndex].store(deltaTicks, std::memory_order_release);
        cadenceWriteIndex.store((writeIndex + 1) & kCadenceRingMask, std::memory_order_release);

        uint32_t warmup = cadenceWarmupCount.load(std::memory_order_relaxed);
        if (warmup < kCadenceRingSize) {
            warmup += 1;
            cadenceWarmupCount.store(warmup, std::memory_order_release);
        }
        if (warmup >= kCadenceEstablishedUpdates) {
            cadenceEstablished.store(true, std::memory_order_release);
        }

        recoveredDeviceOffsetTicks.store(ASFW::Timing::normalizeOffsetDomain(recoveredOffsetTicks),
                                         std::memory_order_release);
        recoveredDeviceSeq.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] uint16_t ReadCadenceDelta(uint32_t index) const noexcept {
        const uint16_t delta = cadenceRing[index & kCadenceRingMask].load(std::memory_order_acquire);
        return delta == 0 ? static_cast<uint16_t>(ASFW::Timing::kSytPacketStepTicks48k) : delta;
    }

    [[nodiscard]] RxCadenceSnapshot ReadCadenceSnapshot() const noexcept;

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
        for (auto& entry : cadenceRing) {
            entry.store(0, std::memory_order_release);
        }
        cadenceWriteIndex.store(0, std::memory_order_release);
        cadenceWarmupCount.store(0, std::memory_order_release);
        cadenceEstablished.store(false, std::memory_order_release);
        recoveredDeviceOffsetTicks.store(0, std::memory_order_release);
        recoveredDeviceSeq.store(0, std::memory_order_release);
    }
};

inline RxCadenceSnapshot ExternalSyncBridge::ReadCadenceSnapshot() const noexcept {
    return RxCadenceSnapshot{
        .established = cadenceEstablished.load(std::memory_order_acquire),
        .writeIndex = cadenceWriteIndex.load(std::memory_order_acquire),
        .warmupCount = cadenceWarmupCount.load(std::memory_order_acquire),
        .recoveredDeviceOffsetTicks = recoveredDeviceOffsetTicks.load(std::memory_order_acquire),
        .seq = recoveredDeviceSeq.load(std::memory_order_acquire),
    };
}

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
            previousSyt_ = ExternalSyncBridge::kNoInfoSyt;
            previousSytValid_ = false;
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

        if (!previousSytValid_) {
            previousSyt_ = syt;
            previousSytValid_ = true;
            recoveredDeviceOffsetTicks_ = ASFW::Timing::sytToFieldTicks(syt);
            bridge.recoveredDeviceOffsetTicks.store(recoveredDeviceOffsetTicks_,
                                                    std::memory_order_release);
            bridge.recoveredDeviceSeq.fetch_add(1, std::memory_order_acq_rel);
        } else {
            const int64_t diff = ASFW::Timing::SYTDiffInOffsets(syt, previousSyt_);
            if (diff > 0 && diff <= 0xFFFF) {
                recoveredDeviceOffsetTicks_ =
                    ASFW::Timing::normalizeOffsetDomain(recoveredDeviceOffsetTicks_ + diff);
                bridge.PublishCadenceDelta(static_cast<uint16_t>(diff),
                                           recoveredDeviceOffsetTicks_);
            }
            previousSyt_ = syt;
        }

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
        previousSyt_ = ExternalSyncBridge::kNoInfoSyt;
        previousSytValid_ = false;
        recoveredDeviceOffsetTicks_ = 0;
    }

    uint32_t ConsecutiveValid() const noexcept {
        return consecutiveValid_;
    }

private:
    uint32_t consecutiveValid_{0};
    uint16_t previousSyt_{ExternalSyncBridge::kNoInfoSyt};
    bool previousSytValid_{false};
    int64_t recoveredDeviceOffsetTicks_{0};
};

} // namespace ASFW::AudioEngine::DirectIsoch
