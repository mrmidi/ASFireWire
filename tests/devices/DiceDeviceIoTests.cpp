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

// The TCAT drivers refuse TX_NUMBER >= 3 and RX_NUMBER > 4 (PopulateDeviceStruct
// in the TCAT SDK kexts). A count outside that fails the read; it is never
// clamped into a smaller device.
TEST(DiceDeviceIoTests, StreamCountsAboveTheVendorLimitsAreRefused) {
    IoRig rig;
    const auto sections = rig.dio.ReadGeneralSections();
    ASSERT_TRUE(sections.has_value());

    rig.bus.Device().SetTxCount(3);
    const auto tx = rig.dio.ReadTxStreamConfig(*sections);
    ASSERT_FALSE(tx.has_value());
    EXPECT_EQ(tx.error(), kIOReturnUnsupported);

    rig.bus.Device().SetRxCount(5);
    const auto rx = rig.dio.ReadRxStreamConfig(*sections);
    ASSERT_FALSE(rx.has_value());
    EXPECT_EQ(rx.error(), kIOReturnUnsupported);
}

TEST(DiceDeviceIoTests, FourRxStreamsAreAccepted) {
    IoRig rig;
    const auto sections = rig.dio.ReadGeneralSections();
    ASSERT_TRUE(sections.has_value());
    rig.bus.Device().SetRxCount(4);  // the Venice RX section holds four entries
    const auto rx = rig.dio.ReadRxStreamConfig(*sections);
    ASSERT_TRUE(rx.has_value());
    EXPECT_EQ(rx->numStreams, 4U);
}

// A section too short for the declared streams must fail the read, not shrink
// the device to the streams that fit.
TEST(DiceDeviceIoTests, TruncatedStreamCoresFailTheRead) {
    IoRig rig;
    auto sections = rig.dio.ReadGeneralSections();
    ASSERT_TRUE(sections.has_value());
    rig.bus.Device().SetRxCount(4);
    // Stream 2's core starts at byte 8 + 2 * 280 = 568.
    sections->rxStreamFormat.size = 568;
    const auto rx = rig.dio.ReadRxStreamConfig(*sections);
    ASSERT_FALSE(rx.has_value());
    EXPECT_EQ(rx.error(), kIOReturnUnderrun);
}

// As in the TCAT drivers, a failed read of any part of the stream section,
// names included, fails the device (PopulateTxStruct -> PopulateDeviceStruct).
TEST(DiceDeviceIoTests, FailedSectionChunkFailsTheRead) {
    IoRig rig;
    const auto sections = rig.dio.ReadGeneralSections();
    ASSERT_TRUE(sections.has_value());
    // Stream 1's core (bytes 288..304) is in the first 512-byte chunk; its
    // label blob is in the second.
    rig.bus.FailNext(OpKind::Read, kDiceBaseAddressLo + sections->txStreamFormat.offset + 512,
                     AsyncStatus::kTimeout);
    const auto tx = rig.dio.ReadTxStreamConfig(*sections);
    ASSERT_FALSE(tx.has_value());
    EXPECT_EQ(tx.error(), kIOReturnTimeout);
}

// Labels past the end of the section keep empty labels: stream 1's core
// (bytes 288..304) is inside the section, its label blob is not.
TEST(DiceDeviceIoTests, LabelsPastTheSectionKeepTheStream) {
    IoRig rig;
    auto sections = rig.dio.ReadGeneralSections();
    ASSERT_TRUE(sections.has_value());
    sections->txStreamFormat.size = 304;
    const auto tx = rig.dio.ReadTxStreamConfig(*sections);
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(tx->numStreams, 2U);
    EXPECT_EQ(tx->TotalPcmChannels(), 24U);
    EXPECT_EQ(tx->streams[1].labels[0], '\0');
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
