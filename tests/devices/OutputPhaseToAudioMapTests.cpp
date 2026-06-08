#include <gtest/gtest.h>
#include "AudioEngine/DirectIsoch/Timing/OutputPhaseToAudioMap.hpp"
#include "AudioEngine/DirectIsoch/Timing/TxOutputPhaseLoop.hpp"

using ASFW::AudioEngine::DirectIsoch::OutputPhaseToAudioMap;
using ASFW::AudioEngine::DirectIsoch::TxOutputPhaseLoop;

TEST(OutputPhaseToAudioMap, InitialStateIsUnanchored) {
    OutputPhaseToAudioMap map;
    EXPECT_FALSE(map.IsAnchored());
    EXPECT_EQ(map.PhaseToFrame(1000), 0u);
}

TEST(OutputPhaseToAudioMap, AnchorSetsState) {
    OutputPhaseToAudioMap map;
    map.Anchor(1000, 2000);
    EXPECT_TRUE(map.IsAnchored());
    EXPECT_EQ(map.PhaseToFrame(1000), 2000u);  // identity at the epoch
}

TEST(OutputPhaseToAudioMap, CorrectOffsetMapping) {
    OutputPhaseToAudioMap map;
    map.Anchor(0, 10000);
    EXPECT_EQ(map.PhaseToFrame(512), 10001u);    // +1 frame
    EXPECT_EQ(map.PhaseToFrame(3072), 10006u);   // +1 cycle (6 frames)
    EXPECT_EQ(map.PhaseToFrame(-3072), 9994u);   // -6 frames
}

// P4 regression: the phase is unwrapped, so the mapping stays exact for the full
// lifetime of the stream, not just the ±4s window the old ExtOffsetDiff allowed.
TEST(OutputPhaseToAudioMap, LargeUnwrappedSpanDoesNotWrap) {
    OutputPhaseToAudioMap map;
    map.Anchor(0, 100000);
    const int64_t tenSeconds = int64_t(10) * 24576000;  // > old 4s ceiling
    EXPECT_EQ(map.PhaseToFrame(tenSeconds), 100000u + static_cast<uint64_t>(tenSeconds / 512));
}

// P7 regression: a phase behind the epoch must clamp to 0, never underflow.
TEST(OutputPhaseToAudioMap, NegativeDeltaClampsToZero) {
    OutputPhaseToAudioMap map;
    map.Anchor(10000, 5);
    EXPECT_EQ(map.PhaseToFrame(0), 0u);
}

TEST(OutputPhaseToAudioMap, LargeAbsoluteFrameRange) {
    OutputPhaseToAudioMap map;
    const uint64_t largeFrame = 1ULL << 40;
    map.Anchor(0, largeFrame);
    EXPECT_EQ(map.PhaseToFrame(512 * 100), largeFrame + 100);
    EXPECT_EQ(map.PhaseToFrame(-512 * 100), largeFrame - 100);
}

TEST(OutputPhaseToAudioMap, ResetClearsAnchor) {
    OutputPhaseToAudioMap map;
    map.Anchor(1000, 2000);
    EXPECT_TRUE(map.IsAnchored());
    map.Reset();
    EXPECT_FALSE(map.IsAnchored());
    EXPECT_EQ(map.PhaseToFrame(1000), 0u);
}

// Mirrors PacketAssembler::BlockingCadence48k's 3:1 DATA:NO-DATA shape at
// 48 kHz / 8-frame packets -- the realistic dataCandidate stream the real call
// site feeds the loop (the assembler is cadence-authoritative; the loop tracks).
bool Cadence48kForMapTest(uint32_t i) {
    return (i % 4) != 3;
}

// Integration (P1+P2+P3): mirror tools/debug/tx_phase_loop_model.py. Drive the loop +
// map exactly as the pipeline does -- the CALLER's cadence (mirrored here by
// Cadence48kForMapTest, since the assembler is authoritative for the wire pattern --
// see TxOutputPhaseLoop's "WHO DECIDES THE WIRE" docs) produces the natural 3:1
// cadence and the loop just tracks it via dataCandidate; the device phase wraps
// every second, writtenEnd advances at 48k, and the map anchors at
// reportedPlayhead - safety. Coverage must hold across multiple seconds and the
// per-second wrap, and ONLY the initial seed may reset the map (lead-health
// NO-DATA reads must not).
TEST(TxTimelineIntegration, CoverageHoldsAcrossSecondsAndWrap) {
    TxOutputPhaseLoop loop;
    OutputPhaseToAudioMap map;

    constexpr int64_t kCyc = TxOutputPhaseLoop::kTicksPerCycle;
    constexpr int64_t kSec = TxOutputPhaseLoop::kOneSecondTicks;
    constexpr uint32_t kFpp = 8;
    constexpr uint32_t kFramesPerCycle = 6;
    constexpr uint64_t kRingCapacity = 4096;
    constexpr uint64_t kReportedOffset = 48;  // TimingCursorPolicy::CursorOffsetFrames(Output)
    constexpr uint64_t kSafety = 768;         // TimingCursorPolicy::PacketLeadFrames(Output)

    uint64_t writtenEnd = kRingCapacity * 2;  // a couple laps already written
    int64_t dev = 0;
    int dataPackets = 0;
    int covered = 0;
    int futureMiss = 0;

    for (uint32_t i = 0; i < 24000; ++i) {    // 3 seconds of isoch cycles
        writtenEnd += kFramesPerCycle;        // CoreAudio producer advances at 48k

        // The cadence decision is the CALLER's (PacketAssembler), already made;
        // the loop mirrors it into emitData and tracks the phase through it.
        auto r = loop.ProcessCycle(i, dev, /*recoveredClockValid=*/true,
                                   /*dataCandidate=*/Cadence48kForMapTest(i), kFpp);
        if (r.resetPhaseMap) {
            map.Reset();
        }

        if (r.emitData && !map.IsAnchored()) {
            const uint64_t reported =
                writtenEnd > kReportedOffset ? writtenEnd - kReportedOffset : 0;
            const uint64_t target = reported > kSafety ? reported - kSafety : 0;
            map.Anchor(r.outputPhaseTicks, (target / kFpp) * kFpp);
        }

        if (r.emitData && map.IsAnchored()) {
            ++dataPackets;
            const uint64_t audioFirst = map.PhaseToFrame(r.outputPhaseTicks);
            const uint64_t audioEnd = audioFirst + kFpp;
            const uint64_t oldestValid =
                writtenEnd > kRingCapacity ? writtenEnd - kRingCapacity : 0;
            if (audioFirst >= oldestValid && audioEnd <= writtenEnd) {
                ++covered;
            } else if (audioEnd > writtenEnd) {
                ++futureMiss;
            }
        }
        dev = (dev + kCyc) % kSec;            // device phase wraps every second
    }

    EXPECT_GT(dataPackets, 17000);
    ASSERT_GT(dataPackets, 0);
    EXPECT_GE(covered * 100.0 / dataPackets, 99.0);  // not silence after 1s
    EXPECT_EQ(futureMiss, 0);                        // anchor leaves margin (P3)
    EXPECT_EQ(loop.GetDiagnostics().timingDiscontinuities, 0u);  // no per-second glitch (P2)
    EXPECT_EQ(loop.GetDiagnostics().resets(), 1u);               // only the initial seed (P1)
}
