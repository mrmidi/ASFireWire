#pragma once

// TargetReadinessGate — holds back the HBA's login-up edge until the device
// answers TEST UNIT READY without a warm-up sense.
//
// Why (HW finding, 2026-07-31, Nikon LS-9000 on Tahoe): the login-up edge
// fires ~30 ms after device power-on, so the SCSI target was created — and the
// SAM family probe ran — against a device still warming up (TUR → NOT READY
// 2/04 "becoming ready" / 2/05 "no response to selection"). The family then
// publishes the IOSCSITargetDevice with the INQUIRY identity but never
// publishes the LUN-0 nub and never re-probes, leaving the target permanently
// invisible to SCSITaskLib apps. Gating the up-edge on readiness means the
// probe always runs against a ready device — the configuration HW-proven to
// publish the full node chain.
//
// Policy: fail-open. Only a positively "still warming" answer delays the
// announcement; anything else (including NOT READY for operational reasons
// like 2/3A medium not present) announces immediately — a wrongly-delayed
// target is worse than the pre-gate behavior, which the budget-exhausted path
// at worst reproduces. Unit attentions are consumed with an immediate
// re-probe; warm-up senses re-probe after a delay.
//
// A probe the target accepts but never answers (its ORB_POINTER write was
// acknowledged, then the ORB timer expired with no status block: FetchAgent
// reports kIOReturnTimeout; the executor has already reset the agent and the
// LUN) is not a warm-up answer. SPC requires TEST UNIT READY, but some SBP-2
// targets do not implement it and stay silent instead of returning CHECK
// CONDITION (HW 2026-11-06, Leaf Valeo 11: INQUIRY answered in ~2 ms, TUR and
// REPORT LUNS never). After kNoAnswerLimit consecutive unanswered probes the
// gate announces (fail-open, as above). Paced retries stay for transport
// failures that are not the target's answer: an ORB_POINTER write that failed
// or was never acknowledged (the target may not have seen the ORB), bus-reset
// suspension, abort.
//
// The gate also records what it learned, for the SCSI family's attach probe
// (AttachProbeAnswer): the target's own answer to the last probe, or GOOD when
// the target does not answer TEST UNIT READY at all. The HBA answers the
// family's attach-probe TUR from it instead of sending it again.
//
// Reconnect re-asserts pass straight through: the login is continuous, the
// target is attached, and probe TURs must never consume unit attentions from
// under a running initiator.
//
// Threading: single-queue component (the driver Default queue) — OnEdge,
// submit completions, and timer callbacks all run there, like the rest of the
// SBP-2 control plane. Cancel() must run before the scheduler dies (bridge
// Shutdown precedes the scheduler reset in ServiceContext::Reset).

#include "../Protocols/SBP2/SCSICommandSet.hpp"
#include "../Scheduling/ITimerScheduler.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace ASFW::Protocols::SBP2 {

class TargetReadinessGate {
public:
    enum class Probe : uint8_t { Ready, RetryNow, RetryDelayed, NoAnswer };

    using SubmitFn = std::function<void(SCSI::CommandRequest,
                                        std::function<void(const SCSI::CommandResult&)>)>;
    using NotifyFn = std::function<void(uint64_t guid, bool loggedIn)>;

    // Probe budget caps the fail-open wait at ~kProbeBudget × kRetryDelayNs
    // (immediate UA re-probes share the budget). HW: the LS-9000 answered
    // ready after 72 probes (~71 s warm-up), so 90 was too tight a margin —
    // 150 covers a slow lamp without stranding a broken device forever.
    static constexpr uint32_t kProbeBudget = 150;
    static constexpr uint64_t kRetryDelayNs = 1'000'000'000ULL;
    // Consecutive unanswered probes (no status block) before failing open; a
    // single lost status block does not decide it.
    static constexpr uint32_t kNoAnswerLimit = 2;

    TargetReadinessGate(Scheduling::ITimerScheduler& scheduler,
                        SubmitFn submit,
                        NotifyFn notify) noexcept;

    // Feed every login edge through here. notify_ fires with the same edge —
    // immediately for down edges and re-asserts, gated for a fresh up edge.
    void OnEdge(uint64_t guid, bool loggedIn);

    // Invalidate outstanding probe/timer callbacks (bridge Shutdown). After
    // this returns, no notify_ fires until the next OnEdge.
    void Cancel();

    // Sense classification table (pure; host-tested).
    [[nodiscard]] static Probe Classify(const SCSI::CommandResult& result) noexcept;

    // The answer for the SCSI family's attach-probe TEST UNIT READY, learned by
    // the gate run that announced the current login: the target's own answer to
    // the last probe (status + autosense), or GOOD when the target did not
    // answer TUR at all. Empty while probing, after a down edge, and when the
    // gate failed open at the probe budget without a definite answer.
    [[nodiscard]] const std::optional<SCSI::CommandResult>& AttachProbeAnswer() const noexcept {
        return attachProbeAnswer_;
    }

private:
    void ProbeOnce(uint64_t guid, uint64_t epoch);
    void OnProbeResult(uint64_t guid, uint64_t epoch, const SCSI::CommandResult& result);
    void Finish(uint64_t guid, const char* reason,
                std::optional<SCSI::CommandResult> attachProbeAnswer);
    void CancelTimer() noexcept;

    Scheduling::ITimerScheduler& scheduler_;
    SubmitFn submit_;
    NotifyFn notify_;

    bool notifiedUp_{false};
    bool gateActive_{false};
    uint64_t epoch_{0};
    uint32_t attempts_{0};
    uint32_t unansweredInARow_{0};
    std::optional<SCSI::CommandResult> attachProbeAnswer_;
    Scheduling::TimerToken timer_{Scheduling::kInvalidTimerToken};
};

} // namespace ASFW::Protocols::SBP2
