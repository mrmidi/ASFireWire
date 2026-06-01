// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRContractVerifierTests.cpp — Unit tests for CSRContractVerifier (Milestone 9).

#include "Bus/CSR/CSRContractVerifier.hpp"
#include "Bus/CSR/CSRResponder.hpp"
#include "Bus/CSR/TopologyMapService.hpp"
#include "Bus/CSR/SpeedMapService.hpp"
#include "Bus/IRM/LocalIRMResourceController.hpp"
#include "Bus/CSR/BroadcastChannelCSR.hpp"
#include "Hardware/HardwareInterface.hpp"
#include <gtest/gtest.h>

using namespace ASFW::Bus;
using namespace ASFW::Driver;
namespace FW = ASFW::FW;

class CSRContractVerifierTests : public ::testing::Test {
protected:
    void SetUp() override {
        hardware_.ResetTestState();
    }

    HardwareInterface hardware_;
    BroadcastChannelCSR broadcastChannel_;
    TopologyMapService topologyMap_{&hardware_};
    SpeedMapService speedMap_;
    LocalIRMResourceController irm_{hardware_, broadcastChannel_};
};

TEST_F(CSRContractVerifierTests, InitialState_IsInvalid) {
    CSRResponder::Deps deps{};
    CSRResponder responder(deps);
    CSRContractVerifier verifier;
    
    // Maps are generation 0/invalid initially
    auto result = verifier.Verify(responder, topologyMap_, speedMap_, irm_);
    EXPECT_FALSE(result.ok);
}

TEST_F(CSRContractVerifierTests, ValidMaps_Ok) {
    CSRResponder::Deps deps{};
    CSRResponder responder(deps);
    CSRContractVerifier verifier;
    
    TopologySnapshot topo{};
    topo.generation = 1;
    topo.nodeCount = 1;
    topo.graphStatus = TopologyGraphStatus::Valid;
    topo.physical.nodes.resize(1);
    topo.physical.nodes[0].linkActive = true;
    
    topologyMap_.Start();
    topologyMap_.Rebuild(topo);
    speedMap_.PublishFromTopology(topo);
    
    auto result = verifier.Verify(responder, topologyMap_, speedMap_, irm_);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.topologyMapGenerationMatch);
    EXPECT_TRUE(result.speedMapGenerationMatch);
}

TEST_F(CSRContractVerifierTests, DetectsUnexpectedSoftwareHits) {
    CSRResponder::Deps deps{};
    CSRResponder responder(deps);
    CSRContractVerifier verifier;
    
    // Simulate remote read of BUS_MANAGER_ID (HW owned) hitting SW responder
    (void)responder.ReadQuadlet(FW::kCSR_BusManagerID);
    
    auto result = verifier.Verify(responder, topologyMap_, speedMap_, irm_);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.hardwareOwnedSoftwareHits, 1);
}
