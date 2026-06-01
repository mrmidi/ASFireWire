// SPDX-License-Identifier: LGPL-3.0-or-later
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
