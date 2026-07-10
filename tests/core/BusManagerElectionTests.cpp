// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElectionTests.cpp — Unit tests for Bus Manager election (FW-18).

#include "Bus/BusManager/BusManagerElection.hpp"
#include "Bus/BusManager/BusManagerElectionDriver.hpp"
#include "Bus/Timing/PostResetTimingCoordinator.hpp"
#include "Controller/ControllerTypes.hpp"
#include "Common/CSRSpace.hpp"
#include "Scheduling/Scheduler.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Bus::BusManagerElection;
using ASFW::Bus::BusManagerElectionDriver;
using ASFW::Bus::DecisionAction;
using ASFW::Bus::BmElectionInputs;
using ASFW::Bus::ElectionOutcome;
using ASFW::Driver::TopologySnapshot;
using ASFW::Driver::RolePolicy;
using ASFW::FW::RoleMode;
using ASFW::FW::FullBMActivityLevel;

class ScopedMockClock {
public:
    explicit ScopedMockClock(std::function<uint64_t()> fn) {
        ASFW::Testing::SetHostMonotonicClockForTesting(std::move(fn));
    }

    ~ScopedMockClock() {
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    ScopedMockClock(const ScopedMockClock&) = delete;
    ScopedMockClock& operator=(const ScopedMockClock&) = delete;
};

class MockAsyncPort : public ASFW::Async::IAsyncControllerPort {
public:
    ASFW::Async::AsyncHandle Read(const ASFW::Async::ReadParams&, ASFW::Async::CompletionCallback) override { return {}; }
    ASFW::Async::AsyncHandle ReadWithRetry(const ASFW::Async::ReadParams&, const ASFW::Async::RetryPolicy&, ASFW::Async::CompletionCallback) override { return {}; }
    ASFW::Async::AsyncHandle Write(const ASFW::Async::WriteParams&, ASFW::Async::CompletionCallback) override { return {}; }
    
    ASFW::Async::AsyncHandle Lock(const ASFW::Async::LockParams&, uint16_t, ASFW::Async::CompletionCallback) override { return {}; }
    
    ASFW::Async::AsyncHandle CompareSwap(const ASFW::Async::CompareSwapParams& params, ASFW::Async::CompareSwapCallback callback) override {
        lastCompareSwapParams = params;
        lastCompareSwapCallback = callback;
        compareSwapCount++;
        return {1};
    }

    ASFW::Async::AsyncHandle PhyRequest(const ASFW::Async::PhyParams&, ASFW::Async::CompletionCallback) override { return {}; }

    bool Cancel(ASFW::Async::AsyncHandle) override { return true; }
    
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
    void OnTimeoutTick() override {}
    [[nodiscard]] ASFW::Async::AsyncWatchdogStats GetWatchdogStats() const override { return {}; }
    
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
            .mode = RoleMode::ClientOnly,
            .activityLevel = FullBMActivityLevel::ObserveOnly,
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
            .activityLevel = FullBMActivityLevel::ElectionOnly,
            .generation = 1,
            .localId = 0,
            .irmId = std::nullopt,
            .wasIncumbent = false,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::DoNotContend);
    }

    // Challenger should contend after grace
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .activityLevel = FullBMActivityLevel::ElectionOnly,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = false,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::ContendAfterGrace);
    }

    // Incumbent should contend immediately
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .activityLevel = FullBMActivityLevel::ElectionOnly,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = true,
            .abdicateObserved = false
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::ContendImmediately);
    }

    // Abdicate should force grace period even for incumbent
    {
        BmElectionInputs inputs{
            .mode = RoleMode::FullBusManager,
            .activityLevel = FullBMActivityLevel::ElectionOnly,
            .generation = 1,
            .localId = 0,
            .irmId = 1,
            .wasIncumbent = true,
            .abdicateObserved = true
        };
        EXPECT_EQ(fsm.Decide(inputs), DecisionAction::ContendAfterGrace);
    }
}

TEST(BusManagerElection, FsmOldValueInterpretation) {
    BusManagerElection fsm;
    
    // Case 1: Value is 0x3F (No BM)
    EXPECT_EQ(fsm.InterpretOldValue(0x3F, 0), ElectionOutcome::WonBM);
    
    // Case 2: Value is our own ID
    EXPECT_EQ(fsm.InterpretOldValue(0, 0), ElectionOutcome::IncumbentReestablished);
    
    // Case 3: Value is another ID
    EXPECT_EQ(fsm.InterpretOldValue(1, 0), ElectionOutcome::RemoteBM);
}

TEST(BusManagerElectionDriver, GatedByMode) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    // AvoidManager Mode: OnTopologyReady should do nothing
    {
        auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::ClientOnly});
        const uint32_t generation = 1;
        const uint64_t now = ASFW::Testing::HostMonotonicNow();
        timing.OnSelfIDComplete(generation, now);

        TopologySnapshot snap{};
        snap.generation = generation;
        snap.localNodeId = 0;
        snap.irmNodeId = 1;
        snap.busBase16 = 0xFFC0;

        driver->OnTopologyReady(snap, now);
        EXPECT_EQ(mockAsync->compareSwapCount, 0);
    }
}

TEST(BusManagerElectionDriver, IncumbentImmediateContention) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    // Setup wasIncumbent = true
    // Mimic winning previous election
    (void)driver->FSM().InterpretOldValue(0x3F, 0); 
    driver->OnBusReset();
    EXPECT_TRUE(driver->WasIncumbent());

    const uint32_t generation = 1;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 0;
    snap.irmNodeId = 1;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap, now);

    // Contends immediately: compareSwapCount should be 1
    EXPECT_EQ(mockAsync->compareSwapCount, 1);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.compareValue, 0x3F);
    EXPECT_EQ(mockAsync->lastCompareSwapParams.swapValue, 0);
}

TEST(BusManagerElectionDriver, FastResetAfterLocalBMWonYieldsStableRemoteRootIRMTopology) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);

    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(
        deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    constexpr uint64_t t0 = 1000000000ULL;
    const uint32_t generation = 21;
    mockAsync->currentGen16 = generation;
    timing.OnSelfIDComplete(generation, t0);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 0;
    snap.rootNodeId = 2;
    snap.irmNodeId = 2;
    snap.nodeCount = 3;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap, t0);
    EXPECT_EQ(mockAsync->compareSwapCount, 0);

    {
        ScopedMockClock clock([t0]() { return t0 + 126000000ULL; });
        queue->DrainAllForTesting();
        ASSERT_EQ(mockAsync->compareSwapCount, 1);
        ASSERT_TRUE(static_cast<bool>(mockAsync->lastCompareSwapCallback));
        mockAsync->lastCompareSwapCallback(ASFW::Async::AsyncStatus::kSuccess, 0x3F, true);
    }

    {
        ScopedMockClock clock([t0]() { return t0 + 384000000ULL; });
        driver->OnBusReset();
    }
    EXPECT_TRUE(driver->WasIncumbent());

    const uint32_t nextGeneration = 22;
    mockAsync->currentGen16 = nextGeneration;
    timing.OnSelfIDComplete(nextGeneration, t0 + 385000000ULL);
    snap.generation = nextGeneration;

    driver->OnTopologyReady(snap, t0 + 385000000ULL);
    EXPECT_EQ(mockAsync->compareSwapCount, 1);
    EXPECT_EQ(driver->GetSnapshot().lastAction, 3);
}

TEST(BusManagerElectionDriver, FastResetYieldClearsWhenTopologyChanges) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);

    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(
        deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    constexpr uint64_t t0 = 2000000000ULL;
    const uint32_t generation = 31;
    mockAsync->currentGen16 = generation;
    timing.OnSelfIDComplete(generation, t0);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 0;
    snap.rootNodeId = 2;
    snap.irmNodeId = 2;
    snap.nodeCount = 3;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap, t0);
    {
        ScopedMockClock clock([t0]() { return t0 + 126000000ULL; });
        queue->DrainAllForTesting();
        ASSERT_EQ(mockAsync->compareSwapCount, 1);
        ASSERT_TRUE(static_cast<bool>(mockAsync->lastCompareSwapCallback));
        mockAsync->lastCompareSwapCallback(ASFW::Async::AsyncStatus::kSuccess, 0x3F, true);
    }

    {
        ScopedMockClock clock([t0]() { return t0 + 300000000ULL; });
        driver->OnBusReset();
    }

    const uint32_t yieldedGeneration = 32;
    mockAsync->currentGen16 = yieldedGeneration;
    timing.OnSelfIDComplete(yieldedGeneration, t0 + 301000000ULL);
    snap.generation = yieldedGeneration;

    driver->OnTopologyReady(snap, t0 + 301000000ULL);
    EXPECT_EQ(driver->GetSnapshot().lastAction, 3);
    EXPECT_EQ(mockAsync->compareSwapCount, 1);

    driver->OnBusReset();

    const uint32_t changedGeneration = 33;
    mockAsync->currentGen16 = changedGeneration;
    timing.OnSelfIDComplete(changedGeneration, t0 + 302000000ULL);
    snap.generation = changedGeneration;
    snap.nodeCount = 4;

    driver->OnTopologyReady(snap, t0 + 302000000ULL);
    EXPECT_EQ(driver->GetSnapshot().lastAction, 2);
    EXPECT_EQ(mockAsync->compareSwapCount, 1);

    {
        ScopedMockClock clock([t0]() { return t0 + 428000000ULL; });
        queue->DrainAllForTesting();
    }

    EXPECT_EQ(mockAsync->compareSwapCount, 2);
}

TEST(BusManagerElectionDriver, DelayedResetAfterLocalBMWonDoesNotArmStormYield) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);

    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(
        deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    constexpr uint64_t t0 = 3000000000ULL;
    const uint32_t generation = 41;
    mockAsync->currentGen16 = generation;
    timing.OnSelfIDComplete(generation, t0);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 0;
    snap.rootNodeId = 0;
    snap.irmNodeId = 2;
    snap.nodeCount = 3;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap, t0);
    {
        ScopedMockClock clock([t0]() { return t0 + 126000000ULL; });
        queue->DrainAllForTesting();
        ASSERT_EQ(mockAsync->compareSwapCount, 1);
        ASSERT_TRUE(static_cast<bool>(mockAsync->lastCompareSwapCallback));
        mockAsync->lastCompareSwapCallback(ASFW::Async::AsyncStatus::kSuccess, 0x3F, true);
    }

    {
        ScopedMockClock clock([t0]() { return t0 + 819000000ULL; });
        driver->OnBusReset();
    }
    EXPECT_TRUE(driver->WasIncumbent());

    const uint32_t nextGeneration = 42;
    mockAsync->currentGen16 = nextGeneration;
    timing.OnSelfIDComplete(nextGeneration, t0 + 820000000ULL);
    snap.generation = nextGeneration;
    snap.localNodeId = 0;
    snap.rootNodeId = 2;
    snap.irmNodeId = 2;

    driver->OnTopologyReady(snap, t0 + 820000000ULL);
    EXPECT_EQ(driver->GetSnapshot().lastAction, 1);
    EXPECT_EQ(mockAsync->compareSwapCount, 2);
}

TEST(BusManagerElectionDriver, ChallengerGracePeriod) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});
    EXPECT_FALSE(driver->WasIncumbent());

    const uint32_t generation = 2;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap, now);

    // Delay scheduled, not executed yet
    EXPECT_EQ(mockAsync->compareSwapCount, 0);

    // ADVANCE CLOCK to open the gate (+125ms)
    {
        ScopedMockClock clock([now]() { return now + 126000000ULL; });

        // Drain queue tasks (drains delayed contention)
        queue->DrainAllForTesting();
    }
}

TEST(BusManagerElectionDriver, GenerationSafetyChecks) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    const uint32_t generation = 3;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    driver->OnTopologyReady(snap, now);

    // A bus reset occurs before the grace period task executes!
    mockAsync->currentGen16 = 4; // Generation advances
    driver->OnBusReset();

    // Now execute delayed grace period task
    queue->DrainAllForTesting();

    // Contention should have been aborted due to stale generation
    EXPECT_EQ(mockAsync->compareSwapCount, 0);
}

TEST(BusManagerElectionDriver, MaxOneAttemptPerGeneration) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    const uint32_t generation = 5;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    // First call: attempt starts
    driver->OnTopologyReady(snap, now);
    
    // In our manual scheduler stub, if delay is 0 it might run immediately
    // or if the gate is OPEN it contends immediately.
    // timing.OnSelfIDComplete(generation, now) makes the gate OPEN immediately for incumbents
    // but CLOSED (+125ms) for challengers.
    
    // Challenger case: should be CLOSED
    EXPECT_EQ(mockAsync->compareSwapCount, 0); 
    
    // Call again before task executes: should do nothing (inFlight guard)
    driver->OnTopologyReady(snap, now);
    EXPECT_EQ(mockAsync->compareSwapCount, 0);
    
    // ADVANCE CLOCK
    {
        ScopedMockClock clock([now]() { return now + 126000000ULL; });
        queue->DrainAllForTesting();
    }
    EXPECT_EQ(mockAsync->compareSwapCount, 1);
    
    // Attempt finished. Now call OnTopologyReady again for same generation.
    // Throttling guard should prevent 2nd attempt.
    driver->OnTopologyReady(snap, now + 126000000ULL);
    // Even if we drain, count should stay 1
    queue->DrainAllForTesting();
    EXPECT_EQ(mockAsync->compareSwapCount, 1); 
}

TEST(BusManagerElection, RejectsInvalidOldValues) {
    BusManagerElection fsm;
    
    // Case 1: All bits set (0xFFFFFFFF)
    EXPECT_EQ(fsm.InterpretOldValue(0xFFFFFFFF, 0), ElectionOutcome::ElectionFailed);
    EXPECT_EQ(fsm.Owner(), ASFW::Bus::BmOwner::Unknown);
    
    // Case 2: Bit 6 set (0x40)
    EXPECT_EQ(fsm.InterpretOldValue(0x40, 0), ElectionOutcome::ElectionFailed);
    
    // Case 3: Sane 6-bit value still works
    EXPECT_EQ(fsm.InterpretOldValue(0x10, 0), ElectionOutcome::RemoteBM);
    EXPECT_EQ(fsm.Owner(), ASFW::Bus::BmOwner::Remote);
    EXPECT_EQ(fsm.OwnerId(), 0x10);
}

TEST(BusManagerElectionDriver, DeferredElectionSuppressedByPolicyChange) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    const uint32_t generation = 10;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    // Schedule delayed election
    driver->OnTopologyReady(snap, now);
    EXPECT_EQ(mockAsync->compareSwapCount, 0);

    // CHANGE POLICY BEFORE TIMER FIRES
    driver->SetRolePolicy(RolePolicy{RoleMode::ClientOnly});

    // Execute task
    queue->DrainAllForTesting();

    // Should be suppressed
    EXPECT_EQ(mockAsync->compareSwapCount, 0);
}

TEST(BusManagerElectionDriver, DeferredElectionSuppressedByActivityChangedToObserveOnly) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    const uint32_t generation = 15;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    // Schedule delayed election
    driver->OnTopologyReady(snap, now);
    EXPECT_EQ(mockAsync->compareSwapCount, 0);

    // CHANGE ACTIVITY BEFORE TIMER FIRES
    driver->SetRolePolicy(RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ObserveOnly});

    // Execute task
    { 
        ScopedMockClock clock([now]() { return now + 126000000ULL; });
        queue->DrainAllForTesting();
    }

    // Should be suppressed
    EXPECT_EQ(mockAsync->compareSwapCount, 0);
}

TEST(BusManagerElectionDriver, DeferredElectionSuppressedAfterDriverStop) {
    OSSharedPtr<IODispatchQueue> queue(new IODispatchQueue(), OSNoRetain);
    queue->SetManualDispatchForTesting(true);
    
    auto scheduler = std::make_shared<ASFW::Driver::Scheduler>();
    scheduler->Bind(queue);

    ASFW::Bus::Timing::PostResetTimingCoordinator timing;
    auto mockAsync = std::make_shared<MockAsyncPort>();

    BusManagerElectionDriver::Deps deps{
        .asyncController = mockAsync.get(),
        .scheduler = scheduler.get(),
        .csrResponder = nullptr,
        .timing = &timing,
        .monotonicNowNs = ASFW::Testing::HostMonotonicNow
    };

    auto driver = std::make_shared<BusManagerElectionDriver>(deps, RolePolicy{RoleMode::FullBusManager, FullBMActivityLevel::ElectionOnly});

    const uint32_t generation = 20;
    mockAsync->currentGen16 = generation;
    const uint64_t now = ASFW::Testing::HostMonotonicNow();
    timing.OnSelfIDComplete(generation, now);

    TopologySnapshot snap{};
    snap.generation = generation;
    snap.localNodeId = 1;
    snap.irmNodeId = 2;
    snap.busBase16 = 0x0;

    // Schedule delayed election
    driver->OnTopologyReady(snap, now);
    EXPECT_EQ(mockAsync->compareSwapCount, 0);

    // STOP DRIVER
    driver->Stop();

    // Execute task
    { 
        ScopedMockClock clock([now]() { return now + 126000000ULL; });
        queue->DrainAllForTesting();
    }

    // Should be suppressed
    EXPECT_EQ(mockAsync->compareSwapCount, 0);
}

} // namespace
