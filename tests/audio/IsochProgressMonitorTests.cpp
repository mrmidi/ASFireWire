#include <gtest/gtest.h>

#include "Isoch/Core/IsochProgressMonitor.hpp"

namespace {

using ASFW::Isoch::Core::IsochProgressMonitor;
using ASFW::Isoch::Core::IsochProgressObservation;
using ASFW::Isoch::Core::IsochProgressThresholds;

constexpr IsochProgressThresholds kThresholds{
    .wakeAfterTicks = 4,
    .snapshotAfterTicks = 20,
    .fatalAfterTicks = 100,
};

TEST(IsochProgressMonitor, RepeatedCallbacksDoNotCountAsCompletionProgress) {
    IsochProgressMonitor monitor;
    monitor.Configure(kThresholds);
    monitor.Reset(/*nowTicks=*/0, /*retiredCursor=*/1200);

    auto decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 19,
        .retiredCursor = 1200,
        .run = true,
        .active = true,
    });
    EXPECT_FALSE(decision.progressed);
    EXPECT_FALSE(decision.snapshot);
    EXPECT_FALSE(decision.requestWake);
    EXPECT_FALSE(decision.fatal);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 20,
        .retiredCursor = 1200,
        .run = true,
        .active = true,
    });
    EXPECT_TRUE(decision.snapshot);
    EXPECT_FALSE(decision.requestWake);
    EXPECT_FALSE(decision.fatal);

    // An arbitrary number of additional callbacks with the same cursor does
    // not reset the epoch or emit duplicate first-fault snapshots.
    for (uint64_t tick = 21; tick < 100; ++tick) {
        decision = monitor.Observe(IsochProgressObservation{
            .nowTicks = tick,
            .retiredCursor = 1200,
            .run = true,
            .active = true,
        });
        EXPECT_FALSE(decision.snapshot);
        EXPECT_FALSE(decision.fatal);
    }

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 100,
        .retiredCursor = 1200,
        .run = true,
        .active = true,
    });
    EXPECT_TRUE(decision.fatal);
    EXPECT_EQ(decision.stalledTicks, 100U);
}

TEST(IsochProgressMonitor, IdleRunningContextGetsOneBoundedWakePerEpoch) {
    IsochProgressMonitor monitor;
    monitor.Configure(kThresholds);
    monitor.Reset(/*nowTicks=*/10, /*retiredCursor=*/42);

    auto decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 14,
        .retiredCursor = 42,
        .run = true,
        .active = false,
    });
    EXPECT_TRUE(decision.snapshot);
    EXPECT_TRUE(decision.requestWake);
    EXPECT_FALSE(decision.fatal);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 30,
        .retiredCursor = 42,
        .run = true,
        .active = false,
    });
    EXPECT_FALSE(decision.snapshot);
    EXPECT_FALSE(decision.requestWake);

    // Real completion progress rearms the bounded recovery for a future epoch.
    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 31,
        .retiredCursor = 43,
        .run = true,
        .active = true,
    });
    EXPECT_TRUE(decision.progressed);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 35,
        .retiredCursor = 43,
        .run = true,
        .active = false,
    });
    EXPECT_TRUE(decision.requestWake);
}

TEST(IsochProgressMonitor, CompletionProgressResetsFatalDeadline) {
    IsochProgressMonitor monitor;
    monitor.Configure(kThresholds);
    monitor.Reset(/*nowTicks=*/0, /*retiredCursor=*/7);

    auto decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 99,
        .retiredCursor = 7,
        .run = true,
        .active = true,
    });
    EXPECT_FALSE(decision.fatal);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 99,
        .retiredCursor = 8,
        .run = true,
        .active = true,
    });
    EXPECT_TRUE(decision.progressed);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 198,
        .retiredCursor = 8,
        .run = true,
        .active = true,
    });
    EXPECT_FALSE(decision.fatal);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 199,
        .retiredCursor = 8,
        .run = true,
        .active = true,
    });
    EXPECT_TRUE(decision.fatal);
}

TEST(IsochProgressMonitor, DoesNotWakeStoppedOrDeadContext) {
    IsochProgressMonitor monitor;
    monitor.Configure(kThresholds);
    monitor.Reset(/*nowTicks=*/0, /*retiredCursor=*/99);

    auto decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 4,
        .retiredCursor = 99,
        .run = false,
        .active = false,
    });
    EXPECT_FALSE(decision.requestWake);

    decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 100,
        .retiredCursor = 99,
        .run = true,
        .active = false,
        .dead = true,
    });
    EXPECT_FALSE(decision.requestWake);
    EXPECT_TRUE(decision.fatal);
}

TEST(IsochProgressMonitor, BackwardsHostClockStartsANewEpoch) {
    IsochProgressMonitor monitor;
    monitor.Configure(kThresholds);
    monitor.Reset(/*nowTicks=*/1000, /*retiredCursor=*/5);

    const auto decision = monitor.Observe(IsochProgressObservation{
        .nowTicks = 999,
        .retiredCursor = 5,
        .run = true,
        .active = true,
    });
    EXPECT_TRUE(decision.progressed);
    EXPECT_EQ(monitor.LastProgressTicks(), 999U);
}

} // namespace
