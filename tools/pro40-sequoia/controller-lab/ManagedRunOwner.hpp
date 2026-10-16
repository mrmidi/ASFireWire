// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerStartupOwner.hpp"
#include "../worker/ManagedRunTiming.hpp"
namespace Pro40Lab {
struct ManagedRunReport {
    Pro40::ManagedRunTiming::Observation timing;
    bool schedulingFailed=false;
};
// Serial owner for worker timing only. Verifier/capture cleanup is still joined
// by the command. Arm before release; never retire merely because time expired.
class ManagedRunOwner final:public std::enable_shared_from_this<ManagedRunOwner> {
    using Policy=Pro40::ManagedRunTiming;
    dispatch_queue_t queue_;
    dispatch_source_t timer_=nullptr;
    std::shared_ptr<WorkerStartupOwner> startup_;
    std::function<Policy::Time()> now_;
    std::function<void(ManagedRunReport)> report_;
    Policy policy_;
    std::atomic<bool> stop_{false};
    bool started_=false,armed_=false,failed_=false,finished_=false;
    std::optional<unsigned> last_;
    ManagedRunReport state_{};
#ifdef PRO40_TIMING_TESTING
    bool failTimer_=false;
#endif
    ManagedRunOwner(dispatch_queue_t queue,std::shared_ptr<WorkerStartupOwner> startup,
        Policy::Time began,std::function<Policy::Time()> now,std::function<void(ManagedRunReport)> report,Policy::Lifetime lifetime):
        queue_(queue),startup_(std::move(startup)),now_(std::move(now)),report_(std::move(report)),policy_(began,lifetime){}
    void tick(){
        dispatch_assert_queue(queue_);if(finished_)return;
        const auto worker=startup_->snapshot();
        state_={policy_.observe(now_(),stop_.load()||startup_->stopQuery().requested(),
            worker.finished && worker.resourcesComplete),failed_};
        if(state_.timing.requestStop)startup_->requestStop();
        const auto &s=state_.timing;
        const unsigned key=unsigned(s.cause)*32+unsigned(s.complete)*16+unsigned(s.cleanupOverdue)*8+
            unsigned(s.commandOverdue)*4+unsigned(s.resultEligible)*2+unsigned(failed_);
        if(s.complete){finished_=true;armed_=false;if(timer_){dispatch_source_cancel(timer_);timer_=nullptr;}}
        if(!last_ || *last_!=key){
            last_=key;auto report=report_;if(finished_)report_={};report(state_);
        }
    }
public:
    static std::shared_ptr<ManagedRunOwner> create(dispatch_queue_t queue,std::shared_ptr<WorkerStartupOwner> startup,
        Policy::Time began,std::function<void(ManagedRunReport)> report,
        std::function<Policy::Time()> now=[]{return Policy::Clock::now();},Policy::Lifetime lifetime=Policy::Lifetime::bounded){
        if(!queue || !startup || !report || !now)return {};
        return std::shared_ptr<ManagedRunOwner>(new ManagedRunOwner(queue,std::move(startup),began,std::move(now),std::move(report),lifetime));
    }
#ifdef PRO40_TIMING_TESTING
    void failTimerForTesting(){dispatch_assert_queue(queue_);failTimer_=true;}
    void pollForTesting(){tick();}
#endif
    [[nodiscard]] bool start(){
        dispatch_assert_queue(queue_);if(started_)return false;started_=true;
#ifdef PRO40_TIMING_TESTING
        if(!failTimer_)
#endif
        timer_=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue_);
        if(!timer_){failed_=true;stop_.store(true);tick();return false;}
        armed_=true;auto retained=shared_from_this();
        dispatch_source_set_event_handler(timer_,^{retained->tick();});
        dispatch_source_set_timer(timer_,dispatch_time(DISPATCH_TIME_NOW,0),10*NSEC_PER_MSEC,NSEC_PER_MSEC);
        dispatch_resume(timer_);return true;
    }
    // The callback must use the signed one-use gate for this exact startup owner.
    // It returns true only after sending release; this class grants no identity trust.
    template<class AuthorizedRelease> [[nodiscard]] bool release(AuthorizedRelease&& authorized){
        dispatch_assert_queue(queue_);if(!armed_ || finished_ || failed_)return false;
        tick();
        if(!policy_.releasePermitted() || !startup_->readyToVerify() || stop_.load())return false;
        if(!std::forward<AuthorizedRelease>(authorized)()){stop_.store(true);tick();return false;}
        if(!policy_.didRelease(now_())){stop_.store(true);tick();return false;}
        return true;
    }
    void requestStop(){
        stop_.store(true);auto retained=shared_from_this();dispatch_async(queue_,^{retained->tick();});
    }
    // Scheduling failure has no self-retaining timer: caller retains and polls.
    void pollAfterStartFailure(){dispatch_assert_queue(queue_);if(failed_)tick();}
    ManagedRunReport snapshot()const{dispatch_assert_queue(queue_);return state_;}
};
}
