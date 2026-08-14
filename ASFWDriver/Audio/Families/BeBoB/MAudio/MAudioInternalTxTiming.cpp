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
    runningCycleTimer_ = 0;
    noDataAccumulator_ = 1;
    nextSequence_ = 0;
    lastCompletionStampCount_ = 0;

    if (sampleRateHz != kInternalTxTimingRateHz ||
        sytInterval != kInternalTxSytInterval) {
        state_ = InternalTxTimingFailed{epoch};
        return false;
    }

    // The vendor calls ResetPort(0) for output.  Do not substitute the bus
    // cycle timer here: completion-based phase re-anchoring is deliberately a
    // later, separate operation.
    state_ = InternalTxTimingRunning{epoch};
    return true;
}

void InternalTxTiming::Disarm() noexcept {
    state_ = InternalTxTimingStopped{StateEpoch(state_)};
    runningCycleTimer_ = 0;
    noDataAccumulator_ = 1;
    nextSequence_ = 0;
    lastCompletionStampCount_ = 0;
}

bool InternalTxTiming::IsArmed() const noexcept {
    return IsRunning(state_);
}

const InternalTxTimingState& InternalTxTiming::State() const noexcept {
    return state_;
}

uint32_t InternalTxTiming::RunningCycleTimer() const noexcept {
    return runningCycleTimer_;
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

    // 8 frames at 48 kHz is 4096 ticks, not one whole 3072-tick cycle.  Adding
    // cycles here pinned the sub-cycle offset and advanced SYT by only 3/4 of
    // real time, so the timestamps implied a 64 kHz stream
    // (vendor kext m_audio_b_FWDCLProgram::SetPacketParameters @ 0x272f4 maps
    // 48000 -> SYT increment 4096; OutputDCLCallback @ 0x263c2 adds it as
    // 2048 + 2048 + (increment & 1) through a helper that carries the offset).
    const uint32_t dataCycleTimer = isData
        ? ASFW::Timing::AddTicksToCycleTimer(runningCycleTimer_,
                                             kInternalTxSytIncrementTicks)
        : runningCycleTimer_;
    outPlan = {
        .sequence = nextSequence_,
        .isData = isData,
        .dataBlocks = static_cast<uint8_t>(
            isData ? kInternalTxSytInterval : 0U),
        .syt = isData
            ? static_cast<uint16_t>(dataCycleTimer & 0xFFFFU)
            : static_cast<uint16_t>(0xFFFFU),
        .baseCycleTimer = runningCycleTimer_,
        .dataCycleTimer = dataCycleTimer,
        .noDataAccumulatorAfter = nextAccumulator,
    };
    return true;
}

bool InternalTxTiming::CommitPacket(const InternalTxPacketPlan& plan,
                                    const bool emittedData) noexcept {
    if (!IsArmed() || plan.sequence != nextSequence_ ||
        plan.baseCycleTimer != runningCycleTimer_ ||
        plan.noDataAccumulatorAfter == 0U ||
        plan.noDataAccumulatorAfter > 4U ||
        (emittedData && !plan.isData)) {
        return false;
    }

    // A planned DATA packet may safely degrade to NO-DATA when CoreAudio has
    // not made a complete immutable PCM range available.  The timestamp base
    // still advances: it tracks elapsed time, not bytes emitted.
    //
    // The vendor has no degrade path -- OutputDCLCallback @ 0x263c2 always has
    // PCM from its own ring -- so its SYT accumulator advances on exactly the
    // three cadence-DATA packets of every four-cycle group, i.e. 3 * 4096 =
    // 12288 ticks per 4 cycles, exactly real time.  Skipping the advance on a
    // degraded packet breaks that invariant and retards the base by 4096 ticks
    // permanently, corrupting every later packet's SYT, not just this one.
    // Linux keeps the same invariant a different way: calculate_syt_offset()
    // (amdtp-stream.c:425) is driven by cycles, never by payload.
    //
    // plan.dataCycleTimer already equals plan.baseCycleTimer for a cadence
    // NO-DATA plan, so this advances only where the cadence says DATA.
    runningCycleTimer_ = plan.dataCycleTimer;
    noDataAccumulator_ = plan.noDataAccumulatorAfter;
    ++nextSequence_;
    return true;
}

InternalTxPhaseObservation InternalTxTiming::ObserveTxCompletion(
    const uint64_t stampCount,
    const uint32_t completionCycleTimer) noexcept {
    InternalTxPhaseObservation observation{};
    if (!IsArmed() || stampCount == 0U ||
        stampCount <= lastCompletionStampCount_) {
        return observation;
    }

    const auto completion = ASFW::Timing::decodeCycleTimer(completionCycleTimer);
    if (completion.cycle >= ASFW::Timing::kCyclesPerSecond) {
        return observation;
    }

    lastCompletionStampCount_ = stampCount;
    const uint32_t targetCycleTimer =
        ASFW::Timing::AddCyclesToCycleTimer(
            completionCycleTimer, kInternalTxLeadCycles);
    const auto target = ASFW::Timing::decodeCycleTimer(targetCycleTimer);
    const auto running = ASFW::Timing::decodeCycleTimer(runningCycleTimer_);

    // Preserve the vendor's deliberately asymmetric normalization exactly:
    // it corrects a negative value below -4000 by one 8000-cycle period, but
    // does not "clean up" a corresponding positive value.
    int32_t phaseCycles = static_cast<int32_t>(running.cycle) -
                          static_cast<int32_t>(target.cycle);
    if (phaseCycles < -4'000) {
        phaseCycles += static_cast<int32_t>(ASFW::Timing::kCyclesPerSecond);
    }

    observation = {
        .accepted = true,
        .reanchored = false,
        .stampCount = stampCount,
        .completionCycleTimer = completionCycleTimer,
        .phaseCycles = phaseCycles,
        .runningCycleTimer = runningCycleTimer_,
    };

    if (phaseCycles >= kInternalTxPhaseWindowStartCycles &&
        phaseCycles < kInternalTxPhaseWindowEndCycles) {
        return observation;
    }

    const uint32_t reanchoredCycleTimer =
        ASFW::Timing::AddCyclesToCycleTimer(
            targetCycleTimer, kInternalTxReanchorExtraCycles);
    runningCycleTimer_ =
        (reanchoredCycleTimer & ~ASFW::Timing::kCycleTimerOffsetMask) |
        (runningCycleTimer_ & ASFW::Timing::kCycleTimerOffsetMask);
    observation.reanchored = true;
    observation.runningCycleTimer = runningCycleTimer_;
    return observation;
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
