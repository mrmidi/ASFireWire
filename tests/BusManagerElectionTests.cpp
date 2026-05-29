// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElectionTests.cpp — Unit tests for Bus Manager election (FW-18).

#include "Bus/BusManager/BusManagerElection.hpp"
#include "Bus/BusManager/BusManagerElectionDriver.hpp"
#include "Controller/ControllerTypes.hpp"
#include "Common/CSRSpace.hpp"
#include "Scheduling/Scheduler.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <optional>

namespace {

using ASFW::Bus::BmElectionInputs;
using ASFW::Bus::BmOwner;
using ASFW::Bus::BusManagerElection;
using ASFW::Bus::BusManagerElectionDriver;
using ASFW::Bus::DecisionAction;
using ASFW::Bus::ElectionOutcome;
using ASFW::Driver::TopologySnapshot;
using ASFW::FW::RoleMode;

class MockAsyncController : public ASFW::Async::IAsyncControllerPort {
public:
    ASFW::Async::AsyncHandle Read(const ASFW::Async::ReadParams&, ASFW::Async::CompletionCallback) override { return {}; }
    ASFW::Async::AsyncHandle ReadWithRetry(const ASFW::Async::ReadParams&, const ASFW::Async::RetryPolicy&, ASFW::Async::CompletionCallback) override { return {}; }
    ASFW::Async::AsyncHandle Write(const ASFW::Async::WriteParams&, ASFW::Async::CompletionCallback) override { return {}; }
    ASFW::Async::AsyncHandle Lock(const ASFW::Async::LockParams&, uint16_t, ASFW::Async::CompletionCallback) override { return {}; }
    
    ASFW::Async::AsyncHandle CompareSwap(const ASFW::Async::CompareSwapParams& params,
                                         ASFW::Async::CompareSwapCallback callback) override {
        lastCompareSwapParams = params;
        lastCompareSwapCallback = callback;
        compareSwapCount++;
        return ASFW::Async::AsyncHandle{1};
    }
    
    ASFW::Async::AsyncHandle PhyRequest(const ASFW::Async::PhyParams&, ASFW::Async::CompletionCallback) override { return {}; }
    bool Cancel(ASFW::Async::AsyncHandle) override { return true; }
    void OnTimeoutTick() override {}
    
    [[nodiscard]] ASFW::Async::AsyncWatchdogStats GetWatchdogStats() const override { return {}; }
    [[nodiscard]] ASFW::Debug::BusResetPacketCapture* GetBusResetCapture() const override { return nullptr; }
    [[nodiscard]] ASFW::Debug::AsyncTraceCapture* GetAsyncTraceCapture() const override { return nullptr; }
    [[nodiscard]] ASFW::Shared::DMAMemoryManager* GetDMAManager() override { return nullptr; }
    [[nodiscard]] ASFWDiagInboundCSRStats* GetInboundCSRStats() const override { return nullptr; }
    [[nodiscard]] std::optional<ASFW::Async::AsyncStatusSnapshot> GetStatusSnapshot() const override { return std::nullopt; }
    
    kern_return_t ArmARContextsOnly() override { return 0; }
    void PostToWorkloop(void (^block)()) override { if (block) block(); }
    void OnTxInterrupt() override {}
    void OnRxRequestInterrupt() override {}
    void OnRxResponseInterrupt() override {}
    void OnBusResetBegin(uint8_t) override {}
    void OnBusResetComplete(uint8_t) override {}
    void ConfirmBusGeneration(uint8_t) override {}
    void StopATContextsOnly() override {}
    void FlushATContexts() override {}
    void RearmATContexts() override {}
    
    [[nodiscard]] ASFW::Async::AsyncBusStateSnapshot GetBusStateSnapshot() const override {
        return ASFW::Async::AsyncBusStateSnapshot{
            .generation16 = currentGen16,
            .generation8 = 0,
            .localNodeID = 0
        };
    }

    ASFW::Async::CompareSwapParams lastCompareSwapParams{};
    ASFW::Async::CompareSwapCallback lastCompareSwapCallback{};
    int compareSwapCount{0};
    uint16_t currentGen16{0};
};

TEST(BusManagerElection, FsmDecisions) {
    BusManagerElection fsm;

    // AvoidManager or other non-FullBusManager roles should not contend
    {
        BmElectionInputs inputs{
            .mode = RoleMode::AppleAvoidManager,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = false,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::DoNotContend);
    }

    // No IRM NodeId should not contend
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .generation = 1,
            .localId = 0,
            .irmId = std::nullopt,
            .wasIncumbent = false,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::DoNotContend);
    }

    // Incumbent: should contend immediately
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = true,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::ContendImmediately);
    }

    // Incumbent with abdicate observed: should contend after grace period (treated as challenger)
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = true,
            .abdicateObserved = true
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::ContendAfterGrace);
    }

    // Challenger (not incumbent): should contend after grace period
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = false,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::ContendAfterGrace);
    }
}

TEST(BusManagerElection, FsmOldValueInterpretation) {
    BusManagerElection fsm;

    // Reg empty (0x3F) -> WonBM
    EXPECT_EQ(fsm.InterpretOldValue(0x3F, 2), ElectionOutcome::WonBM);
    EXPECT_EQ(fsm.Owner(), BmOwner::Local);
    EXPECT_EQ(fsm.OwnerId(), 2);

    // Reg matching our ID -> IncumbentReestablished
    EXPECT_EQ(fsm.InterpretOldValue(2, 2), ElectionOutcome::IncumbentReestablished);
    EXPECT_EQ(fsm.Owner(), BmOwner::Local);
    EXPECT_EQ(fsm.OwnerId(), 2);

    // Reg matching other ID -> RemoteBM
    EXPECT_EQ(fsm.InterpretOldValue(5, 2), ElectionOutcome::RemoteBM);
    EXPECT_EQ(fsm.Owner(), BmOwner::Remote);
    EXPECT_EQ(fsm.OwnerId(), 5);
}

TEST(BusManagerElectionDriver, GatedByMode) {
    auto mockAsync = std::make_shared<MockAsyncController>();
    OSSharedPtr<IODispatchQueue> queue{new IODispatchQueue(), OSNoRetain};
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr
    };

    // AvoidManager Mode: OnTopologyReady should do nothing
    {
        auto driver = std::make_shared<BusManagerElectionDriver>(deps, RoleMode::AppleAvoidManager);
        TopologySnapshot snap{};
        snap.generation = 1;
        snap.localNodeId = 0;
        snap.irmNodeId = 1;
        snap.busBase16 = 0xFFC0;

        driver->OnTopologyReady(snap);
        EXPECT_EQ(mockAsync->compareSwapCount, 0);
        EXPECT_EQ(queue->DrainAllForTesting(), 0);
    }
}

TEST(BusManagerElectionDriver, IncumbentImmediateContention) {
    auto mockAsync = std::make_shared<MockAsyncController>();
    mockAsync->currentGen16 = 1;

    OSSharedPtr<IODispatchQueue> queue{new IODispatchQueue(), OSNoRetain};
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RoleMode::FullBusManager);

    // Setup wasIncumbent = true
    // Mimic winning previous election
    (void)driver->FSM().InterpretOldValue(0x3F, 0); 
    driver->OnBusReset();
    EXPECT_TRUE(driver->WasIncumbent());

    TopologySnapshot snap{};
    snap.generation = 1;
    snap.localNodeId = 0;
    snap.irmNodeId = 1;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap);

    // Contends immediately: compareSwapCount should be 1
    EXPECT_EQ(mockAsync->compareSwapCount, 1);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.compareValue, 0x3F);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.swapValue, 0);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.destinationID, 1); // ComposeNodeID(0, 1)

    // Complete compareswap
    mockAsync->lastCompareSwapCallback(ASFW::Async::AsyncStatus::kSuccess, 0x3F, true);
    EXPECT_EQ(driver->FSM().Owner(), BmOwner::Local);
}

TEST(BusManagerElectionDriver, ChallengerGracePeriod) {
    auto mockAsync = std::make_shared<MockAsyncController>();
    mockAsync->currentGen16 = 2;

    OSSharedPtr<IODispatchQueue> queue{new IODispatchQueue(), OSNoRetain};
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RoleMode::FullBusManager);
    EXPECT_FALSE(driver->WasIncumbent());

    TopologySnapshot snap{};
    snap.generation = 2;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap);

    // Delay scheduled, not executed yet
    EXPECT_EQ(mockAsync->compareSwapCount, 0);

    // Drain queue tasks (drains delayed contention)
    size_t run = queue->DrainAllForTesting();
    EXPECT_GE(run, 1u);
    EXPECT_EQ(mockAsync->compareSwapCount, 1);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.swapValue, 1);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.destinationID, 2);

    // Callback remote node wins
    mockAsync->lastCompareSwapCallback(ASFW::Async::AsyncStatus::kSuccess, 4, false);
    EXPECT_EQ(driver->FSM().Owner(), BmOwner::Remote);
    EXPECT_EQ(driver->FSM().OwnerId(), 4);
}

TEST(BusManagerElectionDriver, GenerationSafetyChecks) {
    auto mockAsync = std::make_shared<MockAsyncController>();
    mockAsync->currentGen16 = 3;

    OSSharedPtr<IODispatchQueue> queue{new IODispatchQueue(), OSNoRetain};
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RoleMode::FullBusManager);

    TopologySnapshot snap{};
    snap.generation = 3;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap);

    // A bus reset occurs before the grace period task executes!
    mockAsync->currentGen16 = 4; // Generation advances
    driver->OnBusReset();

    // Now execute delayed grace period task
    queue->DrainAllForTesting();

    // Contention should have been aborted due to stale generation
    EXPECT_EQ(mockAsync->compareSwapCount, 0);
}

} // namespace
