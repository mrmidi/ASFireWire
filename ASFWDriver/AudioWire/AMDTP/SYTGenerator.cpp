//
// SYTGenerator.cpp
// ASFWDriver
//
// RX-seeded 48 kHz packet-step SYT generation
//

#include "SYTGenerator.hpp"
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
    seeded_ = false;
    dataPacketCount_ = 0;
    ASFW_LOG(Isoch, "SYTGenerator: Reset (RX-seeded timeline)");
}

void SYTGenerator::seedFromRxSyt(uint16_t rxSyt) noexcept {
    if (!initialized_) {
        return;
    }

    currentSytTickIndex_ = DecodeSytToTickIndex(rxSyt);
    seeded_ = true;
    dataPacketCount_ = 0;
    ASFW_LOG(Isoch, "SYTGenerator: Seeded from RX SYT 0x%04x -> tick=%u",
             rxSyt, currentSytTickIndex_);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
uint16_t SYTGenerator::computeDataSYT(uint32_t transmitCycle, uint32_t samplesInPacket) noexcept {
    if (!initialized_) return kNoInfo;
    if (!seeded_ || samplesInPacket == 0 || packetStepTicks_ == 0) return kNoInfo;

    const uint32_t txCycle4 = transmitCycle & 0x0F;
    const uint32_t txTicks = txCycle4 * kGeneratorTicksPerCycle;

    int32_t diffTicks = static_cast<int32_t>(currentSytTickIndex_) - static_cast<int32_t>(txTicks);
    // Wrap to signed range [-24576, 24575] representing the 16-cycle domain
    constexpr int32_t kHalfDomain = 24576;
    constexpr int32_t kDomain = 49152;
    if (diffTicks >= kHalfDomain) {
        diffTicks -= kDomain;
    } else if (diffTicks < -kHalfDomain) {
        diffTicks += kDomain;
    }

    int32_t delayCycles = diffTicks / static_cast<int32_t>(kGeneratorTicksPerCycle);
    int32_t ticks12 = diffTicks % static_cast<int32_t>(kGeneratorTicksPerCycle);
    if (ticks12 < 0) {
        ticks12 += kGeneratorTicksPerCycle;
        delayCycles -= 1;
    }

    const uint32_t cycle4 = (txCycle4 + static_cast<uint32_t>(delayCycles)) & 0x0F;
    const uint16_t syt = static_cast<uint16_t>((cycle4 << 12) | static_cast<uint32_t>(ticks12));

    // The 48 kHz blocking milestone advances SYT strictly in the sample domain,
    // independent of the number of OHCI bus cycles elapsed between DATA packets.
    (void)samplesInPacket;
    currentSytTickIndex_ += packetStepTicks_;
    if (currentSytTickIndex_ >= sytTickWrap_) {
        currentSytTickIndex_ -= sytTickWrap_;
    }

    dataPacketCount_++;
    return syt;
}

void SYTGenerator::nudgeOffsetTicks(int32_t deltaTicks) noexcept {
    if (!initialized_ || !seeded_ || deltaTicks == 0 || sytTickWrap_ == 0) {
        return;
    }

    int64_t adjusted = static_cast<int64_t>(currentSytTickIndex_) + static_cast<int64_t>(deltaTicks);
    const int64_t wrap = static_cast<int64_t>(sytTickWrap_);
    adjusted %= wrap;
    if (adjusted < 0) {
        adjusted += wrap;
    }

    currentSytTickIndex_ = static_cast<uint32_t>(adjusted);
}

} // namespace ASFW::Encoding
