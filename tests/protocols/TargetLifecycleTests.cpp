// TargetLifecycleTests — the HBA's target-0 create/destroy edge handling,
// against fake UserCreateTargetForID / UserDestroyTargetForID calls.

#include "ASFWDriver/SCSIController/TargetLifecycle.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using ASFW::Protocols::SBP2::TargetLifecycle;

namespace {

constexpr uint64_t kGuid = 0x00206B4004000000ULL;

// Records the kernel calls in order; results and session state are scriptable.
struct FakeHBA {
    std::vector<std::string> calls;
    bool sessionReady{true};
    kern_return_t createResult{kIOReturnSuccess};
    kern_return_t destroyResult{kIOReturnSuccess};
    std::function<void()> duringCreate;

    TargetLifecycle::Ops Ops() {
        return {
            .sessionReady = [this] { return sessionReady; },
            .destroyTarget =
                [this] {
                    calls.emplace_back("destroy");
                    return destroyResult;
                },
            .createTarget =
                [this] {
                    calls.emplace_back("create");
                    if (duringCreate) {
                        duringCreate();
                    }
                    return createResult;
                },
        };
    }
};

using Calls = std::vector<std::string>;

TEST(TargetLifecycleTests, UpEdgeClearsStaleNodeThenCreates) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());

    lifecycle.OnEdge(kGuid, true);

    EXPECT_EQ((Calls{"destroy", "create"}), hba.calls);
    EXPECT_TRUE(lifecycle.IsTargetAttached());
}

TEST(TargetLifecycleTests, RepeatedUpEdgeKeepsTheTarget) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    lifecycle.OnEdge(kGuid, true);
    hba.calls.clear();

    lifecycle.OnEdge(kGuid, true); // reconnect re-assert

    EXPECT_TRUE(hba.calls.empty());
    EXPECT_TRUE(lifecycle.IsTargetAttached());
}

TEST(TargetLifecycleTests, StaleUpEdgeCreatesNothing) {
    FakeHBA hba;
    hba.sessionReady = false;
    TargetLifecycle lifecycle(hba.Ops());

    lifecycle.OnEdge(kGuid, true);

    EXPECT_TRUE(hba.calls.empty());
    EXPECT_FALSE(lifecycle.IsTargetAttached());
}

TEST(TargetLifecycleTests, DownEdgeDestroysAttachedTarget) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    lifecycle.OnEdge(kGuid, true);
    hba.calls.clear();

    lifecycle.OnEdge(kGuid, false);

    EXPECT_EQ((Calls{"destroy"}), hba.calls);
    EXPECT_FALSE(lifecycle.IsTargetAttached());
}

TEST(TargetLifecycleTests, DownEdgeWithoutTargetDoesNothing) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());

    lifecycle.OnEdge(kGuid, false);

    EXPECT_TRUE(hba.calls.empty());
}

TEST(TargetLifecycleTests, FailedDestroyStillDetaches) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    lifecycle.OnEdge(kGuid, true);
    hba.destroyResult = kIOReturnError;

    lifecycle.OnEdge(kGuid, false);

    EXPECT_FALSE(lifecycle.IsTargetAttached());
}

// The session drops while the create is still blocked in the kernel (the SAM
// probe runs inside UserCreateTargetForID). The down edge queues behind it on
// the serial lifecycle queue; once the create returns it must destroy.
TEST(TargetLifecycleTests, DownEdgeQueuedBehindCreateDestroysAfterIt) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    std::vector<bool> queuedEdges;
    hba.duringCreate = [&] { queuedEdges.push_back(false); };

    lifecycle.OnEdge(kGuid, true);
    for (bool up : queuedEdges) {
        lifecycle.OnEdge(kGuid, up);
    }

    EXPECT_EQ((Calls{"destroy", "create", "destroy"}), hba.calls);
    EXPECT_FALSE(lifecycle.IsTargetAttached());
}

// The create fails (probe aborted by the unplug). No target is recorded, and
// the next login's pre-create destroy clears whatever node the kernel left.
TEST(TargetLifecycleTests, FailedCreateIsClearedByNextLogin) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    hba.createResult = kIOReturnError;
    lifecycle.OnEdge(kGuid, true);
    lifecycle.OnEdge(kGuid, false);
    EXPECT_FALSE(lifecycle.IsTargetAttached());
    hba.calls.clear();

    hba.createResult = kIOReturnSuccess;
    lifecycle.OnEdge(kGuid, true);

    EXPECT_EQ((Calls{"destroy", "create"}), hba.calls);
    EXPECT_TRUE(lifecycle.IsTargetAttached());
}

TEST(TargetLifecycleTests, StoppingGatesBothEdges) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    lifecycle.OnEdge(kGuid, true);
    hba.calls.clear();

    lifecycle.MarkStopping();
    lifecycle.OnEdge(kGuid, false);
    lifecycle.OnEdge(kGuid, true);

    EXPECT_TRUE(hba.calls.empty());
    EXPECT_TRUE(lifecycle.IsStopping());
}

// Stop lands while the create is in flight: the create's result is still
// recorded, but the framework, not an explicit destroy, tears the target down.
TEST(TargetLifecycleTests, StopDuringCreateSkipsLaterDestroy) {
    FakeHBA hba;
    TargetLifecycle lifecycle(hba.Ops());
    hba.duringCreate = [&] { lifecycle.MarkStopping(); };

    lifecycle.OnEdge(kGuid, true);
    lifecycle.OnEdge(kGuid, false);

    EXPECT_EQ((Calls{"destroy", "create"}), hba.calls);
}

} // namespace
