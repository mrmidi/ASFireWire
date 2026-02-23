//
// SYTGenerator.cpp
// ASFWDriver
//
// Cycle-based SYT generation (Linux-style, from amdtp-stream.c)
//

#include "SYTGenerator.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Encoding {

void SYTGenerator::initialize(double sampleRate) noexcept {
    // TODO: Support sample rates beyond 48kHz
    if (sampleRate == 48000.0) {
        ticksPerSample_ = kTicksPerSample48k;
    } else {
        ASFW_LOG(Isoch, "SYTGenerator: Unsupported rate %.0f Hz, using 48kHz params", sampleRate);
        ticksPerSample_ = kTicksPerSample48k;
    }

    sytOffsetWrap_ = 16 * kTicksPerCycle;  // 49152

    reset();
    initialized_ = true;

    const uint32_t defaultIntervalTicks = 8u * ticksPerSample_;
    ASFW_LOG(Isoch, "SYTGenerator: Initialized cycle-based mode for %.0f Hz, "
             "ticksPerSample=%u defaultIntervalTicks(8)=%u wrapTicks=%u transferDelay=0x%x",
             sampleRate, ticksPerSample_, defaultIntervalTicks, sytOffsetWrap_, kTransferDelayTicks);
}

void SYTGenerator::reset() noexcept {
    sytOffsetTicks_ = 0;
    dataPacketCount_ = 0;
    ASFW_LOG(Isoch, "SYTGenerator: Reset (cycle-based mode)");
}

uint16_t SYTGenerator::computeDataSYT(uint32_t transmitCycle, uint32_t samplesInPacket) noexcept {
    if (!initialized_) return kNoInfo;
    if (samplesInPacket == 0 || ticksPerSample_ == 0) return kNoInfo;

    // Total presentation offset = sample position offset + transfer delay
    uint32_t totalTicks = sytOffsetTicks_ + kTransferDelayTicks;

    // Split into whole cycles and remaining ticks
    uint32_t extraCycles = totalTicks / kTicksPerCycle;
    uint32_t remainingTicks = totalTicks % kTicksPerCycle;

    // Presentation cycle = transmit cycle + extra cycles from offset
    uint32_t presentationCycle = transmitCycle + extraCycles;

    // Encode SYT: 4-bit cycle | 12-bit tick offset
    uint16_t syt = static_cast<uint16_t>(
        ((presentationCycle & 0xF) << 12) | (remainingTicks & 0xFFF));

    // Advance offset for next DATA packet
    const uint32_t intervalTicks = samplesInPacket * ticksPerSample_;
    sytOffsetTicks_ += intervalTicks;
    if (sytOffsetTicks_ >= sytOffsetWrap_) {
        sytOffsetTicks_ -= sytOffsetWrap_;
    }

    dataPacketCount_++;
    return syt;
}

void SYTGenerator::nudgeOffsetTicks(int32_t deltaTicks) noexcept {
    if (!initialized_ || deltaTicks == 0 || sytOffsetWrap_ == 0) {
        return;
    }

    int64_t adjusted = static_cast<int64_t>(sytOffsetTicks_) + static_cast<int64_t>(deltaTicks);
    const int64_t wrap = static_cast<int64_t>(sytOffsetWrap_);
    adjusted %= wrap;
    if (adjusted < 0) {
        adjusted += wrap;
    }

    sytOffsetTicks_ = static_cast<uint32_t>(adjusted);
}

} // namespace ASFW::Encoding
