// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-69b characterization tests for RestartSessionStore — the per-GUID session store + restart-id
// allocator extracted from DiceDuplexRestartCoordinator. Pins the observable contract of the
// former coordinator members (LoadSession/StoreSession/GetSession/AllocateRestartId) and the
// lock-held accessors (FindSessionLocked/EraseSessionLocked) used inside the coordinator's
// multi-domain critical sections. Host tests, no hardware.

#include <gtest/gtest.h>

#include <DriverKit/IOLib.h>

#include "Audio/Protocols/Backends/RestartSessionStore.hpp"

namespace {

using namespace ASFW::Audio::Backends;

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

// LoadSession returns a default session stamped with the guid on a miss (former member behaviour).
TEST_F(RestartSessionStoreTest, LoadMissReturnsDefaultWithGuid) {
    const auto s = store_.LoadSession(0x11);
    EXPECT_EQ(s.guid, 0x11u);
    EXPECT_EQ(s.restartId, 0u);
}

TEST_F(RestartSessionStoreTest, StoreThenLoadAndGetRoundTrip) {
    DiceRestartSession s{};
    s.guid = 0x22;
    s.restartId = 9;
    store_.StoreSession(s);

    const auto loaded = store_.LoadSession(0x22);
    EXPECT_EQ(loaded.guid, 0x22u);
    EXPECT_EQ(loaded.restartId, 9u);

    const auto got = store_.GetSession(0x22);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->restartId, 9u);
}

TEST_F(RestartSessionStoreTest, GetMissIsNullopt) {
    EXPECT_FALSE(store_.GetSession(0x33).has_value());
}

// guid==0 guards on Load/Store/Get match the former members (Load returns default, Store no-ops).
TEST_F(RestartSessionStoreTest, ZeroGuidGuardsMatchOriginals) {
    DiceRestartSession s{};  // guid == 0
    s.restartId = 7;
    store_.StoreSession(s);                       // guarded no-op
    EXPECT_FALSE(store_.GetSession(0).has_value());
    EXPECT_EQ(store_.LoadSession(0).guid, 0u);    // default{.guid=0}
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
    EXPECT_EQ(store.LoadSession(0x11).guid, 0x11u);  // default (no lock)
    EXPECT_EQ(store.AllocateRestartId(), 0u);        // 0 on no lock
    EXPECT_FALSE(store.GetSession(0x11).has_value());
    DiceRestartSession s{};
    s.guid = 0x11;
    s.restartId = 5;
    store.StoreSession(s);                           // no-op, must not deref null lock
    EXPECT_FALSE(store.GetSession(0x11).has_value());

    lock = IOLockAlloc();
    ASSERT_NE(lock, nullptr);
    store.StoreSession(s);
    EXPECT_EQ(store.LoadSession(0x11).restartId, 5u);
    IOLockFree(lock);
}

// FindSessionLocked returns a mutable pointer to the in-map session; mutations persist and are
// visible via the self-locking reads (same underlying map) — this is what the coordinator's
// multi-domain holds rely on.
TEST_F(RestartSessionStoreTest, FindSessionLockedMutatesInPlace) {
    DiceRestartSession s{};
    s.guid = 0x44;
    store_.StoreSession(s);

    auto* p = store_.FindSessionLocked(0x44);
    ASSERT_NE(p, nullptr);
    p->restartId = 12;
    p->hasPendingClockRequest = true;

    const auto got = store_.GetSession(0x44);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->restartId, 12u);
    EXPECT_TRUE(got->hasPendingClockRequest);
}

TEST_F(RestartSessionStoreTest, FindMissIsNullptrAndEraseRemoves) {
    EXPECT_EQ(store_.FindSessionLocked(0x55), nullptr);
    DiceRestartSession s{};
    s.guid = 0x55;
    store_.StoreSession(s);
    EXPECT_NE(store_.FindSessionLocked(0x55), nullptr);
    store_.EraseSessionLocked(0x55);
    EXPECT_EQ(store_.FindSessionLocked(0x55), nullptr);
    EXPECT_FALSE(store_.GetSession(0x55).has_value());
}

// The const FindSessionLocked overload is callable on a const store (used by the const
// IsRestartEpochCurrent path).
TEST_F(RestartSessionStoreTest, ConstFindSessionLockedReads) {
    DiceRestartSession s{};
    s.guid = 0x66;
    s.restartId = 3;
    store_.StoreSession(s);

    const RestartSessionStore& c = store_;
    const auto* p = c.FindSessionLocked(0x66);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->restartId, 3u);
    EXPECT_EQ(c.FindSessionLocked(0x77), nullptr);
}

}  // namespace
