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
    // Acceptance at the old achieved rate cannot prepare the next StartIO.
    // Refuse the transaction instead of publishing an unready clock.
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error(), kIOReturnTimeout);
    EXPECT_FALSE(rig.driver.IsRunning());
    EXPECT_EQ(rig.bus.Enable(), 0);
}

TEST(DiceMultirateClockTests, LockedAdatUsesConfiguredRateDespiteCounterDrift) {
    using namespace ASFW::Audio::DICE;
    DuplexRig rig;
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, static_cast<uint32_t>(ClockSource::ADAT));
    for (const auto rate : {48000U, 96000U, 48000U}) {
        uint32_t selection = 0;
        ASSERT_TRUE(DiceClockSelectForRate(rate, ClockSource::ADAT, selection));
        rig.bus.SetClockSelectWriteHandler([&] {
            rig.bus.SetGlobalClockState(StatusBits::kSourceLocked | (selection & ClockSelect::kRateMask),
                                       rate + 93, NotifyBits::kClockAccepted);
        });
        const auto applied = rig.driver.ApplyClock({rate, selection});
        ASSERT_TRUE(applied) << rate << " status " << (applied ? 0 : applied.error());
        EXPECT_EQ(applied->appliedClock.sampleRateHz, rate);
        EXPECT_EQ(applied->runtimeCaps.sampleRateHz, rate);
        EXPECT_EQ(rig.bus.Enable(), 0);
    }
    const auto locked96 = StatusBits::kSourceLocked | (ClockRateIndex::k96000 << StatusBits::kNominalRateShift);
    EXPECT_EQ(DiceConfiguredRate(96093, locked96, 0x405), 96000U);
    EXPECT_EQ(DiceConfiguredRate(96093, locked96, 0x205), 0U);
    EXPECT_EQ(DiceConfiguredRate(96093, locked96, 0x40c), 96093U);
    // Configuration can remain known while the clock is unlocked; this is not admission.
    EXPECT_EQ(DiceConfiguredRate(96093, 0, 0x205), 48000U);
}

TEST(DiceMultirateClockTests, UnlockedSourceCannotApplyClockOrEnableDuplex) {
    using namespace ASFW::Audio::DICE;
    for (const auto source : {ClockSource::Internal, ClockSource::ADAT}) {
        uint32_t selection = 0;
        ASSERT_TRUE(DiceClockSelectForRate(48000, source, selection));
        {
            DuplexRig rig;
            rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, static_cast<uint32_t>(source));
            rig.bus.SetClockSelectWriteHandler([&] {
                rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
                                           48000, NotifyBits::kClockAccepted);
            });
            EXPECT_FALSE(rig.driver.ApplyClock({48000, selection}));
            EXPECT_FALSE(rig.driver.IsRunning());
            EXPECT_EQ(rig.bus.Enable(), 0);
        }
        {
            DuplexRig rig;
            rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, static_cast<uint32_t>(source));
            rig.bus.SetClockSelectWriteHandler([&] {
                rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift,
                                           48000, NotifyBits::kClockAccepted);
            });
            const ASFW::Audio::AudioDuplexChannels channels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0};
            EXPECT_FALSE(rig.driver.Prepare(channels, {48000, selection}, true));
            EXPECT_FALSE(rig.driver.IsRunning());
            EXPECT_EQ(rig.bus.Enable(), 0);
        }
    }
}

TEST(DiceMultirateClockTests, LockedAdatCanStartButLockLossStillFailsConfirmation) {
    using namespace ASFW::Audio::DICE;
    DuplexRig rig;
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, static_cast<uint32_t>(ClockSource::ADAT));
    const ASFW::Audio::AudioDuplexChannels channels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0};
    // A rate-only caller's Internal placeholder must not replace the live ADAT source.
    ASSERT_TRUE(rig.driver.Prepare(channels, {48000, 0x20c}, true));
    EXPECT_EQ(rig.bus.Device().GlobalQuad(GlobalOffset::kClockSelect), 0x205U);
    ASSERT_TRUE(rig.driver.ProgramRx());
    ASSERT_TRUE(rig.driver.ProgramTxAndEnable());
    rig.bus.SetGlobalClockState(ClockRateIndex::k48000 << StatusBits::kNominalRateShift, 48000, 0);
    EXPECT_FALSE(rig.driver.Confirm());
    EXPECT_FALSE(rig.driver.IsRunning());
    EXPECT_EQ(rig.bus.Enable(), 0);
}

TEST(DiceMultirateClockTests, OtherExternalSourcesAreNotAdmittedByThisIncrement) {
    DuplexRig rig;
    EXPECT_FALSE(rig.driver.ApplyClock({48000, 0x207})); // Word Clock
    EXPECT_FALSE(rig.driver.ApplyClock({48000, 0x208})); // FireWire receive stream
    EXPECT_EQ(rig.bus.Enable(), 0);
    for (const auto selection : {0x207U, 0x208U}) {
        DuplexRig current;
        current.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, selection);
        EXPECT_FALSE(current.driver.ApplyClock({48000, 0x20c}));
        EXPECT_EQ(current.bus.Device().GlobalQuad(GlobalOffset::kClockSelect), selection);
        EXPECT_TRUE(std::ranges::all_of(current.bus.Operations(), [](const auto& op) {
            return op.kind == OpKind::Read;
        }));
    }
}
