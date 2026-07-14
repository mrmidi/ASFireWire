#include "AmdtpCadence.hpp"

#include "AmdtpTiming.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Blocking mode (IEC 61883-6): at 48 kHz, 6 audio frames arrive per 125 µs bus
// cycle (48000 / 8000) and a data packet always carries SYT_INTERVAL = 8
// frames, yielding N,D,D,D (6000 data packets per 8000 cycles); at 44.1 kHz
// the same rule yields 441 data packets per 640 cycles.
//
// The adapter seeds the deadline engine one subtick-cycle short of one full
// block: the k-th block's deadline lands in cycle c exactly when
// (c+1)*rate >= (k+1)*sytInterval*8000, i.e. "emit as soon as one full block
// of frames is pending" -- the FFADO accumulate-then-test rule (cip.c) this
// class previously implemented directly. See the header for the seed policy.
//
// Note: this is cadence only. Alignment of the no-data slot relative to SYT
// is owned by the timing model, not the cadence.

namespace {
constexpr uint8_t kFramesPerCycle48k = 6;

constexpr uint64_t AccumulatorEquivalentSeedSubticks(
    uint8_t sytIntervalFrames) noexcept {
    return Timing::kTicksPerSecond * sytIntervalFrames -
           Timing::kTicksPerCycle;
}
} // namespace

bool BlockingCadence::Configure(uint32_t sampleRateHz,
                                uint8_t sytIntervalFrames) noexcept {
    return engine_.Configure(sampleRateHz, sytIntervalFrames,
                             AccumulatorEquivalentSeedSubticks(
                                 sytIntervalFrames));
}

void BlockingCadence::Reset() noexcept {
    engine_.Reset();
}

bool BlockingCadence::CurrentCycleIsData() const noexcept {
    return engine_.CurrentDecision().isData;
}

uint8_t BlockingCadence::CurrentCycleDataFrames() const noexcept {
    return engine_.CurrentDecision().dataBlocks;
}

uint64_t BlockingCadence::TotalCycles() const noexcept {
    return engine_.TotalCycles();
}

void BlockingCadence::AdvanceCycle() noexcept {
    engine_.AdvanceCycle();
}

// Non-blocking mode at 48 kHz: the per-cycle frame count is integral (6), so
// every cycle is a data packet carrying exactly 6 frames; no no-data packets.

void NonBlocking48kCadence::Reset() noexcept {
    totalCycles_ = 0;
}

bool NonBlocking48kCadence::CurrentCycleIsData() const noexcept {
    return true;
}

uint8_t NonBlocking48kCadence::CurrentCycleDataFrames() const noexcept {
    return kFramesPerCycle48k;
}

uint64_t NonBlocking48kCadence::TotalCycles() const noexcept {
    return totalCycles_;
}

void NonBlocking48kCadence::AdvanceCycle() noexcept {
    ++totalCycles_;
}

bool RationalBlockingCadence::Configure(uint32_t sampleRateHz,
                                        uint8_t sytInterval,
                                        uint64_t initialDeadlineSubticks) noexcept {
    if (sampleRateHz == 0 || sytInterval == 0) {
        denominator_ = 0;
        sytInterval_ = 0;
        cycleSpanSubticks_ = 0;
        stepSubticks_ = 0;
        initialDeadlineSubticks_ = 0;
        deadlineSubticks_ = 0;
        totalCycles_ = 0;
        return false;
    }

    const uint64_t cycleSpan = uint64_t(Timing::kTicksPerCycle) * sampleRateHz;
    const uint64_t step = Timing::kTicksPerSecond * sytInterval;
    if (step < cycleSpan) {
        denominator_ = 0;
        sytInterval_ = 0;
        cycleSpanSubticks_ = 0;
        stepSubticks_ = 0;
        initialDeadlineSubticks_ = 0;
        deadlineSubticks_ = 0;
        totalCycles_ = 0;
        return false;
    }

    denominator_ = sampleRateHz;
    sytInterval_ = sytInterval;
    cycleSpanSubticks_ = cycleSpan;
    stepSubticks_ = step;
    initialDeadlineSubticks_ = initialDeadlineSubticks;
    Reset();
    return true;
}

void RationalBlockingCadence::Reset() noexcept {
    deadlineSubticks_ = initialDeadlineSubticks_;
    totalCycles_ = 0;
}

bool RationalBlockingCadence::IsConfigured() const noexcept {
    return denominator_ != 0;
}

RationalBlockingDecision RationalBlockingCadence::CurrentDecision() const noexcept {
    if (!IsConfigured() || deadlineSubticks_ >= cycleSpanSubticks_) {
        return {};
    }

    return {
        .isData = true,
        .dataBlocks = sytInterval_,
        .sytOffsetTicks = static_cast<uint16_t>(deadlineSubticks_ / denominator_),
    };
}

uint64_t RationalBlockingCadence::TotalCycles() const noexcept {
    return totalCycles_;
}

void RationalBlockingCadence::AdvanceCycle() noexcept {
    if (!IsConfigured()) {
        return;
    }

    if (deadlineSubticks_ < cycleSpanSubticks_) {
        deadlineSubticks_ += stepSubticks_;
    }
    deadlineSubticks_ -= cycleSpanSubticks_;
    ++totalCycles_;
}

} // namespace ASFW::Protocols::Audio::AMDTP
