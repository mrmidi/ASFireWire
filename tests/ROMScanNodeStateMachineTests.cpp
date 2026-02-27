#include <gtest/gtest.h>

#include "../ASFWDriver/ConfigROM/ROMScanNodeStateMachine.hpp"

using ASFW::Discovery::ROMScanNodeStateMachine;
using ASFW::Discovery::FwSpeed;

TEST(ROMScanNodeStateMachineTests, DefaultStateIsIdle) {
    ROMScanNodeStateMachine node;
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
    EXPECT_FALSE(node.IsTerminal());
}

TEST(ROMScanNodeStateMachineTests, AcceptsExpectedNominalTransitions) {
    ROMScanNodeStateMachine node(5, 11, FwSpeed::S100, 3);

    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingBIB));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingRootDir));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingDetails));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::Complete));
    EXPECT_TRUE(node.IsTerminal());
}

TEST(ROMScanNodeStateMachineTests, RejectsInvalidTransition) {
    ROMScanNodeStateMachine node(6, 12, FwSpeed::S100, 2);

    EXPECT_FALSE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingDetails));
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
}

TEST(ROMScanNodeStateMachineTests, ResetForGenerationReinitializesNodeData) {
    ROMScanNodeStateMachine node(6, 12, FwSpeed::S100, 2);
    node.MutableROM().vendorName = "X";
    node.SetBIBInProgress(true);
    node.ForceState(ROMScanNodeStateMachine::State::Failed);

    node.ResetForGeneration(20, 7, FwSpeed::S200, 4);

    EXPECT_EQ(node.NodeId(), 7);
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
    EXPECT_EQ(node.CurrentSpeed(), FwSpeed::S200);
    EXPECT_EQ(node.RetriesLeft(), 4);
    EXPECT_EQ(node.ROM().gen, 20);
    EXPECT_EQ(node.ROM().nodeId, 7);
    EXPECT_TRUE(node.ROM().vendorName.empty());
    EXPECT_FALSE(node.BIBInProgress());
}
