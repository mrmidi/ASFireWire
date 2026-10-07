// SPDX-License-Identifier: Apache-2.0
#include "DICEDuplexTestSupport.hpp"
#include "Audio/Runtime/RateValidation.hpp"

using namespace ASFW::Testing::DICE;

TEST(DiceMultirateClockTests, BatchProgramsAndConfirmsEveryStandardRate) {
    static_assert(ASFW::Audio::Runtime::kDiceHardwareBatch);
    for (const auto& rate : ASFW::Audio::DICE::kDiceRateTable) {
        DuplexRig rig;
        uint32_t selection = 0;
        ASSERT_TRUE(ASFW::Audio::DICE::DiceClockSelectForRate(rate.hz, ClockSource::Internal, selection));
        const auto applied = rig.driver.ApplyClock({rate.hz, selection});
        ASSERT_TRUE(applied) << rate.hz << " status " << (applied ? 0 : applied.error());
        EXPECT_EQ(applied->appliedClock.sampleRateHz, rate.hz);
        EXPECT_EQ(applied->runtimeCaps.sampleRateHz, rate.hz);
        EXPECT_FALSE(rig.driver.IsRunning());
        EXPECT_EQ(rig.bus.Enable(), 0);
    }
}

TEST(DiceMultirateClockTests, RequestedHighRateRequiresActualReadback) {
    DuplexRig rig;
    rig.bus.SetClockSelectWriteHandler([&] {
        rig.bus.SetGlobalClockState(StatusBits::kSourceLocked |
            (ClockRateIndex::k48000 << StatusBits::kNominalRateShift), 48000, NotifyBits::kClockAccepted);
    });
    uint32_t selection = 0;
    ASSERT_TRUE(ASFW::Audio::DICE::DiceClockSelectForRate(96000, ClockSource::Internal, selection));
    const auto applied = rig.driver.ApplyClock({96000, selection});
    // CLOCK_ACCEPTED is advisory. The family reports achieved hardware state,
    // and the shared transaction classifies this as unchanged, not 96 kHz.
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->appliedClock.sampleRateHz, 48000);
    EXPECT_EQ(applied->runtimeCaps.sampleRateHz, 48000);
    EXPECT_FALSE(rig.driver.IsRunning());
    EXPECT_EQ(rig.bus.Enable(), 0);
}
