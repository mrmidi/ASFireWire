// SPDX-License-Identifier: Apache-2.0
#include "ASFWDriver/Protocols/SBP2/Session/DriverKitSessionScheduler.hpp"
#include <Block.h>
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

namespace ASFW::Protocols::SBP2 {
struct SessionSchedulerTestPeer {
    static void Attach(DriverKitSessionScheduler& scheduler, IOTimerDispatchSource* timer,
                       OSAction* action, std::shared_ptr<ASFW::Common::TeardownCompletion> completion = {}) {
        scheduler.timer_ = OSSharedPtr(timer, OSNoRetain);
        scheduler.action_ = OSSharedPtr(action, OSNoRetain);
        scheduler.teardownCompletion_ = std::move(completion);
    }
};
}

namespace {
using namespace ASFW::Protocols::SBP2;

class DriverHoldingAction final : public OSAction {
public:
    explicit DriverHoldingAction(OSObject* driver, bool deferred = false) : driver_(driver), deferred_(deferred) { driver_->retain(); }
    ~DriverHoldingAction() override {
        if (driver_) driver_->release();
    }
    kern_return_t Cancel(void (^handler)(void)) override {
        if (deferred_) {
            completion_ = Block_copy(handler);
            return kIOReturnSuccess;
        }
        // Model OSAction.iig's target lifetime: cancellation drops its target
        // even if another task still owns a reference to the action itself.
        if (driver_) {
            driver_->release();
            driver_ = nullptr;
        }
        return OSAction::Cancel(handler);
    }
    void CompleteCancellation() {
        if (driver_) {
            driver_->release();
            driver_ = nullptr;
        }
        auto completion = completion_;
        completion_ = nullptr;
        completion();
        Block_release(completion);
    }
private:
    OSObject* driver_;
    bool deferred_;
    void (^completion_)(void){nullptr};
};

// Model the SDK's handler retain and deferred cancellation. Destroying the
// scheduler must neither leak the action's driver nor free the timer before
// cancellation completion. This is an ownership model, not a hardware test.
class DeferredTimer final : public IOTimerDispatchSource {
public:
    explicit DeferredTimer(OSAction* action) : action_(action, OSRetain) {}
    kern_return_t Cancel(void (^handler)(void)) override {
        ++cancelCalls;
        completion_ = Block_copy(handler);
        return kIOReturnSuccess;
    }
    void CompleteCancellation() {
        action_.reset();
        auto completion = completion_;
        completion_ = nullptr;
        completion();
        Block_release(completion);
    }
    int cancelCalls{0};
private:
    OSSharedPtr<OSAction> action_;
    void (^completion_)(void){nullptr};
};

TEST(DriverKitSessionSchedulerTeardownTests, ResetBreaksDriverRetainAfterCancellationCompletes) {
    OSSharedPtr<OSObject> driver(new OSObject(), OSNoRetain);
    auto* action = new DriverHoldingAction(driver.get());
    OSSharedPtr<DeferredTimer> timer(new DeferredTimer(action), OSNoRetain);
    timer->retain(); // scheduler's ownership
    {
        DriverKitSessionScheduler scheduler;
        SessionSchedulerTestPeer::Attach(scheduler, timer.get(), action);
        ASSERT_EQ(driver->GetRetainCount(), 2);
        scheduler.Reset();
        scheduler.Reset();
        EXPECT_EQ(timer->cancelCalls, 1);
        EXPECT_EQ(timer->GetRetainCount(), 2);
        EXPECT_EQ(driver->GetRetainCount(), 2);
    }
    // The completion captures only the source/action, so even destruction of
    // the scheduler before completion cannot leave a dangling scheduler view.
    timer->CompleteCancellation();
    EXPECT_EQ(driver->GetRetainCount(), 1);
    EXPECT_EQ(timer->GetRetainCount(), 1);
}

TEST(DriverKitSessionSchedulerTeardownTests, ExternalActionReferenceCannotKeepDriverAliveAfterCancellation) {
    OSSharedPtr<OSObject> driver(new OSObject(), OSNoRetain);
    auto* action = new DriverHoldingAction(driver.get());
    OSSharedPtr<OSAction> externalAction(action, OSRetain);
    OSSharedPtr<DeferredTimer> timer(new DeferredTimer(action), OSNoRetain);
    timer->retain();
    DriverKitSessionScheduler scheduler;
    SessionSchedulerTestPeer::Attach(scheduler, timer.get(), action);
    scheduler.Reset();
    ASSERT_EQ(driver->GetRetainCount(), 2);

    timer->CompleteCancellation();

    EXPECT_EQ(externalAction->GetRetainCount(), 1);
    EXPECT_EQ(driver->GetRetainCount(), 1);
    EXPECT_EQ(timer->GetRetainCount(), 1);
}

class RejectedTimer final : public IOTimerDispatchSource {
public:
    kern_return_t Cancel(void (^)(void)) override { return kIOReturnError; }
};

TEST(DriverKitSessionSchedulerTeardownTests, FailedCancellationReleasesOwnedReferences) {
    OSSharedPtr<OSObject> driver(new OSObject(), OSNoRetain);
    OSSharedPtr<RejectedTimer> timer(new RejectedTimer(), OSNoRetain);
    timer->retain();
    DriverKitSessionScheduler scheduler;
    SessionSchedulerTestPeer::Attach(scheduler, timer.get(), new DriverHoldingAction(driver.get()));
    scheduler.Reset();
    EXPECT_EQ(driver->GetRetainCount(), 1);
    EXPECT_EQ(timer->GetRetainCount(), 1);
}
}

TEST(DriverKitSessionSchedulerTeardownTests, FinalStopWaitsForSourceAndActionCancellation) {
    auto completion = std::make_shared<ASFW::Common::TeardownCompletion>();
    OSSharedPtr<OSObject> driver(new OSObject(), OSNoRetain);
    auto* action = new DriverHoldingAction(driver.get(), true);
    OSSharedPtr<DriverHoldingAction> actionObserver(action, OSRetain);
    OSSharedPtr<DeferredTimer> timer(new DeferredTimer(action), OSNoRetain);
    timer->retain();
    DriverKitSessionScheduler scheduler;
    SessionSchedulerTestPeer::Attach(scheduler, timer.get(), action, completion);
    scheduler.Reset();
    int finalStopCalls = 0;
    completion->FinishWhenDrained([&] { ++finalStopCalls; });
    EXPECT_EQ(finalStopCalls, 0);
    timer->CompleteCancellation();
    EXPECT_EQ(finalStopCalls, 0); // source retired, action completion pending
    actionObserver->CompleteCancellation();
    EXPECT_EQ(finalStopCalls, 1);
    EXPECT_EQ(driver->GetRetainCount(), 1);
    EXPECT_EQ(timer->GetRetainCount(), 1);
    EXPECT_EQ(actionObserver->GetRetainCount(), 1);
}

TEST(DriverKitSessionSchedulerTeardownTests, AlreadyDrainedCancellationFinishesStopImmediately) {
    auto completion = std::make_shared<ASFW::Common::TeardownCompletion>();
    auto done = completion->Begin();
    done(); // provider revocation finished before Stop arrived
    int calls = 0;
    completion->FinishWhenDrained([&] { ++calls; });
    EXPECT_EQ(calls, 1);
}

TEST(DriverKitSessionSchedulerTeardownTests, FinalStopWaitsForAllSourceOwners) {
    auto completion = std::make_shared<ASFW::Common::TeardownCompletion>();
    auto interruptDone = completion->Begin();
    auto timerDone = completion->Begin();
    auto notificationDone = completion->Begin();
    int calls = 0;
    completion->FinishWhenDrained([&] { ++calls; });
    timerDone();
    notificationDone();
    EXPECT_EQ(calls, 0);
    interruptDone();
    EXPECT_EQ(calls, 1);
}

TEST(DriverKitSessionSchedulerTeardownTests, ConcurrentCancellationsRacingWithStopFinishExactlyOnce) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto fence = std::make_shared<ASFW::Common::TeardownCompletion>();
        std::atomic<int> remaining{8};
        std::atomic<int> stopCalls{0};
        std::atomic<int> pendingAtStop{-1};
        std::vector<std::function<void()>> completions;
        for (int i = 0; i < 8; ++i) completions.push_back(fence->Begin());
        std::barrier start(9);
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&, i] {
                start.arrive_and_wait();
                if (i % 2) std::this_thread::yield();
                --remaining;
                completions[i]();
            });
        }
        start.arrive_and_wait();
        fence->FinishWhenDrained([&] {
            pendingAtStop = remaining.load();
            ++stopCalls;
        });
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(stopCalls.load(), 1);
        EXPECT_EQ(pendingAtStop.load(), 0);
    }
}
