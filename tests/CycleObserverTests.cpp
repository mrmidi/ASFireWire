// CycleObserverTests.cpp — cycle-start/lost evidence tracker (FW-7).

#include <cstdint>

#include <gtest/gtest.h>

#include "ASFWDriver/Bus/Role/CycleObserver.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"

using ASFW::Driver::IntEventBits;
using ASFW::Driver::Role::CycleObserver;

TEST(CycleObserverTests, FreshObserver_NoEvidence) {
    CycleObserver obs;
    EXPECT_FALSE(obs.Observation().cycleStartObserved);
    EXPECT_FALSE(obs.Observation().cycleLostObserved);
    EXPECT_FALSE(obs.CycleInconsistentSeen());
}

TEST(CycleObserverTests, CycleSynch_SetsStart_AndEdgeFiresOnce) {
    CycleObserver obs;
    EXPECT_TRUE(obs.OnInterrupt(1, IntEventBits::kCycleSynch)); // first → changed
    EXPECT_TRUE(obs.Observation().cycleStartObserved);
    // Subsequent cycleSynch in the same generation must NOT report a change
    // (this is what lets the caller avoid the per-cycle flood into RoleCoordinator).
    EXPECT_FALSE(obs.OnInterrupt(1, IntEventBits::kCycleSynch));
}

TEST(CycleObserverTests, CycleLost_SetsLost) {
    CycleObserver obs;
    EXPECT_TRUE(obs.OnInterrupt(2, IntEventBits::kCycleLost));
    EXPECT_TRUE(obs.Observation().cycleLostObserved);
    EXPECT_FALSE(obs.Observation().cycleStartObserved);
}

TEST(CycleObserverTests, GenerationChange_ResetsEvidence) {
    CycleObserver obs;
    obs.OnInterrupt(1, IntEventBits::kCycleSynch | IntEventBits::kCycleLost);
    EXPECT_TRUE(obs.Observation().cycleStartObserved);
    EXPECT_TRUE(obs.Observation().cycleLostObserved);

    // New generation wipes evidence.
    EXPECT_FALSE(obs.OnInterrupt(2, 0u)); // no bits set → no change, but gen reset
    EXPECT_EQ(obs.Generation(), 2u);
    EXPECT_FALSE(obs.Observation().cycleStartObserved);
    EXPECT_FALSE(obs.Observation().cycleLostObserved);
}

TEST(CycleObserverTests, CycleInconsistent_RecordedButNotAnEdge) {
    CycleObserver obs;
    EXPECT_FALSE(obs.OnInterrupt(1, IntEventBits::kCycleInconsistent)); // not a start/lost edge
    EXPECT_TRUE(obs.CycleInconsistentSeen());
    EXPECT_FALSE(obs.Observation().cycleStartObserved);
    EXPECT_FALSE(obs.Observation().cycleLostObserved);
}

TEST(CycleObserverTests, UnrelatedBits_NoChange) {
    CycleObserver obs;
    EXPECT_FALSE(obs.OnInterrupt(1, IntEventBits::kBusReset));
    EXPECT_FALSE(obs.Observation().cycleStartObserved);
    EXPECT_FALSE(obs.Observation().cycleLostObserved);
}
