// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioInternalTxTiming.hpp"

#include "../../Wire/AMDTP/AmdtpTiming.hpp"

namespace ASFW::Audio::BeBoB {

namespace {
constexpr uint32_t kInternalTxTransferDelayTicks = 12'800;
static_assert(ASFW::Timing::kSytInterval48k == kMAudioInternalTxSytInterval);
static_assert(ASFW::Timing::kSytPacketStepTicks48k == 4'096);
static_assert(kInternalTxTransferDelayTicks == 8'704 + 4'096);
} // namespace

bool MAudioInternalTxTiming::Arm() noexcept {
    nextSequence_ = 0;
    armed_ = cadence_.Configure(kMAudioInternalTxSampleRateHz,
                                kMAudioInternalTxSytInterval, 0);
    return armed_;
}

void MAudioInternalTxTiming::Disarm() noexcept {
    cadence_.Reset();
    nextSequence_ = 0;
    armed_ = false;
}

bool MAudioInternalTxTiming::IsArmed() const noexcept {
    return armed_ && cadence_.IsConfigured();
}

uint32_t MAudioInternalTxTiming::TransferDelayTicks() const noexcept {
    return IsArmed() ? kInternalTxTransferDelayTicks : 0;
}

bool MAudioInternalTxTiming::PreviewNextPacket(PacketPlan& outPlan) const noexcept {
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

bool MAudioInternalTxTiming::CommitPacket(const PacketPlan& plan,
                                          const bool emittedData) noexcept {
    if (!IsArmed() || plan.sequence != nextSequence_ ||
        plan.cadenceCycle != cadence_.TotalCycles() ||
        (emittedData && !plan.isData)) {
        return false;
    }
    // A producer-starved DATA slot becomes NO-DATA on wire, but it occupied a
    // cycle in the vendor's 3:1 cadence and must not shift subsequent SYTs.
    cadence_.AdvanceCycle();
    ++nextSequence_;
    return true;
}

} // namespace ASFW::Audio::BeBoB
