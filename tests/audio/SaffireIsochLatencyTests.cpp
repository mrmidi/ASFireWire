#include <gtest/gtest.h>

#include "Audio/Runtime/SaffireIsochLatency.hpp"
#include "Audio/Runtime/TimingCursorPolicy.hpp"

// Pins every cell of the lifted Saffire latency model (the
// Saffire::UpdateIsochBufferParams trace): the 4×6 delay-packet table with
// rate bumps, the DMA-program depth, frames-per-packet, the derived frame
// counts, and the TimingCursorPolicy derivation (single-sourcing the
// previously hand-planted 48/128 cursor offsets — no behavior change).

namespace {

using ASFW::Audio::AudioDirection;
using ASFW::Audio::SaffireIsochBufferParams;
using ASFW::Audio::SaffireIsochLatency;
using ASFW::Audio::SaffireLatencyMode;
using ASFW::Audio::TimingCursorPolicy;

TEST(SaffireIsochLatencyTests, BaseTableAt48k) {
    struct Row {
        SaffireLatencyMode mode;
        uint32_t input;
        uint32_t output;
    };
    constexpr Row kRows[4] = {
        {SaffireLatencyMode::kLowest, 14, 2},
        {SaffireLatencyMode::kLow, 16, 6},
        {SaffireLatencyMode::kMedium, 18, 10},
        {SaffireLatencyMode::kSafest, 20, 14},
    };
    for (const auto& row : kRows) {
        SaffireIsochBufferParams params{};
        ASSERT_TRUE(SaffireIsochLatency::Lookup(row.mode, 48000, params));
        EXPECT_EQ(params.inputDelayPackets, row.input);
        EXPECT_EQ(params.outputDelayPackets, row.output);
        EXPECT_EQ(params.framesPerDataPacket, 8u);
        EXPECT_EQ(params.dmaProgramDepthPackets, 160u);
    }
}

TEST(SaffireIsochLatencyTests, QuotedFrameFigures) {
    // Trace prose: 48 kHz lowest-mode output = 2 × 8 = 16 frames. The
    // prose's "safest = 160 frames" matches the INPUT column (20 × 8); the
    // table's output mode 3 is 14 × 8 = 112 — flagged in the header, the
    // table wins in code.
    SaffireIsochBufferParams lowest{};
    SaffireIsochBufferParams safest{};
    ASSERT_TRUE(
        SaffireIsochLatency::Lookup(SaffireLatencyMode::kLowest, 48000, lowest));
    ASSERT_TRUE(
        SaffireIsochLatency::Lookup(SaffireLatencyMode::kSafest, 48000, safest));
    EXPECT_EQ(lowest.OutputDelayFrames(), 16u);
    EXPECT_EQ(safest.OutputDelayFrames(), 112u);
    EXPECT_EQ(safest.InputDelayFrames(), 160u);
}

TEST(SaffireIsochLatencyTests, RateBumpsAndDepth) {
    struct RateRow {
        uint32_t rate;
        uint32_t bump;
        uint32_t framesPerPacket;
        uint32_t depth;
    };
    // Depth-in-time: 160/80/40 packets × 125 µs = 20/10/5 ms (the trace
    // prose's "constant ~20 ms" only matches the ≤48 k family — second
    // flagged discrepancy; the table wins).
    constexpr RateRow kRates[6] = {
        {44100, 0, 8, 160},  {48000, 0, 8, 160},  {88200, 2, 16, 80},
        {96000, 2, 16, 80},  {176400, 4, 32, 40}, {192000, 4, 32, 40},
    };
    for (const auto& rateRow : kRates) {
        SaffireIsochBufferParams params{};
        ASSERT_TRUE(SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow,
                                                rateRow.rate, params));
        EXPECT_EQ(params.inputDelayPackets, 16u + rateRow.bump);
        EXPECT_EQ(params.outputDelayPackets, 6u + rateRow.bump);
        EXPECT_EQ(params.framesPerDataPacket, rateRow.framesPerPacket);
        EXPECT_EQ(params.dmaProgramDepthPackets, rateRow.depth);
        EXPECT_EQ(params.DmaProgramDepthMicroseconds(), rateRow.depth * 125u);
    }
}

TEST(SaffireIsochLatencyTests, PacketCountedDelayIsRateScaled) {
    SaffireIsochBufferParams at48k{};
    SaffireIsochBufferParams at96k{};
    ASSERT_TRUE(
        SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow, 48000, at48k));
    ASSERT_TRUE(
        SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow, 96000, at96k));
    EXPECT_EQ(at48k.outputDelayPackets * 125u, 750u);  // µs of bus time
    EXPECT_EQ(at96k.outputDelayPackets * 125u, 1000u); // +2 bump applied
    EXPECT_EQ(at48k.OutputDelayFrames(), 48u);
    EXPECT_EQ(at96k.OutputDelayFrames(), 128u);
}

TEST(SaffireIsochLatencyTests, RejectsUnsupportedRates) {
    SaffireIsochBufferParams params{};
    EXPECT_FALSE(
        SaffireIsochLatency::Lookup(SaffireLatencyMode::kLowest, 22050, params));
    EXPECT_FALSE(SaffireIsochLatency::Lookup(SaffireLatencyMode::kLowest,
                                             384000, params));
}

TEST(SaffireIsochLatencyTests, TimingCursorPolicyDerivesFromTable) {
    // Single-sourcing check: the policy's cursor offsets and snapshot delay
    // packets must equal the table at the policy's mode — and stay at the
    // previously hand-planted values (48/128, 6/16). Compile-time
    // static_asserts in TimingCursorPolicy.hpp pin the same facts.
    constexpr auto policy = TimingCursorPolicy::MakeDice48kBlocking();
    EXPECT_EQ(policy.CursorOffsetFrames(AudioDirection::Output), 48u);
    EXPECT_EQ(policy.CursorOffsetFrames(AudioDirection::Input), 128u);

    const auto snapshot = policy.Snapshot();
    EXPECT_EQ(snapshot.outputDelayPackets, 6u);
    EXPECT_EQ(snapshot.inputDelayPackets, 16u);

    SaffireIsochBufferParams params{};
    ASSERT_TRUE(SaffireIsochLatency::Lookup(TimingCursorPolicy::LatencyMode(),
                                            policy.SampleRateHz(), params));
    EXPECT_EQ(policy.CursorOffsetFrames(AudioDirection::Output),
              params.OutputDelayFrames());
    EXPECT_EQ(policy.CursorOffsetFrames(AudioDirection::Input),
              params.InputDelayFrames());
}

} // namespace
