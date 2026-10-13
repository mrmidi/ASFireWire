// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierExchangeOwner.hpp"
#include "VerifierCleanupOwner.hpp"
#include "VerifierIdentityOutcome.hpp"
namespace Pro40Lab {
// One retained serial timer owns Resources from launch through cleanup. Resources
// must own journal, launch and the original ServiceOperations instance. Callers
// route authenticated IPC/independent observation callbacks onto this queue.
template<class Resources>
class VerifierSessionOwner final : public std::enable_shared_from_this<VerifierSessionOwner<Resources>> {
    dispatch_queue_t queue_;
    dispatch_source_t timer_=nil;
    std::shared_ptr<Resources> resources_;
    std::unique_ptr<VerifierExchangeOwner> exchange_;
    std::shared_ptr<VerifierCleanupOwner<Resources>> cleanup_;
    std::function<uint64_t()> now_;
    std::function<bool()> localDone_;
    std::function<NSDictionary *()> removal_;
    std::function<void(NSDictionary *)> report_;
    std::function<void()> pollExit_;
    NSDictionary *lastReport_=nil;
    uint64_t deadline_,last_=0;
    bool stop_=false,started_=false,finished_=false;
    bool submitted_=false;
    std::optional<VerifierSessionCompletion> completion_;
#ifdef PRO40_JOURNAL_TESTING
    bool failTimer_=false;
#endif
    ActionPermission permit(){
        auto time=now_();
        if(stop_)return ActionPermission::cancelled;
        if(time<last_)return ActionPermission::invalidObservation;
        last_=time;return time>=deadline_?ActionPermission::expired:ActionPermission::allowed;
    }
    void scheduleTick(){auto retained=this->shared_from_this();dispatch_async(queue_,^{retained->tick();});}
    void beginCleanup(){
        cleanup_=VerifierCleanupOwner<Resources>::create(queue_,resources_,removal_,localDone_,now_,deadline_);
        cleanup_->start();
    }
    void tick(bool observeExit=false){
        dispatch_assert_queue(queue_);if(finished_)return;
        exchange_->poll();
        // Only the timer samples: queued progress/report updates cannot cause a
        // tight observation loop. Permission is rechecked after collector work.
        if(observeExit && awaitsExit()){
            pollExit_();
            exchange_->poll();
        }
        if(cleanup_)cleanup_->poll();
        const bool local=localDone_();
        const bool pending=cleanup_ && cleanup_->callbackPending();
        const bool reconciled=cleanup_ && cleanup_->state()==VerifierCleanupState::complete;
        const bool blocked=exchange_->state()==VerifierExchangeState::blocked ||
            (cleanup_ && cleanup_->state()==VerifierCleanupState::blocked);
        const bool done=cleanup_?cleanup_->readyToRetire():blocked && local;
        NSString *phase=reconciled?@"complete":blocked?(pending?@"blocked-pending-callback":local?@"blocked":@"blocked-pending-local-cleanup"):
            cleanup_?@"cleanup":exchange_->state()==VerifierExchangeState::awaitingReady?@"awaiting-ready":
            exchange_->state()==VerifierExchangeState::awaitingRelease?@"awaiting-release":@"awaiting-exit";
        NSDictionary *report=@{@"mode":@"verifier-session",@"phase":phase,@"supervision_finished":@(done),
            @"registration_reconciled":@(reconciled),@"callback_pending":@(pending),@"local_cleanup_complete":@(local),
            @"helper_clean_exit_proven":@(cleanup_ && cleanup_->cleanExitAndRemoval()),@"audio_admitted":@NO};
        if(done){
            completion_=VerifierSessionCompletion{submitted_,true,stop_,local,reconciled,
                cleanup_ && cleanup_->cleanExitAndRemoval(),pending,now_()};
            finished_=true;dispatch_source_cancel(timer_);timer_=nil;
            pollExit_={};cleanup_.reset();exchange_.reset();resources_.reset();
        }
        if(![lastReport_ isEqual:report]){lastReport_=report;report_(report);}
    }
    VerifierSessionOwner(dispatch_queue_t q,std::shared_ptr<Resources> r,std::function<uint64_t()> n,
        uint64_t deadline,std::function<bool()> local,std::function<NSDictionary *()> removal,
        std::function<void(NSDictionary *)> report):queue_(q),resources_(std::move(r)),now_(std::move(n)),
        localDone_(std::move(local)),removal_(std::move(removal)),report_(std::move(report)),deadline_(deadline){}
public:
    std::optional<VerifierSessionCompletion> completionEvidence()const{dispatch_assert_queue(queue_);return completion_;}
    bool supervisionStarted()const{dispatch_assert_queue(queue_);return started_;}
#ifdef PRO40_JOURNAL_TESTING
    void failTimerForTesting(){dispatch_assert_queue(queue_);failTimer_=true;}
#endif
    bool installExitPoll(std::function<void()> poll){
        dispatch_assert_queue(queue_);
        if(started_ || pollExit_ || !poll)return false;
        pollExit_=std::move(poll);return true;
    }
    // Callback sampling uses the same retained launch/proof as journal writes.
    // No separate identity configuration and no extra ownership cycle.
    std::optional<VerifierJobSnapshot> jobSnapshot(const VerifierObservation& o,uint64_t now)const{
        dispatch_assert_queue(queue_);
        if(!started_ || finished_ || !resources_ || !resources_->launch)return {};
        return resources_->launch->jobSnapshot(o,now);
    }
    bool acceptsReady()const{dispatch_assert_queue(queue_);return started_ && !finished_ && exchange_->state()==VerifierExchangeState::awaitingReady;}
    bool awaitsExit()const{dispatch_assert_queue(queue_);return started_ && !finished_ && exchange_->state()==VerifierExchangeState::awaitingExit;}
    static std::shared_ptr<VerifierSessionOwner> create(dispatch_queue_t q,std::shared_ptr<Resources> resources,
        std::function<uint64_t()> now,uint64_t deadline,std::function<bool()> local,
        std::function<NSDictionary *()> removal,std::function<void(NSDictionary *)> report,
        std::function<void()> activate,std::function<void()> release,std::function<void()> cancel){
        if(!q || !resources || !resources->launch || !now || !deadline || !local || !removal || !report ||
           !activate || !release || !cancel)return {};
        auto owner=std::shared_ptr<VerifierSessionOwner>(new VerifierSessionOwner(q,resources,now,deadline,local,removal,report));
        auto p=owner.get(); // retained by the active timer, never a cross-queue callback
        owner->exchange_=std::make_unique<VerifierExchangeOwner>(*resources->launch,[p]{return p->permit();},now,
            [p,activate]{p->submitted_=true;activate();},release,cancel,[p]{p->beginCleanup();});
        return owner;
    }
    bool start(const VerifierObservation& baseline){
        dispatch_assert_queue(queue_);if(started_ || !pollExit_)return false;
#ifdef PRO40_JOURNAL_TESTING
        if(failTimer_)return false;
#endif
        timer_=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue_);
        if(!timer_)return false; // no launch action issued; caller still owns resources
        started_=true;auto retained=this->shared_from_this();
        dispatch_source_set_event_handler(timer_,^{retained->tick(true);});
        dispatch_source_set_timer(timer_,dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),100*NSEC_PER_MSEC,10*NSEC_PER_MSEC);
        dispatch_resume(timer_);
        const bool issued=exchange_->start(baseline);scheduleTick();return issued;
    }
    bool ready(const VerifierObservation& o,const AuthenticatedVerifierPeer& p){dispatch_assert_queue(queue_);if(!started_||finished_)return false;const bool accepted=exchange_->ready(o,p);scheduleTick();return accepted;}
    void released(const AuthenticatedVerifierPeer& p){dispatch_assert_queue(queue_);if(!started_||finished_)return;(void)exchange_->released(p);scheduleTick();}
    void exited(const VerifierObservation& o){dispatch_assert_queue(queue_);if(!started_||finished_)return;(void)exchange_->exited(o);scheduleTick();}
    void failed(){dispatch_assert_queue(queue_);if(!started_||finished_)return;exchange_->failed();scheduleTick();}
    void requestStop(){dispatch_assert_queue(queue_);stop_=true;if(cleanup_)cleanup_->requestStop();if(started_)scheduleTick();}
};
}
