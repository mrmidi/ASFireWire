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

TEST(IsochBandwidthAllocation, VeniceF24StreamSetFitsWithinLedgerAtS200PerAppleIOFWIsochChannel) {
    // Under Apple IOFWIsochChannel wire parity (IOFWIsochChannel.cpp:664),
    // bandwidth reservations charge only the packet term against BANDWIDTH_AVAILABLE.
    // The 1229-unit set-aside in the 125us cycle already accounts for async traffic
    // and gap overhead.
    //
    // For Midas Venice F24 at S200 with 4 streams (16, 8, 16, 8 slots):
    // Stream 0 (16 slots): 1064 units
    // Stream 1 (8 slots):   552 units
    // Stream 2 (16 slots): 1064 units
    // Stream 3 (8 slots):   552 units
    // Total: 3232 units <= 4915 units (kMaxBandwidthUnitsS400).
    // All 4 streams fit comfortably with 1683 units of headroom on any bus.
    constexpr uint32_t kBudget = ASFW::IRM::kMaxBandwidthUnitsS400;
    const uint32_t slots[4] = {16, 8, 16, 8};

    uint32_t totalAtS200 = 0;
    for (const uint32_t s : slots) {
        totalAtS200 += PacketBandwidthUnits(8 + 8 * s * 4, 1);
    }
    EXPECT_EQ(totalAtS200, 3232U);
    EXPECT_LE(totalAtS200, kBudget);

    // Individual stream charges at S200:
    EXPECT_EQ(PacketBandwidthUnits(8 + 8 * 16 * 4, 1), 1064U);
    EXPECT_EQ(PacketBandwidthUnits(8 + 8 * 8 * 4, 1), 552U);

    // At S400 for comparison:
    uint32_t totalAtS400 = 0;
    for (const uint32_t s : slots) {
        totalAtS400 += PacketBandwidthUnits(8 + 8 * s * 4, 2);
    }
    EXPECT_EQ(totalAtS400, 1616U);
    EXPECT_LE(totalAtS400, kBudget);
}

} // namespace
