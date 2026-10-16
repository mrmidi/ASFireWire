// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierWorkerEntry.hpp"
#include "WorkerStartupOwner.hpp"
namespace Pro40Lab {
// Owner-queue adapter between the signed supervisor's entry and worker startup.
// It establishes no trust: Supervisor supplies fresh terminal evidence as before.
// Keep this adapter and startup owner until collection and capture cleanup finish.
template<class Supervisor> class VerifierStartupEntry final {
    std::shared_ptr<WorkerStartupOwner> startup_;
    VerifierWorkerEntry<Supervisor> entry_;
    bool attempted_=false,released_=false,completed_=false;
public:
    using ChildStart=typename VerifierWorkerEntry<Supervisor>::ChildStart;
    VerifierStartupEntry(std::shared_ptr<Supervisor> supervisor,
        const Pro40Audio::NativeVerifiedSourceConfiguration &configuration,
        std::shared_ptr<WorkerStartupOwner> startup):startup_(std::move(startup)),
        entry_(std::move(supervisor),configuration.request(),Pro40Audio::ControllerReceiptObligation::none){}
    VerifierStartupEntry(const VerifierStartupEntry&)=delete;
    VerifierStartupEntry& operator=(const VerifierStartupEntry&)=delete;
    // This in-process path has no receipt pipe. Capture Mach rights are external
    // and must be independently confirmed below, alongside startup's endpoints.
    [[nodiscard]] ChildStart begin(const Pro40Audio::NativeVerifiedSourceConfiguration &configuration){
        if(attempted_)return ChildStart::consumed;
        attempted_=true;
        if(!startup_ || !startup_->readyToVerify()){
            if(startup_)startup_->requestStop();return ChildStart::refused;
        }
        const auto result=entry_.beginChild(configuration,startup_->retainedChild(),startup_->stopQuery());
        released_=result==ChildStart::released;
        return result;
    }
    // No caller-supplied result or PID. Collection must be terminal for the same
    // retained worker. False preserves obligations; callers may retry after cleanup.
    [[nodiscard]] bool finish(bool captureEndpointsClosed){
        if(!attempted_ || completed_ || !startup_ || !captureEndpointsClosed)return false;
        const auto state=startup_->snapshot();
        if(!state.finished || !state.resourcesComplete || state.startupError || state.setupCleanupError)return false;
        const auto collector=startup_->collector();
        if(!collector || !collector->cleanupComplete())return false;
        if(released_ && !entry_.completeChild(*collector))return false;
        if(!entry_.confirmSenderReaped() || !entry_.confirmSenderEndpointsClosed())return false;
        completed_=true;return true;
    }
    std::optional<Pro40Audio::NativeServiceAttemptResult> snapshot()const{
        return completed_?entry_.snapshot():std::nullopt;
    }
};
}
