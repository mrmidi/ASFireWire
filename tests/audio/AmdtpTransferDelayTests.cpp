// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AmdtpTransferDelayTests.cpp
// Tests

#include "Audio/Wire/AMDTP/AmdtpTransferDelay.hpp"
#include "Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Encoding::AmdtpTransferDelayTicks;
using ASFW::Encoding::CipStreamMode;

TEST(AmdtpTransferDelayTests, NonBlockingModeUsesConstantBaseBuffering) {
    // Linux amdtp-stream.c:302:
    // TRANSFER_DELAY_TICKS (11776) - TICKS_PER_CYCLE (3072) = 8704 ticks (~354 µs)
    constexpr uint32_t kExpectedNonBlockingTicks = 8704;

    const uint32_t rates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};
    for (uint32_t rate : rates) {
        auto geom = ASFW::Encoding::AmdtpRateGeometryForSampleRate(rate);
        ASSERT_TRUE(geom.has_value());
        EXPECT_EQ(AmdtpTransferDelayTicks(rate, geom->sytIntervalFrames, CipStreamMode::NonBlocking),
                  kExpectedNonBlockingTicks)
            << "Rate " << rate << " Hz failed non-blocking delay check";
    }
}

TEST(AmdtpTransferDelayTests, BlockingModeAddsSytIntervalPeriod) {
    // Linux amdtp-stream.c:305-308:
    // delay += (TICKS_PER_SECOND * syt_interval) / rate
    // For 48k (syt=8), 96k (syt=16), 192k (syt=32):
    // additional = (24576000 * 8) / 48000 = 4096 ticks
    // delay = 8704 + 4096 = 12800 ticks (~520.8 µs)
    EXPECT_EQ(AmdtpTransferDelayTicks(48000, 8, CipStreamMode::Blocking), 12800U);
    EXPECT_EQ(AmdtpTransferDelayTicks(96000, 16, CipStreamMode::Blocking), 12800U);
    EXPECT_EQ(AmdtpTransferDelayTicks(192000, 32, CipStreamMode::Blocking), 12800U);

    // For 32k (syt=8):
    // additional = (24576000 * 8) / 32000 = 6144 ticks
    // delay = 8704 + 6144 = 14848 ticks
    EXPECT_EQ(AmdtpTransferDelayTicks(32000, 8, CipStreamMode::Blocking), 14848U);

    // For 44.1k (syt=8), 88.2k (syt=16), 176.4k (syt=32):
    // additional = (24576000 * 8) / 44100 = 4458 ticks (integer truncated)
    // delay = 8704 + 4458 = 13162 ticks
    EXPECT_EQ(AmdtpTransferDelayTicks(44100, 8, CipStreamMode::Blocking), 13162U);
    EXPECT_EQ(AmdtpTransferDelayTicks(88200, 16, CipStreamMode::Blocking), 13162U);
    EXPECT_EQ(AmdtpTransferDelayTicks(176400, 32, CipStreamMode::Blocking), 13162U);
}

TEST(AmdtpTransferDelayTests, DefaultModeIsBlocking) {
    EXPECT_EQ(AmdtpTransferDelayTicks(48000, 8), 12800U);
    EXPECT_EQ(AmdtpTransferDelayTicks(96000, 16), 12800U);
}

TEST(AmdtpTransferDelayTests, ZeroRateReturnsZero) {
    EXPECT_EQ(AmdtpTransferDelayTicks(0, 8, CipStreamMode::Blocking), 0U);
    EXPECT_EQ(AmdtpTransferDelayTicks(0, 8, CipStreamMode::NonBlocking), 0U);
}

} // namespace
