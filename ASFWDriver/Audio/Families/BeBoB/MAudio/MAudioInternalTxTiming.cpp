// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioInternalTxTiming.hpp"

namespace ASFW::Audio::Families::BeBoB::MAudio {

namespace {

[[nodiscard]] constexpr bool IsRunning(const InternalTxTimingState& state) noexcept {
    return std::holds_alternative<InternalTxTimingRunning>(state);
}

} // namespace

bool InternalTxTiming::Arm(const StartEpoch epoch, const uint32_t sampleRateHz,
                           const uint8_t sytInterval) noexcept {
    nextSequence_ = 0;
    transferDelayTicks_ = 0;

    const auto geometry = ::ASFW::Encoding::AmdtpRateGeometryForSampleRate(sampleRateHz);
    // Device capabilities still gate which rates can be selected. This only
    // verifies that the supplied packet geometry matches that rate's IEC
    // 61883-6 family, instead of silently retaining a 48 kHz cadence.
    if (!geometry ||
        (sampleRateHz != 48'000 && sampleRateHz != 96'000 &&
         sampleRateHz != 192'000) ||
        geometry->sytIntervalFrames != sytInterval ||
        !cadence_.Configure(sampleRateHz, sytInterval, 0)) {
        state_ = InternalTxTimingFailed{epoch};
        return false;
    }

    transferDelayTicks_ = InternalTxTransferDelayTicks(sampleRateHz, sytInterval);
    state_ = InternalTxTimingRunning{epoch};
    return true;
}

void InternalTxTiming::Disarm() noexcept {
    state_ = InternalTxTimingStopped{StateEpoch(state_)};
    cadence_.Reset();
    transferDelayTicks_ = 0;
    nextSequence_ = 0;
}

bool InternalTxTiming::IsArmed() const noexcept {
    return IsRunning(state_);
}

const InternalTxTimingState& InternalTxTiming::State() const noexcept {
    return state_;
}

uint32_t InternalTxTiming::TransferDelayTicks() const noexcept {
    return transferDelayTicks_;
}

bool InternalTxTiming::PreviewNextPacket(InternalTxPacketPlan& outPlan) const noexcept {
    if (!IsArmed()) return false;

    const auto decision = cadence_.CurrentDecision();
    outPlan = {
        .sequence = nextSequence_,
        .cadenceCycle = cadence_.TotalCycles(),
        .isData = decision.isData,
        .dataBlocks = decision.dataBlocks,
        .sytOffsetTicks = decision.sytOffsetTicks,
    };
    return true;
}

bool InternalTxTiming::CommitPacket(const InternalTxPacketPlan& plan,
                                    const bool emittedData) noexcept {
    if (!IsArmed() || plan.sequence != nextSequence_ ||
        plan.cadenceCycle != cadence_.TotalCycles() ||
        (emittedData && !plan.isData)) {
        return false;
    }

    // A PCM-starved DATA decision is emitted as NO-DATA, but it still consumed
    // a physical isochronous cycle. Advance unconditionally so a producer
    // fault cannot shift the backend wire cadence.
    cadence_.AdvanceCycle();
    ++nextSequence_;
    return true;
}

StartEpoch InternalTxTiming::StateEpoch(const InternalTxTimingState& state) noexcept {
    if (const auto* stopped = std::get_if<InternalTxTimingStopped>(&state)) return stopped->epoch;
    if (const auto* running = std::get_if<InternalTxTimingRunning>(&state)) return running->epoch;
    if (const auto* failed = std::get_if<InternalTxTimingFailed>(&state)) return failed->epoch;
    return {};
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
