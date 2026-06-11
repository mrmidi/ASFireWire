#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Wire/AMDTP/TxTimingModel.hpp"
#include "SimulatedCycleTimeline.hpp"

namespace ASFW::Testing {

using Driver::TxTimingModel;
using LeadHealth = TxTimingModel::LeadHealth;

TEST(TxTimingModelTests, SeedAndGraftExactValues) {
    SimulatedCycleTimeline timeline{0};
    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    const auto first = model.PeekNextDataSyt(timeline);
    EXPECT_TRUE(first.seededThisCall);
    EXPECT_EQ(first.syt, 0x10B0);
    EXPECT_EQ(first.leadTicks, 3248);
    EXPECT_EQ(first.health, LeadHealth::kAccepted);

    const auto again = model.PeekNextDataSyt(timeline);
    EXPECT_FALSE(again.seededThisCall);
    EXPECT_EQ(again.syt, 0x10B0);
}

TEST(TxTimingModelTests, StepSequence) {
    SimulatedCycleTimeline timeline{0};
    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    constexpr uint16_t kExpected[12] = {
        0x10B0, 0x24B0, 0x38B0, 0x50B0, 0x64B0, 0x78B0,
        0x90B0, 0xA4B0, 0xB8B0, 0xD0B0, 0xE4B0, 0xF8B0,
    };
    for (int i = 0; i < 12; ++i) {
        const auto decision = model.PeekNextDataSyt(timeline);
        EXPECT_EQ(decision.syt, kExpected[i]);
        model.CommitDataPacket();
    }
    EXPECT_EQ(model.PeekNextDataSyt(timeline).syt, 0x10B0);
}

TEST(TxTimingModelTests, LeadHealthBands) {
    SimulatedCycleTimeline timeline{0};
    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    auto leadTo = [&](int64_t target) {
        const auto current = model.PeekNextDataSyt(timeline);
        model.NudgeOffsetTicks(static_cast<int32_t>(target - current.leadTicks));
        return model.PeekNextDataSyt(timeline);
    };

    EXPECT_EQ(leadTo(3071).health, LeadHealth::kTightWarn);
    EXPECT_EQ(leadTo(3072).health, LeadHealth::kAccepted);
    EXPECT_EQ(leadTo(7619).health, LeadHealth::kAccepted);
    EXPECT_EQ(leadTo(7620).health, LeadHealth::kGate);
    EXPECT_EQ(leadTo(12286).health, LeadHealth::kGate);
    EXPECT_EQ(leadTo(12287).health, LeadHealth::kEscalate);
    EXPECT_EQ(leadTo(0).health, LeadHealth::kTightWarn);
    EXPECT_EQ(leadTo(-1).health, LeadHealth::kLate);
}

TEST(TxTimingModelTests, GraftDisabled) {
    SimulatedCycleTimeline timeline{0};
    TxTimingModel model{};
    TxTimingModel::Config config{};
    config.graftEnabled = false;
    model.Configure(config);

    const auto decision = model.PeekNextDataSyt(timeline);
    EXPECT_EQ(decision.syt, 0x1400);
    EXPECT_EQ(decision.leadTicks, 4096);
}

TEST(TxTimingModelTests, MonotonicGuard) {
    SimulatedCycleTimeline timeline{0};
    timeline.AdvanceTicks(3500);
    TxTimingModel model{};
    TxTimingModel::Config config{};
    config.presentationDelayTicks = 100;
    model.Configure(config);

    const auto decision = model.PeekNextDataSyt(timeline);
    EXPECT_EQ(model.OutputPhaseTicks(), 6320);
    EXPECT_EQ(decision.leadTicks, 2820);
    EXPECT_EQ(decision.syt & 0xFFFu, 0x0B0);
    EXPECT_EQ(decision.health, LeadHealth::kTightWarn);
}

TEST(TxTimingModelTests, ReArmReseeds) {
    SimulatedCycleTimeline timeline{0};
    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});

    (void)model.PeekNextDataSyt(timeline);
    model.CommitDataPacket();

    timeline.AdvanceToFrame(48000);
    model.ArmTransmitCycleAnchor();
    const auto reseeded = model.PeekNextDataSyt(timeline);
    EXPECT_TRUE(reseeded.seededThisCall);
    EXPECT_EQ(reseeded.syt & 0xFFFu, 0x0B0);
    EXPECT_TRUE(reseeded.leadTicks > 0 &&
                reseeded.leadTicks <
                    TxTimingModel::Config{}.acceptLeadTicks);
}

TEST(TxTimingModelTests, NotSeededBeforeFirstPeek) {
    TxTimingModel model{};
    model.Configure(TxTimingModel::Config{});
    EXPECT_FALSE(model.IsSeeded());
    model.CommitDataPacket();
    EXPECT_EQ(model.OutputPhaseTicks(), 0);
}

} // namespace ASFW::Testing
