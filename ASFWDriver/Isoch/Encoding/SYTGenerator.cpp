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
        sytIntervalTicks_ = kSytInterval * kTicksPerSample48k;  // 8 * 512 = 4096
    } else {
        ASFW_LOG(Isoch, "SYTGenerator: Unsupported rate %.0f Hz, using 48kHz params", sampleRate);
        sytIntervalTicks_ = kSytInterval * kTicksPerSample48k;
    }

    sytOffsetWrap_ = 16 * kTicksPerCycle;  // 49152

    reset();
    initialized_ = true;

    ASFW_LOG(Isoch, "SYTGenerator: Initialized cycle-based mode for %.0f Hz, "
             "intervalTicks=%u wrapTicks=%u transferDelay=0x%x",
             sampleRate, sytIntervalTicks_, sytOffsetWrap_, kTransferDelayTicks);
}

void SYTGenerator::reset() noexcept {
    sytOffsetTicks_ = 0;
    dataPacketCount_ = 0;
    ASFW_LOG(Isoch, "SYTGenerator: Reset (cycle-based mode)");
}

uint16_t SYTGenerator::computeDataSYT(uint32_t transmitCycle) noexcept {
    if (!initialized_) return kNoInfo;

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
    sytOffsetTicks_ += sytIntervalTicks_;
    if (sytOffsetTicks_ >= sytOffsetWrap_) {
        sytOffsetTicks_ -= sytOffsetWrap_;
    }

    dataPacketCount_++;
    return syt;
}

} // namespace ASFW::Encoding
