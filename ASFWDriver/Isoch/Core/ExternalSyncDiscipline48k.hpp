#pragma once

#include <cstdint>

namespace ASFW::Isoch::Core {

class ExternalSyncDiscipline48k {
public:
    static constexpr int32_t kTickDomain = 16 * 3072;  // 49152
    static constexpr int32_t kTicksPerCycle = kTickDomain / 16;  // 3072
    static constexpr int32_t kTicksPerSample = 512;  // 24.576MHz / 48kHz
    static constexpr int32_t kSamplesPerDataPacket = 8;  // IEC 61883-6 blocking @ 48kHz
    static constexpr int32_t kPacketIntervalTicks = kTicksPerSample * kSamplesPerDataPacket;  // 4096
    static constexpr int32_t kDeadbandTicks = 32;
    static constexpr int32_t kStepTicks = 1;
    static constexpr uint32_t kBaselineWindow = 8;
    static constexpr uint32_t kCorrectionCooldownPackets = 32;  // ~5.3ms @ 48k/8-sample packets

    struct Result {
        bool active{false};
        bool locked{false};
        int32_t phaseErrorTicks{0};
        int32_t correctionTicks{0};
        bool staleOrUnlockEvent{false};
    };

    void Reset() noexcept {
        active_ = false;
        baselineLocked_ = false;
        baselineCount_ = 0;
        baselineAccum_ = 0;
        baselinePhaseTicks_ = 0;
        lastPhaseErrorTicks_ = 0;
        correctionCooldown_ = 0;
        correctionCount_ = 0;
        staleOrUnlockCount_ = 0;
    }

    [[nodiscard]] Result Update(bool enabled, uint16_t txSyt, uint16_t rxSyt) noexcept {
        Result result{};
        if (!enabled) {
            if (active_ || baselineLocked_ || baselineCount_ != 0) {
                ++staleOrUnlockCount_;
                result.staleOrUnlockEvent = true;
            }
            active_ = false;
            baselineLocked_ = false;
            baselineCount_ = 0;
            baselineAccum_ = 0;
            baselinePhaseTicks_ = 0;
            lastPhaseErrorTicks_ = 0;
            correctionCooldown_ = 0;
            result.active = false;
            result.locked = false;
            result.phaseErrorTicks = 0;
            result.correctionTicks = 0;
            return result;
        }

        active_ = true;
        // NOTE: rxSyt and txSyt are sampled at different times (IR vs IT pipeline).
        // That makes the "absolute" 16-cycle tick difference ambiguous by whole
        // DATA-packet intervals (4096 ticks @ 48k/8-sample blocking).
        //
        // For clock discipline we only care about the fractional phase within a
        // packet interval, so wrap the detector to that domain to avoid 4096-tick
        // jumps when sampling latency shifts by +/-1 packet.
        const int32_t rawPhase = WrapSignedIntervalTicks(SytToTickIndex(rxSyt) - SytToTickIndex(txSyt));

        if (!baselineLocked_) {
            baselineAccum_ += rawPhase;
            ++baselineCount_;
            if (baselineCount_ >= kBaselineWindow) {
                baselinePhaseTicks_ = static_cast<int32_t>(baselineAccum_ / static_cast<int64_t>(baselineCount_));
                baselineLocked_ = true;
                baselineCount_ = 0;
                baselineAccum_ = 0;
            }
            lastPhaseErrorTicks_ = 0;
            result.active = active_;
            result.locked = baselineLocked_;
            result.phaseErrorTicks = 0;
            result.correctionTicks = 0;
            return result;
        }

        const int32_t phaseError = WrapSignedIntervalTicks(rawPhase - baselinePhaseTicks_);
        lastPhaseErrorTicks_ = phaseError;

        int32_t correction = 0;
        const int32_t absError = phaseError >= 0 ? phaseError : -phaseError;
        if (correctionCooldown_ > 0) {
            --correctionCooldown_;
        } else if (absError > kDeadbandTicks) {
            correction = (phaseError > 0) ? kStepTicks : -kStepTicks;
            correctionCooldown_ = kCorrectionCooldownPackets;
            ++correctionCount_;
        }

        result.active = active_;
        result.locked = baselineLocked_;
        result.phaseErrorTicks = phaseError;
        result.correctionTicks = correction;
        return result;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool locked() const noexcept { return baselineLocked_; }
    [[nodiscard]] int32_t lastPhaseErrorTicks() const noexcept { return lastPhaseErrorTicks_; }
    [[nodiscard]] uint64_t correctionCount() const noexcept { return correctionCount_; }
    [[nodiscard]] uint64_t staleOrUnlockCount() const noexcept { return staleOrUnlockCount_; }

    [[nodiscard]] static int32_t SytToTickIndex(uint16_t syt) noexcept {
        const int32_t cycle4 = static_cast<int32_t>((syt >> 12) & 0x0F);
        const int32_t ticks12 = static_cast<int32_t>(syt & 0x0FFF);
        // SYT encodes a 4-bit cycle index (lower 4 bits of cycle count) and a
        // 12-bit tick offset within a 125us cycle (24.576MHz -> 3072 ticks).
        //
        // Convert to a monotonic tick index in the 16-cycle domain [0..49151].
        return (cycle4 * kTicksPerCycle) + (ticks12 % kTicksPerCycle);
    }

    [[nodiscard]] static int32_t WrapSignedTicks(int32_t ticks) noexcept {
        constexpr int32_t half = kTickDomain / 2;
        int32_t wrapped = ticks % kTickDomain;
        if (wrapped >= half) {
            wrapped -= kTickDomain;
        } else if (wrapped < -half) {
            wrapped += kTickDomain;
        }
        return wrapped;
    }

    [[nodiscard]] static int32_t WrapSignedIntervalTicks(int32_t ticks) noexcept {
        constexpr int32_t half = kPacketIntervalTicks / 2;
        int32_t wrapped = ticks % kPacketIntervalTicks;
        if (wrapped >= half) {
            wrapped -= kPacketIntervalTicks;
        } else if (wrapped < -half) {
            wrapped += kPacketIntervalTicks;
        }
        return wrapped;
    }

private:
    bool active_{false};
    bool baselineLocked_{false};
    uint32_t baselineCount_{0};
    int64_t baselineAccum_{0};
    int32_t baselinePhaseTicks_{0};
    int32_t lastPhaseErrorTicks_{0};
    uint32_t correctionCooldown_{0};
    uint64_t correctionCount_{0};
    uint64_t staleOrUnlockCount_{0};
};

} // namespace ASFW::Isoch::Core
