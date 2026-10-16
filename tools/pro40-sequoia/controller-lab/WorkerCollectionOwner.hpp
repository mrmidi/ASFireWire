// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../worker/WorkerResultCollector.hpp"
#include <dispatch/dispatch.h>
#include <atomic>
#include <functional>
namespace Pro40Lab {
struct WorkerCollectionReport {
    Pro40::WorkerResultCollector::Fault fault;
    bool collectionFinished,cleanupComplete,schedulingFailed;
    std::optional<Pro40::NativeSessionResult> result;
};
// Owner-queue timer retains collector/child through faults and late cleanup.
// Arm successfully BEFORE releasing a verified worker. On start failure caller
// must retain this owner and explicitly pollAfterStartFailure until cleanup.
class WorkerCollectionOwner final : public std::enable_shared_from_this<WorkerCollectionOwner> {
    dispatch_queue_t queue_;
    dispatch_source_t timer_=nullptr;
    std::shared_ptr<Pro40::WorkerResultCollector> collector_;
    std::function<void(WorkerCollectionReport)> report_;
    std::atomic<bool> stop_{false};
    bool attempted_=false,schedulingFailed_=false,finished_=false;
    std::optional<unsigned> lastState_;
#ifdef PRO40_COLLECTION_TESTING
    bool failTimer_=false;
#endif
    WorkerCollectionOwner(dispatch_queue_t queue,std::shared_ptr<Pro40::WorkerResultCollector> collector,
        std::function<void(WorkerCollectionReport)> report):queue_(queue),collector_(std::move(collector)),report_(std::move(report)){}
    void tick(){
        dispatch_assert_queue(queue_);if(finished_)return;
        collector_->poll(stopQuery());
        const auto state=snapshot();
        const unsigned key=unsigned(state.fault)*8+unsigned(state.collectionFinished)*4+
            unsigned(state.cleanupComplete)*2+unsigned(state.schedulingFailed);
        // Cleanup failure/unknown ownership remains retained, never retired on
        // deadline alone. Report changes only; no periodic success logging.
        if(state.cleanupComplete){
            finished_=true;if(timer_){dispatch_source_cancel(timer_);timer_=nullptr;}
        }
        if(!lastState_ || *lastState_!=key){
            lastState_=key;auto report=report_;
            if(finished_)report_={}; // Break caller/report cycles at retirement.
            report(state);
        }
    }
public:
    // Borrowed query for admission on the owner queue. Keep this owner alive
    // through the call; collection observes the same atomic cancellation flag.
    Pro40::StopQuery stopQuery()noexcept{
        return {[](void *p)noexcept{return static_cast<WorkerCollectionOwner*>(p)->stop_.load();},this};
    }
    static std::shared_ptr<WorkerCollectionOwner> create(dispatch_queue_t queue,
        std::shared_ptr<Pro40::WorkerResultCollector> collector,std::function<void(WorkerCollectionReport)> report){
        if(!queue || !collector || !collector->retainedChild() || !report)return {};
        return std::shared_ptr<WorkerCollectionOwner>(new WorkerCollectionOwner(queue,std::move(collector),std::move(report)));
    }
#ifdef PRO40_COLLECTION_TESTING
    void failTimerForTesting(){dispatch_assert_queue(queue_);if(!attempted_)failTimer_=true;}
#endif
    [[nodiscard]] bool start(){
        dispatch_assert_queue(queue_);if(attempted_)return false;attempted_=true;
#ifdef PRO40_COLLECTION_TESTING
        if(!failTimer_)
#endif
        timer_=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue_);
        if(!timer_){schedulingFailed_=true;stop_.store(true);collector_->retainedChild()->cancel();return false;}
        auto retained=shared_from_this();
        dispatch_source_set_event_handler(timer_,^{retained->tick();});
        dispatch_source_set_timer(timer_,dispatch_time(DISPATCH_TIME_NOW,0),10*NSEC_PER_MSEC,NSEC_PER_MSEC);
        dispatch_resume(timer_);return true;
    }
    // Can be called from another thread. The atomic stop remains observable even
    // before the queued wake-up; the wake-up itself owns no indefinite loop.
    void requestStop(){
        stop_.store(true);auto retained=shared_from_this();
        dispatch_async(queue_,^{if(retained->attempted_)retained->tick();});
    }
    void pollAfterStartFailure(){dispatch_assert_queue(queue_);if(schedulingFailed_)tick();}
    WorkerCollectionReport snapshot()const{
        dispatch_assert_queue(queue_);
        return {collector_->failure(),collector_->finished(),collector_->cleanupComplete(),schedulingFailed_,collector_->result()};
    }
};
}
