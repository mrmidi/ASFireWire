#include "TestHarness.hpp"

#include "../Core/TxTimingModel.hpp"
#include "../Lab/SimulatedCycleTimeline.hpp"

// P5 rehearsal at the unit level: the Saffire SYT contract — seed + constant
// device sub-cycle graft (0x0B0), fixed 4096-tick step in the 16-cycle
// domain (the +1,+1,+2 cycle pattern at 48 kHz blocking), and the
// transmit-lead health bands (tight ≤ 3071 / accept < 7620 / gate ≥ 7620 /
// escalate ≥ 12287). Constants per SYTGenerator.hpp, TxOutputPhaseLoop.hpp,
// and tools/debug/tx_phase_loop_model.py in the DICE tree.

namespace ASFW::LabTests {

using Driver::TxTimingModel;
using Lab::SimulatedCycleTimeline;

void RunTxTimingModelTests(TestContext& ctx) {
    using LeadHealth = TxTimingModel::LeadHealth;

    // Seed + graft, exact values: now=0, delay 4096 → 4096-1024+0x0B0 = 3248
    // → SYT cycle 1, offset 0x0B0 → 0x10B0, lead 3248 (accepted).
    {
        SimulatedCycleTimeline timeline{0};
        TxTimingModel model{};
        model.Configure(TxTimingModel::Config{});

        const auto first = model.PeekNextDataSyt(timeline);
        CHECK(ctx, first.seededThisCall);
        CHECK_EQ_U32(ctx, first.syt, 0x10B0);
        CHECK_EQ_U64(ctx, static_cast<uint64_t>(first.leadTicks), 3248);
        CHECK(ctx, first.health == LeadHealth::kAccepted);

        // Peek is pure: same answer until a commit.
        const auto again = model.PeekNextDataSyt(timeline);
        CHECK(ctx, !again.seededThisCall);
        CHECK_EQ_U32(ctx, again.syt, 0x10B0);
    }

    // Step sequence: +4096 per committed data packet → offsets walk
    // {0x0B0, 0x4B0, 0x8B0}, cycles advance +1,+1,+2; 12 packets close the
    // 16-cycle (49152-tick) domain exactly.
    {
        SimulatedCycleTimeline timeline{0};
        TxTimingModel model{};
        model.Configure(TxTimingModel::Config{});

        constexpr uint16_t kExpected[12] = {
            0x10B0, 0x24B0, 0x38B0, 0x50B0, 0x64B0, 0x78B0,
            0x90B0, 0xA4B0, 0xB8B0, 0xD0B0, 0xE4B0, 0xF8B0,
        };
        for (int i = 0; i < 12; ++i) {
            const auto decision = model.PeekNextDataSyt(timeline);
            CHECK_EQ_U32(ctx, decision.syt, kExpected[i]);
            model.CommitDataPacket();
        }
        // Wrap: the 13th packet repeats the first SYT.
        CHECK_EQ_U32(ctx, model.PeekNextDataSyt(timeline).syt, 0x10B0);
    }

    // Lead-health bands at exact boundaries, driven via NudgeOffsetTicks.
    {
        SimulatedCycleTimeline timeline{0};
        TxTimingModel model{};
        model.Configure(TxTimingModel::Config{});

        auto leadTo = [&](int64_t target) {
            const auto current = model.PeekNextDataSyt(timeline);
            model.NudgeOffsetTicks(static_cast<int32_t>(target - current.leadTicks));
            return model.PeekNextDataSyt(timeline);
        };

        CHECK(ctx, leadTo(3071).health == LeadHealth::kTightWarn);
        CHECK(ctx, leadTo(3072).health == LeadHealth::kAccepted);
        CHECK(ctx, leadTo(7619).health == LeadHealth::kAccepted);
        CHECK(ctx, leadTo(7620).health == LeadHealth::kGate);
        CHECK(ctx, leadTo(12286).health == LeadHealth::kGate);
        CHECK(ctx, leadTo(12287).health == LeadHealth::kEscalate);
        CHECK(ctx, leadTo(0).health == LeadHealth::kTightWarn);
        CHECK(ctx, leadTo(-1).health == LeadHealth::kLate);
    }

    // Graft disabled: phase = now + delay exactly (offset 1024 → 0x1400).
    {
        SimulatedCycleTimeline timeline{0};
        TxTimingModel model{};
        TxTimingModel::Config config{};
        config.graftEnabled = false;
        model.Configure(config);

        const auto decision = model.PeekNextDataSyt(timeline);
        CHECK_EQ_U32(ctx, decision.syt, 0x1400);
        CHECK_EQ_U64(ctx, static_cast<uint64_t>(decision.leadTicks), 4096);
    }

    // Monotonic guard: when the graft would land at/behind "now", whole
    // cycles are re-added until the lead is positive (sub-cycle preserved).
    {
        SimulatedCycleTimeline timeline{0};
        timeline.AdvanceTicks(3500);
        TxTimingModel model{};
        TxTimingModel::Config config{};
        config.presentationDelayTicks = 100; // 3600 → graft 3248 ≤ now → 6320
        model.Configure(config);

        const auto decision = model.PeekNextDataSyt(timeline);
        CHECK_EQ_U64(ctx, static_cast<uint64_t>(model.OutputPhaseTicks()), 6320);
        CHECK_EQ_U64(ctx, static_cast<uint64_t>(decision.leadTicks), 2820);
        CHECK_EQ_U32(ctx, decision.syt & 0xFFFu, 0x0B0);
        CHECK(ctx, decision.health == LeadHealth::kTightWarn);
    }

    // Re-arm: a fresh anchor reseeds from the current timeline position with
    // the graft lattice intact.
    {
        SimulatedCycleTimeline timeline{0};
        TxTimingModel model{};
        model.Configure(TxTimingModel::Config{});

        (void)model.PeekNextDataSyt(timeline);
        model.CommitDataPacket();

        timeline.AdvanceToFrame(48000); // one second later
        model.ArmTransmitCycleAnchor();
        const auto reseeded = model.PeekNextDataSyt(timeline);
        CHECK(ctx, reseeded.seededThisCall);
        CHECK_EQ_U32(ctx, reseeded.syt & 0xFFFu, 0x0B0);
        CHECK(ctx, reseeded.leadTicks > 0 &&
                       reseeded.leadTicks <
                           TxTimingModel::Config{}.acceptLeadTicks);
    }

    // Not seeded before the first peek.
    {
        TxTimingModel model{};
        model.Configure(TxTimingModel::Config{});
        CHECK(ctx, !model.IsSeeded());
        model.CommitDataPacket(); // no-op while unseeded
        CHECK_EQ_U64(ctx, static_cast<uint64_t>(model.OutputPhaseTicks()), 0);
    }
}

} // namespace ASFW::LabTests
