// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// PowerLinkPolicyCoordinatorTests.cpp — Unit tests for PowerLinkPolicyCoordinator (Milestone 8).

#include "Bus/BusManager/PowerLinkPolicyCoordinator.hpp"
#include "gtest/gtest.h"
#include "gmock/gmock.h"

using namespace ASFW::Bus;
using namespace ASFW::FW;
using namespace ASFW::Driver;

class PowerLinkPolicyCoordinatorTests : public ::testing::Test {
protected:
    PowerLinkPolicyCoordinator coordinator_{PowerLinkPolicyConfig{}};
};

static ASFW::Driver::TopologySnapshot MakeTwoNodePowerTopology(uint8_t localPower,
                                                               uint8_t remotePower,
                                                               bool remoteLinkActive = false) {
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.physical.nodes.resize(2);
    topo.physical.nodes[0].physicalId = 0;
    topo.physical.nodes[0].linkActive = true;
    topo.physical.nodes[0].maxSpeedMbps = 400;
    topo.physical.nodes[0].powerClass = localPower;
    topo.physical.nodes[1].physicalId = 1;
    topo.physical.nodes[1].linkActive = remoteLinkActive;
    topo.physical.nodes[1].maxSpeedMbps = 400;
    topo.physical.nodes[1].powerClass = remotePower;
    return topo;
}

TEST_F(PowerLinkPolicyCoordinatorTests, InitialState) {
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, PowerPolicyDecision::None);
}

TEST_F(PowerLinkPolicyCoordinatorTests, ClientOnly_SuppressesPowerPolicy) {
    PowerLinkPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::ClientOnly;
    
    EXPECT_EQ(coordinator_.Plan(in, coordinator_.BuildCandidates(in)), PowerPolicyDecision::SuppressedByRoleMode);
}

TEST_F(PowerLinkPolicyCoordinatorTests, ObserveOnlyPowerPolicy_Suppresses) {
    PowerLinkPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::ObserveOnly;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in, coordinator_.BuildCandidates(in)), PowerPolicyDecision::SuppressedByPolicyLevel);
}

TEST_F(PowerLinkPolicyCoordinatorTests, NotBMOrFallbackIRM_Suppresses) {
    PowerLinkPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = false;
    in.localIsIRM = false;
    
    EXPECT_EQ(coordinator_.Plan(in, coordinator_.BuildCandidates(in)), PowerPolicyDecision::SuppressedNotBMOrFallbackIRM);
}

TEST_F(PowerLinkPolicyCoordinatorTests, InvalidTopology_Suppresses) {
    PowerLinkPolicyInputs in{};
    in.topologyValid = false;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = true;
    
    EXPECT_EQ(coordinator_.Plan(in, {}), PowerPolicyDecision::SuppressedByTopology);
}

TEST_F(PowerLinkPolicyCoordinatorTests, PowerBudgetUnknown_DefersByDefault) {
    PowerLinkPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = true;
    in.powerBudgetStatus = PowerBudgetStatus::Unknown;
    
    EXPECT_EQ(coordinator_.Plan(in, coordinator_.BuildCandidates(in)), PowerPolicyDecision::DeferredPowerBudgetUnknown);
}

TEST_F(PowerLinkPolicyCoordinatorTests, LinkInactiveRemoteNode_BecomesCandidate) {
    PowerLinkPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    topo.physical.nodes.resize(2);
    topo.physical.nodes[0].physicalId = 0; // Local
    topo.physical.nodes[0].linkActive = true;
    topo.physical.nodes[0].maxSpeedMbps = 400;
    topo.physical.nodes[1].physicalId = 1; // Remote, link inactive
    topo.physical.nodes[1].linkActive = false;
    topo.physical.nodes[1].maxSpeedMbps = 400;
    
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = true;
    in.localNodeId = 0;
    in.rootNodeId = 0;
    in.powerBudgetStatus = PowerBudgetStatus::Sufficient;
    
    auto candidates = coordinator_.BuildCandidates(in);
    EXPECT_EQ(coordinator_.Plan(in, candidates), PowerPolicyDecision::LinkOnRequired);
    
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].nodeId, 1);
    EXPECT_TRUE(candidates[0].eligibleForLinkOn);
}

TEST_F(PowerLinkPolicyCoordinatorTests, PowerBudgetEstimate_SufficientForBusPoweredNode) {
    auto topo = MakeTwoNodePowerTopology(
        static_cast<uint8_t>(PowerClass::SelfPower_15W),
        static_cast<uint8_t>(PowerClass::BusPowered_UpTo3W));

    PowerLinkPolicyInputs in{};
    in.topology = &topo;
    in.topologyValid = true;

    const auto estimate = coordinator_.EstimatePowerBudget(in);
    EXPECT_EQ(estimate.status, PowerBudgetStatus::Sufficient);
    EXPECT_EQ(estimate.availableMilliWatts, 15000u);
    EXPECT_EQ(estimate.requiredMilliWatts, 3000u);
    EXPECT_EQ(estimate.unknownPowerClassNodes, 0u);
}

TEST_F(PowerLinkPolicyCoordinatorTests, PowerBudgetEstimate_InsufficientWithoutProvider) {
    auto topo = MakeTwoNodePowerTopology(
        static_cast<uint8_t>(PowerClass::NoPower),
        static_cast<uint8_t>(PowerClass::BusPowered_UpTo3W));

    PowerLinkPolicyInputs in{};
    in.topology = &topo;
    in.topologyValid = true;

    const auto estimate = coordinator_.EstimatePowerBudget(in);
    EXPECT_EQ(estimate.status, PowerBudgetStatus::Insufficient);
    EXPECT_EQ(estimate.availableMilliWatts, 0u);
    EXPECT_EQ(estimate.requiredMilliWatts, 3000u);
}

TEST_F(PowerLinkPolicyCoordinatorTests, PowerBudgetEstimate_ReservedClassIsUnknown) {
    auto topo = MakeTwoNodePowerTopology(
        static_cast<uint8_t>(PowerClass::SelfPower_15W),
        static_cast<uint8_t>(PowerClass::Reserved101));

    PowerLinkPolicyInputs in{};
    in.topology = &topo;
    in.topologyValid = true;

    const auto estimate = coordinator_.EstimatePowerBudget(in);
    EXPECT_EQ(estimate.status, PowerBudgetStatus::Unknown);
    EXPECT_EQ(estimate.unknownPowerClassNodes, 1u);
}

TEST_F(PowerLinkPolicyCoordinatorTests, InsufficientPowerSuppressesLinkOn) {
    auto topo = MakeTwoNodePowerTopology(
        static_cast<uint8_t>(PowerClass::NoPower),
        static_cast<uint8_t>(PowerClass::BusPowered_UpTo3W));

    PowerLinkPolicyInputs in{};
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = true;
    in.localNodeId = 0;
    in.rootNodeId = 0;

    struct MockExecutor : public ILinkOnExecutor {
        MOCK_METHOD(bool, SendLinkOnPacket, (uint32_t, uint16_t, uint8_t), (override));
    } executor;

    EXPECT_CALL(executor, SendLinkOnPacket(testing::_, testing::_, testing::_)).Times(0);

    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, PowerPolicyDecision::DeferredInsufficientPower);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, PowerPolicyAction::None);
    EXPECT_EQ(coordinator_.Snapshot().eligibleNodeCount, 1u);
    EXPECT_EQ(coordinator_.Snapshot().linkOnSubmittedCount, 0u);
}

TEST_F(PowerLinkPolicyCoordinatorTests, EvaluateComputesSufficientBudgetAndSendsLinkOn) {
    auto topo = MakeTwoNodePowerTopology(
        static_cast<uint8_t>(PowerClass::SelfPower_15W),
        static_cast<uint8_t>(PowerClass::BusPowered_UpTo3W));

    PowerLinkPolicyInputs in{};
    in.generation = 9;
    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = true;
    in.localNodeId = 0;
    in.rootNodeId = 0;

    struct MockExecutor : public ILinkOnExecutor {
        MOCK_METHOD(bool, SendLinkOnPacket, (uint32_t, uint16_t, uint8_t), (override));
    } executor;

    EXPECT_CALL(executor, SendLinkOnPacket(9, testing::_, 1)).WillOnce(testing::Return(true));

    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().powerBudgetStatus, PowerBudgetStatus::Sufficient);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, PowerPolicyDecision::LinkOnRequired);
    EXPECT_EQ(coordinator_.Snapshot().lastAction, PowerPolicyAction::SendLinkOnPackets);
    EXPECT_EQ(coordinator_.Snapshot().linkOnSubmittedCount, 1u);
    EXPECT_EQ(coordinator_.Snapshot().linkOnSuccessCount, 1u);
}

TEST_F(PowerLinkPolicyCoordinatorTests, AttemptLimit_PreventsRepeatedLinkOn) {
    PowerLinkPolicyInputs in{};
    ASFW::Driver::TopologySnapshot topo{};
    topo.nodeCount = 2;
    topo.physical.nodes.resize(2);
    topo.physical.nodes[0].physicalId = 0;
    topo.physical.nodes[0].linkActive = true;
    topo.physical.nodes[0].maxSpeedMbps = 400;
    topo.physical.nodes[1].physicalId = 1;
    topo.physical.nodes[1].linkActive = false;
    topo.physical.nodes[1].maxSpeedMbps = 400;

    in.topology = &topo;
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
    in.localIsBM = true;
    in.localNodeId = 0;
    in.rootNodeId = 0;
    in.powerBudgetStatus = PowerBudgetStatus::Sufficient;

    struct MockExecutor : public ILinkOnExecutor {
        MOCK_METHOD(bool, SendLinkOnPacket, (uint32_t, uint16_t, uint8_t), (override));
    } executor;

    EXPECT_CALL(executor, SendLinkOnPacket(testing::_, testing::_, 1)).WillOnce(testing::Return(true));

    // First evaluation: Sends Link-On
    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, PowerPolicyDecision::LinkOnRequired);
    EXPECT_EQ(coordinator_.Snapshot().linkOnSubmittedCount, 1);

    // Second evaluation: Throttled
    coordinator_.Evaluate(in, executor);
    EXPECT_EQ(coordinator_.Snapshot().lastDecision, PowerPolicyDecision::LinkOnAlreadyAttemptedThisGeneration);
    EXPECT_EQ(coordinator_.Snapshot().linkOnSubmittedCount, 1);
}
