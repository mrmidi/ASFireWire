// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativePreSessionController.hpp"
#include "../NativeVerifiedRunnerContinuation.hpp"
#include "../NativeVerifiedSourceConfiguration.hpp"
#include "../worker/WorkerResultCollector.hpp"
#include <bsm/libbsm.h>
#include <memory>
namespace Pro40Lab {
// Owner-queue composition only. Refresh terminal evidence at consumption rather
// than sealing it before an asynchronous handoff. No device lease or IO here.
// The continuation must retain the original peer and observe stop during running.
template<class Supervisor>
class VerifierWorkerEntry final {
    std::shared_ptr<Supervisor> owner_;
    Pro40Audio::NativeIdentityRequest expected_;
    Pro40Audio::NativePreSessionController controller_;
    bool consumed_=false;
    std::shared_ptr<Pro40::WorkerChildOwner> child_;
    bool prepare(Pro40::StopQuery stop){
        using namespace Pro40Audio;
        consumed_=true;
        if(!owner_ || !expected_.valid()){
            (void)controller_.abortBeforeEntry();return false;
        }
        auto outcome=owner_->identityOutcome(stop.requested());
        if(!outcome || !(outcome->expected==expected_)){
            (void)controller_.abortBeforeEntry();return false;
        }
        return controller_.seal(std::move(*outcome));
    }
public:
    VerifierWorkerEntry(std::shared_ptr<Supervisor> owner,Pro40Audio::NativeIdentityRequest expected,
        Pro40Audio::ControllerReceiptObligation receipt=Pro40Audio::ControllerReceiptObligation::required):
        owner_(std::move(owner)),expected_(expected),
        controller_(expected,Pro40Audio::ControllerSenderObligation::required,receipt){}
    VerifierWorkerEntry(const VerifierWorkerEntry&)=delete;
    VerifierWorkerEntry& operator=(const VerifierWorkerEntry&)=delete;
    template<class Continue>
    [[nodiscard]] Pro40Audio::ServiceEntryResult enter(Pro40::StopQuery stop,Continue&& continuation){
        using namespace Pro40Audio;
        if(consumed_)return {ServiceEntryStatus::consumed,std::nullopt};
        if(!prepare(stop))return {ServiceEntryStatus::denied,std::nullopt};
        // Same live query reaches both final gate and continuation/runner.
        auto result=controller_.enter(stop,std::forward<Continue>(continuation));
        return result?*result:ServiceEntryResult{ServiceEntryStatus::denied,std::nullopt};
    }
    // Trusted owner observations only: these methods do not close or reap anything.
    [[nodiscard]] bool confirmReceiptClosed(){return controller_.confirmReceiptClosed();}
    [[nodiscard]] bool confirmSenderReaped(){return controller_.confirmSenderReaped();}
    [[nodiscard]] bool confirmSenderEndpointsClosed(){return controller_.confirmSenderEndpointsClosed();}
    std::optional<Pro40Audio::NativeServiceAttemptResult> snapshot()const{return controller_.snapshot();}
    // Retained through result collection AND later cleanup observation. Keep this
    // entry (or another shared owner) alive while the child remains unresolved.
    std::shared_ptr<Pro40::WorkerChildOwner> retainedChild()const noexcept{return child_;}
    enum class ChildStart {released, refused, consumed};
    // Owner queue only. Arm retained collection BEFORE this call. This returns
    // after admission, not after running; it never represents an audio result.
    [[nodiscard]] ChildStart beginChild(
        const Pro40Audio::NativeVerifiedSourceConfiguration &configuration,
        std::shared_ptr<Pro40::WorkerChildOwner> child,Pro40::StopQuery stop){
        using namespace Pro40Audio;
        if(consumed_)return ChildStart::consumed;
        child_=std::move(child);
        auto refuse=[&]{if(child_)child_->cancel();return ChildStart::refused;};
        if(!prepare(stop))return refuse();
        const auto status=controller_.begin(stop);
        if(!status || *status!=ServiceEntryStatus::entered)return refuse();
        const auto request=configuration.request();
        if(!(request==expected_) || request.role!=1 || !child_ ||
           child_->child().pid()!=audit_token_to_pid(request.token)){
            (void)controller_.complete(Pro40::NativeSessionResult{64,{}});return refuse();
        }
        const auto sent=child_->release(request.requestID,stop);
        if(sent!=Pro40::WorkerChildOwner::Release::sent){
            (void)controller_.complete(Pro40::NativeSessionResult{stop.requested()?2:
                sent==Pro40::WorkerChildOwner::Release::ioError?74:64,{}});
            return refuse();
        }
        return ChildStart::released;
    }
    // Consume only the retained worker's finalized collector. Timeout or framing
    // failure may have no result: keep that absence, never manufacture success.
    [[nodiscard]] bool completeChild(const Pro40::WorkerResultCollector &collector){
        if(!child_ || collector.retainedChild()!=child_ || !collector.finished() ||
           !collector.cleanupComplete())return false;
        return controller_.complete(collector.result());
    }
    // Synchronous adapter used by existing diagnostics. Admission shares the
    // same one-use path; scheduled callers use beginChild/completeChild instead.
    template<class Collect>
    [[nodiscard]] Pro40Audio::ServiceEntryResult enterChild(
        const Pro40Audio::NativeVerifiedSourceConfiguration &configuration,
        std::shared_ptr<Pro40::WorkerChildOwner> child,Pro40::StopQuery stop,Collect&& collect){
        using namespace Pro40Audio;
        const auto started=beginChild(configuration,std::move(child),stop);
        if(started==ChildStart::consumed)return {ServiceEntryStatus::consumed,{}};
        if(started==ChildStart::released)
            (void)controller_.complete(std::forward<Collect>(collect)(*child_,stop));
        if(child_)child_->cancel();
        return controller_.entryResult().value_or(ServiceEntryResult{ServiceEntryStatus::denied,{}});
    }
    template<class Runner>
    [[nodiscard]] Pro40Audio::ServiceEntryResult enterRunner(
        const Pro40Audio::NativeVerifiedSourceConfiguration &configuration,Pro40::StopQuery stop,Runner&& runner){
        const auto request=configuration.request();
        return enterRunner(configuration.options(),request.epoch,stop,[&](const Pro40::NativeSessionOptions &options){
            // Bind the complete locally frozen request, not merely GUID/epoch.
            if(!(request==expected_))return Pro40::NativeSessionResult{64,{}};
            return std::forward<Runner>(runner)(options);
        });
    }
    template<class Runner>
    [[nodiscard]] Pro40Audio::ServiceEntryResult enterRunner(Pro40::NativeSessionOptions options,
        uint64_t sessionEpoch,Pro40::StopQuery stop,Runner&& runner){
        Pro40Audio::NativeVerifiedRunnerContinuation continuation(expected_,std::move(options),sessionEpoch);
        return enter(stop,[&](Pro40::StopQuery live){return continuation.run(live,std::forward<Runner>(runner));});
    }
};
}
