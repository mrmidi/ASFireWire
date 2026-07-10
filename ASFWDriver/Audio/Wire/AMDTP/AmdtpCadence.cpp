#include "AmdtpCadence.hpp"

#include "AmdtpTiming.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

// Blocking mode at 48 kHz (IEC 61883-6): 6 audio frames arrive per 125 µs bus
// cycle (48000 / 8000), a data packet always carries SYT_INTERVAL = 8 frames.
// Accumulate 6 frames per cycle, emit 8 whenever ≥ 8 are pending:
//
//   pending: 0 → no-data → 6 → data → 4 → data → 2 → data → 0 → ...
//
// yielding the N,D,D,D pattern: exactly 6000 data packets per 8000 cycles.
// phase_ holds the pending (not yet emitted) frame count {0, 6, 4, 2}.
//
// Note: this is cadence only. Alignment of the no-data slot relative to SYT
// is owned by the timing model (Milestone 2), not the cadence.

namespace {
constexpr uint8_t kFramesPerCycle48k = 6;
constexpr uint8_t kSytInterval48k = 8;
} // namespace

void Blocking48kCadence::Reset() noexcept {
    phase_ = 0;
    totalCycles_ = 0;
}

bool Blocking48kCadence::CurrentCycleIsData() const noexcept {
    return static_cast<uint8_t>(phase_ + kFramesPerCycle48k) >= kSytInterval48k;
}

uint8_t Blocking48kCadence::CurrentCycleDataFrames() const noexcept {
    return CurrentCycleIsData() ? kSytInterval48k : 0;
}

uint64_t Blocking48kCadence::TotalCycles() const noexcept {
    return totalCycles_;
}

void Blocking48kCadence::AdvanceCycle() noexcept {
    phase_ = static_cast<uint8_t>(phase_ + kFramesPerCycle48k -
                                  CurrentCycleDataFrames());
    ++totalCycles_;
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
