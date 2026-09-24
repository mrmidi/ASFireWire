// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDeviceIoTests.cpp - Blocking DICE register access against the simulated device.

#include <gtest/gtest.h>

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"

#include "Audio/Protocols/DICE/Core/DiceDeviceIo.hpp"

namespace {

using namespace ASFW::Testing::DICE;
using ASFW::Audio::DICE::DiceDeviceIo;
using ASFW::Testing::FakeDiceWaitClock;
using ASFW::Testing::FakeTimerScheduler;

struct IoRig {
    IoRig() : bus(DiceDeviceImages::kVeniceF24) {
        bus.Device().ResetToIdle();
    }

    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io{bus, bus, routeState.registry, routeState.route};
    DICETransaction transaction{io};
    FakeTimerScheduler timer;
    FakeDiceWaitClock clock{timer};
    DiceDeviceIo dio{io, transaction, clock};

    [[nodiscard]] uint32_t TxBase() const { return bus.Device().TxSectionBase(); }
    [[nodiscard]] uint32_t GlobalBase() const { return bus.Device().GlobalBase(); }
};

TEST(DiceDeviceIoTests, ReadQuadReturnsTheDeviceRegister) {
    IoRig rig;
    const auto count = rig.dio.ReadQuad(rig.TxBase() + TxOffset::kNumber);
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 2U);  // Venice F24: two capture streams
    EXPECT_EQ(rig.clock.Backoffs(), 0U) << "a synchronous completion never waits";
}

TEST(DiceDeviceIoTests, WriteQuadLandsInTheDevice) {
    IoRig rig;
    ASSERT_TRUE(rig.dio.WriteQuad(rig.GlobalBase() + GlobalOffset::kEnable, 1U).has_value());
    EXPECT_EQ(rig.bus.Device().Enable(), 1U);
}

TEST(DiceDeviceIoTests, CompareSwapReturnsThePreviousValue) {
    IoRig rig;
    const auto previous = rig.dio.CompareSwap64(rig.GlobalBase() + GlobalOffset::kOwnerHi,
                                                kOwnerNoOwner, 0xFFC0000100000000ULL);
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(*previous, kOwnerNoOwner);
    EXPECT_EQ(rig.bus.Device().Owner(), 0xFFC0000100000000ULL);
}

TEST(DiceDeviceIoTests, ReadBlockReturnsTheBytes) {
    IoRig rig;
    const auto table = rig.dio.ReadBlock(0, 40);
    ASSERT_TRUE(table.has_value());
    ASSERT_EQ(table->size(), 40U);
    EXPECT_EQ(ASFW::FW::ReadBE32(table->data()), 0x0AU);  // global section offset
}

TEST(DiceDeviceIoTests, TransactionReadsReuseTheParsers) {
    IoRig rig;
    const auto sections = rig.dio.ReadGeneralSections();
    ASSERT_TRUE(sections.has_value());
    EXPECT_EQ(sections->global.offset, 0x28U);
    const auto tx = rig.dio.ReadTxStreamConfig(*sections);
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(tx->numStreams, 2U);
    EXPECT_EQ(tx->TotalPcmChannels(), 24U);
    const auto global = rig.dio.ReadGlobalStateFull(*sections);
    ASSERT_TRUE(global.has_value());
    EXPECT_EQ(global->sampleRate, 48000U);
}

TEST(DiceDeviceIoTests, TransportFailureIsReturnedNotHidden) {
    IoRig rig;
    rig.bus.FailNext(OpKind::Read, kDiceBaseAddressLo + rig.TxBase(), AsyncStatus::kTimeout);
    const auto count = rig.dio.ReadQuad(rig.TxBase() + TxOffset::kNumber);
    ASSERT_FALSE(count.has_value());
    EXPECT_EQ(count.error(), kIOReturnTimeout);
}

TEST(DiceDeviceIoTests, StaleGenerationFails) {
    IoRig rig;
    rig.bus.BusReset();
    const auto count = rig.dio.ReadQuad(rig.TxBase() + TxOffset::kNumber);
    ASSERT_FALSE(count.has_value());
    EXPECT_NE(count.error(), kIOReturnSuccess);
}

TEST(DiceDeviceIoTests, MissingCompletionHitsTheSafetyDeadlineInsteadOfHanging) {
    IoRig rig;
    rig.bus.DropNext(OpKind::Read, kDiceBaseAddressLo + rig.TxBase());
    const uint64_t before = rig.timer.NowNs();
    const auto count = rig.dio.ReadQuad(rig.TxBase() + TxOffset::kNumber);
    ASSERT_FALSE(count.has_value());
    EXPECT_EQ(count.error(), kIOReturnTimeout);
    EXPECT_EQ((rig.timer.NowNs() - before) / 1'000'000ULL, DiceDeviceIo::kSafetyDeadlineMs);
}

} // namespace
