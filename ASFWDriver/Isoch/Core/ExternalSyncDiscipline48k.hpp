#pragma once

#include <cstdint>

namespace ASFW::Isoch::Core {

/// Per-packet TX-RX phase discipline modelled on Saffire.kext's adjustOutputPhase.
///
/// Called on every TX DATA packet.  Two regimes:
///   - **First-pass**: snaps TX SYT to RX SYT immediately (full error correction)
///   - **Tracking**: deadband-based full-error correction on every packet (no cooldown)
///
/// Phase detector wraps in the packet-interval domain (4096 ticks @ 48k/8-sample)
/// so whole-packet sampling latency jitter is invisible.
class ExternalSyncDiscipline48k {
public:
    static constexpr int32_t kTickDomain = 16 * 3072;  // 49152
    static constexpr int32_t kTicksPerCycle = kTickDomain / 16;  // 3072
    static constexpr int32_t kTicksPerSample = 512;  // 24.576MHz / 48kHz
    static constexpr int32_t kSamplesPerDataPacket = 8;  // IEC 61883-6 blocking @ 48kHz
    static constexpr int32_t kPacketIntervalTicks = kTicksPerSample * kSamplesPerDataPacket;  // 4096

    /// Deadband: ignore phase errors smaller than this (steady-state jitter tolerance).
    /// ~1/8 of a packet interval — matches Saffire's tolerance band.
    static constexpr int32_t kDeadbandTicks = 512;

    /// Safety limit: if corrected offset exceeds this, emit SYT=0xFFFF.
    /// ~4 cycles worth of ticks, matching Saffire's 12287-tick safety valve.
    static constexpr int32_t kSafetyLimitTicks = 12287;

    struct Result {
        bool active{false};
        bool locked{false};
        int32_t phaseErrorTicks{0};
        int32_t correctionTicks{0};
        bool staleOrUnlockEvent{false};
        bool firstPassSnap{false};    // true on the single first-pass correction
        bool safetyGateOpen{false};   // true when offset is within safety limit
    };

    void Reset() noexcept {
        active_ = false;
        firstPass_ = true;
        lastPhaseErrorTicks_ = 0;
        correctionCount_ = 0;
        staleOrUnlockCount_ = 0;
        minPhaseError_ = 0;
        maxPhaseError_ = 0;
    }

    [[nodiscard]] Result Update(bool enabled, uint16_t txSyt, uint16_t rxSyt) noexcept {
        Result result{};
        if (!enabled) {
            if (active_) {
                ++staleOrUnlockCount_;
                result.staleOrUnlockEvent = true;
            }
            active_ = false;
            firstPass_ = true;
            lastPhaseErrorTicks_ = 0;
            result.active = false;
            result.locked = false;
            result.safetyGateOpen = true;
            return result;
        }

        active_ = true;

        const int32_t rawDiff = SytToTickIndex(rxSyt) - SytToTickIndex(txSyt);

        int32_t correction = 0;

        if (firstPass_) {
            // First-pass: use FULL 49152-tick domain to see and correct multi-packet
            // offsets (e.g. the 12288-tick / 3-packet lag seen in FireBug captures).
            // Saffire's adjustOutputPhase does this via the first-pass flag at streamCtx+404.
            const int32_t fullPhase = WrapSignedTicks(rawDiff);
            correction = fullPhase;
            firstPass_ = false;
            ++correctionCount_;
            result.firstPassSnap = true;
            lastPhaseErrorTicks_ = fullPhase;
        } else {
            // Steady-state: use PACKET-INTERVAL domain [-2048..+2047] to track only
            // fractional drift. The bridge RX SYT naturally lags TX by ~1 packet due
            // to sampling latency — this offset is benign and must NOT be corrected.
            // Only crystal drift (sub-packet-interval) matters here.
            const int32_t intervalPhase = WrapSignedIntervalTicks(rawDiff);
            const int32_t absError = intervalPhase >= 0 ? intervalPhase : -intervalPhase;
            if (absError > kDeadbandTicks) {
                correction = intervalPhase;
                ++correctionCount_;
            }
            lastPhaseErrorTicks_ = intervalPhase;
        }

        // Track min/max for diagnostics (like Saffire's streamCtx+396/400)
        if (lastPhaseErrorTicks_ < minPhaseError_) {
            minPhaseError_ = lastPhaseErrorTicks_;
        }
        if (lastPhaseErrorTicks_ > maxPhaseError_) {
            maxPhaseError_ = lastPhaseErrorTicks_;
        }

        // Safety gate: is the resulting offset within bounds?
        // Caller should emit SYT=0xFFFF if gate is closed.
        const int32_t residual = lastPhaseErrorTicks_ - correction;
        const int32_t absResidual = residual >= 0 ? residual : -residual;
        result.safetyGateOpen = (absResidual <= kSafetyLimitTicks);

        result.active = true;
        result.locked = !firstPass_;  // locked after first pass completes
        result.phaseErrorTicks = lastPhaseErrorTicks_;
        result.correctionTicks = correction;
        return result;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool locked() const noexcept { return !firstPass_ && active_; }
    [[nodiscard]] int32_t lastPhaseErrorTicks() const noexcept { return lastPhaseErrorTicks_; }
    [[nodiscard]] int32_t minPhaseError() const noexcept { return minPhaseError_; }
    [[nodiscard]] int32_t maxPhaseError() const noexcept { return maxPhaseError_; }
    [[nodiscard]] uint64_t correctionCount() const noexcept { return correctionCount_; }
    [[nodiscard]] uint64_t staleOrUnlockCount() const noexcept { return staleOrUnlockCount_; }

    [[nodiscard]] static int32_t SytToTickIndex(uint16_t syt) noexcept {
        const int32_t cycle4 = static_cast<int32_t>((syt >> 12) & 0x0F);
        const int32_t ticks12 = static_cast<int32_t>(syt & 0x0FFF);
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
    bool firstPass_{true};
    int32_t lastPhaseErrorTicks_{0};
    int32_t minPhaseError_{0};
    int32_t maxPhaseError_{0};
    uint64_t correctionCount_{0};
    uint64_t staleOrUnlockCount_{0};
};

} // namespace ASFW::Isoch::Core
