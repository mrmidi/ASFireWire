#include <gtest/gtest.h>

#include <vector>

#include "ASFWDriver/Async/Track/TransactionCompletionHandler.hpp"
#include "ASFWDriver/Async/Core/TransactionManager.hpp"
#include "ASFWDriver/Async/Track/LabelAllocator.hpp"
#include "ASFWDriver/Async/Track/TxCompletion.hpp"
#include "ASFWDriver/Async/Engine/ATTrace.hpp"  // NowUs()
#include "Logging/Logging.hpp"  // For log stubs in tests

using namespace ASFW::Async;

namespace {

struct CallbackRecorder {
    int called{0};
    kern_return_t lastKr{kIOReturnError};
    std::vector<uint8_t> lastData{};
};

struct Harness {
    LabelAllocator allocator{};
    TransactionManager mgr{};
    TransactionCompletionHandler handler;
    bool initOk{false};

    Harness() : handler(&mgr, &allocator) {
        initOk = mgr.Initialize().has_value();
    }

    Transaction* AllocateTxn(uint8_t label,
                             uint32_t gen,
                             uint16_t nodeId,
                             uint8_t tcode,
                             CompletionStrategy strategy,
                             CallbackRecorder& cb) {
        auto res = mgr.Allocate(TLabel{label}, BusGeneration{gen}, NodeID{nodeId});
        EXPECT_TRUE(res.has_value());
        if (!res) {
            return nullptr;
        }
        Transaction* txn = *res;
        txn->SetCompletionStrategy(strategy);
        txn->SetTCode(tcode);
        txn->SetResponseHandler([&cb](kern_return_t kr, std::span<const uint8_t> data) {
            cb.called++;
            cb.lastKr = kr;
            cb.lastData.assign(data.begin(), data.end());
        });
        txn->TransitionTo(TransactionState::Submitted, "test");
        txn->TransitionTo(TransactionState::ATPosted, "test");
        return txn;
    }
};

TxCompletion MakeTx(uint8_t label, uint8_t ackCode, uint8_t eventCode = 0) {
    TxCompletion c{};
    c.tLabel = label;
    c.ackCode = ackCode;
    c.eventCode = static_cast<OHCIEventCode>(eventCode);
    return c;
}

}  // namespace

TEST(CompletionRefactorPlan, AckCompleteWriteCompletesOnAT) {
    Harness h;
    ASSERT_TRUE(h.initOk);
    CallbackRecorder cb;
    auto* txn = h.AllocateTxn(/*label=*/1, /*gen=*/1, /*node=*/0x1234,
                              /*tcode=*/0x1, CompletionStrategy::CompleteOnAT, cb);
    ASSERT_NE(txn, nullptr);

    h.handler.OnATCompletion(MakeTx(/*label=*/1, /*ackCode=*/0x0));

    EXPECT_EQ(cb.called, 1);
    EXPECT_EQ(cb.lastKr, kIOReturnSuccess);
    EXPECT_EQ(h.mgr.Find(TLabel{1}), nullptr);  // Extracted on completion
}

TEST(CompletionRefactorPlan, AckPendingWriteWaitsForARThenCompletes) {
    Harness h;
    ASSERT_TRUE(h.initOk);
    CallbackRecorder cb;
    auto* txn = h.AllocateTxn(/*label=*/2, /*gen=*/2, /*node=*/0x2233,
                              /*tcode=*/0x1, CompletionStrategy::CompleteOnAT, cb);
    ASSERT_NE(txn, nullptr);

    h.handler.OnATCompletion(MakeTx(/*label=*/2, /*ackCode=*/0x1));

    // Should still be managed and waiting for AR
    Transaction* live = h.mgr.Find(TLabel{2});
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(live->state(), TransactionState::AwaitingAR);
    EXPECT_EQ(cb.called, 0);

    auto key = live->GetMatchKey();
    h.handler.OnARResponse(key, /*rcode=*/0x0, {});

    EXPECT_EQ(cb.called, 1);
    EXPECT_EQ(cb.lastKr, kIOReturnSuccess);
    EXPECT_EQ(h.mgr.Find(TLabel{2}), nullptr);
}

TEST(CompletionRefactorPlan, ARArrivesBeforeAT_WinsRace) {
    Harness h;
    ASSERT_TRUE(h.initOk);
    CallbackRecorder cb;
    auto* txn = h.AllocateTxn(/*label=*/3, /*gen=*/3, /*node=*/0x3333,
                              /*tcode=*/0x1, CompletionStrategy::CompleteOnAT, cb);
    ASSERT_NE(txn, nullptr);

    auto key = txn->GetMatchKey();
    h.handler.OnARResponse(key, /*rcode=*/0x0, {});

    EXPECT_EQ(cb.called, 1);
    EXPECT_EQ(cb.lastKr, kIOReturnSuccess);
    EXPECT_EQ(h.mgr.Find(TLabel{3}), nullptr);

    // AT completion after AR should be ignored
    h.handler.OnATCompletion(MakeTx(/*label=*/3, /*ackCode=*/0x0));
    EXPECT_EQ(cb.called, 1);  // still only once
}

TEST(CompletionRefactorPlan, ReadRequiresAR_EvenIfAckComplete) {
    Harness h;
    ASSERT_TRUE(h.initOk);
    CallbackRecorder cb;
    auto* txn = h.AllocateTxn(/*label=*/4, /*gen=*/4, /*node=*/0x4444,
                              /*tcode=*/0x4, CompletionStrategy::CompleteOnAR, cb);
    ASSERT_NE(txn, nullptr);
    txn->SetSkipATCompletion(true);  // mimic RegisterTx behavior for reads

    h.handler.OnATCompletion(MakeTx(/*label=*/4, /*ackCode=*/0x0));

    Transaction* live = h.mgr.Find(TLabel{4});
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(cb.called, 0);  // no completion yet

    h.handler.OnARResponse(live->GetMatchKey(), /*rcode=*/0x0, {});

    EXPECT_EQ(cb.called, 1);
    EXPECT_EQ(cb.lastKr, kIOReturnSuccess);
    EXPECT_EQ(h.mgr.Find(TLabel{4}), nullptr);
}

TEST(CompletionRefactorPlan, BusyAckExtendsDeadlineNoCompletion) {
    Harness h;
    ASSERT_TRUE(h.initOk);
    CallbackRecorder cb;
    auto* txn = h.AllocateTxn(/*label=*/5, /*gen=*/5, /*node=*/0x5555,
                              /*tcode=*/0x1, CompletionStrategy::CompleteOnAT, cb);
    ASSERT_NE(txn, nullptr);
    const uint64_t before = txn->deadlineUs();
    txn->SetDeadline(before);  // ensure initialized

    h.handler.OnATCompletion(MakeTx(/*label=*/5, /*ackCode=*/0x4));

    Transaction* live = h.mgr.Find(TLabel{5});
    ASSERT_NE(live, nullptr);
    EXPECT_GT(live->deadlineUs(), before);
    EXPECT_EQ(live->state(), TransactionState::ATCompleted);
    EXPECT_EQ(cb.called, 0);
}
