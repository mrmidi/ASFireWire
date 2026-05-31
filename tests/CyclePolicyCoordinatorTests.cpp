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
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::LocalRootEnableCycleMaster);
}

TEST_F(CyclePolicyCoordinatorTests, LocalRootCmcUnknownDefers) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    in.localCmcKnown = false;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::DeferLocalCmcUnknown);
}

TEST_F(CyclePolicyCoordinatorTests, LocalRootCmcFalseRequiresRootSelection) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = true;
    in.localCmcKnown = true;
    in.localCmcCapable = false;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RootSelectionRequired);
}

TEST_F(CyclePolicyCoordinatorTests, CycleStartObservedSuppressesAllCycleRepair) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.localIsBM = true;
    in.localIsRoot = true;
    in.cycleStartObserved = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::AlreadySatisfiedCycleStartObserved);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootCmcUnknownDefers) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootCmcKnown = false;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::DeferRootCmcUnknown);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootCmcFalseRequiresRootSelection) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootCmcKnown = true;
    in.rootCmcCapable = false;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RootSelectionRequired);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootCmcTrueButRemoteCmstrNotAllowedSuppresses) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::CyclePolicyAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::SuppressedByActivityLevel);
}

TEST_F(CyclePolicyCoordinatorTests, RemoteRootCmcTrueAndRemoteCmstrAllowedPlansWrite) {
    CyclePolicyInputs in{};
    in.topologyValid = true;
    in.roleMode = RoleMode::FullBusManager;
    in.activityLevel = FullBMActivityLevel::RemoteCmstrAllowed;
    in.localIsBM = true;
    in.localIsRoot = false;
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    in.rootNodeId = 2;
    
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
    in.localCmcKnown = true;
    in.localCmcCapable = true;
    
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
    in.rootCmcKnown = true;
    in.rootCmcCapable = true;
    
    EXPECT_EQ(planner_.Plan(in), CyclePolicyDecision::RootSelectionRequired);
}
