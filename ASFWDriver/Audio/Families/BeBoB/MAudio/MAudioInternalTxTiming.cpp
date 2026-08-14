// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioInternalTxTiming.hpp"

#include "../../../../Common/TimingUtils.hpp"

namespace ASFW::Audio::Families::BeBoB::MAudio {

namespace {

[[nodiscard]] constexpr bool IsRunning(
    const InternalTxTimingState& state) noexcept {
    return std::holds_alternative<InternalTxTimingRunning>(state);
}

} // namespace

bool InternalTxTiming::Arm(const StartEpoch epoch,
                           const uint32_t sampleRateHz,
                           const uint8_t sytInterval) noexcept {
    sytPhaseTicks_ = kInternalTxSytPhaseSeedTicks;
    noDataAccumulator_ = 1;
    nextSequence_ = 0;

    if (sampleRateHz != kInternalTxTimingRateHz ||
        sytInterval != kInternalTxSytInterval) {
        state_ = InternalTxTimingFailed{epoch};
        return false;
    }

    // The vendor calls ResetPort(0) for output.  Only the sub-cycle phase is
    // seeded: where a packet lands in bus time comes from that packet's own
    // transmit cycle, supplied by the caller.
    state_ = InternalTxTimingRunning{epoch};
    return true;
}

void InternalTxTiming::Disarm() noexcept {
    state_ = InternalTxTimingStopped{StateEpoch(state_)};
    sytPhaseTicks_ = kInternalTxSytPhaseSeedTicks;
    noDataAccumulator_ = 1;
    nextSequence_ = 0;
}

bool InternalTxTiming::IsArmed() const noexcept {
    return IsRunning(state_);
}

const InternalTxTimingState& InternalTxTiming::State() const noexcept {
    return state_;
}

uint32_t InternalTxTiming::SytPhaseTicks() const noexcept {
    return sytPhaseTicks_;
}

bool InternalTxTiming::PreviewNextPacket(
    InternalTxPacketPlan& outPlan) const noexcept {
    if (!IsArmed()) {
        return false;
    }

    uint8_t nextAccumulator = static_cast<uint8_t>(noDataAccumulator_ + 1U);
    const bool isData = nextAccumulator <= 4U;
    if (!isData) {
        nextAccumulator = static_cast<uint8_t>(nextAccumulator - 4U);
    }

    // 8 frames at 48 kHz is 4096 ticks -- one cycle plus 1024 -- so the phase
    // steps 1024 per DATA packet modulo 3072 while the packet's transmit cycle
    // steps 1.  The wrap from 2048 back to 0 therefore lands on the cadence
    // NO-DATA, whose two-cycle gap makes up the missing 2048 and holds the
    // inter-packet SYT delta at exactly 4096 ticks.
    // (vendor kext m_audio_b_FWDCLProgram::SetPacketParameters @ 0x272f4 maps
    // 48000 -> SYT increment 4096; Linux derives the same value at
    // amdtp-stream.c:306 and emits the same phase sequence at :425-461.)
    const uint32_t sytPhaseTicks = isData
        ? (sytPhaseTicks_ + kInternalTxSytIncrementTicks) %
              ASFW::Timing::kTicksPerCycle
        : sytPhaseTicks_;
    outPlan = {
        .sequence = nextSequence_,
        .isData = isData,
        .dataBlocks = static_cast<uint8_t>(
            isData ? kInternalTxSytInterval : 0U),
        .basePhaseTicks = sytPhaseTicks_,
        .sytPhaseTicks = sytPhaseTicks,
        .noDataAccumulatorAfter = nextAccumulator,
    };
    return true;
}

bool InternalTxTiming::CommitPacket(const InternalTxPacketPlan& plan,
                                    const bool emittedData) noexcept {
    if (!IsArmed() || plan.sequence != nextSequence_ ||
        plan.basePhaseTicks != sytPhaseTicks_ ||
        plan.noDataAccumulatorAfter == 0U ||
        plan.noDataAccumulatorAfter > 4U ||
        (emittedData && !plan.isData)) {
        return false;
    }

    // A planned DATA packet may safely degrade to NO-DATA when CoreAudio has
    // not made a complete immutable PCM range available, or when the caller
    // cannot yet name the packet's transmit cycle.  The phase still advances:
    // it tracks elapsed time, not bytes emitted.
    //
    // The vendor has no degrade path -- OutputDCLCallback @ 0x263c2 always has
    // PCM from its own ring -- so its SYT accumulator advances on exactly the
    // three cadence-DATA packets of every four-cycle group, i.e. 3 * 4096 =
    // 12288 ticks per 4 cycles, exactly real time.  Skipping the advance on a
    // degraded packet breaks that invariant, and because the phase and the
    // four-packet cadence accumulator share a four-cycle period, it also slips
    // the 2048 -> 0 wrap off the NO-DATA slot and corrupts every later delta.
    // Linux keeps the same invariant a different way: calculate_syt_offset()
    // (amdtp-stream.c:425) is driven by cycles, never by payload.
    //
    // plan.sytPhaseTicks already equals plan.basePhaseTicks for a cadence
    // NO-DATA plan, so this advances only where the cadence says DATA.
    sytPhaseTicks_ = plan.sytPhaseTicks;
    noDataAccumulator_ = plan.noDataAccumulatorAfter;
    ++nextSequence_;
    return true;
}

StartEpoch InternalTxTiming::StateEpoch(
    const InternalTxTimingState& state) noexcept {
    if (const auto* stopped = std::get_if<InternalTxTimingStopped>(&state)) {
        return stopped->epoch;
    }
    if (const auto* running = std::get_if<InternalTxTimingRunning>(&state)) {
        return running->epoch;
    }
    if (const auto* failed = std::get_if<InternalTxTimingFailed>(&state)) {
        return failed->epoch;
    }
    return {};
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
