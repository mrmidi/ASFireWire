// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FW-67 characterization tests for ClockRequestBroker. These pin the clock-token, latest-wins
// pending queue, completion mailbox, eviction, and restart-session mirror behaviour extracted
// from AudioDuplexCoordinator. Host tests, no hardware.

#include <gtest/gtest.h>

#include <DriverKit/IOLib.h>

#include "Audio/Duplex/ClockRequestBroker.hpp"

namespace {

using ASFW::Audio::Duplex::ClockRequestBroker;
using ASFW::Audio::Duplex::RestartSessionStore;
using DiceClockRequestCompletion = ASFW::Audio::DuplexClockRequestCompletion;
using DiceClockRequestOutcome = ASFW::Audio::DuplexClockRequestOutcome;
using DiceRestartReason = ASFW::Audio::DuplexRestartReason;
using DiceRestartSession = ASFW::Audio::DuplexRestartSession;
using ASFW::Audio::Devices::AudioEndpointId;

constexpr AudioEndpointId kEndpoint{17};

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
        DiceRestartReason reason = DiceRestartReason::kManualReconfigure) {
        IOLockLock(lock_);
        ClockRequestBroker::PendingClockRequest request{
            .desiredClock = {.sampleRateHz = sampleRateHz},
            .reason = reason,
            .token = broker_.AllocateTokenLocked(),
        };
        EXPECT_FALSE(broker_.QueuePendingLocked(kEndpoint, request).has_value());
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
    DiceRestartSession session{};
    session.endpointId = kEndpoint;
    store_.StoreSession(session);

    IOLockLock(lock_);
    const ClockRequestBroker::PendingClockRequest first{
        .desiredClock = {.sampleRateHz = 44100},
        .reason = DiceRestartReason::kSampleRateChange,
        .token = broker_.AllocateTokenLocked(),
    };
    EXPECT_FALSE(broker_.QueuePendingLocked(kEndpoint, first).has_value());

    const ClockRequestBroker::PendingClockRequest replacement{
        .desiredClock = {.sampleRateHz = 48000},
        .reason = DiceRestartReason::kClockSourceChange,
        .token = broker_.AllocateTokenLocked(),
    };
    const auto superseded = broker_.QueuePendingLocked(kEndpoint, replacement);
    ASSERT_TRUE(superseded.has_value());
    EXPECT_EQ(superseded->token, first.token);
    EXPECT_EQ(superseded->desiredClock.sampleRateHz, 44100u);
    IOLockUnlock(lock_);

    const auto mirrored = store_.GetSession(kEndpoint);
    ASSERT_TRUE(mirrored.has_value());
    EXPECT_TRUE(mirrored->hasPendingClockRequest);
    EXPECT_EQ(mirrored->pendingClock.sampleRateHz, 48000u);
    EXPECT_EQ(mirrored->pendingReason, DiceRestartReason::kClockSourceChange);

    ClockRequestBroker::PendingClockRequest consumed{};
    ASSERT_TRUE(broker_.TryConsumePending(kEndpoint, consumed));
    EXPECT_EQ(consumed.token, replacement.token);
    EXPECT_EQ(consumed.desiredClock.sampleRateHz, 48000u);
}

TEST_F(ClockRequestBrokerTest, ConsumeClearsPendingMirrorAndOnlyConsumesOnce) {
    DiceRestartSession session{};
    session.endpointId = kEndpoint;
    store_.StoreSession(session);
    const auto request = QueueLocked(48000);

    ClockRequestBroker::PendingClockRequest consumed{};
    ASSERT_TRUE(broker_.TryConsumePending(kEndpoint, consumed));
    EXPECT_EQ(consumed.token, request.token);
    EXPECT_EQ(consumed.desiredClock.sampleRateHz, request.desiredClock.sampleRateHz);
    EXPECT_FALSE(broker_.TryConsumePending(kEndpoint, consumed));

    const auto mirrored = store_.GetSession(kEndpoint);
    ASSERT_TRUE(mirrored.has_value());
    EXPECT_FALSE(mirrored->hasPendingClockRequest);
    EXPECT_EQ(mirrored->pendingClock.sampleRateHz, 0u);
    EXPECT_EQ(mirrored->pendingReason, DiceRestartReason::kInitialStart);
}

TEST_F(ClockRequestBrokerTest, CompleteDeliversOnceAndUpdatesLastClockCompletion) {
    DiceRestartSession session{};
    session.endpointId = kEndpoint;
    store_.StoreSession(session);

    const DiceClockRequestCompletion completion{
        .token = 17,
        .desiredClock = {.sampleRateHz = 96000},
        .reason = DiceRestartReason::kManualReconfigure,
        .outcome = DiceClockRequestOutcome::kApplied,
        .status = kIOReturnSuccess,
        .restartId = 9,
        .generation = ASFW::FW::Generation{3},
    };
    broker_.Complete(completion, kEndpoint);

    const auto afterComplete = store_.GetSession(kEndpoint);
    ASSERT_TRUE(afterComplete.has_value());
    ASSERT_TRUE(afterComplete->lastClockCompletion.has_value());
    EXPECT_EQ(afterComplete->lastClockCompletion->token, completion.token);
    EXPECT_EQ(afterComplete->lastClockCompletion->outcome, DiceClockRequestOutcome::kApplied);

    DiceClockRequestCompletion taken{};
    ASSERT_TRUE(broker_.TryTakeCompleted(kEndpoint, completion.token, taken));
    EXPECT_EQ(taken.token, completion.token);
    EXPECT_EQ(taken.status, kIOReturnSuccess);
    EXPECT_FALSE(broker_.TryTakeCompleted(kEndpoint, completion.token, taken));
}

TEST_F(ClockRequestBrokerTest, FailPendingPublishesFailureWithCurrentSessionEpoch) {
    DiceRestartSession session{};
    session.endpointId = kEndpoint;
    session.restartId = 11;
    session.topologyGeneration = ASFW::FW::Generation{7};
    store_.StoreSession(session);
    const auto request = QueueLocked(88200, DiceRestartReason::kClockSourceChange);

    broker_.FailPending(kEndpoint, DiceClockRequestOutcome::kAbortedByStop, kIOReturnAborted);

    DiceClockRequestCompletion completion{};
    ASSERT_TRUE(broker_.TryTakeCompleted(kEndpoint, request.token, completion));
    EXPECT_EQ(completion.desiredClock.sampleRateHz, 88200u);
    EXPECT_EQ(completion.reason, DiceRestartReason::kClockSourceChange);
    EXPECT_EQ(completion.outcome, DiceClockRequestOutcome::kAbortedByStop);
    EXPECT_EQ(completion.status, kIOReturnAborted);
    EXPECT_EQ(completion.restartId, 11u);
    EXPECT_EQ(completion.generation.value, 7u);
}

// The original coordinator bounded each GUID's completion mailbox at 32 tokens. This avoids a
// waiter that never returns allowing an unbounded per-device accumulation.
TEST_F(ClockRequestBrokerTest, CompletionMailboxEvictsOldestAfterThirtyTwoTokens) {
    for (uint64_t token = 1; token <= 33; ++token) {
        broker_.Complete(DiceClockRequestCompletion{.token = token}, kEndpoint);
    }

    DiceClockRequestCompletion completion{};
    EXPECT_FALSE(broker_.TryTakeCompleted(kEndpoint, 1, completion));
    ASSERT_TRUE(broker_.TryTakeCompleted(kEndpoint, 2, completion));
    EXPECT_EQ(completion.token, 2u);
    ASSERT_TRUE(broker_.TryTakeCompleted(kEndpoint, 33, completion));
    EXPECT_EQ(completion.token, 33u);
}

TEST_F(ClockRequestBrokerTest, ClearLockedDropsPendingAndCompletedRequests) {
    const auto request = QueueLocked(48000);
    broker_.Complete(DiceClockRequestCompletion{.token = 44}, kEndpoint);

    IOLockLock(lock_);
    broker_.ClearLocked(kEndpoint);
    IOLockUnlock(lock_);

    ClockRequestBroker::PendingClockRequest pending{};
    DiceClockRequestCompletion completion{};
    EXPECT_FALSE(broker_.TryConsumePending(kEndpoint, pending));
    EXPECT_FALSE(broker_.TryTakeCompleted(kEndpoint, 44, completion));
    EXPECT_NE(request.token, 0u);
}

// The broker follows the same borrowed-lock lifecycle as the gate/store: self-locking paths
// short-circuit until the coordinator has allocated its IOLock.
TEST(ClockRequestBrokerNullLock, SelfLockingOperationsShortCircuitWithoutBorrowedLock) {
    IOLock* lock = nullptr;
    RestartSessionStore store{&lock};
    ClockRequestBroker broker{&lock, store};
    ClockRequestBroker::PendingClockRequest pending{};
    DiceClockRequestCompletion completion{};

    EXPECT_FALSE(broker.TryConsumePending(kEndpoint, pending));
    EXPECT_FALSE(broker.TryTakeCompleted(kEndpoint, 1, completion));
    broker.Complete(DiceClockRequestCompletion{.token = 1}, kEndpoint);
    broker.FailPending(kEndpoint, DiceClockRequestOutcome::kFailed, kIOReturnError);
    EXPECT_FALSE(broker.TryTakeCompleted(kEndpoint, 1, completion));
}

} // namespace
