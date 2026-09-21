// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Isochronous bandwidth allocation math, cross-checked against both reference
// stacks. This is what a node subtracts from BANDWIDTH_AVAILABLE before it may
// transmit on an isochronous channel, so getting it wrong either refuses a
// stream set that fits (too strict) or oversubscribes the cycle (too loose).
//
// Packet term - Apple computes it in a single expression
// (references/IOFireWireFamily.kmodproj/IOFWIsochChannel.cpp:664):
//     bandwidth = (fPacketSize/4 + 3) * 16 / (1 << inSpeed)
// Linux derives the same number as "bytes at S400"
// (references/linux-sound-firewire-stack/firewire/iso-resources.c:48-61).
// Both transcriptions are asserted here against our shared implementation.
//
// Overhead term - Linux charges a per-allocation arbitration cost derived from
// the live gap count (iso-resources.c:64-76). Apple charges nothing at all.

#include <gtest/gtest.h>

#include "Bus/IRM/IRMTypes.hpp"

namespace {

using ASFW::IRM::BandwidthOverheadForGapCount;
using ASFW::IRM::PacketBandwidthUnits;

// Transcription of IOFWIsochChannel.cpp:664.
uint32_t AppleBandwidth(uint32_t packetSize, uint32_t speed) {
    return (packetSize / 4 + 3) * 16 / (1U << speed);
}

// Transcription of iso-resources.c:48-61 (packet_bandwidth).
uint32_t LinuxPacketBandwidth(uint32_t maxPayloadBytes, uint32_t speed) {
    const uint32_t bytes = 3 * 4 + ((maxPayloadBytes + 3) & ~3U);
    return speed <= 2 ? bytes * (1U << (2 - speed)) : (bytes + (1U << (speed - 2)) - 1) / (1U << (speed - 2));
}

TEST(IsochBandwidthAllocation, PacketTermMatchesAppleIOFWIsochChannel) {
    // Quadlet-aligned payloads, which is every AM824 packet we build.
    for (uint32_t payload = 0; payload <= 1024; payload += 4) {
        for (uint32_t speed = 0; speed <= 3; ++speed) {
            EXPECT_EQ(PacketBandwidthUnits(payload, static_cast<uint8_t>(speed)),
                      AppleBandwidth(payload, speed))
                << "payload=" << payload << " speed=" << speed;
        }
    }
}

TEST(IsochBandwidthAllocation, PacketTermMatchesLinuxIsoResources) {
    for (uint32_t payload = 0; payload <= 1024; payload += 4) {
        for (uint32_t speed = 0; speed <= 3; ++speed) {
            EXPECT_EQ(PacketBandwidthUnits(payload, static_cast<uint8_t>(speed)),
                      LinuxPacketBandwidth(payload, speed))
                << "payload=" << payload << " speed=" << speed;
        }
    }
}

TEST(IsochBandwidthAllocation, PacketTermIsAnAudioPacketAtEachSpeed) {
    // 16 AM824 slots x 8 events + 8 CIP header bytes: the 520-byte packet the
    // 48 kHz DICE profiles build. 130 payload quadlets + 3 header quadlets.
    constexpr uint32_t kPayload = 8 + 8 * 16 * 4;
    EXPECT_EQ(kPayload, 520U);
    EXPECT_EQ(PacketBandwidthUnits(kPayload, 2), 532U);   // S400
    EXPECT_EQ(PacketBandwidthUnits(kPayload, 1), 1064U);  // S200
    EXPECT_EQ(PacketBandwidthUnits(kPayload, 0), 2128U);  // S100
    EXPECT_EQ(PacketBandwidthUnits(kPayload, 3), 266U);   // S800
}

TEST(IsochBandwidthAllocation, OverheadFollowsLinuxGapCountDerivation) {
    // gap_count * 97 / 10 + 89, with 63 falling back to the pessimistic 512.
    EXPECT_EQ(BandwidthOverheadForGapCount(0), 89U);
    EXPECT_EQ(BandwidthOverheadForGapCount(5), 137U);   // 1 hop: a two-node bus
    EXPECT_EQ(BandwidthOverheadForGapCount(16), 244U);
    EXPECT_EQ(BandwidthOverheadForGapCount(62), 690U);
    EXPECT_EQ(BandwidthOverheadForGapCount(63), 512U);  // unoptimised fallback
}

TEST(IsochBandwidthAllocation, OptimisedGapCountCostsLessThanTheUnoptimisedFallback) {
    // The whole point of running a bus manager: an optimised two-node bus is
    // charged 137 units per allocation instead of 512.
    EXPECT_LT(BandwidthOverheadForGapCount(5), BandwidthOverheadForGapCount(63));
}

TEST(IsochBandwidthAllocation, VeniceF24StreamSetFitsOnlyOnAnOptimisedBusAtS200) {
    // The reported failing configuration: a Midas Venice F24 clamped to S200
    // by its link, carrying two playback streams (16, 8 slots) and two capture
    // streams (16, 8 slots). Charged against a full 4915-unit ledger.
    constexpr uint32_t kBudget = ASFW::IRM::kMaxBandwidthUnitsS400;
    const uint32_t slots[4] = {16, 8, 16, 8};

    const auto total = [&](uint8_t speed, uint8_t gapCount) {
        uint32_t sum = 0;
        for (const uint32_t s : slots) {
            sum += PacketBandwidthUnits(8 + 8 * s * 4, speed) +
                   BandwidthOverheadForGapCount(gapCount);
        }
        return sum;
    };

    EXPECT_GT(total(1, 63), kBudget);  // S200, unoptimised: refused, and rightly so
    EXPECT_LE(total(1, 5), kBudget);   // S200, optimised by the bus manager: fits
    EXPECT_LE(total(2, 63), kBudget);  // S400 has room even unoptimised
    EXPECT_LE(total(2, 5), kBudget);

    // Totals match the review comment and reference calculation:
    // S200 / gap 63: 5280 units
    // S200 / gap 5:  3780 units
    // S400 / gap 63: 3664 units
    // S400 / gap 5:  2164 units
    EXPECT_EQ(total(1, 63), 5280U);
    EXPECT_EQ(total(1, 5), 3780U);
    EXPECT_EQ(total(2, 63), 3664U);
    EXPECT_EQ(total(2, 5), 2164U);
}

TEST(IsochBandwidthAllocation, AppleWouldAcceptWhatWeRefuseOnAnUnoptimisedBus) {
    // Documented divergence: Apple charges no overhead term, so it accepts the
    // same stream set on an unoptimised bus. At 4915 units the ledger is about
    // 98.3us of a ~100us isochronous window, and gap-count-63 arbitration does
    // not fit in what Apple leaves over. We follow Linux and refuse.
    const uint32_t slots[4] = {16, 8, 16, 8};
    uint32_t applePlan = 0;
    uint32_t ourPlan = 0;
    for (const uint32_t s : slots) {
        applePlan += PacketBandwidthUnits(8 + 8 * s * 4, 1);
        ourPlan += PacketBandwidthUnits(8 + 8 * s * 4, 1) + BandwidthOverheadForGapCount(63);
    }
    EXPECT_LE(applePlan, ASFW::IRM::kMaxBandwidthUnitsS400);
    EXPECT_GT(ourPlan, ASFW::IRM::kMaxBandwidthUnitsS400);
}

} // namespace
