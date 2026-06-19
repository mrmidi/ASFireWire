// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SpeedMapServiceTests.cpp — Unit tests for SpeedMapService (Milestone 9).

#include "Bus/CSR/SpeedMapService.hpp"
#include "Bus/TopologyTypes.hpp"
#include <gtest/gtest.h>

using namespace ASFW::Bus;
using namespace ASFW::Driver;

class SpeedMapServiceTests : public ::testing::Test {
protected:
    SpeedMapService service_;
};

TEST_F(SpeedMapServiceTests, InitialState) {
    EXPECT_EQ(service_.Snapshot().status, SpeedMapStatus::Invalid);
}

TEST_F(SpeedMapServiceTests, InvalidateClearsState) {
    service_.Invalidate(5);
    EXPECT_EQ(service_.Snapshot().generation, 5);
    EXPECT_EQ(service_.Snapshot().status, SpeedMapStatus::Invalid);
}

TEST_F(SpeedMapServiceTests, SingleNode_AllSelfSpeedsValid) {
    TopologySnapshot topo{};
    topo.generation = 1;
    topo.nodeCount = 1;
    topo.localNodeId = 0;
    topo.graphStatus = TopologyGraphStatus::Valid;
    
    TopologyNodeRecord node0{};
    node0.physicalId = 0;
    node0.linkActive = true;
    node0.maxSpeedMbps = 400;
    topo.physical.nodes.push_back(node0);

    EXPECT_TRUE(service_.PublishFromTopology(topo));
    auto snap = service_.Snapshot();
    EXPECT_EQ(snap.status, SpeedMapStatus::Valid);
    EXPECT_EQ(snap.speedMatrix[0][0], FireWireSpeedCode::S400);
    
    // SPEED_MAP is obsolete in IEEE 1394-2008; ASFW serves a bounded legacy
    // 0x400-byte CSR image with 255 payload quadlets after the header.
    uint32_t q0 = 0;
    EXPECT_TRUE(service_.ReadQuadlet(0, q0));
    EXPECT_EQ(q0, (255u << 16) | 1);
}

TEST_F(SpeedMapServiceTests, TwoNodes_MinEndpointSpeed) {
    TopologySnapshot topo{};
    topo.generation = 2;
    topo.nodeCount = 2;
    topo.localNodeId = 0;
    topo.graphStatus = TopologyGraphStatus::Valid;
    
    TopologyNodeRecord node0{};
    node0.physicalId = 0;
    node0.linkActive = true;
    node0.maxSpeedMbps = 400;
    node0.links[0] = {true, 1, 0};
    
    TopologyNodeRecord node1{};
    node1.physicalId = 1;
    node1.linkActive = true;
    node1.maxSpeedMbps = 200; // S200
    node1.links[0] = {true, 0, 0};
    
    topo.physical.nodes.push_back(node0);
    topo.physical.nodes.push_back(node1);

    EXPECT_TRUE(service_.PublishFromTopology(topo));
    auto snap = service_.Snapshot();
    EXPECT_EQ(snap.speedMatrix[0][1], FireWireSpeedCode::S200);
    EXPECT_EQ(snap.speedMatrix[1][0], FireWireSpeedCode::S200);
}

TEST_F(SpeedMapServiceTests, ThreeNodeChain_MinSpeedAlongPath) {
    TopologySnapshot topo{};
    topo.generation = 3;
    topo.nodeCount = 3;
    topo.localNodeId = 0;
    topo.graphStatus = TopologyGraphStatus::Valid;
    
    // Node 0 (S400) -- S400 -- Node 1 (S400) -- S100 -- Node 2 (S400)
    
    TopologyNodeRecord node0{};
    node0.physicalId = 0;
    node0.linkActive = true;
    node0.maxSpeedMbps = 400;
    node0.links[0] = {true, 1, 0};
    
    TopologyNodeRecord node1{};
    node1.physicalId = 1;
    node1.linkActive = true;
    node1.maxSpeedMbps = 100; // Bottleneck!
    node1.links[0] = {true, 0, 0};
    node1.links[1] = {true, 2, 0};
    
    TopologyNodeRecord node2{};
    node2.physicalId = 2;
    node2.linkActive = true;
    node2.maxSpeedMbps = 400;
    node2.links[0] = {true, 1, 1};
    
    topo.physical.nodes.push_back(node0);
    topo.physical.nodes.push_back(node1);
    topo.physical.nodes.push_back(node2);

    EXPECT_TRUE(service_.PublishFromTopology(topo));
    auto snap = service_.Snapshot();
    
    // Path 0-2 goes through node 1 (S100), so speed is S100
    EXPECT_EQ(snap.speedMatrix[0][2], FireWireSpeedCode::S100);
}

TEST_F(SpeedMapServiceTests, EncodingOrder_LegacyPackedTwoBitEntries) {
    TopologySnapshot topo{};
    topo.generation = 1;
    topo.nodeCount = 2;
    topo.graphStatus = TopologyGraphStatus::Valid;
    
    TopologyNodeRecord node0{};
    node0.physicalId = 0; node0.linkActive = true; node0.maxSpeedMbps = 400;
    node0.links[0] = {true, 1, 0};
    
    TopologyNodeRecord node1{};
    node1.physicalId = 1; node1.linkActive = true; node1.maxSpeedMbps = 400;
    node1.links[0] = {true, 0, 0};
    
    topo.physical.nodes.push_back(node0);
    topo.physical.nodes.push_back(node1);

    service_.PublishFromTopology(topo);
    auto encoded = service_.EncodedQuadlets();
    
    // index = i*64 + j.
    // speed[0][1] is index 1.
    // In quadlet 1 (q1), entry 1 is at bits [3:2].
    // Value for S400 is 2 (10b).
    // so q1 should have bits [3:2] = 10b => value 8?
    // index / 16 + 1 selects the payload quadlet after q0.
    // index % 16 is 1, so q1 |= 2 << 2 => 8.
    
    EXPECT_EQ(encoded[1] & 0x0000000C, 2 << 2);
}

TEST_F(SpeedMapServiceTests, LegacyCSRWindowIsBoundedToOneKiB) {
    TopologySnapshot topo{};
    topo.generation = 9;
    topo.nodeCount = 1;
    topo.localNodeId = 0;
    topo.graphStatus = TopologyGraphStatus::Valid;

    TopologyNodeRecord node0{};
    node0.physicalId = 0;
    node0.linkActive = true;
    node0.maxSpeedMbps = 400;
    topo.physical.nodes.push_back(node0);

    EXPECT_TRUE(service_.PublishFromTopology(topo));
    EXPECT_EQ(service_.Snapshot().encodedLengthQuadlets, 256u);

    uint32_t value = 0;
    EXPECT_TRUE(service_.ReadQuadlet(1020, value));
    EXPECT_FALSE(service_.ReadQuadlet(1024, value));
}
