// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-69b characterization tests for RestartSessionStore — the per-GUID session store + restart-id
// allocator extracted from AudioDuplexCoordinator. Pins the observable contract of the
// former coordinator members (LoadSession/StoreSession/GetSession/AllocateRestartId) and the
// lock-held accessors (FindSessionLocked/EraseSessionLocked) used inside the coordinator's
// multi-domain critical sections. Host tests, no hardware.

#include <gtest/gtest.h>

#include <DriverKit/IOLib.h>

#include "Audio/Duplex/RestartSessionStore.hpp"

namespace {

using namespace ASFW::Audio::Duplex;
using ASFW::Audio::Devices::AudioEndpointId;
using ASFW::Audio::DuplexRestartSession;

constexpr AudioEndpointId kEndpoint11{0x11};
constexpr AudioEndpointId kEndpoint22{0x22};
constexpr AudioEndpointId kEndpoint33{0x33};
constexpr AudioEndpointId kEndpoint44{0x44};
constexpr AudioEndpointId kEndpoint55{0x55};
constexpr AudioEndpointId kEndpoint66{0x66};
constexpr AudioEndpointId kEndpoint77{0x77};
constexpr AudioEndpointId kNoEndpoint{};

class RestartSessionStoreTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_NE(lock_, nullptr); }
    void TearDown() override {
        if (lock_) {
            IOLockFree(lock_);
        }
    }

    IOLock* lock_ = IOLockAlloc();
    RestartSessionStore store_{&lock_};
};

// LoadSession returns a default session stamped with the endpoint on a miss.
TEST_F(RestartSessionStoreTest, LoadMissReturnsDefaultWithEndpoint) {
    const auto s = store_.LoadSession(kEndpoint11);
    EXPECT_EQ(s.endpointId, kEndpoint11);
    EXPECT_EQ(s.restartId, 0u);
}

TEST_F(RestartSessionStoreTest, StoreThenLoadAndGetRoundTrip) {
    DuplexRestartSession s{};
    s.endpointId = kEndpoint22;
    s.restartId = 9;
    store_.StoreSession(s);

    const auto loaded = store_.LoadSession(kEndpoint22);
    EXPECT_EQ(loaded.endpointId, kEndpoint22);
    EXPECT_EQ(loaded.restartId, 9u);

    const auto got = store_.GetSession(kEndpoint22);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->restartId, 9u);
}

TEST_F(RestartSessionStoreTest, GetMissIsNullopt) {
    EXPECT_FALSE(store_.GetSession(kEndpoint33).has_value());
}

// An empty endpoint is never a valid store key.
TEST_F(RestartSessionStoreTest, EmptyEndpointGuardsStorage) {
    DuplexRestartSession s{};
    s.restartId = 7;
    store_.StoreSession(s);                       // guarded no-op
    EXPECT_FALSE(store_.GetSession(kNoEndpoint).has_value());
    EXPECT_EQ(store_.LoadSession(kNoEndpoint).endpointId, kNoEndpoint);
}

TEST_F(RestartSessionStoreTest, AllocateRestartIdMonotonicFromOne) {
    EXPECT_EQ(store_.AllocateRestartId(), 1u);
    EXPECT_EQ(store_.AllocateRestartId(), 2u);
    EXPECT_EQ(store_.AllocateRestartId(), 3u);
}

// The store borrows &lock_ and reads it fresh each call: a null lock short-circuits every
// self-locking method exactly as the former members did, and it starts working once allocated.
TEST(RestartSessionStoreNullLock, NoLockShortCircuitsSelfLockingApi) {
    IOLock* lock = nullptr;
    RestartSessionStore store{&lock};
    EXPECT_EQ(store.LoadSession(kEndpoint11).endpointId, kEndpoint11);
    EXPECT_EQ(store.AllocateRestartId(), 0u);        // 0 on no lock
    EXPECT_FALSE(store.GetSession(kEndpoint11).has_value());
    DuplexRestartSession s{};
    s.endpointId = kEndpoint11;
    s.restartId = 5;
    store.StoreSession(s);                           // no-op, must not deref null lock
    EXPECT_FALSE(store.GetSession(kEndpoint11).has_value());

    lock = IOLockAlloc();
    ASSERT_NE(lock, nullptr);
    store.StoreSession(s);
    EXPECT_EQ(store.LoadSession(kEndpoint11).restartId, 5u);
    IOLockFree(lock);
}

// FindSessionLocked returns a mutable pointer to the in-map session; mutations persist and are
// visible via the self-locking reads (same underlying map) — this is what the coordinator's
// multi-domain holds rely on.
TEST_F(RestartSessionStoreTest, FindSessionLockedMutatesInPlace) {
    DuplexRestartSession s{};
    s.endpointId = kEndpoint44;
    store_.StoreSession(s);

    auto* p = store_.FindSessionLocked(kEndpoint44);
    ASSERT_NE(p, nullptr);
    p->restartId = 12;
    p->hasPendingClockRequest = true;

    const auto got = store_.GetSession(kEndpoint44);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->restartId, 12u);
    EXPECT_TRUE(got->hasPendingClockRequest);
}

TEST_F(RestartSessionStoreTest, FindMissIsNullptrAndEraseRemoves) {
    EXPECT_EQ(store_.FindSessionLocked(kEndpoint55), nullptr);
    DuplexRestartSession s{};
    s.endpointId = kEndpoint55;
    store_.StoreSession(s);
    EXPECT_NE(store_.FindSessionLocked(kEndpoint55), nullptr);
    store_.EraseSessionLocked(kEndpoint55);
    EXPECT_EQ(store_.FindSessionLocked(kEndpoint55), nullptr);
    EXPECT_FALSE(store_.GetSession(kEndpoint55).has_value());
}

// The const FindSessionLocked overload is callable on a const store (used by the const
// IsRestartEpochCurrent path).
TEST_F(RestartSessionStoreTest, ConstFindSessionLockedReads) {
    DuplexRestartSession s{};
    s.endpointId = kEndpoint66;
    s.restartId = 3;
    store_.StoreSession(s);

    const RestartSessionStore& c = store_;
    const auto* p = c.FindSessionLocked(kEndpoint66);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->restartId, 3u);
    EXPECT_EQ(c.FindSessionLocked(kEndpoint77), nullptr);
}

}  // namespace
