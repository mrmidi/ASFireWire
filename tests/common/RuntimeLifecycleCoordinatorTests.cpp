// SPDX-License-Identifier: Apache-2.0

#include "Service/Lifecycle/RuntimeLifecycleCoordinator.hpp"

#include <memory>

#include "gtest/gtest.h"

namespace {

using ASFW::Driver::ControllerState;
using ASFW::Driver::ControllerStateMachine;
using ASFW::Driver::QuiesceReason;
using ASFW::Driver::RuntimeLifecycleCoordinator;
using ASFW::Driver::StartStage;

class RuntimeLifecycleCoordinatorTests : public ::testing::Test {
protected:
    RuntimeLifecycleCoordinatorTests()
        : state_(std::make_shared<ControllerStateMachine>()), coordinator_(state_) {}

    std::shared_ptr<ControllerStateMachine> state_;
    RuntimeLifecycleCoordinator coordinator_;
};

TEST_F(RuntimeLifecycleCoordinatorTests, StartAndPlannedStopFollowOneStateAuthority) {
    ASSERT_TRUE(coordinator_.BeginStart("start", 1));
    coordinator_.MarkStageComplete(StartStage::kProviderOpened);
    ASSERT_TRUE(coordinator_.CompleteStart("running", 2));
    EXPECT_TRUE(coordinator_.AdmitsNormalWork());

    const auto plan = coordinator_.BeginQuiesce(QuiesceReason::kPlannedStop, "stop", 3);
    ASSERT_TRUE(plan.has_value());
    EXPECT_TRUE(plan->runTeardown);
    EXPECT_FALSE(plan->revokeImmediately);
    EXPECT_EQ(plan->completedStartStage, StartStage::kRunning);

    coordinator_.CompleteQuiesce(*plan, "stopped", 4);
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kStopped);
    EXPECT_FALSE(coordinator_.AdmitsNormalWork());
}

TEST_F(RuntimeLifecycleCoordinatorTests, DuplicateStartDoesNotRerunResourcePipeline) {
    ASSERT_TRUE(coordinator_.BeginStart("start", 1));
    EXPECT_FALSE(coordinator_.BeginStart("duplicate start", 2));
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kStarting);
}

TEST_F(RuntimeLifecycleCoordinatorTests, FailedStartUnwindsOnlyCompletedStages) {
    ASSERT_TRUE(coordinator_.BeginStart("start", 1));
    coordinator_.MarkStageComplete(StartStage::kQueueReady);

    const auto plan = coordinator_.BeginFailedStart("queue setup failed", 2);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->reason, QuiesceReason::kStartFailure);
    EXPECT_EQ(plan->completedStartStage, StartStage::kQueueReady);
    EXPECT_TRUE(plan->runTeardown);

    coordinator_.CompleteQuiesce(*plan, "failed start released", 3);
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kStopped);
}

TEST_F(RuntimeLifecycleCoordinatorTests, RevocationDominatesActivePlannedTeardown) {
    ASSERT_TRUE(coordinator_.BeginStart("start", 1));
    ASSERT_TRUE(coordinator_.CompleteStart("running", 2));

    const auto planned = coordinator_.BeginQuiesce(QuiesceReason::kPlannedStop, "stop", 3);
    ASSERT_TRUE(planned.has_value());
    ASSERT_TRUE(planned->runTeardown);

    const auto revoked =
        coordinator_.BeginQuiesce(QuiesceReason::kProviderRevoked, "provider removed", 4);
    ASSERT_TRUE(revoked.has_value());
    EXPECT_TRUE(revoked->revokeImmediately);
    EXPECT_FALSE(revoked->runTeardown);
    EXPECT_EQ(revoked->stateBefore, ControllerState::kQuiescing);
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kRevoked);

    coordinator_.CompleteQuiesce(*planned, "released", 5);
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kStopped);
}

TEST_F(RuntimeLifecycleCoordinatorTests, SuspendEndsSuspendedAndResumeStartsAgain) {
    ASSERT_TRUE(coordinator_.BeginStart("start", 1));
    ASSERT_TRUE(coordinator_.CompleteStart("running", 2));
    const auto suspend =
        coordinator_.BeginQuiesce(QuiesceReason::kSystemSuspend, "suspend", 3);
    ASSERT_TRUE(suspend.has_value());
    coordinator_.CompleteQuiesce(*suspend, "suspended", 4);
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kSuspended);

    EXPECT_TRUE(coordinator_.BeginStart("resume", 5));
    EXPECT_EQ(coordinator_.CurrentState(), ControllerState::kStarting);
}

} // namespace
