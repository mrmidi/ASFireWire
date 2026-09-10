// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Runtime/PublicationRangeRing.hpp"
#include <gtest/gtest.h>

using namespace ASFW::Audio::Runtime;

TEST(PublicationRangeRingTests, EmptyRingLookup) {
    PublicationRangeRing ring;
    uint64_t earliest = 0;
    uint64_t latest = 0;

    auto res = ring.LookupPacketCoverage(1, 0, 16, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Pending);
}

TEST(PublicationRangeRingTests, SingleRangeCovered) {
    PublicationRangeRing ring;
    // Record publication of frames [1000, 1512) at host tick 50000..50000
    ring.Record(1, 1000, 1512, 50000, 50000);

    uint64_t earliest = 0;
    uint64_t latest = 0;

    // A packet of 16 frames starting at 1000
    auto res = ring.LookupPacketCoverage(1, 1000, 16, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Resolved);
    EXPECT_EQ(earliest, 50000ULL);
    EXPECT_EQ(latest, 50000ULL);

    // A packet of 16 frames starting at 1400 (inside range)
    res = ring.LookupPacketCoverage(1, 1400, 16, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Resolved);
    EXPECT_EQ(earliest, 50000ULL);
    EXPECT_EQ(latest, 50000ULL);
}

TEST(PublicationRangeRingTests, PacketBeyondCommittedIsPending) {
    PublicationRangeRing ring;
    ring.Record(1, 1000, 1100, 50000, 50000);

    uint64_t earliest = 0;
    uint64_t latest = 0;

    // Packet covers [1090, 1110) which straddles beyond the committed end 1100
    auto res = ring.LookupPacketCoverage(1, 1090, 20, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Pending);

    // Packet covers [1200, 1220) which is completely in the future
    res = ring.LookupPacketCoverage(1, 1200, 20, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Pending);
}

TEST(PublicationRangeRingTests, GapDetection) {
    PublicationRangeRing ring;
    // Record [1000, 1100) at 50000
    ring.Record(1, 1000, 1100, 50000, 50000);
    // Record [1150, 1300) at 60000 - there is a gap [1100, 1150)
    ring.Record(1, 1150, 1300, 60000, 60000);

    uint64_t earliest = 0;
    uint64_t latest = 0;

    // Query across the gap: [1090, 1120)
    auto res = ring.LookupPacketCoverage(1, 1090, 30, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Gap);

    // Query completely inside the gap: [1110, 1130)
    res = ring.LookupPacketCoverage(1, 1110, 20, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Gap);
}

TEST(PublicationRangeRingTests, EpochMismatch) {
    PublicationRangeRing ring;
    ring.Record(1, 1000, 1200, 50000, 50000);

    uint64_t earliest = 0;
    uint64_t latest = 0;

    // Query with different epoch
    auto res = ring.LookupPacketCoverage(2, 1000, 16, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::EpochMismatch);
}

TEST(PublicationRangeRingTests, AgedOutAfterRingWraps) {
    PublicationRangeRing ring;

    // Insert 300 sequential contiguous ranges of 100 frames each
    for (uint64_t i = 0; i < 300; ++i) {
        ring.Record(1, i * 100, (i + 1) * 100, 10000 + i * 1000, 10000 + i * 1000);
    }

    uint64_t earliest = 0;
    uint64_t latest = 0;

    // The oldest ranges (< 44) have been overwritten in the 256-slot ring
    auto res = ring.LookupPacketCoverage(1, 100, 16, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::AgedOut);

    // The newest ranges (e.g. 290) are present and Resolved
    res = ring.LookupPacketCoverage(1, 29050, 16, earliest, latest);
    EXPECT_EQ(res, PublicationCoverageResult::Resolved);
}

namespace {
struct PublishingDuringLookup {
    PublicationRangeRing* ring;
    unsigned remaining;
    unsigned calls{0};
    static void Advance(void* raw) {
        auto& self = *static_cast<PublishingDuringLookup*>(raw);
        ++self.calls;
        if (self.remaining == 0) return;
        --self.remaining;
        const auto n = self.ring->Count();
        self.ring->Record(1, n * 100, (n + 1) * 100, n * 1000, n * 1000 + 10);
    }
};
void FillHistory(PublicationRangeRing& ring) {
    for (uint64_t i = 0; i < kPublicationRangeSlots; ++i)
        ring.Record(1, i * 100, (i + 1) * 100, i * 1000, i * 1000 + 10);
}
}

TEST(PublicationRangeRingTests, UnrelatedEvictionDuringScanRetriesRecentPacket) {
    PublicationRangeRing ring;
    FillHistory(ring);
    PublishingDuringLookup hook{&ring, 1};
    ring.SetLookupSnapshotHook(PublishingDuringLookup::Advance, &hook);
    uint64_t earliest = 99, latest = 99;
    EXPECT_EQ(ring.LookupPacketCoverage(1, 25050, 8, earliest, latest),
              PublicationCoverageResult::Resolved);
    EXPECT_EQ(earliest, 250000U);
    EXPECT_EQ(latest, 250010U);
    EXPECT_EQ(hook.calls, 2U);
}

TEST(PublicationRangeRingTests, SustainedContentionIsBoundedAndNotEviction) {
    PublicationRangeRing ring;
    FillHistory(ring);
    PublishingDuringLookup hook{&ring, 10};
    ring.SetLookupSnapshotHook(PublishingDuringLookup::Advance, &hook);
    uint64_t earliest = 99, latest = 99;
    EXPECT_EQ(ring.LookupPacketCoverage(1, 25050, 8, earliest, latest),
              PublicationCoverageResult::ReadCollision);
    EXPECT_EQ(hook.calls, 3U);
    EXPECT_EQ(earliest, 0U);
    EXPECT_EQ(latest, 0U);
    ring.SetLookupSnapshotHook(nullptr, nullptr);
    EXPECT_EQ(ring.LookupPacketCoverage(1, 25050, 8, earliest, latest),
              PublicationCoverageResult::Resolved);
}

TEST(PublicationRangeRingTests, EvictedTargetIsProvenAfterRetry) {
    PublicationRangeRing ring;
    FillHistory(ring);
    PublishingDuringLookup hook{&ring, 1};
    ring.SetLookupSnapshotHook(PublishingDuringLookup::Advance, &hook);
    uint64_t earliest = 0, latest = 0;
    EXPECT_EQ(ring.LookupPacketCoverage(1, 50, 8, earliest, latest),
              PublicationCoverageResult::AgedOut);
}

TEST(PublicationRangeRingTests, NeverRecordedPrefixIsNotEviction) {
    PublicationRangeRing ring;
    ring.Record(1, 1000, 1100, 100, 110);
    uint64_t earliest = 0, latest = 0;
    EXPECT_EQ(ring.LookupPacketCoverage(1, 950, 8, earliest, latest), PublicationCoverageResult::Gap);
    EXPECT_EQ(ring.LookupPacketCoverage(2, 950, 8, earliest, latest), PublicationCoverageResult::EpochMismatch);
}
