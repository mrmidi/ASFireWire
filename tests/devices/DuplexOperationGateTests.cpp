// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-68 characterization tests for DuplexOperationGate, the per-GUID duplex operation gate
// extracted from DiceDuplexRestartCoordinator. These pin the observable behaviour of the former
// coordinator members (TryAcquireGuid/ReleaseGuid/RequestStopIntent/ClearStopIntent/
// IsStopRequested) and the lock-held variants used inside the coordinator's multi-domain
// critical sections, so the extraction is provably behaviour-preserving. Host tests, no hardware.

#include <gtest/gtest.h>

#include <DriverKit/IOLib.h>

#include "Audio/Protocols/Backends/DuplexOperationGate.hpp"

namespace {

using ASFW::Audio::Backends::DuplexOperationGate;

class DuplexOperationGateTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_NE(lock_, nullptr); }
    void TearDown() override {
        if (lock_) {
            IOLockFree(lock_);
        }
    }

    IOLock* lock_ = IOLockAlloc();
    DuplexOperationGate gate_{&lock_};
};

// Acquire is a test-and-set on the active set: first claim wins, a second claim of the same GUID
// fails until Release. (Matches the former TryAcquireGuid returning `inserted`.)
TEST_F(DuplexOperationGateTest, AcquireIsTestAndSet) {
    EXPECT_TRUE(gate_.Acquire(0x11));
    EXPECT_FALSE(gate_.Acquire(0x11));
    gate_.Release(0x11);
    EXPECT_TRUE(gate_.Acquire(0x11));
}

TEST_F(DuplexOperationGateTest, DistinctGuidsAcquireIndependently) {
    EXPECT_TRUE(gate_.Acquire(0x11));
    EXPECT_TRUE(gate_.Acquire(0x22));
    EXPECT_FALSE(gate_.Acquire(0x11));
    EXPECT_FALSE(gate_.Acquire(0x22));
}

TEST_F(DuplexOperationGateTest, StopIntentSetAndClear) {
    EXPECT_FALSE(gate_.IsStopRequested(0x11));
    gate_.RequestStop(0x11);
    EXPECT_TRUE(gate_.IsStopRequested(0x11));
    gate_.ClearStop(0x11);
    EXPECT_FALSE(gate_.IsStopRequested(0x11));

    // Clearing a GUID with no stop-intent is a safe no-op (erase of an absent key) and does
    // not disturb another GUID's intent.
    gate_.RequestStop(0x22);
    gate_.ClearStop(0x11);  // 0x11 not present
    EXPECT_TRUE(gate_.IsStopRequested(0x22));
}

// The two sets are orthogonal: claiming a GUID does not set stop-intent, stop-intent does not
// release the claim, and intent on one GUID does not affect another.
TEST_F(DuplexOperationGateTest, ActiveAndStopIntentAreIndependent) {
    gate_.Acquire(0x11);
    EXPECT_FALSE(gate_.IsStopRequested(0x11));  // acquiring does not set stop
    gate_.RequestStop(0x11);
    EXPECT_TRUE(gate_.IsStopRequested(0x11));   // stop is set
    EXPECT_TRUE(gate_.IsActiveLocked(0x11));    // and the claim still stands

    gate_.Acquire(0x22);
    gate_.RequestStop(0x22);
    EXPECT_TRUE(gate_.IsActiveLocked(0x11));    // 0x22's activity/intent leaves 0x11 alone
    EXPECT_TRUE(gate_.IsStopRequested(0x11));
}

// guid==0: RequestStop/ClearStop/IsStopRequested short-circuit (former members guarded guid==0);
// Acquire/Release have NO zero guard (former TryAcquireGuid/ReleaseGuid did not either).
TEST_F(DuplexOperationGateTest, ZeroGuidGuardsMatchOriginals) {
    gate_.RequestStop(0);
    EXPECT_FALSE(gate_.IsStopRequested(0));
    EXPECT_FALSE(gate_.IsStopRequestedLocked(0));  // guarded: nothing was inserted
    gate_.ClearStop(0);                            // guarded no-op
    EXPECT_FALSE(gate_.IsStopRequestedLocked(0));

    EXPECT_TRUE(gate_.Acquire(0));                  // no zero guard on Acquire -> inserts 0
    EXPECT_TRUE(gate_.IsActiveLocked(0));
    gate_.Release(0);                               // no zero guard on Release -> erases 0
    EXPECT_FALSE(gate_.IsActiveLocked(0));
}

// The gate borrows &lock_ and reads it fresh each call: a null lock short-circuits every
// self-locking method exactly as the former members did, and the gate starts working the moment
// the borrowed lock is allocated.
TEST(DuplexOperationGateNullLock, NoLockShortCircuitsSelfLockingApi) {
    IOLock* lock = nullptr;
    DuplexOperationGate gate{&lock};
    EXPECT_FALSE(gate.Acquire(0x11));
    gate.Release(0x11);       // no-op, must not deref a null lock
    gate.RequestStop(0x11);   // no-op
    EXPECT_FALSE(gate.IsStopRequested(0x11));

    lock = IOLockAlloc();
    ASSERT_NE(lock, nullptr);
    EXPECT_TRUE(gate.Acquire(0x11));
    IOLockFree(lock);
}

// Lock-held variants do only the set op (no internal locking) — used inside RequestClockConfig /
// ClearSession's single critical section.
TEST_F(DuplexOperationGateTest, LockedVariantsMutateWithoutLocking) {
    EXPECT_FALSE(gate_.IsActiveLocked(0x11));
    gate_.AcquireLocked(0x11);
    EXPECT_TRUE(gate_.IsActiveLocked(0x11));
    gate_.ReleaseLocked(0x11);
    EXPECT_FALSE(gate_.IsActiveLocked(0x11));

    gate_.RequestStop(0x22);
    EXPECT_TRUE(gate_.IsStopRequestedLocked(0x22));
    gate_.ClearStopLocked(0x22);
    EXPECT_FALSE(gate_.IsStopRequestedLocked(0x22));
}

// The self-locking public API and the lock-held variants operate on the same underlying sets.
TEST_F(DuplexOperationGateTest, SelfLockingAndLockedVariantsShareState) {
    gate_.Acquire(0x33);                       // self-locking claim
    EXPECT_TRUE(gate_.IsActiveLocked(0x33));   // visible to the locked read
    gate_.ReleaseLocked(0x33);                 // locked release
    EXPECT_TRUE(gate_.Acquire(0x33));          // claimable again via self-locking path
}

// The read-only methods are const and callable on a const gate (matches the former const
// IsStopRequested member; the const IsStopRequested still locks the borrowed lock_).
TEST_F(DuplexOperationGateTest, ConstReadMethodsCallableOnConstInstance) {
    gate_.Acquire(0x11);
    gate_.RequestStop(0x22);
    const DuplexOperationGate& c = gate_;
    EXPECT_TRUE(c.IsStopRequested(0x22));
    EXPECT_TRUE(c.IsActiveLocked(0x11));
    EXPECT_TRUE(c.IsStopRequestedLocked(0x22));
    EXPECT_FALSE(c.IsStopRequestedLocked(0x11));
}

} // namespace
