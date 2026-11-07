// TargetReadinessGate — see TargetReadinessGate.hpp.

#include "TargetReadinessGate.hpp"

#include "../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Protocols::SBP2 {

namespace {
constexpr uint8_t kStatusCheckCondition = 0x02;
constexpr uint8_t kStatusBusy = 0x08;
constexpr uint8_t kSenseKeyNotReady = 0x02;
constexpr uint8_t kSenseKeyUnitAttention = 0x06;
constexpr uint8_t kAscBecomingReady = 0x04;
constexpr uint8_t kAscNoResponseToSelection = 0x05;
} // namespace

TargetReadinessGate::TargetReadinessGate(Scheduling::ITimerScheduler& scheduler,
                                         SubmitFn submit,
                                         NotifyFn notify) noexcept
    : scheduler_(scheduler)
    , submit_(std::move(submit))
    , notify_(std::move(notify)) {}

void TargetReadinessGate::OnEdge(uint64_t guid, bool loggedIn) {
    if (!loggedIn) {
        ++epoch_; // invalidate outstanding probe + timer callbacks
        CancelTimer();
        gateActive_ = false;
        notifiedUp_ = false;
        attachProbeAnswer_.reset();
        notify_(guid, false);
        return;
    }
    if (notifiedUp_) {
        // Reconnect re-assert: login continuous, target attached — pass
        // through without probes (see header).
        notify_(guid, true);
        return;
    }
    if (gateActive_) {
        return; // duplicate up edge while probing — one gate is enough
    }
    gateActive_ = true;
    attempts_ = 0;
    unansweredInARow_ = 0;
    attachProbeAnswer_.reset();
    ProbeOnce(guid, ++epoch_);
}

void TargetReadinessGate::Cancel() {
    ++epoch_;
    CancelTimer();
    gateActive_ = false;
}

void TargetReadinessGate::ProbeOnce(uint64_t guid, uint64_t epoch) {
    ++attempts_;
    // The completion callback may outlive this gate inside the bridge's task
    // queue, but it only ever runs while the owning bridge is alive (the
    // bridge's Shutdown drains it after Cancel() has bumped epoch_), so the
    // raw `this` capture cannot be dereferenced after free.
    submit_(SCSI::BuildTestUnitReadyRequest(),
            [this, guid, epoch](const SCSI::CommandResult& result) {
                OnProbeResult(guid, epoch, result);
            });
}

void TargetReadinessGate::OnProbeResult(uint64_t guid, uint64_t epoch,
                                        const SCSI::CommandResult& result) {
    if (epoch != epoch_ || !gateActive_) {
        return; // superseded by a later edge or Cancel()
    }
    const Probe probe = Classify(result);
    if (probe == Probe::Ready) {
        // Keep the target's own answer for the family's attach-probe TUR.
        SCSI::CommandResult answer{};
        answer.scsiStatusValid = true;
        answer.scsiStatus = result.scsiStatusValid ? result.scsiStatus : 0x00;
        answer.senseData = result.senseData;
        Finish(guid, "device ready", std::move(answer));
        return;
    }
    if (probe == Probe::NoAnswer) {
        if (++unansweredInARow_ >= kNoAnswerLimit) {
            // The target does not answer TEST UNIT READY; the login is up, so
            // it is present. The family's attach probe gets GOOD.
            SCSI::CommandResult good{};
            good.scsiStatusValid = true;
            good.scsiStatus = 0x00;
            Finish(guid, "no status for TEST UNIT READY", std::move(good));
            return;
        }
    } else {
        unansweredInARow_ = 0;
    }
    if (attempts_ >= kProbeBudget) {
        Finish(guid, "probe budget exhausted — announcing anyway", std::nullopt);
        return;
    }
    if (probe == Probe::RetryNow || probe == Probe::NoAnswer) {
        // NoAnswer: the executor already reset the agent and the LUN before
        // completing the probe, so the next one can go out at once.
        ProbeOnce(guid, epoch);
        return;
    }
    timer_ = scheduler_.ScheduleAfter(kRetryDelayNs, [this, guid, epoch]() {
        timer_ = Scheduling::kInvalidTimerToken;
        if (epoch == epoch_ && gateActive_) {
            ProbeOnce(guid, epoch);
        }
    });
}

void TargetReadinessGate::Finish(uint64_t guid, const char* reason,
                                 std::optional<SCSI::CommandResult> attachProbeAnswer) {
    gateActive_ = false;
    notifiedUp_ = true;
    attachProbeAnswer_ = std::move(attachProbeAnswer);
    ASFW_LOG(Controller,
             "[SBP2Bridge] readiness gate: %{public}s after %u probe(s) — announcing target",
             reason, attempts_);
    notify_(guid, true);
}

void TargetReadinessGate::CancelTimer() noexcept {
    if (timer_ != Scheduling::kInvalidTimerToken) {
        scheduler_.Cancel(timer_);
        timer_ = Scheduling::kInvalidTimerToken;
    }
}

TargetReadinessGate::Probe
TargetReadinessGate::Classify(const SCSI::CommandResult& result) noexcept {
    if (result.transportStatus == kIOReturnTimeout) {
        // The target had the probe's ORB (its ORB_POINTER write was
        // acknowledged) and wrote no status block before the ORB timer
        // expired. A write that failed or was never acknowledged is -1 and
        // stays on the paced path below.
        return Probe::NoAnswer;
    }
    if (result.transportStatus != 0) {
        // Transport failure (bus-reset suspension window, abort): no sense
        // evidence either way — retry paced; a terminal edge or the budget
        // ends the loop.
        return Probe::RetryDelayed;
    }
    if (!result.scsiStatusValid) {
        return Probe::Ready; // GOOD with status omitted (SBP-2 targets may skip it)
    }
    if (result.scsiStatus == kStatusBusy) {
        return Probe::RetryDelayed;
    }
    if (result.scsiStatus != kStatusCheckCondition) {
        return Probe::Ready;
    }
    if (result.senseData.size() < 13) {
        return Probe::Ready; // undecodable autosense — fail open
    }
    const uint8_t key = result.senseData[2] & 0x0F;
    if (key == kSenseKeyUnitAttention) {
        // UA consumed (e.g. 6/29 power-on) — re-probe immediately for the
        // state behind it.
        return Probe::RetryNow;
    }
    const uint8_t asc = result.senseData[12];
    if (key == kSenseKeyNotReady &&
        (asc == kAscBecomingReady || asc == kAscNoResponseToSelection)) {
        return Probe::RetryDelayed; // genuinely warming up (HW: 2/04/01 and 2/05/00)
    }
    // Any other CHECK — including operational NOT READY like 2/3A medium not
    // present — means the device is up; announce and let the initiator see the
    // real state.
    return Probe::Ready;
}

} // namespace ASFW::Protocols::SBP2
