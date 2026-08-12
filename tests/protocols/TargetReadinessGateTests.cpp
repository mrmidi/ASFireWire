// TargetReadinessGateTests — the login-up readiness gate (HW finding
// 2026-07-31: creating the SCSI target while the device is still warming up
// strands a childless IOSCSITargetDevice; see TargetReadinessGate.hpp).

#include "ASFWDriver/SCSIController/TargetReadinessGate.hpp"

#include "FakeTimerScheduler.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <utility>
#include <vector>

using ASFW::Protocols::SBP2::TargetReadinessGate;
using ASFW::Protocols::SBP2::SCSI::CommandRequest;
using ASFW::Protocols::SBP2::SCSI::CommandResult;
using ASFW::Testing::FakeTimerScheduler;
using Probe = TargetReadinessGate::Probe;

namespace {

constexpr ASFW::Discovery::DeviceInstanceId kInstance{42};
constexpr uint64_t kSecondNs = 1'000'000'000ull;

CommandResult MakeResult(int transport, bool statusValid, uint8_t status,
                         uint8_t key = 0, uint8_t asc = 0, uint8_t ascq = 0) {
    CommandResult result{};
    result.transportStatus = transport;
    result.scsiStatusValid = statusValid;
    result.scsiStatus = status;
    if (statusValid && status == 0x02) {
        result.senseData.assign(18, 0);
        result.senseData[0] = 0x70;
        result.senseData[2] = key;
        result.senseData[12] = asc;
        result.senseData[13] = ascq;
    }
    return result;
}

CommandResult Good() { return MakeResult(0, true, 0x00); }
CommandResult Warming205() { return MakeResult(0, true, 0x02, 0x02, 0x05, 0x00); }
CommandResult Warming204() { return MakeResult(0, true, 0x02, 0x02, 0x04, 0x01); }
CommandResult PowerOnUA() { return MakeResult(0, true, 0x02, 0x06, 0x29, 0x00); }

struct GateRig {
    FakeTimerScheduler scheduler;
    std::vector<std::function<void(const CommandResult&)>> probes;
    std::vector<std::pair<ASFW::Discovery::DeviceInstanceId, bool>> notified;
    TargetReadinessGate gate{
        scheduler,
        [this](CommandRequest request, std::function<void(const CommandResult&)> cb) {
            lastCdb = request.cdb;
            probes.push_back(std::move(cb));
        },
        [this](ASFW::Discovery::DeviceInstanceId instanceId, bool up) {
            notified.emplace_back(instanceId, up);
        }};
    std::vector<uint8_t> lastCdb;

    // Completes the most recent probe.
    void CompleteProbe(const CommandResult& result) {
        ASSERT_FALSE(probes.empty());
        auto cb = std::move(probes.back());
        cb(result);
    }
};

// --- Classify table ---------------------------------------------------------

TEST(TargetReadinessGateTests, ClassifyTable) {
    EXPECT_EQ(TargetReadinessGate::Classify(Good()), Probe::Ready);
    // GOOD with the status block's command-set bytes omitted.
    EXPECT_EQ(TargetReadinessGate::Classify(MakeResult(0, false, 0x00)), Probe::Ready);
    // Transport failure: paced retry.
    EXPECT_EQ(TargetReadinessGate::Classify(MakeResult(-536870165, false, 0x00)),
              Probe::RetryDelayed);
    // BUSY: paced retry.
    EXPECT_EQ(TargetReadinessGate::Classify(MakeResult(0, true, 0x08)), Probe::RetryDelayed);
    // Unit attention: consume, immediate re-probe.
    EXPECT_EQ(TargetReadinessGate::Classify(PowerOnUA()), Probe::RetryNow);
    // Warm-up senses observed on HW.
    EXPECT_EQ(TargetReadinessGate::Classify(Warming204()), Probe::RetryDelayed);
    EXPECT_EQ(TargetReadinessGate::Classify(Warming205()), Probe::RetryDelayed);
    // Operational NOT READY (2/3A medium not present): device is up — announce.
    EXPECT_EQ(TargetReadinessGate::Classify(MakeResult(0, true, 0x02, 0x02, 0x3A, 0x00)),
              Probe::Ready);
    // Other CHECKs (e.g. 5/24 illegal request) do not block announcement.
    EXPECT_EQ(TargetReadinessGate::Classify(MakeResult(0, true, 0x02, 0x05, 0x24, 0x00)),
              Probe::Ready);
    // CHECK with undecodable autosense: fail open.
    CommandResult bare = MakeResult(0, true, 0x02, 0x02, 0x04, 0x01);
    bare.senseData.clear();
    EXPECT_EQ(TargetReadinessGate::Classify(bare), Probe::Ready);
}

// --- Gate flow ---------------------------------------------------------------

TEST(TargetReadinessGateTests, ReadyDeviceAnnouncesOnFirstProbe) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    ASSERT_EQ(rig.probes.size(), 1u);
    ASSERT_FALSE(rig.lastCdb.empty());
    EXPECT_EQ(rig.lastCdb[0], 0x00); // TEST UNIT READY
    EXPECT_TRUE(rig.notified.empty());

    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_EQ(rig.notified[0], std::make_pair(kInstance, true));
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
}

TEST(TargetReadinessGateTests, WarmupDelaysAnnouncementUntilReady) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.CompleteProbe(Warming205());
    EXPECT_TRUE(rig.notified.empty());
    EXPECT_EQ(rig.scheduler.PendingCount(), 1u);

    rig.scheduler.Advance(kSecondNs);
    ASSERT_EQ(rig.probes.size(), 2u);
    rig.CompleteProbe(Warming204());
    rig.scheduler.Advance(kSecondNs);
    ASSERT_EQ(rig.probes.size(), 3u);

    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_EQ(rig.notified[0], std::make_pair(kInstance, true));
}

TEST(TargetReadinessGateTests, UnitAttentionReprobesImmediately) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.CompleteProbe(PowerOnUA());
    // No timer: the UA was consumed and the follow-up probe is already out.
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
    ASSERT_EQ(rig.probes.size(), 2u);
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_TRUE(rig.notified[0].second);
}

TEST(TargetReadinessGateTests, DownEdgeCancelsGateAndSuppressesLateProbe) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    ASSERT_EQ(rig.probes.size(), 1u);

    rig.gate.OnEdge(kInstance, false);
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_EQ(rig.notified[0], std::make_pair(kInstance, false));

    // The outstanding probe completes after the down edge: stale epoch, no-op.
    rig.CompleteProbe(Good());
    EXPECT_EQ(rig.notified.size(), 1u);
}

TEST(TargetReadinessGateTests, DownEdgeDuringDelayedRetryCancelsTimer) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.CompleteProbe(Warming205());
    EXPECT_EQ(rig.scheduler.PendingCount(), 1u);

    rig.gate.OnEdge(kInstance, false);
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
    rig.scheduler.Advance(10 * kSecondNs);
    EXPECT_EQ(rig.probes.size(), 1u); // no further probes
}

TEST(TargetReadinessGateTests, ReassertAfterAnnouncementPassesThroughWithoutProbe) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);

    // Reconnect re-assert: login continuous — no probe TURs near a running
    // initiator, immediate pass-through.
    rig.gate.OnEdge(kInstance, true);
    EXPECT_EQ(rig.probes.size(), 1u);
    ASSERT_EQ(rig.notified.size(), 2u);
    EXPECT_TRUE(rig.notified[1].second);
}

TEST(TargetReadinessGateTests, DuplicateUpEdgeWhileProbingStartsNoSecondGate) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.gate.OnEdge(kInstance, true);
    EXPECT_EQ(rig.probes.size(), 1u);
    EXPECT_TRUE(rig.notified.empty());
}

TEST(TargetReadinessGateTests, FreshUpAfterDownRunsANewGate) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.CompleteProbe(Good());
    rig.gate.OnEdge(kInstance, false);
    ASSERT_EQ(rig.notified.size(), 2u);

    rig.gate.OnEdge(kInstance, true);
    EXPECT_EQ(rig.probes.size(), 2u); // gated again, not passed through
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 3u);
    EXPECT_TRUE(rig.notified[2].second);
}

TEST(TargetReadinessGateTests, NeverReadyFailsOpenAtProbeBudget) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    for (uint32_t i = 0; i < TargetReadinessGate::kProbeBudget - 1; ++i) {
        rig.CompleteProbe(Warming205());
        rig.scheduler.Advance(kSecondNs);
    }
    ASSERT_EQ(rig.probes.size(), TargetReadinessGate::kProbeBudget);
    EXPECT_TRUE(rig.notified.empty());

    rig.CompleteProbe(Warming205());
    ASSERT_EQ(rig.notified.size(), 1u); // announced anyway (fail-open)
    EXPECT_TRUE(rig.notified[0].second);
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
}

TEST(TargetReadinessGateTests, CancelSuppressesEverything) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    rig.CompleteProbe(Warming205());
    EXPECT_EQ(rig.scheduler.PendingCount(), 1u);

    rig.gate.Cancel();
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
    rig.scheduler.Advance(10 * kSecondNs);
    EXPECT_EQ(rig.probes.size(), 1u);
    EXPECT_TRUE(rig.notified.empty());
}

TEST(TargetReadinessGateTests, TransportErrorRetriesPacedThenAnnouncesWhenReady) {
    GateRig rig;
    rig.gate.OnEdge(kInstance, true);
    // Bus-reset suspension window: the probe fails at the transport level.
    rig.CompleteProbe(MakeResult(-536870165, false, 0x00));
    EXPECT_TRUE(rig.notified.empty());
    EXPECT_EQ(rig.scheduler.PendingCount(), 1u);

    rig.scheduler.Advance(kSecondNs);
    ASSERT_EQ(rig.probes.size(), 2u);
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_TRUE(rig.notified[0].second);
}

} // namespace
