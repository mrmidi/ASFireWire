// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-67 characterization tests for ClockRequestBroker. These pin the clock-token, latest-wins
// pending queue, completion mailbox, eviction, and restart-session mirror behaviour extracted
// from AudioDuplexCoordinator. Host tests, no hardware.

#include <gtest/gtest.h>

#include <DriverKit/IOLib.h>

#include "Audio/Protocols/Backends/ClockRequestBroker.hpp"

namespace {

using ASFW::Audio::Backends::ClockRequestBroker;
using ASFW::Audio::Backends::RestartSessionStore;
using ASFW::Audio::DuplexClockRequestCompletion;
using ASFW::Audio::DuplexClockRequestOutcome;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::DuplexRestartSession;

constexpr uint64_t kGuid = 0x0011223344556677ULL;

class ClockRequestBrokerTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_NE(lock_, nullptr); }
    void TearDown() override {
        if (lock_) {
            IOLockFree(lock_);
        }
    }

    ClockRequestBroker::PendingClockRequest QueueLocked(
        uint32_t sampleRateHz,
        DuplexRestartReason reason = DuplexRestartReason::kManualReconfigure) {
        IOLockLock(lock_);
        ClockRequestBroker::PendingClockRequest request{
            .desiredClock = {.sampleRateHz = sampleRateHz},
            .reason = reason,
            .token = broker_.AllocateTokenLocked(),
        };
        EXPECT_FALSE(broker_.QueuePendingLocked(kGuid, request).has_value());
        IOLockUnlock(lock_);
        return request;
    }

    IOLock* lock_ = IOLockAlloc();
    RestartSessionStore store_{&lock_};
    ClockRequestBroker broker_{&lock_, store_};
};

TEST_F(ClockRequestBrokerTest, TokenAllocationStartsAtOneAndIsMonotonic) {
    IOLockLock(lock_);
    EXPECT_EQ(broker_.AllocateTokenLocked(), 1u);
    EXPECT_EQ(broker_.AllocateTokenLocked(), 2u);
    EXPECT_EQ(broker_.AllocateTokenLocked(), 3u);
    IOLockUnlock(lock_);
}

// RequestClockConfig holds the coordinator lock across queue insertion and session mutation.
// The broker must preserve that atomic latest-wins update, including its session mirror.
TEST_F(ClockRequestBrokerTest, QueuePendingLatestWinsAndMirrorsRestartSession) {
    DuplexRestartSession session{};
    session.guid = kGuid;
    store_.StoreSession(session);

    IOLockLock(lock_);
    const ClockRequestBroker::PendingClockRequest first{
        .desiredClock = {.sampleRateHz = 44100},
        .reason = DuplexRestartReason::kSampleRateChange,
        .token = broker_.AllocateTokenLocked(),
    };
    EXPECT_FALSE(broker_.QueuePendingLocked(kGuid, first).has_value());

    const ClockRequestBroker::PendingClockRequest replacement{
        .desiredClock = {.sampleRateHz = 48000},
        .reason = DuplexRestartReason::kClockSourceChange,
        .token = broker_.AllocateTokenLocked(),
    };
    const auto superseded = broker_.QueuePendingLocked(kGuid, replacement);
    ASSERT_TRUE(superseded.has_value());
    EXPECT_EQ(superseded->token, first.token);
    EXPECT_EQ(superseded->desiredClock.sampleRateHz, 44100u);
    IOLockUnlock(lock_);

    const auto mirrored = store_.GetSession(kGuid);
    ASSERT_TRUE(mirrored.has_value());
    EXPECT_TRUE(mirrored->hasPendingClockRequest);
    EXPECT_EQ(mirrored->pendingClock.sampleRateHz, 48000u);
    EXPECT_EQ(mirrored->pendingReason, DuplexRestartReason::kClockSourceChange);

    ClockRequestBroker::PendingClockRequest consumed{};
    ASSERT_TRUE(broker_.TryConsumePending(kGuid, consumed));
    EXPECT_EQ(consumed.token, replacement.token);
    EXPECT_EQ(consumed.desiredClock.sampleRateHz, 48000u);
}

TEST_F(ClockRequestBrokerTest, ConsumeClearsPendingMirrorAndOnlyConsumesOnce) {
    DuplexRestartSession session{};
    session.guid = kGuid;
    store_.StoreSession(session);
    const auto request = QueueLocked(48000);

    ClockRequestBroker::PendingClockRequest consumed{};
    ASSERT_TRUE(broker_.TryConsumePending(kGuid, consumed));
    EXPECT_EQ(consumed.token, request.token);
    EXPECT_EQ(consumed.desiredClock.sampleRateHz, request.desiredClock.sampleRateHz);
    EXPECT_FALSE(broker_.TryConsumePending(kGuid, consumed));

    const auto mirrored = store_.GetSession(kGuid);
    ASSERT_TRUE(mirrored.has_value());
    EXPECT_FALSE(mirrored->hasPendingClockRequest);
    EXPECT_EQ(mirrored->pendingClock.sampleRateHz, 0u);
    EXPECT_EQ(mirrored->pendingReason, DuplexRestartReason::kInitialStart);
}

TEST_F(ClockRequestBrokerTest, CompleteDeliversOnceAndUpdatesLastClockCompletion) {
    DuplexRestartSession session{};
    session.guid = kGuid;
    store_.StoreSession(session);

    const DuplexClockRequestCompletion completion{
        .token = 17,
        .desiredClock = {.sampleRateHz = 96000},
        .reason = DuplexRestartReason::kManualReconfigure,
        .outcome = DuplexClockRequestOutcome::kApplied,
        .status = kIOReturnSuccess,
        .restartId = 9,
        .generation = ASFW::FW::Generation{3},
    };
    broker_.Complete(completion, kGuid);

    const auto afterComplete = store_.GetSession(kGuid);
    ASSERT_TRUE(afterComplete.has_value());
    ASSERT_TRUE(afterComplete->lastClockCompletion.has_value());
    EXPECT_EQ(afterComplete->lastClockCompletion->token, completion.token);
    EXPECT_EQ(afterComplete->lastClockCompletion->outcome, DuplexClockRequestOutcome::kApplied);

    DuplexClockRequestCompletion taken{};
    ASSERT_TRUE(broker_.TryTakeCompleted(kGuid, completion.token, taken));
    EXPECT_EQ(taken.token, completion.token);
    EXPECT_EQ(taken.status, kIOReturnSuccess);
    EXPECT_FALSE(broker_.TryTakeCompleted(kGuid, completion.token, taken));
}

TEST_F(ClockRequestBrokerTest, FailPendingPublishesFailureWithCurrentSessionEpoch) {
    DuplexRestartSession session{};
    session.guid = kGuid;
    session.restartId = 11;
    session.topologyGeneration = ASFW::FW::Generation{7};
    store_.StoreSession(session);
    const auto request = QueueLocked(88200, DuplexRestartReason::kClockSourceChange);

    broker_.FailPending(kGuid, DuplexClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

    DuplexClockRequestCompletion completion{};
    ASSERT_TRUE(broker_.TryTakeCompleted(kGuid, request.token, completion));
    EXPECT_EQ(completion.desiredClock.sampleRateHz, 88200u);
    EXPECT_EQ(completion.reason, DuplexRestartReason::kClockSourceChange);
    EXPECT_EQ(completion.outcome, DuplexClockRequestOutcome::kAbortedByStop);
    EXPECT_EQ(completion.status, kIOReturnAborted);
    EXPECT_EQ(completion.restartId, 11u);
    EXPECT_EQ(completion.generation.value, 7u);
}

// The original coordinator bounded each GUID's completion mailbox at 32 tokens. This avoids a
// waiter that never returns allowing an unbounded per-device accumulation.
TEST_F(ClockRequestBrokerTest, CompletionMailboxEvictsOldestAfterThirtyTwoTokens) {
    for (uint64_t token = 1; token <= 33; ++token) {
        broker_.Complete(DuplexClockRequestCompletion{.token = token}, kGuid);
    }

    DuplexClockRequestCompletion completion{};
    EXPECT_FALSE(broker_.TryTakeCompleted(kGuid, 1, completion));
    ASSERT_TRUE(broker_.TryTakeCompleted(kGuid, 2, completion));
    EXPECT_EQ(completion.token, 2u);
    ASSERT_TRUE(broker_.TryTakeCompleted(kGuid, 33, completion));
    EXPECT_EQ(completion.token, 33u);
}

TEST_F(ClockRequestBrokerTest, ClearLockedDropsPendingAndCompletedRequests) {
    const auto request = QueueLocked(48000);
    broker_.Complete(DuplexClockRequestCompletion{.token = 44}, kGuid);

    IOLockLock(lock_);
    broker_.ClearLocked(kGuid);
    IOLockUnlock(lock_);

    ClockRequestBroker::PendingClockRequest pending{};
    DuplexClockRequestCompletion completion{};
    EXPECT_FALSE(broker_.TryConsumePending(kGuid, pending));
    EXPECT_FALSE(broker_.TryTakeCompleted(kGuid, 44, completion));
    EXPECT_NE(request.token, 0u);
}

// The broker follows the same borrowed-lock lifecycle as the gate/store: self-locking paths
// short-circuit until the coordinator has allocated its IOLock.
TEST(ClockRequestBrokerNullLock, SelfLockingOperationsShortCircuitWithoutBorrowedLock) {
    IOLock* lock = nullptr;
    RestartSessionStore store{&lock};
    ClockRequestBroker broker{&lock, store};
    ClockRequestBroker::PendingClockRequest pending{};
    DuplexClockRequestCompletion completion{};

    EXPECT_FALSE(broker.TryConsumePending(kGuid, pending));
    EXPECT_FALSE(broker.TryTakeCompleted(kGuid, 1, completion));
    broker.Complete(DuplexClockRequestCompletion{.token = 1}, kGuid);
    broker.FailPending(kGuid, DuplexClockRequestOutcome::kFailed, kIOReturnError);
    EXPECT_FALSE(broker.TryTakeCompleted(kGuid, 1, completion));
}

} // namespace
