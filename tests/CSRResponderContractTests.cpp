// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRResponderContractTests.cpp — Verifies CSR ownership split (Milestone 9).

#include "Bus/CSR/CSRResponder.hpp"
#include "Bus/CSR/CSRContract.hpp"
#include "Bus/CSR/BroadcastChannelCSR.hpp"
#include "Bus/CSR/TopologyMapService.hpp"
#include "Bus/CSR/SpeedMapService.hpp"
#include "Hardware/HardwareInterface.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace ASFW::Bus;
using namespace ASFW::FW;
using testing::_;
using testing::Return;

namespace {

class FakeRoot : public IRootStatus {
public:
    MOCK_METHOD(bool, IsLocalRoot, (), (const, noexcept, override));
};

class FakeCycleMaster : public ICycleMasterControl {
public:
    MOCK_METHOD(void, SetCycleMaster, (bool), (noexcept, override));
    MOCK_METHOD(bool, IsCycleMasterEnabled, (), (const, noexcept, override));
};

class CSRResponderContractTests : public ::testing::Test {
protected:
    void SetUp() override {
        hardware_.ResetTestState();
        deps_.root = &root_;
        deps_.cycleMaster = &cycleMaster_;
        deps_.broadcastChannel = &broadcastChannel_;
        deps_.topologyMap = &topologyMap_;
        deps_.speedMap = &speedMap_;
    }

    ASFW::Driver::HardwareInterface hardware_;
    FakeRoot root_;
    FakeCycleMaster cycleMaster_;
    BroadcastChannelCSR broadcastChannel_;
    TopologyMapService topologyMap_{&hardware_};
    SpeedMapService speedMap_;
    CSRResponder::Deps deps_{};
};

TEST_F(CSRResponderContractTests, CoreIRMRegistersAreNotMine) {
    CSRResponder responder(deps_);
    
    // BUS_MANAGER_ID (0x21C)
    EXPECT_FALSE(responder.ReadQuadlet(CSRContract::kBusManagerId).mine);
    EXPECT_FALSE(responder.WriteQuadlet(CSRContract::kBusManagerId, 0).mine);
    
    // BANDWIDTH_AVAILABLE (0x220)
    EXPECT_FALSE(responder.ReadQuadlet(CSRContract::kBandwidthAvailable).mine);
    EXPECT_FALSE(responder.WriteQuadlet(CSRContract::kBandwidthAvailable, 0).mine);
    
    // CHANNELS_AVAILABLE_HI (0x224)
    EXPECT_FALSE(responder.ReadQuadlet(CSRContract::kChannelsAvailableHi).mine);
    EXPECT_FALSE(responder.WriteQuadlet(CSRContract::kChannelsAvailableHi, 0).mine);
    
    // CHANNELS_AVAILABLE_LO (0x228)
    EXPECT_FALSE(responder.ReadQuadlet(CSRContract::kChannelsAvailableLo).mine);
    EXPECT_FALSE(responder.WriteQuadlet(CSRContract::kChannelsAvailableLo, 0).mine);
    
    EXPECT_EQ(responder.UnexpectedResourceCsrSoftwareCount(), 8);
}

TEST_F(CSRResponderContractTests, TopologyMapIsSoftwareOwned) {
    CSRResponder responder(deps_);
    
    // Read from start of region
    auto res = responder.ReadQuadlet(CSRContract::kTopologyMapBase);
    EXPECT_TRUE(res.mine);
    
    // TOPOLOGY_MAP is read-only in SW
    EXPECT_TRUE(responder.WriteQuadlet(CSRContract::kTopologyMapBase, 0).mine);
    EXPECT_EQ(responder.WriteQuadlet(CSRContract::kTopologyMapBase, 0).rcode, ASFW::Async::ResponseCode::TypeError);
}

TEST_F(CSRResponderContractTests, SpeedMapIsSoftwareOwned) {
    CSRResponder responder(deps_);
    
    // Read from start of region
    auto res = responder.ReadQuadlet(CSRContract::kSpeedMapBase);
    EXPECT_TRUE(res.mine);
    
    // SPEED_MAP is read-only in SW
    EXPECT_TRUE(responder.WriteQuadlet(CSRContract::kSpeedMapBase, 0).mine);
    EXPECT_EQ(responder.WriteQuadlet(CSRContract::kSpeedMapBase, 0).rcode, ASFW::Async::ResponseCode::TypeError);
}

TEST_F(CSRResponderContractTests, BroadcastChannelIsSoftwareOwned) {
    CSRResponder responder(deps_);
    
    auto res = responder.ReadQuadlet(CSRContract::kBroadcastChannel);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ASFW::Async::ResponseCode::Complete);
    
    EXPECT_TRUE(responder.WriteQuadlet(CSRContract::kBroadcastChannel, 0xFFFFFFFFu).mine);
}

} // namespace
