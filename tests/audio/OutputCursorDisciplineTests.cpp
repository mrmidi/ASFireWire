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

// Lead too small (cursor too close to writtenEnd -> underrun risk): rebase back.
TEST(OutputCursorDiscipline, UnderrunSideRebasesToTarget) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd - 100;  // lead 100 << target
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_TRUE(r.resynced);
    EXPECT_EQ(r.newCursor, writtenEnd - kLead);
}

// Lead too large (cursor lagging too far): rebase forward to target.
TEST(OutputCursorDiscipline, OverLeadRebasesToTarget) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd - 4000;  // lead 4000 >> target
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_TRUE(r.resynced);
    EXPECT_EQ(r.newCursor, writtenEnd - kLead);
}

// Cursor has overtaken writtenEnd entirely (severe underrun): always rebase.
TEST(OutputCursorDiscipline, CursorPastWrittenEndRebases) {
    const uint64_t writtenEnd = 100000;
    const uint64_t cursor = writtenEnd + 50;
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    EXPECT_TRUE(r.resynced);
    EXPECT_EQ(r.newCursor, writtenEnd - kLead);
}

// Early startup: writtenEnd below the target lead clamps target to 0.
TEST(OutputCursorDiscipline, WrittenEndBelowTargetClampsToZero) {
    const uint64_t writtenEnd = 200;  // < kLead
    const uint64_t cursor = 0;
    const auto r = DisciplineOutputCursor(cursor, writtenEnd, kLead, kDeadband);
    // lead = 200, target = 768 -> |200-768| = 568 > deadband -> resync to clamped 0
    EXPECT_TRUE(r.resynced);
    EXPECT_EQ(r.newCursor, 0u);
}

// constexpr usability (compile-time evaluation).
TEST(OutputCursorDiscipline, IsConstexpr) {
    // lead = 1000, target = 768, |diff| = 232 < deadband 256 -> no resync.
    constexpr auto rInBand = DisciplineOutputCursor(99000, 100000, kLead, kDeadband);
    static_assert(!rInBand.resynced, "232 < 256 deadband should not resync");
    static_assert(rInBand.newCursor == 99000, "cursor unchanged within deadband");

    // lead = 100, target = 768, |diff| = 668 > deadband -> resync to writtenEnd-target.
    constexpr auto rOut = DisciplineOutputCursor(99900, 100000, kLead, kDeadband);
    static_assert(rOut.resynced, "668 > 256 deadband should resync");
    static_assert(rOut.newCursor == 100000 - kLead, "rebase to target lead");
    (void)rInBand;
    (void)rOut;
}
