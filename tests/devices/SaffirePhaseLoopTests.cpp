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
    EXPECT_EQ(r0.outputPhaseTicks, kLead);                // dev(0) + lead

    auto r1 = loop.ProcessCycle(kCyc, 8, /*isData=*/true);
    EXPECT_FALSE(r1.rebased);
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
}

// A real device-phase jump is a discontinuity: re-seed at device + operating lead.
TEST(TxOutputPhaseLoop, GlitchRecovery) {
    TxOutputPhaseLoop loop;
    (void)loop.ProcessCycle(1000, 8, /*isData=*/true);     // seed
    auto r = loop.ProcessCycle(1000 + 50000, 8, /*isData=*/true);  // device jump

    const auto diag = loop.GetDiagnostics();
    EXPECT_EQ(diag.glitches, 1u);
    EXPECT_TRUE(r.rebased);
    EXPECT_EQ(r.leadTicks, kLead);
    EXPECT_EQ(r.outputPhaseTicks, 51000 + kLead);
}
