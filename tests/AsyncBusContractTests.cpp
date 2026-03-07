#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

#include "ASFWDriver/Async/AsyncSubsystem.hpp"
#include "ASFWDriver/Async/FireWireBusImpl.hpp"
#include "ASFWDriver/Async/Track/Tracking.hpp"
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
    async.GetGenerationTracker().OnSyntheticBusReset(10);
    async.HostTest_SetDeferPostedWork(true);

    ASFW::Driver::TopologyManager topo;
    ASFW::Async::FireWireBusImpl bus(async, topo);

    bool called = false;
    ASFW::Async::AsyncStatus status = ASFW::Async::AsyncStatus::kSuccess;

    const ASFW::FW::Generation current{async.GetBusState().generation16};
    const ASFW::FW::Generation stale{current.value + 1};

    ASFW::Async::FWAddress addr{0xFFFF, 0xF0000400};
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
