// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceControllerTests.cpp — Unit tests for LocalIRMResourceController.

#include "Bus/IRM/LocalIRMResourceController.hpp"
#include "Bus/CSR/BroadcastChannelCSR.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "gtest/gtest.h"

using namespace ASFW::Bus;
using namespace ASFW::Driver;

class LocalIRMResourceControllerTests : public ::testing::Test {
protected:
    void SetUp() override {
        hardware_.ResetTestState();
    }

    HardwareInterface hardware_;
    BroadcastChannelCSR broadcastChannel_;
};

TEST_F(LocalIRMResourceControllerTests, InitialState) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);
    EXPECT_EQ(controller.Snapshot().state, LocalIRMResourceState::Disabled);
    EXPECT_EQ(broadcastChannel_.Read(), 0x8000001F);
}

TEST_F(LocalIRMResourceControllerTests, BusResetResetsState) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);
    broadcastChannel_.MarkValidChannel31();
    
    controller.OnBusResetStarted(5);
    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::InitialRegistersProgrammed);
    EXPECT_EQ(snap.generation, 5);
    EXPECT_EQ(broadcastChannel_.Read(), 0x8000001F);
}

TEST_F(LocalIRMResourceControllerTests, TopologyReady_NotIRM_DisablesHosting) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);
    controller.OnTopologyReady(10, 0, 2, true); // Local=0, IRM=2
    
    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::NotLocalIRM);
    EXPECT_FALSE(snap.localIsIRM);
    EXPECT_EQ(broadcastChannel_.Read(), 0x8000001F);
}

TEST_F(LocalIRMResourceControllerTests, TopologyReady_IsIRM_ProbesAndSetsValid) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);
    
    // Local=2, IRM=2
    controller.OnTopologyReady(10, 2, 2, true);
    
    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::ReadyDefaults);
    EXPECT_TRUE(snap.localIsIRM);
    EXPECT_TRUE(snap.activeProbeAttempted);
    EXPECT_TRUE(snap.activeProbeSucceeded);
    EXPECT_EQ(broadcastChannel_.Read(), 0xC000001F);
}

TEST_F(LocalIRMResourceControllerTests, TopologyReady_IsIRM_ProbesReadyChanged) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);
    
    // Setup hardware with some non-default value in BANDWIDTH_AVAILABLE (select 1)
    (void)hardware_.WriteLocalIRMResource(1, 1000);
    
    controller.OnTopologyReady(10, 2, 2, true);
    
    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::ReadyChanged);
    EXPECT_EQ(snap.bandwidthAvailable, 1000);
}

TEST_F(LocalIRMResourceControllerTests, TopologyReady_IsIRM_Channel31AvailableLeavesBroadcastInvalid) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);

    // CHANNELS_AVAILABLE_HI bit 0 means channel 31 still appears available.
    (void)hardware_.WriteLocalIRMResource(2, 0xFFFFFFFFu);

    controller.OnTopologyReady(10, 2, 2, true);

    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::ReadyChanged);
    EXPECT_EQ(broadcastChannel_.Read(), 0x8000001F);
}

TEST_F(LocalIRMResourceControllerTests, RoleDoesNotAllowIRMHost_DisablesHosting) {
    hardware_.SetInitialIRMRegistersProgrammed(true);
    LocalIRMResourceController controller(hardware_, broadcastChannel_);
    controller.OnTopologyReady(10, 2, 2, false); // Local=IRM but role=ClientOnly
    
    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::Disabled);
    EXPECT_EQ(broadcastChannel_.Read(), 0x8000001F);
}
