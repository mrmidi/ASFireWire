// TxOutputPhaseLoop.cpp
// ASFW - Phase-driven TX output timing derived from the Saffire reference model.

#include "TxOutputPhaseLoop.hpp"
#include <cstdlib>

namespace ASFW::AudioEngine::DirectIsoch {

void TxOutputPhaseLoop::Reset() noexcept {
    outputPhaseTicks_ = 0;
    lastDevicePhaseTicks_ = -1;
    lastTransmitCycle_ = 0;
    haveLastTransmitCycle_ = false;
    slipCounter_ = 0;
    seeded_ = false;
    diag_ = {};
}

void TxOutputPhaseLoop::Seed(int64_t devicePhaseTicks) noexcept {
    outputPhaseTicks_ = devicePhaseTicks + kOperatingLead;
    seeded_ = true;
    slipCounter_ = 0;
}

TxOutputPhaseLoop::Decision
TxOutputPhaseLoop::ProcessCycle(uint32_t transmitCycle,
                                int64_t devicePhaseTicks,
                                bool recoveredClockValid,
                                bool dataCandidate,
                                uint32_t framesPerPacket) noexcept {
    Decision dec{};

    // How many isoch cycles elapsed since the previous call. The caller is NOT
    // guaranteed to be invoked exactly once per cycle -- assuming so is what
    // manufactured false discontinuities out of ordinary scheduling gaps.
    uint32_t cycleDelta = 1;
    if (haveLastTransmitCycle_) {
        cycleDelta = transmitCycle - lastTransmitCycle_;  // wraparound-safe (uint32)
        if (cycleDelta == 0) cycleDelta = 1;
    }
    lastTransmitCycle_ = transmitCycle;
    haveLastTransmitCycle_ = true;

    // 0. Clock not usable: hold everything, untouched anchors. The next valid
    //    sample re-seeds fresh, exactly like start-of-stream -- a clock dropout
    //    and recovery is not a "discontinuity" of the seeded phase, it is simply
    //    the absence and later presence of a usable phase to seed from. There is
    //    no device phase to gate against, so continuity and emission collapse into
    //    one combined NO-DATA outcome -- the one legitimate place that happens.
    if (!recoveredClockValid) {
        seeded_ = false;
        lastDevicePhaseTicks_ = -1;
        slipCounter_ = 0;
        diag_.clockInvalidNoData++;
        dec.continuityEvent = ContinuityEvent::kClockInvalid;
        dec.emissionEvent = EmissionEvent::kClockInvalidNoData;
        dec.outputPhaseTicks = outputPhaseTicks_;
        return dec;
    }

    bool resetMap = false;
    bool armAnchor = false;
    int64_t usedDevicePhase = devicePhaseTicks;
    ContinuityEvent continuityEvent;

    // 1. Timestamp continuity -- the ONLY classification that may declare a real
    //    discontinuity, and the ONLY thing that chooses which device-phase value
    //    (raw vs. slip-corrected prediction) emission measures against below. It
    //    does NOT decide whether to emit DATA -- that is the lead gate's job alone,
    //    run unconditionally in step 2 regardless of which branch fires here.
    if (!seeded_) {
        Seed(devicePhaseTicks);
        continuityEvent = ContinuityEvent::kInitialSeed;
        resetMap = true;
        armAnchor = true;
        diag_.initialSeeds++;
    } else {
        const int64_t expectedNext =
            (lastDevicePhaseTicks_ + static_cast<int64_t>(cycleDelta) * kTicksPerCycle) % kOneSecondTicks;
        const int64_t diff = ASFW::Timing::extOffsetDiff1s(devicePhaseTicks, expectedNext);

        if (std::llabs(diff) <= kContinuityToleranceTicks) {
            continuityEvent = ContinuityEvent::kNominal;
            slipCounter_ = 0;
        } else if (std::llabs(std::llabs(diff) - kTicksPerCycle) <= kContinuityToleranceTicks
                   && slipCounter_ < kSlipToleranceLimit) {
            continuityEvent = ContinuityEvent::kOneCycleSlipCompensated;
            usedDevicePhase = expectedNext;  // trust the prediction, absorb the jitter
            slipCounter_++;
            diag_.oneCycleSlipsCompensated++;
        } else {
            Seed(devicePhaseTicks);
            continuityEvent = ContinuityEvent::kTimingDiscontinuity;
            resetMap = true;
            armAnchor = true;
            diag_.timingDiscontinuities++;
        }
    }
    lastDevicePhaseTicks_ = devicePhaseTicks;

    // 2. Lead gate -- a pure HEALTH read, runs UNCONDITIONALLY whenever the clock is
    //    valid, independent of `dataCandidate` and of which continuity branch fired
    //    above. The wire DATA/NO-DATA cadence is decided upstream by the caller
    //    (PacketAssembler's spec-mandated CIP/DBC sequencing -- the device expects it
    //    bit-for-bit) and arrives here already made as `dataCandidate`; this loop
    //    TRACKS that decision rather than re-deciding it, so emitData mirrors it
    //    exactly and the phase advances in lockstep with it. What's left for the
    //    lead gate to report is purely diagnostic: was the lead in the safe operating
    //    range when we shipped or withheld this cycle? "Shipping while thin" and
    //    "idle while loose" are both meaningful warnings -- logged and counted, never
    //    overriding the cadence. Never resets anchors: at 48 kHz / 8-frame packets
    //    the assembler's natural 3:1 cadence keeps the lead oscillating inside the
    //    accept window by construction, so kLeadGateNoData recurring here is healthy
    //    telemetry, not an error -- it must stay cheap and silent.
    const int64_t lead = ASFW::Timing::extOffsetDiff1s(outputPhaseTicks_, usedDevicePhase);
    EmissionEvent emissionEvent;
    if (lead < kLeadAcceptTicks) {
        emissionEvent = EmissionEvent::kLeadAcceptedData;
        diag_.leadAcceptedData++;
    } else {
        emissionEvent = EmissionEvent::kLeadGateNoData;
        diag_.leadGateNoData++;
    }

    dec.emitData = dataCandidate;              // mirror the caller's cadence decision
    dec.outputPhaseTicks = outputPhaseTicks_;  // phase used for THIS packet, pre-advance
    dec.leadTicks = lead;
    dec.resetPhaseMap = resetMap;
    dec.armTransmitAnchor = armAnchor;
    dec.continuityEvent = continuityEvent;
    dec.emissionEvent = emissionEvent;
    dec.tight  = lead < kLeadTightTicks;
    dec.tooFar = lead > kLeadRejectTicks;
    if (dec.tight)  diag_.tightWarnings++;
    if (dec.tooFar) diag_.farWarnings++;

    if (dataCandidate) {
        outputPhaseTicks_ += static_cast<int64_t>(framesPerPacket) * kTicksPerFrame;
    }

    return dec;
}

} // namespace ASFW::AudioEngine::DirectIsoch
