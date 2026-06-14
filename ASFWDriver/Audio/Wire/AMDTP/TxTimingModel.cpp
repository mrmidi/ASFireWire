#include "TxTimingModel.hpp"

#include "../IEC61883/Syt.hpp"

#include <cstdlib>

namespace ASFW::Driver {

// Port of the Saffire.kext TX phase state machine:
//   ReadFirewireBuffers 0xd69e-0xd81e: recover RX cadence and master phase.
//   FillFirewireBuffers 0xec14-0xed72: seed one cycle ahead, initialize the
//   cadence reader 256 entries behind RX, gate lead, then advance by observed
//   cadence only after a DATA packet is emitted.
//   adjustOutputPhase 0xc9c2-0xcded: forced/deadbanded phase correction.

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
    forceAdjust_ = true;
    cadenceEpoch_ = 0;
    cadenceReadIndex_ = RxSytCadence::kNoInfo;
    pendingCadenceTicks_ = 0;
}

void TxTimingModel::RearmAfterSkippedDataSlot() noexcept {
    Reset();
}

bool TxTimingModel::IsSeeded() const noexcept {
    return seeded_;
}

TxTimingModel::Decision TxTimingModel::PeekNextDataSyt(
    int64_t packetAnchorTicks,
    const RxSytCadence& rxCadence) noexcept {
    Decision decision{};

    RxSytCadence::Snapshot rx{};
    if (!rxCadence.TrySnapshot(rx) || !rx.established ||
        rx.rollingCadenceTicks == 0) {
        return decision;
    }

    packetAnchorTicks =
        ASFW::Timing::normalizeOffsetDomain(packetAnchorTicks);
    decision.packetAnchorTicks = packetAnchorTicks;
    decision.recoveredPhaseTicks = rx.recoveredPhaseTicks;
    decision.rollingCadenceTicks = rx.rollingCadenceTicks;

    if (cadenceEpoch_ != rx.epoch) {
        Reset();
        cadenceEpoch_ = rx.epoch;
    }

    if (!seeded_) {
        phaseTicks_ = ASFW::Timing::normalizeOffsetDomain(
            packetAnchorTicks + config_.initialLeadTicks);
        seeded_ = true;
        forceAdjust_ = true;
        decision.seededThisCall = true;
    }

    decision.phaseTicksPre = phaseTicks_;
    phaseTicks_ = AdjustOutputPhase(packetAnchorTicks, phaseTicks_, rx, decision);
    decision.phaseTicksPost = phaseTicks_;

    if (cadenceReadIndex_ == RxSytCadence::kNoInfo) {
        cadenceReadIndex_ = static_cast<uint16_t>(
            (rx.writeIndex + RxSytCadence::kReadDelay) &
            (RxSytCadence::kEntryCount - 1));
    }

    // Wire SYT = fill phase + IEC 61883-6 TRANSFER_DELAY (Linux compute_syt,
    // amdtp-stream.c:1019). The lead/health gate below stays on the raw
    // phase: it is the Saffire fill-vs-execution governor, a different
    // quantity from the receiver-facing presentation time.
    decision.syt =
        SytForPhase(phaseTicks_ + config_.xmitTransferDelayTicks);
    decision.leadTicks =
        ASFW::Timing::extOffsetDiff(phaseTicks_, packetAnchorTicks);
    decision.wireLeadTicks =
        decision.leadTicks + config_.xmitTransferDelayTicks;
    decision.health = HealthForLead(decision.leadTicks);

    // Keep the original upper Saffire governor gate. wireLeadTicks is
    // diagnostic only: the 2026-06-12 bench showed that treating a one-cycle
    // receiver margin as a reset condition caused repeated timing reseeds
    // without stabilizing DICE ARX1.
    if (decision.health == LeadHealth::kGate ||
        decision.health == LeadHealth::kEscalate) {
        phaseTicks_ = 0;
        seeded_ = false;
        forceAdjust_ = true;
        cadenceReadIndex_ = RxSytCadence::kNoInfo;
        pendingCadenceTicks_ = 0;
        decision.reseeded = true;
        decision.syt = SytFormatter::kNoInfo;
        return decision;
    }

    pendingCadenceTicks_ = rxCadence.ReadEntry(cadenceReadIndex_);
    decision.pendingCadenceTicks = pendingCadenceTicks_;
    decision.cadenceReadIndex = cadenceReadIndex_;
    if (pendingCadenceTicks_ == 0) {
        phaseTicks_ = 0;
        seeded_ = false;
        forceAdjust_ = true;
        cadenceReadIndex_ = RxSytCadence::kNoInfo;
        decision = {};
    }
    return decision;
}

void TxTimingModel::CommitDataPacket() noexcept {
    if (seeded_ && pendingCadenceTicks_ != 0) {
        phaseTicks_ = ASFW::Timing::normalizeOffsetDomain(
            phaseTicks_ + pendingCadenceTicks_);
        cadenceReadIndex_ = static_cast<uint16_t>(
            (cadenceReadIndex_ + 1) & (RxSytCadence::kEntryCount - 1));
        pendingCadenceTicks_ = 0;
    }
}

int64_t TxTimingModel::AdjustOutputPhase(
    int64_t executionPhaseTicks,
    int64_t candidatePhaseTicks,
    const RxSytCadence::Snapshot& rx,
    Decision& decision) noexcept {
    const int64_t rxRecoveredPhaseTicksDelayFree =
        ASFW::Timing::normalizeOffsetDomain(rx.recoveredPhaseTicks - config_.xmitTransferDelayTicks);
    const int64_t phaseError =
        ASFW::Timing::extOffsetDiff(candidatePhaseTicks,
                                   rxRecoveredPhaseTicksDelayFree);
    decision.rxPhaseDelayFree = rxRecoveredPhaseTicksDelayFree;
    decision.phaseError = phaseError;
    const int64_t cadenceScale =
        static_cast<int64_t>(config_.sytIntervalFrames) << 8;
    if (cadenceScale == 0 || rx.rollingCadenceTicks == 0) {
        return candidatePhaseTicks;
    }

    const int64_t rolling = rx.rollingCadenceTicks;
    int64_t remainder = 0;
    int64_t complement = 0;
    if (phaseError >= 0) {
        remainder = (phaseError * cadenceScale) % rolling;
        complement = rolling - remainder;
    } else {
        remainder = ((-phaseError) * cadenceScale) % rolling;
        complement = remainder;
    }

    int64_t correctionTicks = 0;
    int64_t frameError = 0;
    if (remainder != 0) {
        correctionTicks = complement / cadenceScale;
        int64_t signedRemainder = remainder;
        if (remainder > rolling / 2) {
            signedRemainder -= rolling;
        }
        frameError = signedRemainder / cadenceScale;
    }
    decision.frameError = frameError;
    decision.correctionTicks = correctionTicks;

    if (!forceAdjust_ &&
        std::abs(frameError) <= config_.phaseDeadband) {
        return candidatePhaseTicks;
    }

    forceAdjust_ = false;
    decision.forceAdjustFired = true;
    // Decompiled Saffire adjustOutputPhase 0xcd4c-0xcd63: forced correction is
    // based on argument 4 (the carried/seeded output phase candidate), not
    // argument 3 (the current transmit anchor phase). The function returns
    // a4 unchanged or a4 + v17, never a3 + v17. The deadband path above
    // is the only path that returns the carried candidate unchanged.
    return ASFW::Timing::normalizeOffsetDomain(
        candidatePhaseTicks + correctionTicks);
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
