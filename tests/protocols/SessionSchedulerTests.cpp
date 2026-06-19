#include <gtest/gtest.h>

#include <vector>

#include "FakeSessionScheduler.hpp"

using ASFW::Testing::FakeSessionScheduler;
using ASFW::Protocols::SBP2::kInvalidSchedulerToken;

TEST(SessionSchedulerTests, FiresOnlyAfterDeadlineCrossed) {
    FakeSessionScheduler sched;
    int fired = 0;
    (void)sched.ScheduleAfter(100, [&fired] { ++fired; });

    sched.Advance(50);
    EXPECT_EQ(0, fired);            // not yet due
    EXPECT_EQ(1u, sched.PendingCount());

    sched.Advance(50);             // now == deadline
    EXPECT_EQ(1, fired);           // fires exactly at the deadline
    EXPECT_EQ(0u, sched.PendingCount());

    sched.Advance(1000);
    EXPECT_EQ(1, fired);           // one-shot: does not re-fire
}

TEST(SessionSchedulerTests, FiresInDeadlineOrderRegardlessOfInsertion) {
    FakeSessionScheduler sched;
    std::vector<int> order;
    (void)sched.ScheduleAfter(300, [&order] { order.push_back(3); });
    (void)sched.ScheduleAfter(100, [&order] { order.push_back(1); });
    (void)sched.ScheduleAfter(200, [&order] { order.push_back(2); });

    sched.Advance(300);
    ASSERT_EQ(3u, order.size());
    EXPECT_EQ(1, order[0]);
    EXPECT_EQ(2, order[1]);
    EXPECT_EQ(3, order[2]);
}

TEST(SessionSchedulerTests, CancelPreventsFire) {
    FakeSessionScheduler sched;
    int fired = 0;
    const auto token = sched.ScheduleAfter(100, [&fired] { ++fired; });

    sched.Cancel(token);
    EXPECT_EQ(0u, sched.PendingCount());

    sched.Advance(1000);
    EXPECT_EQ(0, fired);

    sched.Cancel(token);                       // double-cancel is a no-op
    sched.Cancel(kInvalidSchedulerToken);      // invalid token is a no-op
}

TEST(SessionSchedulerTests, ReentrantScheduleFiresWithinSameAdvanceWhenDue) {
    FakeSessionScheduler sched;
    std::vector<int> order;
    // First callback (at t=100) schedules a second at +50 (absolute t=150),
    // which is still within the t=200 advance window and must fire too.
    (void)sched.ScheduleAfter(100, [&] {
        order.push_back(1);
        (void)sched.ScheduleAfter(50, [&order] { order.push_back(2); });
    });

    sched.Advance(200);
    ASSERT_EQ(2u, order.size());
    EXPECT_EQ(1, order[0]);
    EXPECT_EQ(2, order[1]);
    EXPECT_EQ(0u, sched.PendingCount());
}

TEST(SessionSchedulerTests, CallbackThatCancelsAnotherIsHonored) {
    FakeSessionScheduler sched;
    int fired = 0;
    ASFW::Protocols::SBP2::SchedulerToken victim = kInvalidSchedulerToken;
    // Earlier callback cancels a later, not-yet-due one.
    (void)sched.ScheduleAfter(100, [&] { sched.Cancel(victim); });
    victim = sched.ScheduleAfter(200, [&fired] { ++fired; });

    sched.Advance(300);
    EXPECT_EQ(0, fired);   // victim was canceled before its deadline
}
