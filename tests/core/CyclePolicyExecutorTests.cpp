// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// CyclePolicyExecutorTests.cpp — Unit tests for CyclePolicyCoordinator executor logic.

#include "Bus/BusManager/CyclePolicyCoordinator.hpp"
#include "gtest/gtest.h"
#include "gmock/gmock.h"

using namespace ASFW::Bus;
using namespace ASFW::FW;
using testing::_;
using testing::Return;

class MockCyclePolicyExecutor : public ICyclePolicyExecutor {
public:
    MOCK_METHOD(bool, EnableLocalCycleMasterMutation, (uint32_t generation), (override));
    MOCK_METHOD(bool, ClearLocalCycleMasterMutation, (uint32_t generation), (override));
    MOCK_METHOD(ASFW::Async::AsyncHandle, WriteRemoteStateSetCmstr, (uint32_t generation, uint16_t busBase16, uint8_t targetNodeId), (override));
};

class CyclePolicyExecutorTests : public ::testing::Test {
protected:
    CyclePolicyCoordinator coordinator_;
    MockCyclePolicyExecutor executor_;
};

static void MarkLocalSelfIdRoot(CyclePolicyInputs& in) {
    in.localSelfIdKnown = true;
    in.localSelfIdLinkActive = true;
    in.localSelfIdContender = true;
}

static void MarkRemoteRootSelfIdContender(CyclePolicyInputs& in, uint8_t rootNode = 2) {
    in.rootNodeId = rootNode;
    in.rootSelfIdKnown = true;
    in.rootSelfIdLinkActive = true;
    in.rootSelfIdContender = true;
}

TEST_F(CyclePolicyExecutorTests, ExecutorEnablesLocalCycleMasterOnlyOncePerGeneration) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    MarkLocalSelfIdRoot(in);
    in.cycleStartObserved = false;

    // First call: should trigger mutation
    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(5)).WillOnce(Return(true));
    coordinator_.Evaluate(in, executor_);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, CyclePolicyAction::EnableLocalCycleMaster);
    EXPECT_EQ(coordinator_.Snapshot().localCycleMasterEnableCount, 1);

    // Second call: should be throttled
    coordinator_.Evaluate(in, executor_);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, CyclePolicyDecision::AlreadySatisfiedLocalCycleMasterEnabled);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, CyclePolicyAction::None);
    EXPECT_EQ(coordinator_.Snapshot().localCycleMasterEnableCount, 1);
}

TEST_F(CyclePolicyExecutorTests, ExecutorSubmitsRemoteCmstrWhenNotRoot) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.busBase16 = 0xFFC0;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false; // Not root!
    MarkRemoteRootSelfIdContender(in, 2);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    in.cycleStartObserved = false;

    ASFW::Async::AsyncHandle handle{123};
    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(_)).Times(0);
    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(5, 0xFFC0, 2)).WillOnce(Return(handle));
    coordinator_.Evaluate(in, executor_);
    
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, CyclePolicyDecision::RemoteRootSetCmstr);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, CyclePolicyAction::WriteRemoteStateSetCmstr);
}

TEST_F(CyclePolicyExecutorTests, ExecutorClearsLocalCycleMasterWhenNotRoot) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.topologyValid = true;
    in.roleMode = RoleMode::ClientOnly;
    in.activityLevel = FullBMActivityLevel::ObserveOnly;
    in.localIsRoot = false;
    in.localCycleMasterEnabled = true;

    EXPECT_CALL(executor_, ClearLocalCycleMasterMutation(5)).WillOnce(Return(true));
    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(_)).Times(0);
    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(_, _, _)).Times(0);

    coordinator_.Evaluate(in, executor_);

    EXPECT_EQ(coordinator_.Snapshot().lastDecision, CyclePolicyDecision::LocalCycleMasterClearNotRoot);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, CyclePolicyAction::ClearLocalCycleMaster);
    EXPECT_EQ(coordinator_.Snapshot().localCycleMasterBefore, true);
    EXPECT_EQ(coordinator_.Snapshot().localCycleMasterAfter, false);
    EXPECT_EQ(coordinator_.Snapshot().localCycleMasterClearCount, 1);
}

TEST_F(CyclePolicyExecutorTests, ExecutorSubmitsRemoteCmstrWriteWithCorrectAddress) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.busBase16 = 0xFFC0;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in, 2);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    in.cycleStartObserved = false;

    ASFW::Async::AsyncHandle handle{123};
    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(5, 0xFFC0, 2)).WillOnce(Return(handle));
    
    coordinator_.Evaluate(in, executor_);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, CyclePolicyAction::WriteRemoteStateSetCmstr);
    EXPECT_EQ(coordinator_.Snapshot().targetNode, 2);
    EXPECT_EQ(coordinator_.Snapshot().remoteCmstrSubmitCount, 1);
}

TEST_F(CyclePolicyExecutorTests, ExecutorSuppressesRemoteCmstrAtElectionOnly) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ElectionOnly;
    in.localIsBM = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in, 2);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    in.cycleStartObserved = false;

    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(_, _, _)).Times(0);
    coordinator_.Evaluate(in, executor_);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, CyclePolicyDecision::SuppressedByActivityLevel);
}

TEST_F(CyclePolicyExecutorTests, RemoteCmstrCallbackStaleGenerationIgnored) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in, 2);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    
    ASFW::Async::AsyncHandle handle{123};
    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(5, _, 2)).WillOnce(Return(handle));
    coordinator_.Evaluate(in, executor_);
    
    // Generation advances
    coordinator_.OnBusResetStarted(6);
    
    // Callback for generation 5 arrives
    coordinator_.OnRemoteCmstrComplete(5, 2, ASFW::Async::AsyncStatus::kSuccess);
    
    EXPECT_EQ(coordinator_.Snapshot().staleGenerationDrops, 1);
    EXPECT_EQ(coordinator_.Snapshot().remoteCmstrInFlight, false);
}

TEST_F(CyclePolicyExecutorTests, ExecutorDoesNotSubmitRemoteCmstrWhenBibCmcFalseAndCycleSeen) {
    CyclePolicyInputs in{};
    in.generation = 5;
    in.busBase16 = 0xFFC0;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in, 2);
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    in.cycleStartObserved = true;

    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(_, _, _)).Times(0);
    coordinator_.Evaluate(in, executor_);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision,
              CyclePolicyDecision::AlreadySatisfiedCycleStartObserved);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, CyclePolicyAction::None);
}

namespace {

CyclePolicyInputs LocalRootInputs(uint32_t generation) {
    CyclePolicyInputs in{};
    in.generation = generation;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    MarkLocalSelfIdRoot(in);
    return in;
}

} // namespace

// The controller clears LinkControl.cycleMaster when it raises cycleTooLong
// (OHCI 1.2 draft Table 6-1, Table 5-17). A cycle master this policy enabled
// is enabled again, as Apple re-applies its last setCycleMaster decision.
TEST_F(CyclePolicyExecutorTests, CycleTooLongReenablesTheCycleMasterThisPolicyEnabled) {
    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(5))
        .Times(2)
        .WillRepeatedly(Return(true));
    coordinator_.Evaluate(LocalRootInputs(5), executor_);
    ASSERT_EQ(coordinator_.Snapshot().localCycleMasterEnableCount, 1U);

    EXPECT_TRUE(coordinator_.OnCycleTooLong(5, executor_));
    EXPECT_EQ(coordinator_.Snapshot().cycleTooLongCount, 1U);
    EXPECT_EQ(coordinator_.Snapshot().cycleMasterRestoreCount, 1U);
}

// Not our decision: the policy never enabled the local cycle master (no
// evaluation yet, or a remote root was chosen), so nothing is written.
TEST_F(CyclePolicyExecutorTests, CycleTooLongLeavesCycleMasterOffWhenThePolicyDidNotEnableIt) {
    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(_)).Times(0);
    EXPECT_FALSE(coordinator_.OnCycleTooLong(5, executor_));

    CyclePolicyInputs remote = LocalRootInputs(5);
    remote.localIsRoot = false;
    remote.busBase16 = 0xFFC0;
    MarkRemoteRootSelfIdContender(remote, 2);
    remote.rootCmcKnown = true;
    remote.rootCmcCapable = true;
    EXPECT_CALL(executor_, WriteRemoteStateSetCmstr(5, 0xFFC0, 2))
        .WillOnce(Return(ASFW::Async::AsyncHandle{7}));
    coordinator_.Evaluate(remote, executor_);
    EXPECT_FALSE(coordinator_.OnCycleTooLong(5, executor_));
    EXPECT_EQ(coordinator_.Snapshot().cycleMasterRestoreCount, 0U);
}

// A bus reset ends the generation's decision; the next evaluation makes a new one.
TEST_F(CyclePolicyExecutorTests, CycleTooLongAfterABusResetDoesNotReenable) {
    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(5)).WillOnce(Return(true));
    coordinator_.Evaluate(LocalRootInputs(5), executor_);
    coordinator_.OnBusResetStarted(6);

    EXPECT_CALL(executor_, EnableLocalCycleMasterMutation(6)).Times(0);
    EXPECT_FALSE(coordinator_.OnCycleTooLong(6, executor_));
    EXPECT_FALSE(coordinator_.OnCycleTooLong(5, executor_));
    EXPECT_EQ(coordinator_.Snapshot().cycleTooLongCount, 2U);
}
