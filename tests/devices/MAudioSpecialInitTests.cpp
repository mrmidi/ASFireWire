// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Hardware-free wire test for the FireWire 1814 / ProjectMix blank-slate
// sequence. It uses the real FCP transport and a deterministic target rather
// than replacing either command submission path with a mock.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioClockCommand.hpp"
#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioSpecialProtocol.hpp"

#include "AvcTestRig.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using ASFW::Audio::BeBoB::MAudioSpecialModel;
using ASFW::Audio::BeBoB::MAudioSpecialProtocol;
using ASFW::Protocols::AVC::FCPFrame;
using ASFW::Testing::AvcReply;
using ASFW::Testing::AvcTestRig;

constexpr uint64_t Milliseconds(uint64_t value) {
    return value * 1'000'000ULL;
}

template <size_t N>
void ExpectFrame(const FCPFrame& actual,
                 const std::array<uint8_t, N>& expected) {
    ASSERT_EQ(actual.length, expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(actual.data[index], expected[index]) << "byte " << index;
    }
}

TEST(MAudioSpecialInitTests, ReplaysVendorBlankSlateSequenceAndTiming) {
    AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());

    MAudioSpecialProtocol protocol(
        rig.Bus(), rig.Bus(), rig.Route(), nullptr, nullptr, &rig.Timers(),
        MAudioSpecialModel::FireWire1814);
    protocol.UpdateRuntimeContext(rig.Route(), rig.Transport());

    bool completed = false;
    IOReturn completionStatus = kIOReturnBusy;
    protocol.InitializeClock([&](IOReturn status) {
        completed = true;
        completionStatus = status;
    });

    // Resolve the initial vendor command. Its accepted response starts the
    // 300 ms interlock but must not enqueue the selector early.
    EXPECT_EQ(rig.Drain(), 1U);
    ASSERT_EQ(rig.Target().CommandCount(), 1U);
    constexpr std::array<uint8_t, 16> clockFrame{
        0x00, 0xFF, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ExpectFrame(rig.Target().Commands()[0], clockFrame);
    EXPECT_FALSE(completed);

    rig.Timers().Advance(Milliseconds(299));
    EXPECT_EQ(rig.Drain(), 0U);
    EXPECT_EQ(rig.Target().CommandCount(), 1U);

    // At 300 ms the selector is submitted. The AVCCdb encoder must produce the
    // exact 12-byte frame observed by FireBug, including quadlet padding.
    rig.Timers().Advance(Milliseconds(1));
    EXPECT_EQ(rig.Drain(), 1U);
    ASSERT_EQ(rig.Target().CommandCount(), 2U);
    constexpr std::array<uint8_t, 12> selectorFrame{
        0x00, 0x08, 0xB8, 0x80, 0x04, 0x10,
        0x02, 0x00, 0x01, 0x00, 0x00, 0x00};
    ExpectFrame(rig.Target().Commands()[1], selectorFrame);
    EXPECT_FALSE(completed);

    // No wire operation separates the kext's second 300 ms sleep from its
    // outer 2500 ms sleep, so ASFW coalesces them into one 2800 ms timer.
    rig.Timers().Advance(Milliseconds(2799));
    EXPECT_FALSE(completed);
    rig.Timers().Advance(Milliseconds(1));
    EXPECT_TRUE(completed);
    EXPECT_EQ(completionStatus, kIOReturnSuccess);
}

TEST(MAudioSpecialInitTests, SelectorRefusalFailsInitializationWithoutSettle) {
    AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());
    rig.Target().Script(AvcReply::Accepted());
    rig.Target().Script(AvcReply::Rejected());

    MAudioSpecialProtocol protocol(
        rig.Bus(), rig.Bus(), rig.Route(), nullptr, nullptr, &rig.Timers(),
        MAudioSpecialModel::FireWire1814);
    protocol.UpdateRuntimeContext(rig.Route(), rig.Transport());

    bool completed = false;
    IOReturn completionStatus = kIOReturnSuccess;
    protocol.InitializeClock([&](IOReturn status) {
        completed = true;
        completionStatus = status;
    });

    EXPECT_EQ(rig.Drain(), 1U);
    rig.Timers().Advance(Milliseconds(300));
    EXPECT_EQ(rig.Drain(), 1U);

    EXPECT_TRUE(completed);
    EXPECT_NE(completionStatus, kIOReturnSuccess);
    EXPECT_EQ(rig.Target().CommandCount(), 2U);
    EXPECT_EQ(rig.Timers().PendingCount(), 0U);
}

} // namespace
