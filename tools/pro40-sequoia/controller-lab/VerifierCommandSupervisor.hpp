// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControllerSessionBinding.hpp"
#include <atomic>
namespace Pro40Lab {
// Retains startup/session resources until terminal report AND local cleanup.
// Only requestStop is callable off queue. A missing callback/finalizer remains
// pending; a host command timeout must not describe it as cleaned up.
template<class Resources>
class VerifierCommandSupervisor final:public std::enable_shared_from_this<VerifierCommandSupervisor<Resources>> {
public:
    using Binding=ControllerSessionBinding<Resources>;
    using Report=std::function<void(NSDictionary *)>;
    using Startup=std::function<Binding(Report,std::function<bool()>)>;
private:
    dispatch_queue_t queue_;
    dispatch_source_t timer_=nil;
    std::shared_ptr<Resources> resources_;
    Binding binding_;
    Report report_;
    std::function<uint64_t()> now_;
    std::atomic<bool> stop_{false};
    bool consumed_=false,finished_=false;
    NSDictionary *terminal_=nil;
    std::optional<Pro40Audio::NativeServiceIdentityOutcome> identityOutcome_;
    uint64_t lastIdentityRead_=0;
    bool identityClockInvalid_=false;
    VerifierCommandSupervisor(dispatch_queue_t q,std::shared_ptr<Resources> r,Report report,std::function<uint64_t()> now):
        queue_(q),resources_(std::move(r)),report_(std::move(report)),now_(std::move(now)){}
    void blocked(NSString *phase){
        terminal_=@{@"mode":@"verifier-command",@"phase":phase,@"registration_reconciled":@NO,
            @"helper_clean_exit_proven":@NO,@"callback_pending":@NO,@"audio_admitted":@NO};
    }
    void tick(){
        dispatch_assert_queue(queue_);if(finished_)return;
        if(stop_.load() && binding_.session)binding_.session->requestStop();
        if(!terminal_)return;
        if(binding_.localCleanupComplete && !binding_.localCleanupComplete())return;
        NSMutableDictionary *final=[terminal_ mutableCopy];
        final[@"supervision_finished"]=@YES;final[@"local_cleanup_complete"]=@YES;
        if(binding_.identityOutcome)identityOutcome_=binding_.identityOutcome(now_(),stop_.load());
        finished_=true;binding_={};resources_.reset();
        if(timer_){dispatch_source_cancel(timer_);timer_=nil;}
        report_(final);
    }
    void received(NSDictionary *report){
        dispatch_assert_queue(queue_);if(finished_)return;
        if([report[@"supervision_finished"] boolValue])terminal_=[report copy];
        else report_(report);
    }
public:
    // Owner queue only. Refresh time/stop at consumption; terminal cleanup proof
    // stays immutable. A saved result is not indefinite admission permission.
    std::optional<Pro40Audio::NativeServiceIdentityOutcome> identityOutcome(bool commandStopped=false){
        dispatch_assert_queue(queue_);
        // The command's atomic stop can precede queued requestStop delivery.
        // Latch it at consumption; a later read cannot reopen permission.
        if(commandStopped)stop_.store(true);
        auto outcome=identityOutcome_;
        if(outcome){
            const auto now=now_();
            if(now<outcome->observedAt || now<lastIdentityRead_)identityClockInvalid_=true;
            lastIdentityRead_=now;
            if(identityClockInvalid_)outcome->deadline=0; // clock regression cannot reopen permission
            outcome->observedAt=now;outcome->cancelled=outcome->cancelled||stop_.load();
        }
        return outcome;
    }
    static std::shared_ptr<VerifierCommandSupervisor> create(dispatch_queue_t q,std::shared_ptr<Resources> r,Report report,
        std::function<uint64_t()> now){
        if(!q || !r || !report || !now)return {};
        return std::shared_ptr<VerifierCommandSupervisor>(new VerifierCommandSupervisor(q,std::move(r),std::move(report),std::move(now)));
    }
    void requestStop()noexcept{
        stop_.store(true);
        auto weak=this->weak_from_this();
        dispatch_async(queue_,^{if(auto owner=weak.lock()){
            if(owner->binding_.session)owner->binding_.session->requestStop();
        }});
    }
    bool start(Startup startup,std::function<VerifierObservation()> baseline){
        dispatch_assert_queue(queue_);if(consumed_ || !startup || !baseline)return false;
        consumed_=true;
        timer_=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue_);
        if(!timer_){blocked(@"supervisor-timer-unavailable");tick();return false;}
        auto retained=this->shared_from_this();std::weak_ptr<VerifierCommandSupervisor> weak=retained;
        dispatch_source_set_event_handler(timer_,^{retained->tick();});
        dispatch_source_set_timer(timer_,dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),100*NSEC_PER_MSEC,10*NSEC_PER_MSEC);
        dispatch_resume(timer_);
        binding_=startup([weak](NSDictionary *r){if(auto owner=weak.lock())owner->received(r);},
            [weak]{auto owner=weak.lock();return !owner || owner->stop_.load();});
        if(!binding_.session){blocked(@"startup-refused");tick();return false;}
        if(stop_.load())binding_.session->requestStop();
        auto observed=baseline();
        if(stop_.load())binding_.session->requestStop();
        const bool launched=binding_.session->start(observed);
        if(!binding_.session->supervisionStarted()){
            resources_->launch->abandon(@"api-error",@"session timer unavailable",now_());
            binding_.cancelPreparation();blocked(@"session-timer-unavailable");tick();
        }
        return launched; // false may still mean active failure/cleanup supervision
    }
};
}
