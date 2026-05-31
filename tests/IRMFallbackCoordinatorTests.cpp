// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// IRMFallbackCoordinatorTests.cpp — Unit tests for IRMFallbackCoordinator (Milestone 4).

#include "Bus/IRM/IRMFallbackCoordinator.hpp"
#include "Bus/Timing/PostResetTimingCoordinator.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Bus/BusManager/BusManagerRuntimeState.hpp"
#include "gtest/gtest.h"

using namespace ASFW::Bus;
using namespace ASFW::Driver;
using ASFW::FW::RoleMode;
using ASFW::FW::FullBMActivityLevel;

class IRMFallbackCoordinatorTests : public ::testing::Test {
protected:
    void SetUp() override {
        hardware_.ResetTestState();
    }

    HardwareInterface hardware_;
    Timing::PostResetTimingCoordinator timing_;
};

TEST_F(IRMFallbackCoordinatorTests, InitialState) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    EXPECT_EQ(coordinator->Snapshot().state, IRMFallbackState::Disabled);
}

TEST_F(IRMFallbackCoordinatorTests, ClientOnly_Disabled) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    RolePolicy policy{RoleMode::ClientOnly};
    TopologySnapshot topo{};
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.localNodeId = 0;
    topo.irmNodeId = 0;
    
    BusManagerRuntimeState bmState{};
    
    coordinator->OnTopologyReady(topo, policy, bmState, 0);
    EXPECT_EQ(coordinator->Snapshot().state, IRMFallbackState::Disabled);
}

TEST_F(IRMFallbackCoordinatorTests, NotLocalIRM_Suppressed) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    RolePolicy policy{RoleMode::IRMResourceHost};
    TopologySnapshot topo{};
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.localNodeId = 0;
    topo.irmNodeId = 2; // Local is not IRM
    
    BusManagerRuntimeState bmState{};
    
    coordinator->OnTopologyReady(topo, policy, bmState, 0);
    EXPECT_EQ(coordinator->Snapshot().state, IRMFallbackState::NotLocalIRM);
}

TEST_F(IRMFallbackCoordinatorTests, LocalIRM_GateClosed_Waiting) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    RolePolicy policy{RoleMode::IRMResourceHost};
    TopologySnapshot topo{};
    topo.generation = 1;
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.localNodeId = 2;
    topo.irmNodeId = 2;
    
    BusManagerRuntimeState bmState{};
    
    uint64_t now = 1000;
    timing_.OnSelfIDComplete(1, now);
    
    coordinator->OnTopologyReady(topo, policy, bmState, now);
    
    auto snap = coordinator->Snapshot();
    EXPECT_EQ(snap.state, IRMFallbackState::WaitingForAnnexHGate);
    EXPECT_FALSE(snap.annexHGateOpen);
}

TEST_F(IRMFallbackCoordinatorTests, LocalIRM_GateOpen_BMExists) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    RolePolicy policy{RoleMode::IRMResourceHost};
    TopologySnapshot topo{};
    topo.generation = 1;
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.localNodeId = 2;
    topo.irmNodeId = 2;
    
    // Remote node 0 is already BM
    (void)hardware_.WriteLocalIRMResource(0, 0); 
    
    BusManagerRuntimeState bmState{};
    
    uint64_t t0 = 1000;
    timing_.OnSelfIDComplete(1, t0);
    
    // Fast forward past +625ms gate
    uint64_t tFallback = t0 + 626000000ULL;
    
    coordinator->OnTopologyReady(topo, policy, bmState, tFallback);
    
    auto snap = coordinator->Snapshot();
    EXPECT_EQ(snap.state, IRMFallbackState::BMExists);
    EXPECT_TRUE(snap.busManagerExists);
    EXPECT_EQ(snap.bmNodeId, 0);
    EXPECT_EQ(snap.plannedAction, IRMFallbackAction::BMAlreadyExists);
}

TEST_F(IRMFallbackCoordinatorTests, LocalIRM_GateOpen_NoBM_PlansAction) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    RolePolicy policy{RoleMode::IRMResourceHost};
    TopologySnapshot topo{};
    topo.generation = 1;
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.localNodeId = 2;
    topo.irmNodeId = 2;
    topo.rootNodeId = 2; // Local is root
    
    // No BM (value 0x3F)
    (void)hardware_.WriteLocalIRMResource(0, 0x3F); 
    
    BusManagerRuntimeState bmState{};
    bmState.cycleStartObserved = false;
    
    uint64_t t0 = 1000;
    timing_.OnSelfIDComplete(1, t0);
    uint64_t tFallback = t0 + 626000000ULL;
    
    coordinator->OnTopologyReady(topo, policy, bmState, tFallback);
    
    auto snap = coordinator->Snapshot();
    EXPECT_EQ(snap.state, IRMFallbackState::NoBMDetected);
    EXPECT_TRUE(snap.noBusManagerDetected);
    EXPECT_EQ(snap.plannedAction, IRMFallbackAction::LocalRootEnableCycleMasterRequired);
}

TEST_F(IRMFallbackCoordinatorTests, StaleGeneration_Suppressed) {
    IRMFallbackCoordinator::Deps deps{&hardware_, &timing_, nullptr};
    auto coordinator = std::make_shared<IRMFallbackCoordinator>(deps);
    
    RolePolicy policy{RoleMode::IRMResourceHost};
    TopologySnapshot topo{};
    topo.generation = 5;
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.localNodeId = 1;
    topo.irmNodeId = 1;
    
    BusManagerRuntimeState bmState{};
    
    timing_.OnSelfIDComplete(5, 1000);
    
    coordinator->OnTopologyReady(topo, policy, bmState, 1000);
    
    // Advance generation in timing coordinator only
    timing_.OnBusResetStarted(5, 2000);
    timing_.OnSelfIDComplete(6, 3000);
    
    coordinator->MaybeEvaluate(4000);
    
    auto snap = coordinator->Snapshot();
    EXPECT_EQ(snap.state, IRMFallbackState::StaleGeneration);
    EXPECT_EQ(snap.staleGenerationDrops, 1);
}
