// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeControllerCleanupTracker.hpp"
namespace Pro40Audio {
// Single control-thread orchestration, not a provisioner or resource owner.
// Construct before acquisition with locally trusted request/obligations. The
// external owner retains XPC/process/endpoint resources through unresolved cleanup.
class NativePreSessionController {
public:
    enum class Phase { awaitingRequest, awaitingEvidence, ready, executing, returned, blocked };
private:
    std::optional<NativeIdentityRequest> expected_;
    NativeControllerCleanupTracker cleanup_;
    std::optional<NativeServiceIdentityDecision> decision_;
    std::optional<ServiceEntryResult> entry_;
    Phase phase_=Phase::awaitingRequest;
    bool mayConfirm()const noexcept{return phase_==Phase::returned||phase_==Phase::blocked;}
public:
    explicit NativePreSessionController(ControllerSenderObligation sender,
        ControllerReceiptObligation receipt=ControllerReceiptObligation::required)noexcept:cleanup_(sender,receipt){}
    NativePreSessionController(NativeIdentityRequest expected,ControllerSenderObligation sender,
        ControllerReceiptObligation receipt=ControllerReceiptObligation::required)noexcept:
        NativePreSessionController(sender,receipt){(void)bindRequest(expected);}
    NativePreSessionController(const NativePreSessionController &)=delete;
    NativePreSessionController &operator=(const NativePreSessionController &)=delete;
    Phase phase()const noexcept{return phase_;}
    // Original kernel audit token is available only after acquiring the peer.
    // Declare obligations first, then bind the full request once before verification.
    [[nodiscard]] bool bindRequest(NativeIdentityRequest expected)noexcept{
        if(phase_!=Phase::awaitingRequest)return false;
        if(!expected.valid()){phase_=Phase::blocked;return false;}
        expected_=expected;phase_=Phase::awaitingEvidence;return true;
    }
    [[nodiscard]] bool seal(NativeServiceIdentityOutcome outcome)noexcept{
        if(phase_!=Phase::awaitingEvidence)return false;
        if(!(outcome.expected==*expected_)){phase_=Phase::blocked;return false;}
        decision_.emplace(std::move(outcome));phase_=Phase::ready;return true;
    }
    // Stop context must outlive this call. The continuation receives the SAME
    // query for runner stop handling after gate approval.
    [[nodiscard]] std::optional<ServiceEntryStatus> begin(Pro40::StopQuery stop){
        if(phase_!=Phase::ready){
            if(phase_==Phase::awaitingRequest||phase_==Phase::awaitingEvidence)phase_=Phase::blocked;
            return std::nullopt;
        }
        phase_=Phase::executing; // Before calling the live stop query.
        NativeServiceEntryGate gate(*decision_);
        const auto status=gate.admit(stop);
        if(status!=ServiceEntryStatus::entered){entry_=ServiceEntryResult{status,{}};phase_=Phase::returned;}
        return status;
    }
    // Owner queue only. Absence means collection failed, never a successful
    // runner result. No terminal snapshot or cleanup confirmation while executing.
    [[nodiscard]] bool complete(std::optional<Pro40::NativeSessionResult> result){
        if(phase_!=Phase::executing)return false;
        entry_=ServiceEntryResult{ServiceEntryStatus::entered,std::move(result)};
        phase_=Phase::returned;return true;
    }
    std::optional<ServiceEntryResult> entryResult()const{return entry_;}
    template<class Continue>
    [[nodiscard]] std::optional<ServiceEntryResult> enter(Pro40::StopQuery stop,Continue &&continuation){
        const auto status=begin(stop);if(!status)return std::nullopt;
        if(*status==ServiceEntryStatus::entered)(void)complete(std::forward<Continue>(continuation)(stop));
        return entry_;
    }
    // Pre-entry acquisition/verification failure: retain cleanup obligations,
    // prohibit later entry, and do not invent a runner result.
    [[nodiscard]] bool abortBeforeEntry()noexcept{
        if(phase_!=Phase::awaitingRequest&&phase_!=Phase::awaitingEvidence&&phase_!=Phase::ready)return false;
        phase_=Phase::blocked;return true;
    }
    [[nodiscard]] bool confirmReceiptClosed()noexcept{return mayConfirm()&&cleanup_.confirmReceiptClosed();}
    [[nodiscard]] bool confirmSenderReaped()noexcept{return mayConfirm()&&cleanup_.confirmSenderReaped();}
    [[nodiscard]] bool confirmSenderEndpointsClosed()noexcept{return mayConfirm()&&cleanup_.confirmSenderEndpointsClosed();}
    ServiceControllerCleanup controllerCleanup()const noexcept{return cleanup_.snapshot();}
    std::optional<NativeServiceAttemptResult> snapshot()const noexcept{
        if(phase_!=Phase::returned)return std::nullopt;
        return std::optional<NativeServiceAttemptResult>(std::in_place,*decision_,*entry_,cleanup_.snapshot());
    }
};
}
