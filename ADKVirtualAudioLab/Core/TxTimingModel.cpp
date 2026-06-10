#include "TxTimingModel.hpp"

#include "../Protocols/Audio/IEC61883/Syt.hpp"

namespace ASFW::Driver {

// Design decisions (see TxTimingModel.hpp and the README, Milestone 2):
//
// 1. Seed-on-peek: the first PeekNextDataSyt after arming derives the phase
//    from the timeline (transmit-cycle anchoring), so the anchor lands on the
//    moment the first data packet is actually being stamped — mirroring
//    SYTGenerator's armTransmitCycleAnchor/computeDataSYT split.
// 2. The graft follows the reference model literally:
//    phase - (phase % kTicksPerCycle) + deviceSubCycleTicks. Because the
//    graft replaces the sub-cycle offset, it can move the phase backward by
//    up to one cycle; a bounded guard then re-adds whole cycles until the
//    lead is positive again — the graft fixes the sub-cycle, the cycle count
//    is free.
// 3. Commit is separate from Peek and advances exactly one packet step:
//    cadence owns the wire, the model tracks emitted data packets only.
// 4. Lead health thresholds are compared the way the Saffire decompile
//    behaves: accept is exclusive (< 7620 ships), tight and escalate are
//    advisory bands, kLate covers a phase already in the past.

using Protocols::Audio::IEC61883::SytFormatter;

void TxTimingModel::Configure(const Config& config) noexcept {
    config_ = config;
    Reset();
}

const TxTimingModel::Config& TxTimingModel::GetConfig() const noexcept {
    return config_;
}

void TxTimingModel::Reset() noexcept {
    phaseTicks_ = 0;
    seeded_ = false;
    anchorArmed_ = true;
}

void TxTimingModel::ArmTransmitCycleAnchor() noexcept {
    anchorArmed_ = true;
}

bool TxTimingModel::IsSeeded() const noexcept {
    return seeded_;
}

TxTimingModel::Decision TxTimingModel::PeekNextDataSyt(
    const Ports::ICycleTimeline& timeline) noexcept {
    Decision decision{};
    const int64_t now = timeline.NowTicks();

    if (anchorArmed_ || !seeded_) {
        int64_t phase = now + config_.presentationDelayTicks;
        if (config_.graftEnabled) {
            phase = phase - (phase % kTicksPerCycle) +
                    static_cast<int64_t>(config_.deviceSubCycleTicks);
            // The graft may have stepped back past "now"; whole cycles are
            // free, the sub-cycle is not. Bounded by construction (the graft
            // moves at most one cycle back).
            while (phase <= now) {
                phase += kTicksPerCycle;
            }
        }
        phaseTicks_ = phase;
        seeded_ = true;
        anchorArmed_ = false;
        decision.seededThisCall = true;
    }

    decision.syt = SytForPhase(phaseTicks_);
    decision.leadTicks = phaseTicks_ - now;
    decision.health = HealthForLead(decision.leadTicks);
    return decision;
}

void TxTimingModel::CommitDataPacket() noexcept {
    if (seeded_) {
        phaseTicks_ += kPacketStepTicks;
    }
}

void TxTimingModel::NudgeOffsetTicks(int32_t deltaTicks) noexcept {
    if (seeded_) {
        phaseTicks_ += deltaTicks;
    }
}

int64_t TxTimingModel::OutputPhaseTicks() const noexcept {
    return phaseTicks_;
}

uint16_t TxTimingModel::SytForPhase(int64_t phaseTicks) const noexcept {
    int64_t domainTick = phaseTicks % kSytDomainTicks;
    if (domainTick < 0) {
        domainTick += kSytDomainTicks;
    }
    const uint32_t cycle = static_cast<uint32_t>(domainTick / kTicksPerCycle);
    const uint32_t offset = static_cast<uint32_t>(domainTick % kTicksPerCycle);
    return SytFormatter::EncodeCycleOffset(cycle, offset);
}

TxTimingModel::LeadHealth TxTimingModel::HealthForLead(
    int64_t leadTicks) const noexcept {
    if (!seeded_) {
        return LeadHealth::kNotSeeded;
    }
    if (leadTicks < 0) {
        return LeadHealth::kLate;
    }
    if (leadTicks <= config_.tightLeadTicks - 1) {
        return LeadHealth::kTightWarn;
    }
    if (leadTicks < config_.acceptLeadTicks) {
        return LeadHealth::kAccepted;
    }
    if (leadTicks < config_.escalateLeadTicks) {
        return LeadHealth::kGate;
    }
    return LeadHealth::kEscalate;
}

} // namespace ASFW::Driver
