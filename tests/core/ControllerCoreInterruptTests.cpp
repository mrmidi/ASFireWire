// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// ControllerCoreInterruptTests.cpp — ControllerCore's interrupt path, driven
// with a real topology and the register-level HardwareInterface stub.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "../ASFWDriver/Bus/SelfIDCapture.hpp"
#include "../ASFWDriver/Bus/TopologyManager.hpp"
#include "../ASFWDriver/Controller/ControllerCore.hpp"
#include "../ASFWDriver/Controller/ControllerStateMachine.hpp"
#include "../ASFWDriver/Hardware/HardwareInterface.hpp"

namespace ASFW::Driver {

class ControllerCoreTestPeer {
public:
    static void TopologyReady(ControllerCore& core, const TopologySnapshot& snapshot) {
        core.OnTopologyReady(snapshot);
    }
};

namespace {

uint32_t BaseSelfId(uint8_t phyId, bool linkActive, bool contender, uint8_t port0) {
    uint32_t quad = 0x80000000;  // tag 2: Self-ID packet 0
    quad |= (uint32_t(phyId) & 0x3F) << 24;
    quad |= linkActive ? (1u << 22) : 0;
    quad |= 63u << 16;           // gap count
    quad |= 2u << 14;            // S400
    quad |= contender ? (1u << 11) : 0;
    quad |= 4u << 8;             // power class
    quad |= (uint32_t(port0) & 0x3) << 6;
    return quad;
}

constexpr uint8_t kPortParent = 2;
constexpr uint8_t kPortChild = 3;

class ControllerCoreInterruptTest : public ::testing::Test {
protected:
    void SetUp() override {
        ControllerCore::Dependencies deps{};
        deps.hardware = hardware_;
        deps.stateMachine = stateMachine_;
        deps.topology = topology_;
        core_ = std::make_shared<ControllerCore>(ControllerConfig{}, RolePolicy::MakeLiveDefault(),
                                                 std::move(deps));
        ASSERT_EQ(stateMachine_->TransitionTo(ControllerState::kStarting, "test", 0),
                  TransitionDisposition::kApplied);
        ASSERT_EQ(stateMachine_->TransitionTo(ControllerState::kRunning, "test", 0),
                  TransitionDisposition::kApplied);
    }

    // Two nodes: node 0 is a device, node 1 is the root. localNodeId picks
    // which one is this controller.
    void BringUpTopology(uint8_t localNodeId) {
        SelfIDCapture::Result selfId;
        selfId.valid = true;
        selfId.generation = kGeneration;
        selfId.quads = {kGeneration << 16,
                        BaseSelfId(0, true, false, kPortParent),
                        BaseSelfId(1, true, true, kPortChild)};
        selfId.sequences = {{1, 1}, {2, 1}};
        const auto snapshot = topology_->UpdateFromSelfID(selfId, 1000, 0x80000000u | localNodeId);
        ASSERT_TRUE(snapshot.has_value());
        ASSERT_EQ(snapshot->rootNodeId, 1);
        ControllerCoreTestPeer::TopologyReady(*core_, *snapshot);
    }

    bool CycleMasterSet() const {
        return (hardware_->GetTestRegister(Register32::kLinkControl) & LinkControlBits::kCycleMaster) != 0;
    }

    // What the controller does on an overlong cycle: raise cycleTooLong and
    // clear cycleMaster (OHCI 1.2 draft Table 6-1).
    void RaiseCycleTooLong() {
        hardware_->SetTestRegister(Register32::kIntEvent, IntEventBits::kCycleTooLong);
        hardware_->SetTestRegister(
            Register32::kLinkControl,
            hardware_->GetTestRegister(Register32::kLinkControl) & ~LinkControlBits::kCycleMaster);
        core_->HandleInterrupt(InterruptSnapshot{.intEvent = IntEventBits::kCycleTooLong});
    }

    static constexpr uint32_t kGeneration = 7;
    std::shared_ptr<HardwareInterface> hardware_ = std::make_shared<HardwareInterface>();
    std::shared_ptr<ControllerStateMachine> stateMachine_ = std::make_shared<ControllerStateMachine>();
    std::shared_ptr<TopologyManager> topology_ = std::make_shared<TopologyManager>();
    std::shared_ptr<ControllerCore> core_;
};

} // namespace

// Root: the cycle policy made this node cycle master. After cycleTooLong the
// controller has cleared cycleMaster; it must be set again, and only after
// the event is acknowledged (cycleMaster stays zero while it is set, Table
// 5-17 — the stub enforces that). Apple re-applies its setCycleMaster
// decision here; Linux sets it unconditionally (ohci.c:2286-2290).
TEST_F(ControllerCoreInterruptTest, CycleTooLongReenablesTheCycleMasterThePolicyChose) {
    BringUpTopology(/*localNodeId=*/1);
    ASSERT_TRUE(CycleMasterSet()) << "the policy should make the local root cycle master";

    RaiseCycleTooLong();

    EXPECT_EQ(hardware_->GetTestRegister(Register32::kIntEvent) & IntEventBits::kCycleTooLong, 0U);
    EXPECT_TRUE(CycleMasterSet());
    EXPECT_EQ(core_->GetCyclePolicyCoordinator()->Snapshot().cycleMasterRestoreCount, 1U);
}

// Not root: the policy never chose this node, so nothing sets cycleMaster.
TEST_F(ControllerCoreInterruptTest, CycleTooLongLeavesCycleMasterOffWhenNotOurs) {
    BringUpTopology(/*localNodeId=*/0);
    ASSERT_FALSE(CycleMasterSet());

    RaiseCycleTooLong();

    EXPECT_FALSE(CycleMasterSet());
    EXPECT_EQ(core_->GetCyclePolicyCoordinator()->Snapshot().cycleMasterRestoreCount, 0U);
}

} // namespace ASFW::Driver
