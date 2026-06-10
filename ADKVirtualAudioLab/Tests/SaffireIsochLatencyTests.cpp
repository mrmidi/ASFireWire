#include "TestHarness.hpp"

#include "../Protocols/Audio/DICE/SaffireIsochLatency.hpp"

// Pins every cell of the lifted Saffire latency model (the
// UpdateIsochBufferParams trace): the 4×6 delay-packet table with rate
// bumps, the DMA-program depth, frames-per-packet, and the derived
// safety-offset frame counts the prose quotes.

namespace ASFW::LabTests {

using Protocols::Audio::DICE::SaffireIsochBufferParams;
using Protocols::Audio::DICE::SaffireIsochLatency;
using Protocols::Audio::DICE::SaffireLatencyMode;

void RunSaffireIsochLatencyTests(TestContext& ctx) {
    // The full base table at 48 kHz (no rate bump, 8 frames/packet).
    {
        struct Row final {
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
            CHECK(ctx, SaffireIsochLatency::Lookup(row.mode, 48000, params));
            CHECK_EQ_U32(ctx, params.inputDelayPackets, row.input);
            CHECK_EQ_U32(ctx, params.outputDelayPackets, row.output);
            CHECK_EQ_U32(ctx, params.framesPerDataPacket, 8);
            CHECK_EQ_U32(ctx, params.dmaProgramDepthPackets, 160);
        }
    }

    // The trace's quoted figures: 48 kHz lowest-mode output = 2 × 8 = 16
    // frames; safest-mode output per the TABLE = 14 × 8 = 112 frames (the
    // prose's "160 frames" matches the INPUT column, 20 × 8 — flagged in the
    // header for adjudication).
    {
        SaffireIsochBufferParams lowest{};
        SaffireIsochBufferParams safest{};
        CHECK(ctx, SaffireIsochLatency::Lookup(SaffireLatencyMode::kLowest,
                                               48000, lowest));
        CHECK(ctx, SaffireIsochLatency::Lookup(SaffireLatencyMode::kSafest,
                                               48000, safest));
        CHECK_EQ_U32(ctx, lowest.OutputSafetyOffsetFrames(), 16);
        CHECK_EQ_U32(ctx, safest.OutputSafetyOffsetFrames(), 112);
        CHECK_EQ_U32(ctx, safest.InputSafetyOffsetFrames(), 160);
    }

    // Rate bumps apply to BOTH columns; frames-per-packet and DMA depth
    // follow the rate family.
    {
        struct RateRow final {
            uint32_t rate;
            uint32_t bump;
            uint32_t framesPerPacket;
            uint32_t depth;
        };
        // Depth-in-time: 160/80/40 packets × 125 µs = 20/10/5 ms. The trace
        // prose calls this "a constant ~20 ms hardware buffer regardless of
        // rate", which only matches the ≤48 k family — second prose/table
        // discrepancy flagged for adjudication; the table wins here too.
        constexpr RateRow kRates[6] = {
            {44100, 0, 8, 160},  {48000, 0, 8, 160},  {88200, 2, 16, 80},
            {96000, 2, 16, 80},  {176400, 4, 32, 40}, {192000, 4, 32, 40},
        };
        for (const auto& rateRow : kRates) {
            SaffireIsochBufferParams params{};
            CHECK(ctx, SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow,
                                                   rateRow.rate, params));
            CHECK_EQ_U32(ctx, params.inputDelayPackets, 16 + rateRow.bump);
            CHECK_EQ_U32(ctx, params.outputDelayPackets, 6 + rateRow.bump);
            CHECK_EQ_U32(ctx, params.framesPerDataPacket,
                         rateRow.framesPerPacket);
            CHECK_EQ_U32(ctx, params.dmaProgramDepthPackets, rateRow.depth);
            CHECK_EQ_U32(ctx, params.DmaProgramDepthMicroseconds(),
                         rateRow.depth * 125u);
        }
    }

    // Packet-counted delay means the time-domain offset is rate-independent
    // until the bump: mode-low output = 6 packets = 750 µs at 48 k; at 96 k
    // it is (6+2) packets but each packet still spans 125 µs of bus time.
    {
        SaffireIsochBufferParams at48k{};
        SaffireIsochBufferParams at96k{};
        CHECK(ctx, SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow, 48000,
                                               at48k));
        CHECK(ctx, SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow, 96000,
                                               at96k));
        CHECK_EQ_U32(ctx, at48k.outputDelayPackets * 125u, 750);
        CHECK_EQ_U32(ctx, at96k.outputDelayPackets * 125u, 1000);
        // Frame counts scale with rate so the packet model holds:
        CHECK_EQ_U32(ctx, at48k.OutputSafetyOffsetFrames(), 48);
        CHECK_EQ_U32(ctx, at96k.OutputSafetyOffsetFrames(), 128);
    }

    // Unsupported rates are rejected, never guessed.
    {
        SaffireIsochBufferParams params{};
        CHECK(ctx, !SaffireIsochLatency::Lookup(SaffireLatencyMode::kLowest,
                                                22050, params));
        CHECK(ctx, !SaffireIsochLatency::Lookup(SaffireLatencyMode::kLowest,
                                                384000, params));
    }

    // The table is constexpr-usable (compile-time single source of truth).
    {
        constexpr auto kCompileTime = [] {
            SaffireIsochBufferParams params{};
            (void)SaffireIsochLatency::Lookup(SaffireLatencyMode::kLow, 48000,
                                              params);
            return params;
        }();
        static_assert(kCompileTime.OutputSafetyOffsetFrames() == 48,
                      "mode 1 output offset at 48 kHz must be 48 frames");
        CHECK_EQ_U32(ctx, kCompileTime.outputDelayPackets, 6);
    }
}

} // namespace ASFW::LabTests
