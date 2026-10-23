#include <gtest/gtest.h>

#include <vector>

#include "../ASFWDriver/ConfigROM/Remote/ROMScanNodeStateMachine.hpp"

using ASFW::Discovery::ROMScanNodeStateMachine;
using ASFW::Discovery::FwSpeed;
using ASFW::Discovery::Generation;

TEST(ROMScanNodeStateMachineTests, DefaultStateIsIdle) {
    ROMScanNodeStateMachine node;
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
    EXPECT_FALSE(node.IsTerminal());
}

TEST(ROMScanNodeStateMachineTests, AcceptsExpectedNominalTransitions) {
    ROMScanNodeStateMachine node(5, Generation{11}, FwSpeed::S100, 3);

    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingBIB));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingRootDir));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingDetails));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::Complete));
    EXPECT_TRUE(node.IsTerminal());
}

TEST(ROMScanNodeStateMachineTests, RejectsInvalidTransition) {
    ROMScanNodeStateMachine node(6, Generation{12}, FwSpeed::S100, 2);

    EXPECT_FALSE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingDetails));
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
}

TEST(ROMScanNodeStateMachineTests, SpeedProbeSitsBetweenBibAndDirectoryOrIrm) {
    for (const auto next : {ROMScanNodeStateMachine::State::ReadingRootDir,
                            ROMScanNodeStateMachine::State::VerifyingIRM_Read}) {
        ROMScanNodeStateMachine node;
        ASSERT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingBIB));
        ASSERT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ProbingSpeed));
        EXPECT_TRUE(node.TransitionTo(next));
    }
}

// An unchanged bus info block completes the node from the previous
// generation's ROM at whichever step precedes the root directory read.
TEST(ROMScanNodeStateMachineTests, UnchangedBusInfoBlockCompletesBeforeTheRootDirectory) {
    using enum ROMScanNodeStateMachine::State;
    const std::vector<std::vector<ROMScanNodeStateMachine::State>> paths = {
        {ReadingBIB},
        {ReadingBIB, ProbingSpeed},
        {ReadingBIB, VerifyingIRM_Read},
        {ReadingBIB, VerifyingIRM_Read, VerifyingIRM_Lock},
        {ReadingBIB, ProbingSpeed, VerifyingIRM_Read, VerifyingIRM_Lock},
    };
    for (const auto& path : paths) {
        ROMScanNodeStateMachine node;
        for (const auto state : path) {
            ASSERT_TRUE(node.TransitionTo(state));
        }
        EXPECT_TRUE(node.TransitionTo(Complete)) << "after " << path.size() << " steps";
    }
}

TEST(ROMScanNodeStateMachineTests, AcceptsConfigROMReadyRetryTransitions) {
    ROMScanNodeStateMachine node(6, Generation{12}, FwSpeed::S100, 2);

    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingBIB));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::WaitingConfigROMReady));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::ReadingRootDir));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::WaitingConfigROMReady));
    EXPECT_TRUE(node.TransitionTo(ROMScanNodeStateMachine::State::Idle));
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
}

TEST(ROMScanNodeStateMachineTests, ResetForGenerationReinitializesNodeData) {
    ROMScanNodeStateMachine node(6, Generation{12}, FwSpeed::S100, 2);
    node.MutableROM().vendorName = "X";
    node.SetBIBInProgress(true);
    node.SetConfigROMReadyRetriesLeft(3);
    node.ForceState(ROMScanNodeStateMachine::State::Failed);

    node.ResetForGeneration(Generation{20}, 7, FwSpeed::S200, 4);

    EXPECT_EQ(node.NodeId(), 7);
    EXPECT_EQ(node.CurrentState(), ROMScanNodeStateMachine::State::Idle);
    EXPECT_EQ(node.CurrentSpeed(), FwSpeed::S200);
    EXPECT_EQ(node.RetriesLeft(), 4);
    EXPECT_EQ(node.ROM().gen.value, 20u);
    EXPECT_EQ(node.ROM().nodeId, 7);
    EXPECT_TRUE(node.ROM().vendorName.empty());
    EXPECT_FALSE(node.BIBInProgress());
    EXPECT_EQ(node.ConfigROMReadyRetriesLeft(), 0);
}
