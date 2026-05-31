// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// RootSelectionCoordinatorTests.cpp — Unit tests for RootSelectionCoordinator (Milestone 6).

#include "Bus/BusManager/RootSelectionCoordinator.hpp"
#include "gtest/gtest.h"
#include "gmock/gmock.h"

using namespace ASFW::Bus;
using namespace ASFW::FW;

class RootSelectionCoordinatorTests : public ::testing::Test {
protected:
    RootSelectionCoordinator coordinator_{RootSelectionConfig{}};
};

TEST_F(RootSelectionCoordinatorTests, InitialState) {
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, RootSelectionDecision::None);
}

TEST_F(RootSelectionCoordinatorTests, ClientOnly_SuppressesRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::ClientOnly;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SuppressedByRoleMode);
}

TEST_F(RootSelectionCoordinatorTests, ObserveOnly_SuppressesRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ObserveOnly;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SuppressedByActivityLevel);
}

TEST_F(RootSelectionCoordinatorTests, ElectionOnly_SuppressesRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ElectionOnly;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SuppressedByActivityLevel);
}

TEST_F(RootSelectionCoordinatorTests, CyclePolicyAllowed_SuppressesRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SuppressedByActivityLevel);
}

TEST_F(RootSelectionCoordinatorTests, ForceRootAllowed_AllowsSelection) {
    RootSelectionInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsBM = true;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false; // Root is unsuitable
    
    // Need a topology with a local candidate
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    topo.physical.nodes.resize(2);
    topo.physical.nodes[0].physicalId = 0; // Local
    topo.physical.nodes[0].linkActive = true;
    topo.physical.nodes[1].physicalId = 1; // Root
    topo.physical.nodes[1].linkActive = true;
    
    in.localNodeId = 0;
    in.rootNodeId = 1;
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    in.topology = &topo;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SelectLocalRoot);
}

TEST_F(RootSelectionCoordinatorTests, CycleStartObserved_SuppressesRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsBM = true;
    in.cycleStartObserved = true;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SuppressedCycleAlreadyObserved);
}

TEST_F(RootSelectionCoordinatorTests, RootCmcUnknown_DefersRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsBM = true;
    in.rootCmcKnown = false;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::DeferredRootEvidenceIncomplete);
}

TEST_F(RootSelectionCoordinatorTests, RootCmcCapable_SuppressesRootSelection) {
    RootSelectionInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsBM = true;
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SuppressedRootAlreadySuitable);
}

TEST_F(RootSelectionCoordinatorTests, FallbackIRM_NoBM_GateOpen_AllowsRootSelection) {
    RootSelectionInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::IRMResourceHost;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsIRM = true;
    in.irmFallbackGateOpen = true;
    in.irmFallbackNoBMDetected = true;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    topo.physical.nodes.resize(2);
    topo.physical.nodes[0].physicalId = 0; // Local
    topo.physical.nodes[0].linkActive = true;
    topo.physical.nodes[1].physicalId = 1; // Root
    topo.physical.nodes[1].linkActive = true;
    
    in.localNodeId = 0;
    in.rootNodeId = 1;
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    in.topology = &topo;
    
    EXPECT_EQ(coordinator_.Plan(in), RootSelectionDecision::SelectLocalRoot);
}

TEST_F(RootSelectionCoordinatorTests, RetryLimit_BoundsForceRootAttempts) {
    RootSelectionInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsBM = true;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    topo.physical.nodes.resize(2);
    topo.physical.nodes[0].physicalId = 0;
    topo.physical.nodes[0].linkActive = true;
    topo.physical.nodes[1].physicalId = 1;
    topo.physical.nodes[1].linkActive = true;
    
    in.localNodeId = 0;
    in.rootNodeId = 1;
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    in.topology = &topo;

    struct MockExecutor : public IRootSelectionExecutor {
        MOCK_METHOD(bool, ForceRootAndResetForBMPolicy, (uint32_t, uint8_t, bool, std::optional<uint8_t>), (override));
    } executor;

    EXPECT_CALL(executor, ForceRootAndResetForBMPolicy(testing::_, testing::_, testing::_, testing::_)).WillRepeatedly(testing::Return(true));

    // Perform 5 attempts (the default limit)
    for (int i = 0; i < 5; ++i) {
        coordinator_.Evaluate(in, executor);
        EXPECT_EQ(coordinator_.Snapshot().lastDecision, RootSelectionDecision::SelectLocalRoot);
    }

    // 6th attempt should hit limit
    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, RootSelectionDecision::FailedRetryLimit);
    EXPECT_TRUE(coordinator_.Snapshot().retryLimitHit);
}

TEST_F(RootSelectionCoordinatorTests, StableTopologyChange_ResetsRetryCounter) {
    RootSelectionInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ForceRootAllowed;
    in.localIsBM = true;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    
    ASFW::Driver::TopologySnapshot topo1{};
    topo1.nodeCount = 2;
    topo1.physical.nodes.resize(2);
    topo1.physical.nodes[0].physicalId = 0;
    topo1.physical.nodes[0].linkActive = true;
    topo1.physical.nodes[1].physicalId = 1;
    topo1.physical.nodes[1].linkActive = true;
    
    in.localNodeId = 0;
    in.rootNodeId = 1;
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    in.topology = &topo1;

    struct MockExecutor : public IRootSelectionExecutor {
        MOCK_METHOD(bool, ForceRootAndResetForBMPolicy, (uint32_t, uint8_t, bool, std::optional<uint8_t>), (override));
    } executor;

    EXPECT_CALL(executor, ForceRootAndResetForBMPolicy(testing::_, testing::_, testing::_, testing::_)).WillRepeatedly(testing::Return(true));

    // Perform 5 attempts on topo1 (limit is 5)
    for (int i = 0; i < 5; ++i) {
        coordinator_.Evaluate(in, executor);
    }
    // 6th attempt should hit limit
    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, RootSelectionDecision::FailedRetryLimit);

    // Change topology (add a node)
    ASFW::Driver::TopologySnapshot topo2{};
    topo2.nodeCount = 3;
    topo2.physical.nodes.resize(3);
    topo2.physical.nodes[0].physicalId = 0;
    topo2.physical.nodes[0].linkActive = true;
    topo2.physical.nodes[1].physicalId = 1;
    topo2.physical.nodes[1].linkActive = true;
    topo2.physical.nodes[2].physicalId = 2;
    topo2.physical.nodes[2].linkActive = true;
    
    in.topology = &topo2;

    // Next attempt should succeed again
    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, RootSelectionDecision::SelectLocalRoot);
    EXPECT_EQ(coordinator_.Snapshot().attemptsThisTopology, 1);
}
