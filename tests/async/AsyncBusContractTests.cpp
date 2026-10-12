#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

#include "ASFWDriver/Async/AsyncSubsystem.hpp"
#include "ASFWDriver/Async/FireWireBusImpl.hpp"
#include "ASFWDriver/Async/Track/Tracking.hpp"
#include "ASFWDriver/Bus/GenerationTracker.hpp"
#include "ASFWDriver/Bus/TopologyManager.hpp"

namespace {

struct DummyCompletionQueue {};

using namespace ASFW::Async;

// Host-side helper that mirrors the minimal transaction-handle cancellation pattern:
// Extract the transaction, mark cancelled, invoke response handler, free label.
[[nodiscard]] bool CancelTransactionHandleForTest(TransactionManager& txnMgr,
                                                  LabelAllocator& allocator, AsyncHandle handle) {
    if (!handle || handle.value < 1 || handle.value > 64) {
        return false;
    }

    const uint8_t label = static_cast<uint8_t>(handle.value - 1);
    auto txn = txnMgr.Extract(TLabel{label});
    if (!txn) {
        return false;
    }

    allocator.Free(label);

    if (!IsTerminalState(txn->state())) {
        txn->TransitionTo(TransactionState::Cancelled, "CancelTransactionHandleForTest");
        txn->InvokeResponseHandler(kIOReturnAborted, 0xFF, {});
    }

    return true;
}

} // namespace

TEST(AsyncBusContract, UnpostedAdmissionRollbackDoesNotExhaustLabelsOrCallCompletion) {
    DummyCompletionQueue queue;
    LabelAllocator allocator;
    allocator.Reset();
    TransactionManager manager;
    ASSERT_TRUE(manager.Initialize());
    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue);
    unsigned callbacks = 0;
    TxMetadata metadata{};
    metadata.generation = 1;
    metadata.callback = [&](AsyncHandle, AsyncStatus, uint8_t, std::span<const uint8_t>) { ++callbacks; };
    // More failures than the complete label pool: none was posted or had a timeout.
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto handle = tracking.RegisterTx(metadata);
        ASSERT_TRUE(handle);
        ASSERT_TRUE(tracking.ReleaseUnposted(handle));
        EXPECT_EQ(manager.Count(), 0u);
        EXPECT_FALSE(allocator.HasAnyLabelsInUse());
    }
    EXPECT_EQ(callbacks, 0u);
    const auto posted = tracking.RegisterTx(metadata);
    ASSERT_TRUE(posted);
    tracking.OnTxPosted(posted, 100, 500000);
    EXPECT_FALSE(tracking.ReleaseUnposted(posted));
    EXPECT_EQ(manager.Count(), 1u);
    EXPECT_TRUE(allocator.HasAnyLabelsInUse());
    tracking.CancelAllAndFreeLabels();
}

TEST(AsyncBusContract, AdmissionGuardReleasesUnlessMarkedPosted) {
    DummyCompletionQueue queue;
    LabelAllocator allocator;
    allocator.Reset();
    TransactionManager manager;
    ASSERT_TRUE(manager.Initialize());
    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue);
    unsigned callbacks = 0;
    TxMetadata metadata{};
    metadata.generation = 1;
    metadata.callback = [&](AsyncHandle, AsyncStatus, uint8_t, std::span<const uint8_t>) { ++callbacks; };
    {   // an early return in Submit(): the guard goes out of scope unposted
        const auto handle = tracking.RegisterTx(metadata);
        ASSERT_TRUE(handle);
        UnpostedRegistrationGuard guard{&tracking, handle};
    }
    EXPECT_EQ(manager.Count(), 0u);
    EXPECT_FALSE(allocator.HasAnyLabelsInUse());
    {   // the normal path: posted, then marked
        const auto handle = tracking.RegisterTx(metadata);
        ASSERT_TRUE(handle);
        UnpostedRegistrationGuard guard{&tracking, handle};
        tracking.OnTxPosted(handle, 100, 500000);
        guard.MarkPosted();
    }
    EXPECT_EQ(manager.Count(), 1u);
    EXPECT_TRUE(allocator.HasAnyLabelsInUse());
    EXPECT_EQ(callbacks, 0u);
    tracking.CancelAllAndFreeLabels();
}

TEST(AsyncBusContract, Cancel_TransactionHandle_FiresExactlyOnceWithAborted) {
    DummyCompletionQueue dummyQueue;

    LabelAllocator allocator;
    allocator.Reset();

    TransactionManager txnMgr;
    auto initRes = txnMgr.Initialize();
    ASSERT_TRUE(initRes) << "TransactionManager::Initialize failed";

    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &txnMgr, dummyQueue);

    std::atomic<uint32_t> called{0};
    AsyncStatus lastStatus = AsyncStatus::kSuccess;

    TxMetadata meta{};
    meta.generation = 1;
    meta.destinationNodeID = 0x0001;
    meta.tCode = 0x0;
    meta.expectedLength = 0;
    meta.callback = [&](AsyncHandle, AsyncStatus status, uint8_t, std::span<const uint8_t>) {
        lastStatus = status;
        called.fetch_add(1, std::memory_order_relaxed);
    };

    const AsyncHandle handle = tracking.RegisterTx(meta);
    ASSERT_TRUE(handle) << "RegisterTx returned invalid handle";

    EXPECT_TRUE(CancelTransactionHandleForTest(txnMgr, allocator, handle));
    EXPECT_EQ(1u, called.load(std::memory_order_relaxed));
    EXPECT_EQ(AsyncStatus::kAborted, lastStatus);
}

TEST(AsyncBusContract, Cancel_UnknownHandle_ReturnsFalse_NoCallback) {
    DummyCompletionQueue dummyQueue;

    LabelAllocator allocator;
    allocator.Reset();

    TransactionManager txnMgr;
    auto initRes = txnMgr.Initialize();
    ASSERT_TRUE(initRes) << "TransactionManager::Initialize failed";

    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &txnMgr, dummyQueue);

    std::atomic<uint32_t> called{0};

    TxMetadata meta{};
    meta.generation = 1;
    meta.destinationNodeID = 0x0001;
    meta.tCode = 0x0;
    meta.expectedLength = 0;
    meta.callback = [&](AsyncHandle, AsyncStatus, uint8_t, std::span<const uint8_t>) {
        called.fetch_add(1, std::memory_order_relaxed);
    };

    // Do not register any transaction for this handle.
    EXPECT_FALSE(CancelTransactionHandleForTest(txnMgr, allocator, AsyncHandle{42}));
    EXPECT_EQ(0u, called.load(std::memory_order_relaxed));
}

TEST(AsyncBusContract, GenerationMismatch_AdapterCompletesStaleGeneration_AsyncNotInline) {
    ASFW::Async::AsyncSubsystem async;
    async.GetGenerationTracker().OnConfirmedBusGeneration(10);
    async.HostTest_SetDeferPostedWork(true);

    ASFW::Driver::TopologyManager topo;
    ASFW::Async::FireWireBusImpl bus(async, topo);

    bool called = false;
    ASFW::Async::AsyncStatus status = ASFW::Async::AsyncStatus::kSuccess;

    const ASFW::FW::Generation current{async.GetBusState().generation16};
    const ASFW::FW::Generation stale{current.value + 1};

    ASFW::Async::FWAddress addr{ASFW::Async::FWAddress::AddressParts{
        .addressHi = 0xFFFF,
        .addressLo = 0xF0000400,
    }};
    (void)bus.ReadBlock(stale, ASFW::FW::NodeId{1}, addr, 4, ASFW::FW::FwSpeed::S100,
                        [&](ASFW::Async::AsyncStatus s, std::span<const uint8_t> payload) {
                            called = true;
                            status = s;
                            EXPECT_TRUE(payload.empty());
                        });

    // Must not invoke callback inline on the submit path.
    EXPECT_FALSE(called);

    async.HostTest_DrainPostedWork();

    EXPECT_TRUE(called);
    EXPECT_EQ(ASFW::Async::AsyncStatus::kStaleGeneration, status);
}

TEST(AsyncBusContract, ReentrantARCompletionFreesLabelBeforeCallback) {
    DummyCompletionQueue dummyQueue;

    LabelAllocator allocator;
    allocator.Reset();

    TransactionManager txnMgr;
    auto initRes = txnMgr.Initialize();
    ASSERT_TRUE(initRes) << "TransactionManager::Initialize failed";

    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &txnMgr, dummyQueue);

    std::optional<AsyncHandle> nestedHandle;

    TxMetadata nestedMeta{};
    nestedMeta.generation = 9;
    nestedMeta.destinationNodeID = 0x0001;
    nestedMeta.tCode = 0x4;
    nestedMeta.expectedLength = 4;
    nestedMeta.completionStrategy = CompletionStrategy::CompleteOnAR;
    nestedMeta.callback = [](AsyncHandle, AsyncStatus, uint8_t, std::span<const uint8_t>) {};

    TxMetadata meta{};
    meta.generation = 9;
    meta.destinationNodeID = 0x0001;
    meta.tCode = 0x4;
    meta.expectedLength = 4;
    meta.completionStrategy = CompletionStrategy::CompleteOnAR;
    meta.callback = [&](AsyncHandle, AsyncStatus status, uint8_t, std::span<const uint8_t>) {
        EXPECT_EQ(AsyncStatus::kSuccess, status);
        nestedHandle = tracking.RegisterTx(nestedMeta);
    };

    const AsyncHandle handle = tracking.RegisterTx(meta);
    ASSERT_TRUE(handle);
    tracking.OnTxPosted(handle, /*nowUsec=*/1000, /*timeoutUsec=*/500000);

    tracking.OnRxResponse(RxResponse{
        .generation = 9,
        .sourceNodeID = 0x0001,
        .destinationNodeID = 0xffc0,
        .tLabel = static_cast<uint8_t>(handle.value - 1),
        .tCode = 0x6,
        .rCode = 0x0,
        .payload = {},
    });

    ASSERT_TRUE(nestedHandle.has_value());
    EXPECT_EQ(2u, nestedHandle->value) << "next request should advance to the next tLabel";
}

TEST(AsyncBusContract, FinalBusyOnReadCompletesAtOnceAsBusyRetryExhausted) {
    // Declared before the transaction manager: tearing it down runs the
    // response handlers, which capture these by reference.
    int calls = 0;
    AsyncStatus status = AsyncStatus::kSuccess;
    DummyCompletionQueue dummyQueue;

    LabelAllocator allocator;
    allocator.Reset();

    TransactionManager txnMgr;
    ASSERT_TRUE(txnMgr.Initialize()) << "TransactionManager::Initialize failed";

    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &txnMgr, dummyQueue);

    TxMetadata meta{};
    meta.generation = 9;
    meta.destinationNodeID = 0x0001;
    meta.tCode = 0x4;
    meta.expectedLength = 4;
    meta.completionStrategy = CompletionStrategy::CompleteOnAR;
    meta.callback = [&](AsyncHandle, AsyncStatus s, uint8_t, std::span<const uint8_t>) {
        ++calls;
        status = s;
    };

    const AsyncHandle handle = tracking.RegisterTx(meta);
    ASSERT_TRUE(handle);
    tracking.OnTxPosted(handle, /*nowUsec=*/1000, /*timeoutUsec=*/500000);

    // A read is parked in AwaitingAR at post time; a final ack_busy (event 0x14,
    // after the controller's ATRetries) means the node never took it.
    TxCompletion busy{};
    busy.tLabel = static_cast<uint8_t>(handle.value - 1);
    busy.eventCode = OHCIEventCode::kAckBusyX;
    tracking.OnTxCompletion(busy);

    EXPECT_EQ(1, calls);
    EXPECT_EQ(AsyncStatus::kBusyRetryExhausted, status);
    EXPECT_EQ(nullptr, txnMgr.Find(TLabel{busy.tLabel}));

    // Its old deadline passing must not complete it a second time.
    tracking.OnTimeoutTick(/*nowUsec=*/1000 + 2'000'000);
    EXPECT_EQ(1, calls);
}

TEST(AsyncBusContract, StaleBitmapRecoveryPreservesGeneration) {
    DummyCompletionQueue dummyQueue;

    LabelAllocator allocator;
    allocator.Reset();
    ASFW::Async::Bus::GenerationTracker tracker{allocator};
    tracker.Reset();
    tracker.OnConfirmedBusGeneration(10);

    TransactionManager txnMgr;
    auto initRes = txnMgr.Initialize();
    ASSERT_TRUE(initRes) << "TransactionManager::Initialize failed";

    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &txnMgr, dummyQueue);

    TxMetadata meta{};
    meta.generation = tracker.GetCurrentState().generation8;
    meta.destinationNodeID = 0x0001;
    meta.tCode = 0x4;
    meta.expectedLength = 4;
    meta.completionStrategy = CompletionStrategy::CompleteOnAR;
    meta.callback = [](AsyncHandle, AsyncStatus, uint8_t, std::span<const uint8_t>) {};

    const AsyncHandle first = tracking.RegisterTx(meta);
    ASSERT_TRUE(first);
    auto firstTxn = txnMgr.Extract(TLabel{static_cast<uint8_t>(first.value - 1)});
    ASSERT_NE(firstTxn, nullptr);

    EXPECT_EQ(10u, tracker.GetCurrentState().generation8);
    EXPECT_EQ(10u, tracker.GetCurrentState().generation16);

    const AsyncHandle second = tracking.RegisterTx(meta);
    ASSERT_TRUE(second);

    EXPECT_EQ(10u, tracker.GetCurrentState().generation8);
    EXPECT_EQ(10u, tracker.GetCurrentState().generation16);
}

// Reproduce the observed PHASE 88 event sequence without hardware: the request
// is accepted (ACK_PENDING), followed by silence or a delayed write response.
TEST(AsyncBusContract, PendingUsesOneSplitDeadlineFromTransmissionForReadsAndWrites) {
    for (const uint8_t tCode : {uint8_t{0x1}, uint8_t{0x4}}) {
        DummyCompletionQueue queue;
        LabelAllocator allocator;
        TransactionManager manager;
        ASSERT_TRUE(manager.Initialize());
        auto timing = std::make_shared<ASFW::FW::SplitTimeout>();
        Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue, nullptr, timing);
        unsigned callbacks = 0;
        AsyncStatus result = AsyncStatus::kSuccess;
        TxMetadata meta{};
        meta.generation = 1; meta.destinationNodeID = 0xffc1; meta.tCode = tCode;
        meta.completionStrategy = tCode == 0x4 ? CompletionStrategy::CompleteOnAR : CompletionStrategy::CompleteOnAT;
        meta.callback = [&](AsyncHandle, AsyncStatus status, uint8_t, std::span<const uint8_t>) {
            ++callbacks; result = status;
        };
        const auto handle = tracking.RegisterTx(meta);
        ASSERT_TRUE(handle);
        const auto label = static_cast<uint8_t>(handle.value - 1);
        tracking.OnTxPosted(handle, 1000, 500000);
        tracking.OnTxCompletion(TxCompletion{.eventCode = OHCIEventCode::kAckPending, .tLabel = label}, 2000);
        tracking.OnTxCompletion(TxCompletion{.eventCode = OHCIEventCode::kAckPending, .tLabel = label}, 900000);
        tracking.OnTimeoutTick(501000); // original submission watchdog must no longer expire it
        EXPECT_EQ(callbacks, 0u);
        manager.WithTransaction(TLabel{label}, [&](Transaction* txn) {
            EXPECT_EQ(txn->deadlineUs(), 2002000u);
            EXPECT_EQ(txn->state(), TransactionState::AwaitingAR);
            EXPECT_EQ(txn->retryCount(), 0u);
        });
        tracking.OnTimeoutTick(2001999);
        EXPECT_EQ(callbacks, 0u);
        tracking.OnTimeoutTick(2002000);
        EXPECT_EQ(callbacks, 1u);
        EXPECT_EQ(result, AsyncStatus::kTimeout);
        EXPECT_EQ(manager.Count(), 0u);
        EXPECT_FALSE(allocator.HasAnyLabelsInUse());
        tracking.OnTimeoutTick(3000000);
        tracking.OnRxResponse(RxResponse{.generation = 1, .sourceNodeID = 0xffc1, .tLabel = label});
        EXPECT_EQ(callbacks, 1u); // a late response cannot revive an expired attempt
    }
}

TEST(AsyncBusContract, DelayedResponseDuringConfiguredSplitWaitCompletesOnce) {
    DummyCompletionQueue queue; LabelAllocator allocator; TransactionManager manager;
    ASSERT_TRUE(manager.Initialize());
    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue);
    unsigned callbacks = 0;
    TxMetadata meta{}; meta.generation = 1; meta.destinationNodeID = 0xffc1; meta.tCode = 0x1;
    meta.callback = [&](AsyncHandle, AsyncStatus status, uint8_t, std::span<const uint8_t>) {
        ++callbacks; EXPECT_EQ(status, AsyncStatus::kSuccess);
    };
    const auto handle = tracking.RegisterTx(meta);
    const auto label = static_cast<uint8_t>(handle.value - 1);
    tracking.OnTxPosted(handle, 1000, 500000);
    tracking.OnTxCompletion(TxCompletion{.eventCode = OHCIEventCode::kAckPending, .tLabel = label}, 2000);
    tracking.OnTimeoutTick(1500000); // beyond the old 1.25-second extension budget
    EXPECT_EQ(callbacks, 0u);
    tracking.OnRxResponse(RxResponse{.generation = 1, .sourceNodeID = 0xffc1, .tLabel = label});
    tracking.OnTimeoutTick(3000000);
    EXPECT_EQ(callbacks, 1u);
}

TEST(AsyncBusContract, ARBeforeATThenDelayedPendingCannotRecreateTransaction) {
    DummyCompletionQueue queue; LabelAllocator allocator; TransactionManager manager;
    ASSERT_TRUE(manager.Initialize());
    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue);
    unsigned callbacks = 0;
    TxMetadata meta{}; meta.generation = 1; meta.destinationNodeID = 0xffc1; meta.tCode = 0x1;
    meta.callback = [&](AsyncHandle, AsyncStatus, uint8_t, std::span<const uint8_t>) { ++callbacks; };
    const auto handle = tracking.RegisterTx(meta);
    const auto label = static_cast<uint8_t>(handle.value - 1);
    tracking.OnTxPosted(handle, 1000, 500000);
    tracking.OnRxResponse(RxResponse{.generation = 1, .sourceNodeID = 0xffc1, .tLabel = label});
    tracking.OnTxCompletion(TxCompletion{.eventCode = OHCIEventCode::kAckPending, .tLabel = label}, 2000);
    EXPECT_EQ(callbacks, 1u);
    EXPECT_EQ(manager.Count(), 0u);
}

TEST(AsyncBusContract, PendingBusResetCancelsWithoutWaitingOrReplay) {
    DummyCompletionQueue queue; LabelAllocator allocator; TransactionManager manager;
    ASSERT_TRUE(manager.Initialize());
    Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue);
    unsigned callbacks = 0;
    TxMetadata meta{}; meta.generation = 1; meta.destinationNodeID = 0xffc1; meta.tCode = 0x1;
    meta.callback = [&](AsyncHandle, AsyncStatus status, uint8_t, std::span<const uint8_t>) {
        ++callbacks; EXPECT_EQ(status, AsyncStatus::kAborted);
    };
    const auto handle = tracking.RegisterTx(meta);
    tracking.OnTxPosted(handle, 1000, 500000);
    tracking.OnTxCompletion(TxCompletion{.eventCode = OHCIEventCode::kAckPending,
        .tLabel = static_cast<uint8_t>(handle.value - 1)}, 2000);
    tracking.CancelByGeneration(1);
    tracking.OnTimeoutTick(3000000);
    EXPECT_EQ(callbacks, 1u);
    EXPECT_EQ(manager.Count(), 0u);
}

TEST(AsyncBusContract, ExpiredLabelIsProtectedForSameNodeAcrossAllocatorWrap) {
    LabelAllocator allocator;
    const auto label = allocator.Allocate(0xffc1, 1000);
    ASSERT_EQ(label, 0u);
    allocator.Quarantine(label, 0xffc1, 4002000);
    allocator.Free(label);
    for (unsigned i = 0; i < 64; ++i) {
        const auto next = allocator.Allocate(0xffc1, 3000000);
        ASSERT_NE(next, LabelAllocator::kInvalidLabel);
        EXPECT_NE(next, label);
        allocator.Free(next);
    }
    allocator.ClearBitmap(); // stale-bitmap recovery must preserve the protection
    EXPECT_EQ(allocator.Allocate(0xffc0, 3000000), label); // another node is independent
    allocator.Free(label);
    allocator.ClearBitmap();
    EXPECT_EQ(allocator.Allocate(0xffc1, 4002000), label);
}

TEST(AsyncBusContract, DelayedATTimestampCorrelationAccountsForEightSecondWrap) {
    using ASFW::FW::SplitTimeout;
    const uint16_t stamp = static_cast<uint16_t>((7u << 13) | 7990u);
    const uint32_t current = 10u << 12;
    EXPECT_EQ(SplitTimeout::RequestTimeUs(stamp, current, 1000000), 997625u);
    EXPECT_EQ(SplitTimeout::RequestTimeUs(100, 100u << 12, 1000000), 1000000u);
}

TEST(AsyncBusContract, TimeoutHistoryForAnotherNodeDoesNotEraseEarlierProtection) {
    LabelAllocator allocator;
    allocator.Quarantine(0, 0xffc1, 4000000);
    allocator.Quarantine(0, 0xffc2, 3500000);
    EXPECT_NE(allocator.Allocate(0xffc1, 3600000), 0);
    allocator.ClearBitmap();
    EXPECT_EQ(allocator.Allocate(0xffc2, 3600000), 0);
}

TEST(AsyncBusContract, NegativeATEventsTerminateReadRatherThanWaitingForData) {
    for (const auto event : {OHCIEventCode::kAckDataError, OHCIEventCode::kAckTypeError,
                             OHCIEventCode::kEvtFlushed, OHCIEventCode::kEvtMissingAck}) {
        DummyCompletionQueue queue; LabelAllocator allocator; TransactionManager manager;
        ASSERT_TRUE(manager.Initialize());
        Track_Tracking<DummyCompletionQueue> tracking(&allocator, &manager, queue);
        unsigned callbacks = 0;
        TxMetadata meta{}; meta.generation = 1; meta.destinationNodeID = 0xffc1; meta.tCode = 0x4;
        meta.completionStrategy = CompletionStrategy::CompleteOnAR;
        meta.callback = [&](AsyncHandle, AsyncStatus status, uint8_t, std::span<const uint8_t>) {
            ++callbacks; EXPECT_NE(status, AsyncStatus::kSuccess);
        };
        const auto handle = tracking.RegisterTx(meta);
        tracking.OnTxPosted(handle, 1000, 500000);
        tracking.OnTxCompletion(TxCompletion{.eventCode = event, .ackCode = 0x1,
            .tLabel = static_cast<uint8_t>(handle.value - 1)});
        EXPECT_EQ(callbacks, 1u);
        EXPECT_EQ(manager.Count(), 0u);
    }
}
