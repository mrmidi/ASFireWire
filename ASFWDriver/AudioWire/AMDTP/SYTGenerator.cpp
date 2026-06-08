//
// SYTGenerator.cpp
// ASFWDriver
//
// RX-seeded 48 kHz packet-step SYT generation
//

#include "SYTGenerator.hpp"
#include "TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Encoding {

namespace {

constexpr uint32_t kGeneratorTicksPerCycle = 3072;

[[nodiscard]] uint16_t EncodeTickIndexToSyt(uint32_t tickIndex) noexcept {
    const uint16_t cycle4 = static_cast<uint16_t>((tickIndex / kGeneratorTicksPerCycle) & 0x0F);
    const uint16_t ticks12 = static_cast<uint16_t>(tickIndex % kGeneratorTicksPerCycle);
    return static_cast<uint16_t>((cycle4 << 12) | ticks12);
}

[[nodiscard]] uint32_t DecodeSytToTickIndex(uint16_t syt) noexcept {
    const uint32_t cycle4 = static_cast<uint32_t>((syt >> 12) & 0x0F);
    const uint32_t ticks12_raw = static_cast<uint32_t>(syt & 0x0FFF);
    const uint32_t extraCycles = ticks12_raw / kGeneratorTicksPerCycle;
    const uint32_t ticks12 = ticks12_raw % kGeneratorTicksPerCycle;
    const uint32_t finalCycle4 = (cycle4 + extraCycles) & 0x0F;
    return (finalCycle4 * kGeneratorTicksPerCycle) + ticks12;
}

[[nodiscard]] int64_t NormalizeEightSecondOffset(int64_t tickOffset) noexcept {
    tickOffset %= ASFW::Timing::kEightSecondTicks;
    if (tickOffset < 0) {
        tickOffset += ASFW::Timing::kEightSecondTicks;
    }
    return tickOffset;
}

} // namespace

void SYTGenerator::initialize(double sampleRate) noexcept {
    // TODO: Support sample rates beyond 48kHz
    if (sampleRate == 48000.0) {
        ticksPerSample_ = kTicksPerSample48k;
    } else {
        ASFW_LOG(Isoch, "SYTGenerator: Unsupported rate %.0f Hz, using 48kHz params", sampleRate);
        ticksPerSample_ = kTicksPerSample48k;
    }

    packetStepTicks_ = 8u * ticksPerSample_;
    sytTickWrap_ = 16 * kTicksPerCycle;  // 49152

    reset();
    initialized_ = true;

    ASFW_LOG(Isoch, "SYTGenerator: Initialized RX-seeded 48k timeline for %.0f Hz, "
             "ticksPerSample=%u packetStepTicks=%u wrapTicks=%u",
             sampleRate, ticksPerSample_, packetStepTicks_, sytTickWrap_);
}

void SYTGenerator::reset() noexcept {
    currentSytTickIndex_ = 0;
    currentSytOffsetTicks_ = 0;
    seeded_ = false;
    needsAnchor_ = false;
    dataPacketCount_ = 0;
    ASFW_LOG(Isoch, "SYTGenerator: Reset");
}

void SYTGenerator::seedFromRxSyt(uint16_t rxSyt) noexcept {
    if (!initialized_) {
        return;
    }

    currentSytTickIndex_ = DecodeSytToTickIndex(rxSyt);
    currentSytOffsetTicks_ = currentSytTickIndex_;
    seeded_ = true;
    needsAnchor_ = false;
    dataPacketCount_ = 0;
    // Hot-path during SYT experiments; keep silent unless a caller logs a
    // deliberately throttled seed/reseed event.
}

void SYTGenerator::armTransmitCycleAnchor() noexcept {
    if (!initialized_) {
        return;
    }

    seeded_ = true;
    needsAnchor_ = true;
    dataPacketCount_ = 0;
    // Called from rebase/reset paths -- can repeat every cycle if the phase loop
    // is unstable, so throttle rather than flood the log.
    ASFW_LOG_RL(Isoch, "syt/armed_anchor", 1000, OS_LOG_TYPE_DEFAULT,
                "SYTGenerator: Armed transmit-cycle anchor (delay=%u ticks)",
                kPresentationDelayTicks);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
uint16_t SYTGenerator::computeDataSYT(uint32_t transmitCycle, uint32_t samplesInPacket) noexcept {
    if (!initialized_) return kNoInfo;
    if (!seeded_ || samplesInPacket == 0 || packetStepTicks_ == 0) return kNoInfo;

    if (needsAnchor_) {
        const uint32_t cycle = transmitCycle % ASFW::Timing::kCyclesPerSecond;
        const int64_t txOffset = ASFW::Timing::tstampToOffsets(0, cycle, 0);
        currentSytOffsetTicks_ =
            NormalizeEightSecondOffset(txOffset + int64_t(kPresentationDelayTicks));
        currentSytTickIndex_ =
            static_cast<uint32_t>(currentSytOffsetTicks_ % int64_t(sytTickWrap_));
        needsAnchor_ = false;
    }

    const uint16_t syt = EncodeTickIndexToSyt(currentSytTickIndex_);

    // Blocking-mode SYT advances in the sample domain (8 samples * 512 ticks),
    // independent of the number of OHCI bus cycles elapsed between DATA packets.
    (void)samplesInPacket;
    currentSytOffsetTicks_ =
        NormalizeEightSecondOffset(currentSytOffsetTicks_ + int64_t(packetStepTicks_));
    currentSytTickIndex_ =
        static_cast<uint32_t>(currentSytOffsetTicks_ % int64_t(sytTickWrap_));

    dataPacketCount_++;
    return syt;
}

void SYTGenerator::nudgeOffsetTicks(int32_t deltaTicks) noexcept {
    if (!initialized_ || !seeded_ || deltaTicks == 0 || sytTickWrap_ == 0) {
        return;
    }

    currentSytOffsetTicks_ =
        NormalizeEightSecondOffset(currentSytOffsetTicks_ + static_cast<int64_t>(deltaTicks));
    currentSytTickIndex_ =
        static_cast<uint32_t>(currentSytOffsetTicks_ % int64_t(sytTickWrap_));
}

} // namespace ASFW::Encoding
