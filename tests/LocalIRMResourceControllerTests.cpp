// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceControllerTests.cpp — Unit tests for LocalIRMResourceController.

#include "Bus/IRM/LocalIRMResourceController.hpp"
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
};

TEST_F(LocalIRMResourceControllerTests, InitializeDefaultsSuccess) {
    LocalIRMResourceController controller(&hardware_);
    EXPECT_EQ(controller.Snapshot().state, LocalIRMResourceState::Disabled);

    bool ok = controller.InitializeDefaults();
    EXPECT_TRUE(ok);

    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.state, LocalIRMResourceState::Initialized);
    EXPECT_TRUE(snap.readbackValid);
    EXPECT_EQ(snap.lastCsrStatus, 0); // OK
    EXPECT_EQ(snap.busManagerId, 0x3F);
    EXPECT_EQ(snap.bandwidthAvailable, 4915);
    EXPECT_EQ(snap.channelsAvailableHi, 0xFFFFFFFF);
    EXPECT_EQ(snap.channelsAvailableLo, 0x7FFFFFFF);
}

TEST_F(LocalIRMResourceControllerTests, ProbeReadbackSuccess) {
    LocalIRMResourceController controller(&hardware_);
    
    // Setup some custom mock values in the stub
    hardware_.WriteLocalIRMResource(0, 10);
    hardware_.WriteLocalIRMResource(1, 2000);
    hardware_.WriteLocalIRMResource(2, 0xAAAAAAAA);
    hardware_.WriteLocalIRMResource(3, 0x55555555);

    bool ok = controller.ProbeReadback();
    EXPECT_TRUE(ok);

    auto snap = controller.Snapshot();
    EXPECT_TRUE(snap.readbackValid);
    EXPECT_EQ(snap.busManagerId, 10);
    EXPECT_EQ(snap.bandwidthAvailable, 2000);
    EXPECT_EQ(snap.channelsAvailableHi, 0xAAAAAAAA);
    EXPECT_EQ(snap.channelsAvailableLo, 0x55555555);
}

TEST_F(LocalIRMResourceControllerTests, CompareSwapUpdatesCache) {
    LocalIRMResourceController controller(&hardware_);
    
    // Init values
    ASSERT_TRUE(controller.InitializeDefaults());

    // Perform CompareSwap on BUS_MANAGER_ID (selectCode 0)
    // compareValue = 0x3F, newValue = 4
    auto lockRes = controller.CompareSwapBusManagerId(0x3F, 4);
    EXPECT_EQ(lockRes.status, LocalCSRLockResult::Status::Success);
    EXPECT_TRUE(lockRes.compareMatched);
    EXPECT_EQ(lockRes.oldValue, 0x3F);

    auto snap = controller.Snapshot();
    EXPECT_EQ(snap.busManagerId, 4);
    EXPECT_EQ(snap.lastCsrStatus, 0); // OK
}

TEST_F(LocalIRMResourceControllerTests, DisableState) {
    LocalIRMResourceController controller(&hardware_);
    ASSERT_TRUE(controller.InitializeDefaults());
    EXPECT_EQ(controller.Snapshot().state, LocalIRMResourceState::Initialized);

    controller.Disable();
    EXPECT_EQ(controller.Snapshot().state, LocalIRMResourceState::Disabled);
    EXPECT_FALSE(controller.Snapshot().readbackValid);
    EXPECT_EQ(controller.Snapshot().busManagerId, 0x3F);
    EXPECT_EQ(controller.Snapshot().bandwidthAvailable, 0);
    EXPECT_EQ(controller.Snapshot().channelsAvailableHi, 0);
    EXPECT_EQ(controller.Snapshot().channelsAvailableLo, 0);
}
