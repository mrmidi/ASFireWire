#include <gtest/gtest.h>
#include "AudioEngine/DirectIsoch/Timing/TxOutputPhaseLoop.hpp"

using ASFW::AudioEngine::DirectIsoch::TxOutputPhaseLoop;

namespace {
constexpr int64_t kCyc = TxOutputPhaseLoop::kTicksPerCycle;   // 3072
constexpr int64_t kSec = TxOutputPhaseLoop::kOneSecondTicks;  // 24,576,000
constexpr int64_t kLead = TxOutputPhaseLoop::kOperatingLead;  // 4608
constexpr int64_t kStep = 8 * TxOutputPhaseLoop::kTicksPerFrame;  // 4096
}  // namespace

TEST(TxOutputPhaseLoop, InitialState) {
    TxOutputPhaseLoop loop;
    EXPECT_EQ(loop.GetDiagnostics().dataPackets, 0u);
    EXPECT_FALSE(loop.Seeded());
}

// Phase seeds at device + operating lead, advances by one packet step per DATA,
// and holds across NO-DATA. (Cadence is supplied by the caller; the loop never
// decides DATA/NO-DATA itself.)
TEST(TxOutputPhaseLoop, AdvancesByPacketStepOnDataHoldsOnNoData) {
    TxOutputPhaseLoop loop;

    auto r0 = loop.ProcessCycle(0, 8, /*isData=*/true);   // first cycle seeds
    EXPECT_TRUE(r0.rebased);
    EXPECT_EQ(r0.rebaseReason, TxOutputPhaseLoop::RebaseReason::kInitialSeed);
    EXPECT_EQ(r0.outputPhaseTicks, kLead);                // dev(0) + lead

    auto r1 = loop.ProcessCycle(kCyc, 8, /*isData=*/true);
    EXPECT_FALSE(r1.rebased);
    EXPECT_EQ(r1.rebaseReason, TxOutputPhaseLoop::RebaseReason::kNone);
    EXPECT_EQ(r1.outputPhaseTicks, kLead + kStep);        // advanced one packet

    auto r2 = loop.ProcessCycle(2 * kCyc, 8, /*isData=*/false);  // NO-DATA holds
    EXPECT_EQ(r2.outputPhaseTicks, kLead + 2 * kStep);

    auto r3 = loop.ProcessCycle(3 * kCyc, 8, /*isData=*/true);   // phase held from r2
    EXPECT_EQ(r3.outputPhaseTicks, kLead + 2 * kStep);
}

// With the assembler's blocking 3:1 cadence supplied, the lead stays inside the
// device's accepted window and the loop seeds exactly once (no per-cycle rebase).
TEST(TxOutputPhaseLoop, RealisticSteadyState48k) {
    TxOutputPhaseLoop loop;
    int64_t dev = 0;
    int dataCount = 0;
    int noDataCount = 0;

    for (int i = 0; i < 800; ++i) {
        const bool isData = (i % 4) != 3;   // D D D N == 6 frames/cycle at 48k
        auto r = loop.ProcessCycle(dev, 8, isData);
        if (isData) ++dataCount; else ++noDataCount;
        if (i > 0) {
            EXPECT_GT(r.leadTicks, 0);
            EXPECT_LE(r.leadTicks, TxOutputPhaseLoop::kLeadRejectTicks);
            EXPECT_FALSE(r.rebased);
        }
        dev = (dev + kCyc) % kSec;
    }

    EXPECT_EQ(dataCount, 600);
    EXPECT_EQ(noDataCount, 200);
    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.rebases, 1u);     // only the initial seed
    EXPECT_EQ(diag.rebasesInitialSeed, 1u);
    EXPECT_EQ(diag.rebasesGlitch, 0u);
    EXPECT_EQ(diag.rebasesLeadReject, 0u);
    EXPECT_EQ(diag.glitches, 0u);
}

// P2 regression: the device phase wraps once per second; that wrap must NOT be
// mistaken for a discontinuity.
TEST(TxOutputPhaseLoop, PerSecondWrapIsNotGlitch) {
    TxOutputPhaseLoop loop;
    int64_t dev = kSec - 10 * kCyc;   // start near the 1-second wrap
    for (int i = 0; i < 40; ++i) {
        (void)loop.ProcessCycle(dev, 8, (i % 4) != 3);
        dev = (dev + kCyc) % kSec;    // wraps mid-run
    }
    EXPECT_EQ(loop.GetDiagnostics().glitches, 0u);
    EXPECT_EQ(loop.GetDiagnostics().rebases, 1u);  // only the initial seed
    EXPECT_EQ(loop.GetDiagnostics().rebasesInitialSeed, 1u);
}

// A real device-phase jump is a discontinuity: re-seed at device + operating lead,
// and the cause must be tagged Glitch (not InitialSeed) so a recurring glitch on
// hardware is distinguishable from one-time start-of-stream seeding.
TEST(TxOutputPhaseLoop, GlitchRecovery) {
    TxOutputPhaseLoop loop;
    auto seed = loop.ProcessCycle(1000, 8, /*isData=*/true);     // seed
    EXPECT_EQ(seed.rebaseReason, TxOutputPhaseLoop::RebaseReason::kInitialSeed);
    auto r = loop.ProcessCycle(1000 + 50000, 8, /*isData=*/true);  // device jump

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.glitches, 1u);
    EXPECT_TRUE(r.rebased);
    EXPECT_EQ(r.rebaseReason, TxOutputPhaseLoop::RebaseReason::kGlitch);
    EXPECT_EQ(r.leadTicks, kLead);
    EXPECT_EQ(r.outputPhaseTicks, 51000 + kLead);

    EXPECT_EQ(diag.rebasesInitialSeed, 1u);
    EXPECT_EQ(diag.rebasesGlitch, 1u);
    EXPECT_EQ(diag.rebasesLeadReject, 0u);
    EXPECT_EQ(diag.rebases, diag.rebasesInitialSeed + diag.rebasesGlitch + diag.rebasesLeadReject);
}

// Sustained 100% DATA cadence (no NO-DATA holds) advances the output phase faster
// than the device phase progresses, so the lead grows past kLeadRejectTicks with no
// device-phase discontinuity. The loop must re-seed and tag the cause as LeadReject
// -- distinct from Glitch (no predicted-phase jump occurred) and from InitialSeed
// (this is a steady-state correction, not start-of-stream). This is the signal the
// reviewer flagged: if LeadReject keeps recurring on hardware, the phase loop is not
// converging on the assembler cadence / recovered device clock relationship.
TEST(TxOutputPhaseLoop, LeadRejectRecovery) {
    TxOutputPhaseLoop loop;
    int64_t dev = 0;
    TxOutputPhaseLoop::CycleResult r{};

    // lead grows by (kStep - kCyc) = 1024 ticks/cycle from the seeded kLead = 4608;
    // it first exceeds kLeadRejectTicks (12287) on the 9th cycle (index 8): 4608 + 8*1024 = 12800.
    for (int i = 0; i < 9; ++i) {
        r = loop.ProcessCycle(dev, 8, /*isData=*/true);
        dev = (dev + kCyc) % kSec;
    }

    EXPECT_TRUE(r.rebased);
    EXPECT_EQ(r.rebaseReason, TxOutputPhaseLoop::RebaseReason::kLeadReject);
    EXPECT_EQ(r.leadTicks, kLead);   // re-seeded back to the operating lead

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.glitches, 0u);
    EXPECT_EQ(diag.rebasesInitialSeed, 1u);
    EXPECT_EQ(diag.rebasesGlitch, 0u);
    EXPECT_EQ(diag.rebasesLeadReject, 1u);
    EXPECT_EQ(diag.rebases, diag.rebasesInitialSeed + diag.rebasesGlitch + diag.rebasesLeadReject);
}
