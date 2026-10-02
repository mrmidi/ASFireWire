// SPDX-License-Identifier: Apache-2.0
#include "ASFWDriver/Shared/Completion/CompletionQueue.hpp"
#include <Block.h>
#include <gtest/gtest.h>

namespace ASFW::Shared {
struct CompletionQueueTestPeer {
    static auto Attach(IODataQueueDispatchSource* source,
                       std::shared_ptr<Common::TeardownCompletion> fence) {
        auto queue = std::unique_ptr<CompletionQueue<uint32_t>>(new CompletionQueue<uint32_t>());
        queue->source_ = OSSharedPtr(source, OSRetain);
        queue->teardownCompletion_ = std::move(fence);
        return queue;
    }
};
}
namespace {
class DeferredSource final : public IODataQueueDispatchSource {
public:
    kern_return_t Cancel(void (^handler)(void)) override {
        ++calls;
        if (fail) return kIOReturnError;
        completion = Block_copy(handler);
        return kIOReturnSuccess;
    }
    void Complete() {
        auto callback = completion;
        completion = nullptr;
        callback();
        Block_release(callback);
    }
    int calls{0};
    bool fail{false};
    void (^completion)(void){nullptr};
};
TEST(CompletionQueueTeardownTests, DestructionWaitsForCancellationAndCancelsOnlyOnce) {
    auto fence = std::make_shared<ASFW::Common::TeardownCompletion>();
    OSSharedPtr<DeferredSource> source(new DeferredSource(), OSNoRetain);
    auto queue = ASFW::Shared::CompletionQueueTestPeer::Attach(source.get(), fence);
    queue->Deactivate();
    queue->Deactivate();
    queue->SetClientUnbound();
    queue.reset();
    EXPECT_EQ(source->calls, 1);
    EXPECT_EQ(source->GetRetainCount(), 2);
    bool stopped = false;
    fence->FinishWhenDrained([&] { stopped = true; });
    EXPECT_FALSE(stopped);
    source->Complete();
    EXPECT_TRUE(stopped);
    EXPECT_EQ(source->GetRetainCount(), 1);
}
TEST(CompletionQueueTeardownTests, FailedCancellationDropsOwnershipAndDrainsFence) {
    auto fence = std::make_shared<ASFW::Common::TeardownCompletion>();
    OSSharedPtr<DeferredSource> source(new DeferredSource(), OSNoRetain);
    source->fail = true;
    auto queue = ASFW::Shared::CompletionQueueTestPeer::Attach(source.get(), fence);
    queue.reset();
    EXPECT_EQ(source->calls, 1);
    EXPECT_EQ(source->GetRetainCount(), 1);
    bool stopped = false;
    fence->FinishWhenDrained([&] { stopped = true; });
    EXPECT_TRUE(stopped);
}
}
