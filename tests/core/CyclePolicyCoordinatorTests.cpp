// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CyclePolicyCoordinatorTests.cpp — Unit tests for CyclePolicyCoordinator.

#include "Bus/BusManager/CyclePolicyCoordinator.hpp"
#include "gtest/gtest.h"

using namespace ASFW::Bus;
using namespace ASFW::FW;

class CyclePolicyCoordinatorTests : public ::testing::Test {
protected:
    CyclePolicyCoordinator planner_;
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

TEST_F(CyclePolicyCoordinatorTests, ClientOnlySuppressesCyclePolicy) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::ClientOnly;
    in.localIsIRM = true;
    in.irmFallbackGateOpen = true;
    in.irmFallbackNoBMDetected = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::SuppressedByRoleMode);
}

TEST_F(CyclePolicyCoordinatorTests, ElectionOnlySuppressesLocalCycleMaster) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::ElectionOnly;
    in.localIsBM = true;
    in.localIsRoot = true;
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::SuppressedByActivityLevel);
}

TEST_F(CyclePolicyCoordinatorTests, LocalBMAndLocalRootPlansLocalCycleMaster) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    MarkLocalSelfIdRoot(in);
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::LocalRootEnableCycleMaster);
}

TEST_F(CyclePolicyCoordinatorTests, LocalRootWithCycleMasterAlreadyEnabledIsSatisfied) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    in.localCycleMasterEnabled = true;
    MarkLocalSelfIdRoot(in);

    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::AlreadySatisfiedLocalCycleMasterEnabled);
}

TEST_F(CyclePolicyCoordinatorTests, LocalCycleMasterSetWhileNotRootPlansClearBeforeRoleSuppression) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::ClientOnly;
    in.activityLevel = FullBMActivityLevel::ObserveOnly;
    in.localIsBM = false;
    in.localIsRoot = false;
    in.localCycleMasterEnabled = true;

    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::LocalCycleMasterClearNotRoot);
}

TEST_F(CyclePolicyCoordinatorTests, LocalRootSelfIDUnknownDefers) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::DeferLocalSelfIDUnknown);
}

TEST_F(CyclePolicyCoordinatorTests, LocalRootLinkInactiveRequiresRootSelection) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    in.localSelfIdKnown = true;
    in.localSelfIdLinkActive = false;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RootSelectionRequired);
}

TEST_F(CyclePolicyCoordinatorTests, CycleStartObservedSuppressesOnlyIrmFallbackRepair) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::IRMResourceHost;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsIRM = true;
    in.irmFallbackGateOpen = true;
    in.irmFallbackNoBMDetected = true;
    in.localIsRoot = true;
    in.cycleStartObserved = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::AlreadySatisfiedCycleStartObserved);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootSelfIDUnknownDefers) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootNodeId = 2;
    in.irmNodeId = 1;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::DeferRootSelfIDUnknown);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootIsIrmDefersCmstrBeforeBib) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootNodeId = 2;
    in.irmNodeId = 2;
    in.rootCmcKnown = false;
    MarkRemoteRootSelfIdContender(in, 2);

    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::DeferRootBibCmcUnknown);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootBibCmcFalseAndCycleSeenSuppressesCmstr) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    MarkRemoteRootSelfIdContender(in);
    in.cycleStartObserved = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::AlreadySatisfiedCycleStartObserved);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootBibCmcFalseWithoutCycleRequiresRootSelection) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    MarkRemoteRootSelfIdContender(in);

    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RootSelectionRequired);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootSelfIDContenderAtCyclePolicyAllowedPlansWrite) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RemoteRootSetCmstr);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootSelfIDContenderStillPlansWriteWhenCycleStartObserved) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    in.cycleStartObserved = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RemoteRootSetCmstr);
}

TEST_F(CyclePolicyCoordinatorTests, IRMFallbackLocalRootPlansLocalCycleMaster) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::IRMResourceHost;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = false;
    in.localIsIRM = true;
    in.irmFallbackGateOpen = true;
    in.irmFallbackNoBMDetected = true;
    in.localIsRoot = true;
    MarkLocalSelfIdRoot(in);
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::LocalRootEnableCycleMaster);
}

TEST_F(CyclePolicyCoordinatorTests, IRMFallbackRemoteRootDoesNotSendRemoteCmstrInIRMResourceHost) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::IRMResourceHost;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsIRM = true;
    in.irmFallbackGateOpen = true;
    in.irmFallbackNoBMDetected = true;
    in.localIsRoot = false;
    MarkRemoteRootSelfIdContender(in);
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RootSelectionRequired);
}
