// Tests for TxOutputPhaseLoop's event/decision model.
//
// The reference contract lives in tools/debug/tx_phase_loop_model.py (and the JSON
// fixtures it emits under tests/fixtures/tx_phase_*.json). These tests drive the
// loop with the SAME input sequences the model's scenarios generate and assert the
// same invariants -- the model is the source of truth for "what should happen";
// these tests are the proof the C++ implementation matches it.
//
// The bug this replaces: every recurring, normal outcome (lead-health NO-DATA reads,
// absorbed slips, clock dropouts) was being folded into a single "rebased" bit that
// reset the phase map and re-armed the SYT anchor -- "TX PHASE RESET reason=Glitch"
// firing on ordinary cadence. Only kInitialSeed and kTimingDiscontinuity may do that
// now; everything else must be silent and cheap.
//
// Continuity and emission are reported as two independent classifications (not one
// combined enum) precisely so kInitialSeed/kOneCycleSlipCompensated can co-occur
// with either emission outcome -- an earlier draft gated emission on "continuity
// nominal" and silently forced seed/slip cycles to NO-DATA, manufacturing an
// artificial sub-one-cycle lead one beat later.
//
// WHO DECIDES THE WIRE: dataCandidate carries PacketAssembler's spec-mandated
// CIP/DBC cadence decision in ALREADY MADE -- emitData mirrors it exactly and the
// phase advances in lockstep with it. EmissionEvent (kLeadAcceptedData /
// kLeadGateNoData) is a pure lead-HEALTH read computed independently of
// dataCandidate -- "was the lead in range when we shipped or withheld this cycle" --
// and can co-occur with either emitData value. Tests below feed a realistic 3:1
// dataCandidate stream (mirroring BlockingCadence48k) through `Cadence48k(i)`
// rather than a constant true, both because that's what the real call site does and
// because it is what makes the lead oscillate the way the health read is meant to
// observe. See TxOutputPhaseLoop.hpp.

#include <gtest/gtest.h>
#include "AudioEngine/DirectIsoch/Timing/TxOutputPhaseLoop.hpp"

using ASFW::AudioEngine::DirectIsoch::TxOutputPhaseLoop;
using Continuity = TxOutputPhaseLoop::ContinuityEvent;
using Emission = TxOutputPhaseLoop::EmissionEvent;

namespace {
constexpr int64_t kCyc  = TxOutputPhaseLoop::kTicksPerCycle;     // 3072
constexpr int64_t kSec  = TxOutputPhaseLoop::kOneSecondTicks;    // 24,576,000
constexpr int64_t kLead = TxOutputPhaseLoop::kOperatingLead;     // 4608
constexpr int64_t kStep = 8 * TxOutputPhaseLoop::kTicksPerFrame; // 4096 (8-frame packet)
constexpr uint32_t kFpp = 8;

int64_t Wrap1s(int64_t ticks) {
    ticks %= kSec;
    if (ticks < 0) ticks += kSec;
    return ticks;
}

// Mirrors PacketAssembler::BlockingCadence48k's 3:1 DATA:NO-DATA shape at
// 48 kHz / 8-frame packets (3*8 + 1*0 == 4*6 frames/cycle) -- the realistic
// dataCandidate stream the call site actually feeds the loop. Tests don't need
// the assembler's exact DBC sequencing, only this ratio, since dataCandidate now
// drives emitData/phase-advance directly.
bool Cadence48k(uint32_t i) {
    return (i % 4) != 3;
}
}  // namespace

TEST(TxOutputPhaseLoop, InitialState) {
    TxOutputPhaseLoop loop;
    EXPECT_FALSE(loop.Seeded());
    EXPECT_EQ(loop.GetDiagnostics().resets(), 0u);
}

// --- Test 1 / Test 2: steady 48k cadence seeds exactly once and never glitches ---
//
// The CALLER's cadence (mirrored here by Cadence48k) produces the wire 3:1
// DATA:NO-DATA pattern; the loop just tracks it via dataCandidate. The lead-health
// read happens to land kLeadGateNoData on exactly the cycles the cadence withholds
// DATA -- not because the gate decided that (it doesn't), but because the
// assembler's cadence is shaped so the lead oscillates back into range right as it
// crosses the accept threshold. That emergent alignment is what this test pins.
TEST(TxOutputPhaseLoop, SteadyState48k_SeedsOnceNoDiscontinuity) {
    TxOutputPhaseLoop loop;
    int64_t dev = 0;
    constexpr int kCycles = 8000;

    for (int i = 0; i < kCycles; ++i) {
        const bool cand = Cadence48k(static_cast<uint32_t>(i));
        auto d = loop.ProcessCycle(static_cast<uint32_t>(i), dev,
                                   /*recoveredClockValid=*/true, /*dataCandidate=*/cand, kFpp);
        // emitData mirrors the caller's cadence decision exactly -- it is never
        // independently derived from the lead.
        EXPECT_EQ(d.emitData, cand);
        if (i == 0) {
            EXPECT_EQ(d.continuityEvent, Continuity::kInitialSeed);
            EXPECT_TRUE(d.resetPhaseMap);
            EXPECT_TRUE(d.armTransmitAnchor);
            // The seed lands at lead == kOperatingLead (comfortably inside the
            // accept window), so the health read is positive in the SAME callback
            // it ships DATA in -- exactly like Saffire.
            EXPECT_EQ(d.emissionEvent, Emission::kLeadAcceptedData);
        } else {
            EXPECT_NE(d.continuityEvent, Continuity::kInitialSeed);
            EXPECT_NE(d.continuityEvent, Continuity::kTimingDiscontinuity);
            EXPECT_FALSE(d.resetPhaseMap);
            EXPECT_FALSE(d.armTransmitAnchor);
        }
        EXPECT_TRUE(d.emissionEvent == Emission::kLeadAcceptedData
                    || d.emissionEvent == Emission::kLeadGateNoData);
        dev = Wrap1s(dev + kCyc);
    }

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.initialSeeds, 1u);
    EXPECT_EQ(diag.timingDiscontinuities, 0u);
    EXPECT_EQ(diag.resets(), 1u);
    EXPECT_EQ(diag.oneCycleSlipsCompensated, 0u);

    // The lead-health distribution mirrors the cadence ratio 1:1 here -- 6000
    // healthy : 2000 drifted -- an emergent property of the assembler's cadence
    // being shaped around exactly this lead trajectory, NOT a cadence decision
    // made by this loop (it makes none).
    EXPECT_EQ(diag.leadAcceptedData, 6000u);
    EXPECT_EQ(diag.leadGateNoData, 2000u);
    EXPECT_GT(diag.leadGateNoData, 0u);
}

// --- Test 3: a caller that is not invoked once-per-cycle must not manufacture
// discontinuities. The old detector assumed "+1 cycle since last call"; this
// reproduces the real bug (sparse callbacks vs. a free-running transmit-cycle
// counter) and proves the cycleDelta-corrected predictor absorbs it silently. ---
TEST(TxOutputPhaseLoop, SparseCallsDoNotManufactureDiscontinuity) {
    TxOutputPhaseLoop loop;
    constexpr uint32_t kStride = 8;
    int64_t dev = 0;
    uint32_t cycle = 0;

    for (int i = 0; i < 200; ++i) {
        auto d = loop.ProcessCycle(cycle, dev, true, Cadence48k(static_cast<uint32_t>(i)), kFpp);
        EXPECT_NE(d.continuityEvent, Continuity::kTimingDiscontinuity)
            << "sparse call " << i << " falsely classified as discontinuity";
        if (i > 0) EXPECT_FALSE(d.resetPhaseMap);  // i==0 is the legitimate InitialSeed
        cycle += kStride;
        dev = Wrap1s(dev + static_cast<int64_t>(kStride) * kCyc);
    }

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.initialSeeds, 1u);
    EXPECT_EQ(diag.timingDiscontinuities, 0u);
    EXPECT_EQ(diag.resets(), 1u);
}

// --- Test 4: the device phase wraps once per second; the wrap itself must not
// trip the continuity detector. ---
TEST(TxOutputPhaseLoop, PerSecondWrapIsNotDiscontinuity) {
    TxOutputPhaseLoop loop;
    int64_t dev = kSec - 10 * kCyc;   // start near the 1-second wrap

    for (uint32_t i = 0; i < 40; ++i) {
        auto d = loop.ProcessCycle(i, dev, true, Cadence48k(i), kFpp);
        EXPECT_NE(d.continuityEvent, Continuity::kTimingDiscontinuity);
        if (i > 0) EXPECT_FALSE(d.resetPhaseMap);  // i==0 is the legitimate InitialSeed
        dev = Wrap1s(dev + kCyc);     // wraps mid-run
    }

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.timingDiscontinuities, 0u);
    EXPECT_EQ(diag.resets(), 1u);
    EXPECT_EQ(diag.initialSeeds, 1u);
}

// --- Test 5: an isolated one-cycle slip (device skipped/repeated a cycle) is
// absorbed -- classified, NOT treated as a discontinuity, and does not reset
// anything. Up to kSlipToleranceLimit consecutive slips are tolerated. ---
TEST(TxOutputPhaseLoop, OneCycleSlipIsAbsorbedNotDiscontinuity) {
    TxOutputPhaseLoop loop;
    int64_t dev = 0;
    int64_t extra = 0;
    bool sawSlip = false;

    for (uint32_t i = 0; i < 60; ++i) {
        if (i == 30) {
            extra += kCyc;   // device phase jumps forward by exactly one cycle, once
        }
        const bool cand = Cadence48k(i);
        auto d = loop.ProcessCycle(i, Wrap1s(dev + extra), true, cand, kFpp);
        if (d.continuityEvent == Continuity::kOneCycleSlipCompensated) {
            sawSlip = true;
            EXPECT_FALSE(d.resetPhaseMap);
            EXPECT_FALSE(d.armTransmitAnchor);
            // "Compensated" means business as usual: emitData still mirrors the
            // caller's cadence untouched, and the lead-health read still runs
            // normally against the trusted prediction -- neither is forced to a
            // different outcome just because continuity classified this cycle as
            // an absorbed slip.
            EXPECT_EQ(d.emitData, cand);
            EXPECT_TRUE(d.emissionEvent == Emission::kLeadAcceptedData
                        || d.emissionEvent == Emission::kLeadGateNoData);
        }
        EXPECT_NE(d.continuityEvent, Continuity::kTimingDiscontinuity);
        dev = Wrap1s(dev + kCyc);
    }

    EXPECT_TRUE(sawSlip);
    const auto diag = loop.GetDiagnostics();
    EXPECT_GE(diag.oneCycleSlipsCompensated, 1u);
    EXPECT_EQ(diag.timingDiscontinuities, 0u);
    EXPECT_EQ(diag.resets(), 1u);  // only the initial seed -- the slip changed nothing
}

// --- Test 6: a real device-phase jump (far more than one cycle) is the one case
// that legitimately resets the map and re-arms the SYT anchor. The cause must be
// tagged kTimingDiscontinuity, not folded into the same bucket as steady-state
// lead-gate / slip outcomes -- that conflation is what made "Glitch" meaningless. ---
TEST(TxOutputPhaseLoop, RealDeviceJumpIsTimingDiscontinuity) {
    TxOutputPhaseLoop loop;
    auto seed = loop.ProcessCycle(0, 1000, true, true, kFpp);
    ASSERT_EQ(seed.continuityEvent, Continuity::kInitialSeed);

    auto jump = loop.ProcessCycle(1, 1000 + 50000, true, true, kFpp);
    EXPECT_EQ(jump.continuityEvent, Continuity::kTimingDiscontinuity);
    EXPECT_TRUE(jump.resetPhaseMap);
    EXPECT_TRUE(jump.armTransmitAnchor);
    EXPECT_EQ(jump.leadTicks, kLead);                 // re-seeded at device + operating lead
    EXPECT_EQ(jump.outputPhaseTicks, 51000 + kLead);
    // The reseed lands at a known-good lead -- the health read is positive in the
    // SAME callback (by the same logic as kInitialSeed). emitData mirrors the
    // caller's cadence decision (true here), as always -- it is not derived from
    // the reseed or the lead.
    EXPECT_EQ(jump.emissionEvent, Emission::kLeadAcceptedData);
    EXPECT_TRUE(jump.emitData);

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.initialSeeds, 1u);
    EXPECT_EQ(diag.timingDiscontinuities, 1u);
    EXPECT_EQ(diag.resets(), 2u);
}

// --- Test 7: the lead-health read NEVER resets the phase map or re-arms the
// anchor, NO MATTER what emitData is doing -- it is a pure diagnostic, decoupled
// from the cadence. To prove the decoupling for real (not just by emergent
// alignment, the way steady48's natural cadence happens to line up) this feeds
// 16-frame packets through the natural 3:1 dataCandidate stream: the output phase
// then races ahead of the device phase the cadence was sized for, so the lead
// drifts past the accept threshold regardless of whether THIS cycle shipped or
// withheld -- producing BOTH kLeadGateNoData+emitData==true ("shipping while
// thin") and kLeadGateNoData+emitData==false ("idle while loose"). Both are
// legitimate warning signals; neither may touch the anchors. This is the specific
// behavior change from the old model (LeadReject used to force a re-seed) -- if
// this regresses, the periodic re-anchor spam comes back. ---
TEST(TxOutputPhaseLoop, LeadGateNoDataNeverResetsAnchors) {
    TxOutputPhaseLoop loop;
    constexpr uint32_t kStressFpp = 16;  // 2x natural -- forces the lead to drift
    int64_t dev = 0;
    uint32_t leadGateNoDataWhileShipping = 0;
    uint32_t leadGateNoDataWhileIdle = 0;

    for (uint32_t i = 0; i < 400; ++i) {
        const bool cand = Cadence48k(i);
        auto d = loop.ProcessCycle(i, dev, true, cand, kStressFpp);
        EXPECT_EQ(d.emitData, cand);  // mirrors the cadence even as the lead drifts
        if (d.emissionEvent == Emission::kLeadGateNoData) {
            if (d.emitData) ++leadGateNoDataWhileShipping;
            else            ++leadGateNoDataWhileIdle;
            EXPECT_FALSE(d.resetPhaseMap);
            EXPECT_FALSE(d.armTransmitAnchor);
        }
        dev = Wrap1s(dev + kCyc);
    }

    // Both combinations occurred -- proof the health read is independent of emitData.
    EXPECT_GT(leadGateNoDataWhileShipping, 0u);
    EXPECT_GT(leadGateNoDataWhileIdle, 0u);
    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.resets(), 1u);  // only the initial seed across 400 cycles -- never the drift
}

// --- Test 8: clock-invalid cycles hold (NO-DATA, no phase advance) and must NOT
// repeatedly reset the map -- losing RX timing is not a phase-map discontinuity,
// it is the absence of a phase to track. ---
TEST(TxOutputPhaseLoop, ClockInvalidHoldsWithoutResettingAnchors) {
    TxOutputPhaseLoop loop;
    // Seed first so we can prove the transition to invalid doesn't reset anything.
    auto seed = loop.ProcessCycle(0, 0, true, true, kFpp);
    ASSERT_EQ(seed.continuityEvent, Continuity::kInitialSeed);
    const int64_t phaseBeforeInvalid = loop.OutputPhaseTicks();

    for (uint32_t i = 1; i <= 10; ++i) {
        auto d = loop.ProcessCycle(i, 0, /*recoveredClockValid=*/false, true, kFpp);
        EXPECT_EQ(d.continuityEvent, Continuity::kClockInvalid);
        EXPECT_EQ(d.emissionEvent, Emission::kClockInvalidNoData);
        EXPECT_FALSE(d.emitData);
        EXPECT_FALSE(d.resetPhaseMap);
        EXPECT_FALSE(d.armTransmitAnchor);
    }

    EXPECT_EQ(loop.OutputPhaseTicks(), phaseBeforeInvalid);  // held, not advanced
    EXPECT_FALSE(loop.Seeded());                              // re-seed is deferred to recovery
    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.clockInvalidNoData, 10u);
    EXPECT_EQ(diag.resets(), 1u);  // the original seed only -- invalid cycles reset nothing
}

// --- Test 9: when the clock becomes valid again, the loop seeds exactly once
// (kInitialSeed), then settles into steady cadence -- recovery is start-of-stream
// behavior, not a discontinuity of a phase that was never lost (it was absent). ---
TEST(TxOutputPhaseLoop, ClockRecoveryReseedsExactlyOnce) {
    TxOutputPhaseLoop loop;
    int64_t dev = 0;
    uint32_t cycle = 0;

    // Run invalid for a while.
    for (int i = 0; i < 20; ++i) {
        auto d = loop.ProcessCycle(cycle++, dev, false, Cadence48k(static_cast<uint32_t>(i)), kFpp);
        EXPECT_EQ(d.continuityEvent, Continuity::kClockInvalid);
        EXPECT_EQ(d.emissionEvent, Emission::kClockInvalidNoData);
        dev = Wrap1s(dev + kCyc);
    }
    EXPECT_FALSE(loop.Seeded());

    // Recovery: the very next valid cycle must seed, and only that one -- and the
    // health read is positive immediately (lead == kOperatingLead, same as any
    // fresh seed). emitData mirrors the cadence we feed it (true here), as always.
    auto recovered = loop.ProcessCycle(cycle++, dev, true, true, kFpp);
    EXPECT_EQ(recovered.continuityEvent, Continuity::kInitialSeed);
    EXPECT_TRUE(recovered.resetPhaseMap);
    EXPECT_TRUE(recovered.armTransmitAnchor);
    EXPECT_EQ(recovered.emissionEvent, Emission::kLeadAcceptedData);
    EXPECT_TRUE(recovered.emitData);
    dev = Wrap1s(dev + kCyc);

    for (int i = 0; i < 200; ++i) {
        auto d = loop.ProcessCycle(cycle++, dev, true, Cadence48k(static_cast<uint32_t>(i)), kFpp);
        EXPECT_NE(d.continuityEvent, Continuity::kInitialSeed);
        EXPECT_NE(d.continuityEvent, Continuity::kTimingDiscontinuity);
        dev = Wrap1s(dev + kCyc);
    }

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.initialSeeds, 1u);
    EXPECT_EQ(diag.timingDiscontinuities, 0u);
    EXPECT_EQ(diag.clockInvalidNoData, 20u);
}

// --- dataCandidate==false is a cadence decision the caller already made (a
// NO-DATA slot in the wire pattern, e.g. PacketAssembler's BlockingCadence48k) --
// NOT a "source not ready" signal with its own classification. The loop just
// tracks it: emitData mirrors it (false), the phase holds (does not advance), and
// -- the crux of the new contract -- the lead-health read keeps running completely
// independently, exactly as it would on a DATA cycle. There is no special
// "withheld" emission outcome anymore; kLeadAcceptedData/kLeadGateNoData report
// lead health whether or not this cycle shipped. ---
TEST(TxOutputPhaseLoop, NoDataCandidateMirrorsHoldsPhaseLeadHealthIndependent) {
    TxOutputPhaseLoop loop;
    auto seed = loop.ProcessCycle(0, 0, true, true, kFpp);
    ASSERT_EQ(seed.continuityEvent, Continuity::kInitialSeed);
    const int64_t phase0 = loop.OutputPhaseTicks();

    auto held = loop.ProcessCycle(1, kCyc, true, /*dataCandidate=*/false, kFpp);
    EXPECT_EQ(held.continuityEvent, Continuity::kNominal);
    EXPECT_FALSE(held.emitData);          // mirrors dataCandidate, not independently derived
    EXPECT_FALSE(held.resetPhaseMap);
    EXPECT_EQ(loop.OutputPhaseTicks(), phase0);  // not advanced -- tracks dataCandidate, not a verdict
    // The seed (cycle 0) shipped DATA -- lead grew from kOperatingLead by one
    // packet-vs-cycle delta (4608 + (4096-3072) = 5632), comfortably inside the
    // accept window. The health read is positive on THIS withheld cycle exactly as
    // it would be on a shipped one -- proof it never special-cases !dataCandidate.
    EXPECT_EQ(held.leadTicks, kLead + (kStep - kCyc));
    EXPECT_EQ(held.emissionEvent, Emission::kLeadAcceptedData);

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.leadAcceptedData, 2u);  // both the seed and the withheld cycle read healthy
    EXPECT_EQ(diag.resets(), 1u);
}
