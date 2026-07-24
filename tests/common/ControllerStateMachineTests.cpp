// SPDX-License-Identifier: Apache-2.0

#include "Controller/ControllerStateMachine.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace {

using ASFW::Driver::ControllerState;
using ASFW::Driver::ControllerStateMachine;
using ASFW::Driver::TransitionDisposition;

TEST(ControllerStateMachineTests, StartsStopped) {
    ControllerStateMachine state;

    EXPECT_EQ(state.CurrentState(), ControllerState::kStopped);
    EXPECT_FALSE(state.LastTransition().has_value());
}

TEST(ControllerStateMachineTests, AcceptsPlannedStopPath) {
    ControllerStateMachine state;

    EXPECT_EQ(state.TransitionTo(ControllerState::kStarting, "start", 1),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.TransitionTo(ControllerState::kRunning, "ready", 2),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.TransitionTo(ControllerState::kQuiescing, "stop", 3),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.TransitionTo(ControllerState::kStopped, "stopped", 4),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.CurrentState(), ControllerState::kStopped);
}

TEST(ControllerStateMachineTests, AcceptsSuspendAndResumePath) {
    ControllerStateMachine state;

    ASSERT_EQ(state.TransitionTo(ControllerState::kStarting, "start", 1),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kRunning, "ready", 2),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kQuiescing, "suspend", 3),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kSuspended, "suspended", 4),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.TransitionTo(ControllerState::kStarting, "resume", 5),
              TransitionDisposition::kApplied);
}

TEST(ControllerStateMachineTests, AcceptsProviderRevocationPath) {
    ControllerStateMachine state;

    ASSERT_EQ(state.TransitionTo(ControllerState::kStarting, "start", 1),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kRunning, "ready", 2),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kRevoked, "provider revoked", 3),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.TransitionTo(ControllerState::kStopped, "released", 4),
              TransitionDisposition::kApplied);
}

TEST(ControllerStateMachineTests, AcceptsFailedStartCleanupPath) {
    ControllerStateMachine state;

    ASSERT_EQ(state.TransitionTo(ControllerState::kStarting, "start", 1),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kFailed, "attach failed", 2),
              TransitionDisposition::kApplied);
    ASSERT_EQ(state.TransitionTo(ControllerState::kQuiescing, "cleanup", 3),
              TransitionDisposition::kApplied);
    EXPECT_EQ(state.TransitionTo(ControllerState::kStopped, "clean", 4),
              TransitionDisposition::kApplied);
}

TEST(ControllerStateMachineTests, RejectsIllegalTransitionWithoutMutation) {
    ControllerStateMachine state;

    EXPECT_FALSE(state.CanTransitionTo(ControllerState::kRunning));
    EXPECT_EQ(state.TransitionTo(ControllerState::kRunning, "skip start", 1),
              TransitionDisposition::kRejected);
    EXPECT_EQ(state.CurrentState(), ControllerState::kStopped);
    EXPECT_FALSE(state.LastTransition().has_value());
}

TEST(ControllerStateMachineTests, DuplicateRequestIsIdempotent) {
    ControllerStateMachine state;

    ASSERT_EQ(state.TransitionTo(ControllerState::kStarting, "start", 1),
              TransitionDisposition::kApplied);
    const auto before = state.LastTransition();
    ASSERT_TRUE(before.has_value());

    EXPECT_EQ(state.TransitionTo(ControllerState::kStarting, "duplicate", 2),
              TransitionDisposition::kIdempotent);

    const auto after = state.LastTransition();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->reason, before->reason);
    EXPECT_EQ(after->timestamp, before->timestamp);
}

TEST(ControllerStateMachineTests, ConcurrentDuplicateRequestsApplyOnce) {
    ControllerStateMachine state;
    std::atomic<uint32_t> applied{0};
    std::atomic<uint32_t> idempotent{0};
    std::atomic<uint32_t> rejected{0};
    std::vector<std::thread> workers;

    constexpr uint32_t kWorkerCount = 12;
    workers.reserve(kWorkerCount);
    for (uint32_t i = 0; i < kWorkerCount; ++i) {
        workers.emplace_back([&state, &applied, &idempotent, &rejected, i] {
            switch (state.TransitionTo(ControllerState::kStarting, "concurrent start", i + 1)) {
            case TransitionDisposition::kApplied:
                ++applied;
                break;
            case TransitionDisposition::kIdempotent:
                ++idempotent;
                break;
            case TransitionDisposition::kRejected:
                ++rejected;
                break;
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(applied.load(), 1U);
    EXPECT_EQ(idempotent.load(), kWorkerCount - 1U);
    EXPECT_EQ(rejected.load(), 0U);
    EXPECT_EQ(state.CurrentState(), ControllerState::kStarting);
}

} // namespace
