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

constexpr uint64_t kGuid = 0x0090B54003FFFFFFull;
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
// The target had the ORB (ORB_POINTER write acknowledged) and wrote no status
// block before the ORB timer expired.
CommandResult NoStatus() { return MakeResult(kIOReturnTimeout, false, 0x00); }
// The probe's ORB_POINTER write failed or was never acknowledged (FetchAgent):
// the target may not have seen the ORB.
CommandResult PointerWriteFailed() { return MakeResult(-1, false, 0x00); }
CommandResult Warming205() { return MakeResult(0, true, 0x02, 0x02, 0x05, 0x00); }
CommandResult Warming204() { return MakeResult(0, true, 0x02, 0x02, 0x04, 0x01); }
CommandResult PowerOnUA() { return MakeResult(0, true, 0x02, 0x06, 0x29, 0x00); }

struct GateRig {
    FakeTimerScheduler scheduler;
    std::vector<std::function<void(const CommandResult&)>> probes;
    std::vector<std::pair<uint64_t, bool>> notified;
    TargetReadinessGate gate{
        scheduler,
        [this](CommandRequest request, std::function<void(const CommandResult&)> cb) {
            lastCdb = request.cdb;
            probes.push_back(std::move(cb));
        },
        [this](uint64_t guid, bool up) { notified.emplace_back(guid, up); }};
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
    // No status block at all: the target does not answer this probe.
    EXPECT_EQ(TargetReadinessGate::Classify(NoStatus()), Probe::NoAnswer);
    // The ORB never reached the target: a transport failure, paced.
    EXPECT_EQ(TargetReadinessGate::Classify(PointerWriteFailed()), Probe::RetryDelayed);
    // CHECK with undecodable autosense: fail open.
    CommandResult bare = MakeResult(0, true, 0x02, 0x02, 0x04, 0x01);
    bare.senseData.clear();
    EXPECT_EQ(TargetReadinessGate::Classify(bare), Probe::Ready);
}

// --- Gate flow ---------------------------------------------------------------

TEST(TargetReadinessGateTests, ReadyDeviceAnnouncesOnFirstProbe) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    ASSERT_EQ(rig.probes.size(), 1u);
    ASSERT_FALSE(rig.lastCdb.empty());
    EXPECT_EQ(rig.lastCdb[0], 0x00); // TEST UNIT READY
    EXPECT_TRUE(rig.notified.empty());

    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_EQ(rig.notified[0], std::make_pair(kGuid, true));
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
}

TEST(TargetReadinessGateTests, WarmupDelaysAnnouncementUntilReady) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
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
    EXPECT_EQ(rig.notified[0], std::make_pair(kGuid, true));
}

TEST(TargetReadinessGateTests, UnitAttentionReprobesImmediately) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
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
    rig.gate.OnEdge(kGuid, true);
    ASSERT_EQ(rig.probes.size(), 1u);

    rig.gate.OnEdge(kGuid, false);
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_EQ(rig.notified[0], std::make_pair(kGuid, false));

    // The outstanding probe completes after the down edge: stale epoch, no-op.
    rig.CompleteProbe(Good());
    EXPECT_EQ(rig.notified.size(), 1u);
}

TEST(TargetReadinessGateTests, DownEdgeDuringDelayedRetryCancelsTimer) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(Warming205());
    EXPECT_EQ(rig.scheduler.PendingCount(), 1u);

    rig.gate.OnEdge(kGuid, false);
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
    rig.scheduler.Advance(10 * kSecondNs);
    EXPECT_EQ(rig.probes.size(), 1u); // no further probes
}

TEST(TargetReadinessGateTests, ReassertAfterAnnouncementPassesThroughWithoutProbe) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);

    // Reconnect re-assert: login continuous — no probe TURs near a running
    // initiator, immediate pass-through.
    rig.gate.OnEdge(kGuid, true);
    EXPECT_EQ(rig.probes.size(), 1u);
    ASSERT_EQ(rig.notified.size(), 2u);
    EXPECT_TRUE(rig.notified[1].second);
}

TEST(TargetReadinessGateTests, DuplicateUpEdgeWhileProbingStartsNoSecondGate) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.gate.OnEdge(kGuid, true);
    EXPECT_EQ(rig.probes.size(), 1u);
    EXPECT_TRUE(rig.notified.empty());
}

TEST(TargetReadinessGateTests, FreshUpAfterDownRunsANewGate) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(Good());
    rig.gate.OnEdge(kGuid, false);
    ASSERT_EQ(rig.notified.size(), 2u);

    rig.gate.OnEdge(kGuid, true);
    EXPECT_EQ(rig.probes.size(), 2u); // gated again, not passed through
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 3u);
    EXPECT_TRUE(rig.notified[2].second);
}

TEST(TargetReadinessGateTests, NeverReadyFailsOpenAtProbeBudget) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
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
    rig.gate.OnEdge(kGuid, true);
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
    rig.gate.OnEdge(kGuid, true);
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

// --- Targets that never answer TEST UNIT READY --------------------------------

TEST(TargetReadinessGateTests, SilentTargetFailsOpenAfterNoAnswerLimit) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(NoStatus());
    // Re-probed at once: the executor already reset the agent and the LUN.
    EXPECT_EQ(rig.scheduler.PendingCount(), 0u);
    ASSERT_EQ(rig.probes.size(), 2u);
    EXPECT_TRUE(rig.notified.empty());
    EXPECT_FALSE(rig.gate.AttachProbeAnswer().has_value());

    rig.CompleteProbe(NoStatus());
    static_assert(TargetReadinessGate::kNoAnswerLimit == 2);
    ASSERT_EQ(rig.notified.size(), 1u);
    EXPECT_TRUE(rig.notified[0].second);
    EXPECT_EQ(rig.probes.size(), 2u);
    // Present (logged in) but silent on TUR: the attach probe gets GOOD.
    const auto& answer = rig.gate.AttachProbeAnswer();
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->transportStatus, 0);
    EXPECT_TRUE(answer->scsiStatusValid);
    EXPECT_EQ(answer->scsiStatus, 0x00);
    EXPECT_TRUE(answer->senseData.empty());
}

TEST(TargetReadinessGateTests, UnansweredCountIsConsecutive) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(NoStatus());
    ASSERT_EQ(rig.probes.size(), 2u);
    // A real (warming) answer in between resets the count.
    rig.CompleteProbe(Warming204());
    rig.scheduler.Advance(kSecondNs);
    ASSERT_EQ(rig.probes.size(), 3u);
    rig.CompleteProbe(NoStatus());
    EXPECT_TRUE(rig.notified.empty());
    ASSERT_EQ(rig.probes.size(), 4u);
    rig.CompleteProbe(Good());
    ASSERT_EQ(rig.notified.size(), 1u);
    ASSERT_TRUE(rig.gate.AttachProbeAnswer().has_value());
    EXPECT_EQ(rig.gate.AttachProbeAnswer()->scsiStatus, 0x00);
}

TEST(TargetReadinessGateTests, BusResetFailuresStayPacedAndNeverCountAsSilence) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    for (int i = 0; i < 3; ++i) {
        rig.CompleteProbe(MakeResult(-536870165 /* kIOReturnAborted */, false, 0x00));
        EXPECT_TRUE(rig.notified.empty());
        EXPECT_EQ(rig.scheduler.PendingCount(), 1u);
        rig.scheduler.Advance(kSecondNs);
    }
    EXPECT_EQ(rig.probes.size(), 4u);
}

// A failed ORB_POINTER write says nothing about TEST UNIT READY: the target may
// never have received the ORB. It must not count toward kNoAnswerLimit.
TEST(TargetReadinessGateTests, FailedPointerWritesStayPacedAndNeverFailOpen) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    for (uint32_t i = 0; i < TargetReadinessGate::kNoAnswerLimit; ++i) {
        rig.CompleteProbe(PointerWriteFailed());
        EXPECT_TRUE(rig.notified.empty());
        EXPECT_FALSE(rig.gate.AttachProbeAnswer().has_value());
        EXPECT_EQ(rig.scheduler.PendingCount(), 1u);  // paced, not re-probed at once
        EXPECT_EQ(rig.probes.size(), i + 1);
        rig.scheduler.Advance(kSecondNs);
    }
    // Still probing; the next probe went out after the paced delay.
    EXPECT_EQ(rig.probes.size(), TargetReadinessGate::kNoAnswerLimit + 1);
    EXPECT_TRUE(rig.notified.empty());
    EXPECT_FALSE(rig.gate.AttachProbeAnswer().has_value());
}

TEST(TargetReadinessGateTests, DeviceAnswerIsKeptForTheAttachProbe) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    // Operational NOT READY (medium not present): announced, answer replayed.
    rig.CompleteProbe(MakeResult(0, true, 0x02, 0x02, 0x3A, 0x00));
    ASSERT_EQ(rig.notified.size(), 1u);
    const auto& answer = rig.gate.AttachProbeAnswer();
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->scsiStatus, 0x02);
    ASSERT_GE(answer->senseData.size(), 13u);
    EXPECT_EQ(answer->senseData[2], 0x02);
    EXPECT_EQ(answer->senseData[12], 0x3A);
}

TEST(TargetReadinessGateTests, GoodWithOmittedStatusIsKeptAsGood) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(MakeResult(0, false, 0x00));
    ASSERT_TRUE(rig.gate.AttachProbeAnswer().has_value());
    EXPECT_TRUE(rig.gate.AttachProbeAnswer()->scsiStatusValid);
    EXPECT_EQ(rig.gate.AttachProbeAnswer()->scsiStatus, 0x00);
}

TEST(TargetReadinessGateTests, DownEdgeClearsTheAttachProbeAnswer) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    rig.CompleteProbe(Good());
    ASSERT_TRUE(rig.gate.AttachProbeAnswer().has_value());
    rig.gate.OnEdge(kGuid, false);
    EXPECT_FALSE(rig.gate.AttachProbeAnswer().has_value());
    // A fresh gate run starts without an answer until it finishes.
    rig.gate.OnEdge(kGuid, true);
    EXPECT_FALSE(rig.gate.AttachProbeAnswer().has_value());
}

TEST(TargetReadinessGateTests, BudgetExhaustionLeavesNoAttachProbeAnswer) {
    GateRig rig;
    rig.gate.OnEdge(kGuid, true);
    for (uint32_t i = 0; i < TargetReadinessGate::kProbeBudget - 1; ++i) {
        rig.CompleteProbe(Warming205());
        rig.scheduler.Advance(kSecondNs);
    }
    rig.CompleteProbe(Warming205());
    ASSERT_EQ(rig.notified.size(), 1u);
    // Still warming: nothing definite, so the family's own TUR goes to the target.
    EXPECT_FALSE(rig.gate.AttachProbeAnswer().has_value());
}

} // namespace
