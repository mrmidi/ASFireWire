#include <gtest/gtest.h>

#include "AudioEngine/Direct/Tx/OutputCursorDiscipline.hpp"

using ASFW::AudioEngine::Direct::Tx::DisciplineOutputCursor;

namespace {
constexpr uint64_t kLead = 768;      // ~1.5 IO periods @48k
constexpr uint64_t kDeadband = 256;  // ~0.5 IO period
} // namespace

// Lead exactly on target: leave the cursor for the caller's per-packet advance.
TEST(OutputCursorDiscipline, OnTargetDoesNotResync) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd - kLead;  // lead == target
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_FALSE(r.resynced);
    EXPECT_EQ(r.newCursor, cursor);
}

// Lead within the deadband on either side: no resync.
TEST(OutputCursorDiscipline, WithinDeadbandDoesNotResync) {
    const uint64_t writtenEnd = 100000;
    // lead = target - deadband (boundary, still inside)
    const uint64_t nearUnder = writtenEnd - (kLead - kDeadband);
    EXPECT_FALSE(DisciplineOutputCursor(nearUnder, writtenEnd, kLead, kDeadband).resynced);
    // lead = target + deadband (boundary, still inside)
    const uint64_t nearOver = writtenEnd - (kLead + kDeadband);
    EXPECT_FALSE(DisciplineOutputCursor(nearOver, writtenEnd, kLead, kDeadband).resynced);
}

// A producer-ahead underrun must not rewind the consumer.
TEST(OutputCursorDiscipline, UnderrunSideNeverMovesBackward) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd - 100;  // lead 100 << target
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_FALSE(r.resynced);
    EXPECT_EQ(r.newCursor, cursor);
}

// Lead too large (cursor lagging too far): rebase forward to target.
TEST(OutputCursorDiscipline, OverLeadRebasesToTarget) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd - 4000;  // lead 4000 >> target
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_TRUE(r.resynced);
    EXPECT_EQ(r.newCursor, writtenEnd - kLead);
}

// Cursor past writtenEnd is an underrun, not permission to replay old data.
TEST(OutputCursorDiscipline, CursorPastWrittenEndNeverMovesBackward) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd + 50;
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_FALSE(r.resynced);
    EXPECT_EQ(r.newCursor, cursor);
}

// Early startup: writtenEnd below the target lead clamps target to 0.
TEST(OutputCursorDiscipline, WrittenEndBelowTargetClampsToZero) {
    const uint64_t writtenEnd = 200;  // < kLead
    const uint64_t cursor = 0;
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_FALSE(r.resynced);
    EXPECT_EQ(r.newCursor, 0u);
}

// constexpr usability (compile-time evaluation).
TEST(OutputCursorDiscipline, IsConstexpr) {
    // lead = 1000, target = 768, |diff| = 232 < deadband 256 -> no resync.
    constexpr auto rInBand = DisciplineOutputCursor(99000, 100000, kLead, kDeadband);
    static_assert(!rInBand.resynced, "232 < 256 deadband should not resync");
    static_assert(rInBand.newCursor == 99000, "cursor unchanged within deadband");

    // A cursor ahead of the target is retained even if the producer lead is small.
    constexpr auto rOut = DisciplineOutputCursor(99900, 100000, kLead, kDeadband);
    static_assert(!rOut.resynced, "forward-only discipline must not rewind");
    static_assert(rOut.newCursor == 99900, "cursor remains monotonic");
    (void)rInBand;
    (void)rOut;
}

TEST(OutputCursorDiscipline, RepeatedWriteBurstsAndPacketConsumptionStayMonotonic) {
    uint64_t cursor = 0;
    uint64_t writtenEnd = 768;
    uint64_t corrections = 0;

    for (uint32_t burst = 0; burst < 64; ++burst) {
        writtenEnd += 128;
        for (uint32_t packet = 0; packet < 16; ++packet) {
            const uint64_t before = cursor;
            const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
            EXPECT_GE(r.newCursor, before);
            corrections += r.resynced ? 1U : 0U;
            cursor = r.newCursor + 8;
        }
    }

    EXPECT_EQ(corrections, 0U);
    EXPECT_EQ(cursor, 64U * 128U);
}
