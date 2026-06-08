// TxOutputPhaseLoop.cpp
// ASFW - Phase-driven TX output timing derived from the Saffire reference model.

#include "TxOutputPhaseLoop.hpp"
#include <cstdlib>

namespace ASFW::AudioEngine::DirectIsoch {

void TxOutputPhaseLoop::Reset() noexcept {
    outputPhaseTicks_ = 0;
    predictedNextDevice_ = -1;
    seeded_ = false;
    diag_ = {};
}

TxOutputPhaseLoop::CycleResult
TxOutputPhaseLoop::ProcessCycle(int64_t devicePhaseTicks,
                                uint32_t framesPerPacket,
                                bool isData) noexcept {
    CycleResult res{};
    res.isData = isData;

    // 1. Discontinuity detection in the 1-second domain. The device phase the pipeline
    //    reconstructs from the transmit cycle wraps once per second; extOffsetDiff1s
    //    makes that wrap a no-op, so only real device jumps trip the detector.
    if (predictedNextDevice_ >= 0) {
        const int64_t derr =
            ASFW::Timing::extOffsetDiff1s(devicePhaseTicks, predictedNextDevice_);
        if (std::llabs(derr) > kGlitchThresholdTicks) {
            seeded_ = false;
            diag_.glitches++;
        }
    }
    predictedNextDevice_ = (devicePhaseTicks + kTicksPerCycle) % kOneSecondTicks;

    // 2. Seed the unwrapped phase the first time, or after a discontinuity.
    if (!seeded_) {
        outputPhaseTicks_ = devicePhaseTicks + kOperatingLead;
        seeded_ = true;
        res.rebased = true;
        diag_.rebases++;
    }

    // 3. Advisory lead (output ahead of device), 1-second domain.
    int64_t lead = ASFW::Timing::extOffsetDiff1s(outputPhaseTicks_, devicePhaseTicks);

    // 4. If the output has drifted out of the device's accepted window, the assembler
    //    cadence and the device clock have separated; re-seed and signal a re-anchor.
    if (!res.rebased && (lead <= 0 || lead > kLeadRejectTicks)) {
        outputPhaseTicks_ = devicePhaseTicks + kOperatingLead;
        res.rebased = true;
        diag_.rebases++;
        lead = ASFW::Timing::extOffsetDiff1s(outputPhaseTicks_, devicePhaseTicks);
    }

    res.tight  = lead < kLeadTightTicks;
    res.tooFar = lead > kLeadRejectTicks;
    if (res.tight)  diag_.tightWarnings++;
    if (res.tooFar) diag_.farWarnings++;

    // 5. Phase used for THIS packet, before advancing.
    res.outputPhaseTicks = outputPhaseTicks_;
    res.leadTicks = lead;

    // 6. Advance by one DATA-packet step on DATA, hold on NO-DATA. Unwrapped.
    if (isData) {
        outputPhaseTicks_ += static_cast<int64_t>(framesPerPacket) * kTicksPerFrame;
        diag_.dataPackets++;
    } else {
        diag_.noDataPackets++;
    }

    return res;
}

} // namespace ASFW::AudioEngine::DirectIsoch
