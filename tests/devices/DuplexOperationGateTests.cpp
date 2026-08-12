// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Characterization tests for the per-endpoint duplex operation gate.
// extracted from AudioDuplexCoordinator. These pin the observable behaviour of the former
// coordinator members (TryAcquireEndpoint/ReleaseEndpoint/RequestStopIntent/ClearStopIntent/
// IsStopRequested) and the lock-held variants used inside the coordinator's multi-domain
// critical sections, so the extraction is provably behaviour-preserving. Host tests, no hardware.

#include <gtest/gtest.h>

#include <DriverKit/IOLib.h>

#include "Audio/Duplex/DuplexOperationGate.hpp"

namespace {

using ASFW::Audio::Duplex::DuplexOperationGate;
using ASFW::Audio::Devices::AudioEndpointId;

constexpr AudioEndpointId kEndpointA{0x11};
constexpr AudioEndpointId kEndpointB{0x22};
constexpr AudioEndpointId kEndpointC{0x33};
constexpr AudioEndpointId kNoEndpoint{};

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

// Acquire is a test-and-set on the active set: first claim wins, a second claim of the same
// endpoint fails until Release.
TEST_F(DuplexOperationGateTest, AcquireIsTestAndSet) {
    EXPECT_TRUE(gate_.Acquire(kEndpointA));
    EXPECT_FALSE(gate_.Acquire(kEndpointA));
    gate_.Release(kEndpointA);
    EXPECT_TRUE(gate_.Acquire(kEndpointA));
}

TEST_F(DuplexOperationGateTest, DistinctEndpointsAcquireIndependently) {
    EXPECT_TRUE(gate_.Acquire(kEndpointA));
    EXPECT_TRUE(gate_.Acquire(kEndpointB));
    EXPECT_FALSE(gate_.Acquire(kEndpointA));
    EXPECT_FALSE(gate_.Acquire(kEndpointB));
}

TEST_F(DuplexOperationGateTest, StopIntentSetAndClear) {
    EXPECT_FALSE(gate_.IsStopRequested(kEndpointA));
    gate_.RequestStop(kEndpointA);
    EXPECT_TRUE(gate_.IsStopRequested(kEndpointA));
    gate_.ClearStop(kEndpointA);
    EXPECT_FALSE(gate_.IsStopRequested(kEndpointA));

    // Clearing an endpoint with no stop-intent is a safe no-op and does not
    // disturb another endpoint's intent.
    gate_.RequestStop(kEndpointB);
    gate_.ClearStop(kEndpointA);  // endpoint A not present
    EXPECT_TRUE(gate_.IsStopRequested(kEndpointB));
}

TEST_F(DuplexOperationGateTest, RemoteLossPersistsPastStopCallbackUntilRediscovery) {
    gate_.MarkRemoteDeviceLost(kEndpointA);
    EXPECT_TRUE(gate_.IsStopRequested(kEndpointA));

    // CoreAudio can deliver StopIO after discovery has retired the endpoint. Its
    // ordinary stop cleanup must not reopen the old session.
    gate_.ClearStop(kEndpointA);
    EXPECT_TRUE(gate_.IsStopRequested(kEndpointA));

    gate_.AcknowledgeDevicePresent(kEndpointA);
    EXPECT_FALSE(gate_.IsStopRequested(kEndpointA));
}

// The two sets are orthogonal: claiming an endpoint does not set stop-intent,
// stop-intent does not release the claim, and intent is endpoint-local.
TEST_F(DuplexOperationGateTest, ActiveAndStopIntentAreIndependent) {
    gate_.Acquire(kEndpointA);
    EXPECT_FALSE(gate_.IsStopRequested(kEndpointA));  // acquiring does not set stop
    gate_.RequestStop(kEndpointA);
    EXPECT_TRUE(gate_.IsStopRequested(kEndpointA));   // stop is set
    EXPECT_TRUE(gate_.IsActiveLocked(kEndpointA));    // and the claim still stands

    gate_.Acquire(kEndpointB);
    gate_.RequestStop(kEndpointB);
    EXPECT_TRUE(gate_.IsActiveLocked(kEndpointA));
    EXPECT_TRUE(gate_.IsStopRequested(kEndpointA));
}

// Empty strong IDs fail closed and can never enter either gate set.
TEST_F(DuplexOperationGateTest, EmptyEndpointIsNeverAccepted) {
    gate_.RequestStop(kNoEndpoint);
    EXPECT_FALSE(gate_.IsStopRequested(kNoEndpoint));
    EXPECT_FALSE(gate_.IsStopRequestedLocked(kNoEndpoint));
    gate_.ClearStop(kNoEndpoint);
    EXPECT_FALSE(gate_.IsStopRequestedLocked(kNoEndpoint));

    EXPECT_FALSE(gate_.Acquire(kNoEndpoint));
    EXPECT_FALSE(gate_.IsActiveLocked(kNoEndpoint));
    gate_.AcquireLocked(kNoEndpoint);
    EXPECT_FALSE(gate_.IsActiveLocked(kNoEndpoint));
    gate_.Release(kNoEndpoint);
    EXPECT_FALSE(gate_.IsActiveLocked(kNoEndpoint));
}

// The gate borrows &lock_ and reads it fresh each call: a null lock short-circuits every
// self-locking method exactly as the former members did, and the gate starts working the moment
// the borrowed lock is allocated.
TEST(DuplexOperationGateNullLock, NoLockShortCircuitsSelfLockingApi) {
    IOLock* lock = nullptr;
    DuplexOperationGate gate{&lock};
    EXPECT_FALSE(gate.Acquire(kEndpointA));
    gate.Release(kEndpointA);
    gate.RequestStop(kEndpointA);
    EXPECT_FALSE(gate.IsStopRequested(kEndpointA));

    lock = IOLockAlloc();
    ASSERT_NE(lock, nullptr);
    EXPECT_TRUE(gate.Acquire(kEndpointA));
    IOLockFree(lock);
}

// Lock-held variants do only the set op (no internal locking) — used inside RequestClockConfig /
// ClearSession's single critical section.
TEST_F(DuplexOperationGateTest, LockedVariantsMutateWithoutLocking) {
    EXPECT_FALSE(gate_.IsActiveLocked(kEndpointA));
    gate_.AcquireLocked(kEndpointA);
    EXPECT_TRUE(gate_.IsActiveLocked(kEndpointA));
    gate_.ReleaseLocked(kEndpointA);
    EXPECT_FALSE(gate_.IsActiveLocked(kEndpointA));

    gate_.RequestStop(kEndpointB);
    EXPECT_TRUE(gate_.IsStopRequestedLocked(kEndpointB));
    gate_.ClearStopLocked(kEndpointB);
    EXPECT_FALSE(gate_.IsStopRequestedLocked(kEndpointB));
}

// The self-locking public API and the lock-held variants operate on the same underlying sets.
TEST_F(DuplexOperationGateTest, SelfLockingAndLockedVariantsShareState) {
    gate_.Acquire(kEndpointC);
    EXPECT_TRUE(gate_.IsActiveLocked(kEndpointC));
    gate_.ReleaseLocked(kEndpointC);
    EXPECT_TRUE(gate_.Acquire(kEndpointC));
}

// The read-only methods are const and callable on a const gate (matches the former const
// IsStopRequested member; the const IsStopRequested still locks the borrowed lock_).
TEST_F(DuplexOperationGateTest, ConstReadMethodsCallableOnConstInstance) {
    gate_.Acquire(kEndpointA);
    gate_.RequestStop(kEndpointB);
    const DuplexOperationGate& c = gate_;
    EXPECT_TRUE(c.IsStopRequested(kEndpointB));
    EXPECT_TRUE(c.IsActiveLocked(kEndpointA));
    EXPECT_TRUE(c.IsStopRequestedLocked(kEndpointB));
    EXPECT_FALSE(c.IsStopRequestedLocked(kEndpointA));
}

} // namespace
