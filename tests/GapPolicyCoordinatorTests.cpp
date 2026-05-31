// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// GapPolicyCoordinatorTests.cpp — Unit tests for GapPolicyCoordinator (Milestone 7).

#include "Bus/BusManager/GapPolicyCoordinator.hpp"
#include "gtest/gtest.h"
#include "gmock/gmock.h"

using namespace ASFW::Bus;
using namespace ASFW::FW;

class GapPolicyCoordinatorTests : public ::testing::Test {
protected:
    GapPolicyCoordinator coordinator_{GapPolicyConfig{}};
};

TEST_F(GapPolicyCoordinatorTests, ClientOnly_SuppressesGapPolicy) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::ClientOnly;
    
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::SuppressedByRoleMode);
}

TEST_F(GapPolicyCoordinatorTests, CyclePolicyAllowed_SuppressesGapPolicy) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::SuppressedByActivityLevel);
}

TEST_F(GapPolicyCoordinatorTests, GapPolicyAllowed_AllowsGapPolicy) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::GapPolicyAllowed;
    in.localIsBM = true;
    in.maxHopsKnown = true;
    in.maxHopsFromRoot = 2;
    in.betaRepeatersKnown = true;
    in.betaRepeatersPresent = false;
    in.currentGapCount = 63;
    in.gapCountConsistent = true;
    
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::GapOptimizationRequired);
}

TEST_F(GapPolicyCoordinatorTests, SingleNodeBus_SuppressesGapPolicy) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 1;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::GapPolicyAllowed;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::SuppressedSingleNodeBus);
}

TEST_F(GapPolicyCoordinatorTests, UnknownBetaRepeaters_DefersByDefault) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::GapPolicyAllowed;
    in.localIsBM = true;
    in.maxHopsKnown = true;
    in.betaRepeatersKnown = false;
    
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::DeferBetaRepeaterUnknown);
}

TEST_F(GapPolicyCoordinatorTests, BetaRepeatersPresent_UsesSafe63) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::GapPolicyAllowed;
    in.localIsBM = true;
    in.maxHopsKnown = true;
    in.maxHopsFromRoot = 1;
    in.betaRepeatersKnown = true;
    in.betaRepeatersPresent = true;
    in.currentGapCount = 63;
    in.gapCountConsistent = true;
    
    // Expected gap for beta is 63, which matches current.
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::AlreadyOptimal);
    EXPECT_EQ(coordinator_.ComputeExpectedGapCount(in, nullptr), 63);
}

TEST_F(GapPolicyCoordinatorTests, Table1394a_HopsMapping) {
    GapPolicyInputs in{};
    in.maxHopsKnown = true;
    in.betaRepeatersKnown = true;
    in.betaRepeatersPresent = false;
    
    in.maxHopsFromRoot = 1;
    EXPECT_EQ(coordinator_.ComputeExpectedGapCount(in, nullptr), 5);
    
    in.maxHopsFromRoot = 2;
    EXPECT_EQ(coordinator_.ComputeExpectedGapCount(in, nullptr), 7);
    
    in.maxHopsFromRoot = 15;
    EXPECT_EQ(coordinator_.ComputeExpectedGapCount(in, nullptr), 40);
    
    in.maxHopsFromRoot = 16;
    EXPECT_EQ(coordinator_.ComputeExpectedGapCount(in, nullptr), 63);
}

TEST_F(GapPolicyCoordinatorTests, GapMismatch_RequiresLongReset) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::GapPolicyAllowed;
    in.localIsBM = true;
    in.maxHopsKnown = true;
    in.betaRepeatersKnown = true;
    in.betaRepeatersPresent = false;
    in.gapCountConsistent = false; // Mismatch!
    
    EXPECT_EQ(coordinator_.Plan(in), GapPolicyDecision::GapMismatchRequiresLongReset);
}

TEST_F(GapPolicyCoordinatorTests, CombinedWithRootSelection) {
    GapPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::GapPolicyAllowed;
    in.localIsBM = true;
    in.maxHopsKnown = true;
    in.maxHopsFromRoot = 2; // Expected = 7
    in.betaRepeatersKnown = true;
    in.betaRepeatersPresent = false;
    in.currentGapCount = 63;
    in.gapCountConsistent = true;
    in.rootNodeId = 1;
    
    // M6 requested root 0
    in.rootSelectionRequired = true;
    in.selectedRootForRootPolicy = 0;

    struct MockExecutor : public IGapPolicyExecutor {
        MOCK_METHOD(bool, ForceRootAndGapResetForBMPolicy, (uint32_t, uint8_t, bool, uint8_t), (override));
    } executor;

    EXPECT_CALL(executor, ForceRootAndGapResetForBMPolicy(testing::_, 0, false, 7)).WillOnce(testing::Return(true));

    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, GapPolicyAction::ForceRootWithGapAndShortReset);
    EXPECT_EQ(coordinator_.Snapshot().targetRoot, 0);
    EXPECT_TRUE(coordinator_.Snapshot().combinedWithRootSelection);
}
