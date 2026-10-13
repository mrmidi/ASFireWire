// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerCollectionOwner.hpp"
#include "../worker/WorkerSpawnPreparation.hpp"
namespace Pro40Lab {
struct WorkerStartupReport {
    int startupError=0,setupCleanupError=0;
    bool spawned=false,finished=false,resourcesComplete=false,stopRequested=false;
    std::optional<WorkerCollectionReport> collection;
};
// Serial-queue composition. No identity claims or worker release here. The
// signed supervisor must independently authorize admission after readyToVerify().
class WorkerStartupOwner final : public std::enable_shared_from_this<WorkerStartupOwner> {
    dispatch_queue_t queue_;
    std::function<void(WorkerStartupReport)> report_;
    Pro40::WorkerSpawnPreparation preparation_;
    std::shared_ptr<Pro40::WorkerChildOwner> child_;
    std::shared_ptr<Pro40::WorkerResultCollector> collector_;
    std::shared_ptr<WorkerCollectionOwner> collection_;
    WorkerStartupReport state_;
    std::atomic<bool> stop_{false};
    bool started_=false,armed_=false;
#ifdef PRO40_COLLECTION_TESTING
    bool failTimer_=false;
#endif
    WorkerStartupOwner(dispatch_queue_t queue,std::function<void(WorkerStartupReport)> report):queue_(queue),report_(std::move(report)){}
    void publish(){auto report=report_;if(state_.finished)report_={};if(report)report(snapshot());}
public:
    static std::shared_ptr<WorkerStartupOwner> create(dispatch_queue_t queue,std::function<void(WorkerStartupReport)> report){
        if(!queue || !report)return {};
        return std::shared_ptr<WorkerStartupOwner>(new WorkerStartupOwner(queue,std::move(report)));
    }
#ifdef PRO40_SPAWN_TESTING
    void failPrepareForTesting(unsigned step){dispatch_assert_queue(queue_);preparation_.failBeforeStep(step);}
    void failSetupCloseForTesting(){dispatch_assert_queue(queue_);failCleanup_=true;}
private:
    bool failCleanup_=false;
public:
#endif
#ifdef PRO40_COLLECTION_TESTING
    void failTimerForTesting(){dispatch_assert_queue(queue_);failTimer_=true;}
#endif
    // argv/env/bootstrap/errorFD need only outlive this synchronous launch call.
    // Failed scheduling requires explicit retained polling, just as collection does.
    [[nodiscard]] bool start(mach_port_t bootstrap,int errorFD,const char *path,
        char *const argv[],char *const env[],std::optional<Pro40::WorkerResultCollector::Clock::time_point> deadline,
        Pro40::WorkerSpawnPreparation::Namespace space=Pro40::WorkerSpawnPreparation::Namespace::replacementBootstrap){
        dispatch_assert_queue(queue_);if(started_)return false;started_=true;
        state_.startupError=stop_.load()?ECANCELED:preparation_.prepare(bootstrap,errorFD,space);
        if(!state_.startupError){
#ifdef PRO40_SPAWN_TESTING
            if(failCleanup_)preparation_.failSetupCloseForTesting();
#endif
            state_.startupError=stop_.load()?ECANCELED:preparation_.launch(path,argv,env);
        }
        child_=preparation_.takeWorker();state_.spawned=bool(child_);
        const int reader=preparation_.takeResultReader();
        state_.setupCleanupError=preparation_.close();
        if(!child_){state_.finished=true;state_.resourcesComplete=!state_.setupCleanupError;publish();return false;}
        collector_=std::make_shared<Pro40::WorkerResultCollector>(child_,reader,deadline);
        auto retained=shared_from_this();
        collection_=WorkerCollectionOwner::create(queue_,collector_,[retained](WorkerCollectionReport report){
            auto &state=retained->state_;state.collection=report;
            state.finished=report.cleanupComplete;
            state.resourcesComplete=report.cleanupComplete && !state.setupCleanupError;
            retained->publish();
        });
        if(state_.startupError || state_.setupCleanupError || stop_.load()){
            stop_.store(true);collection_->requestStop();
        }
#ifdef PRO40_COLLECTION_TESTING
        if(failTimer_)collection_->failTimerForTesting();
#endif
        armed_=collection_->start();
        if(!armed_ && !state_.startupError)state_.startupError=EIO;
        return readyToVerify();
    }
    bool readyToVerify()const{
        dispatch_assert_queue(queue_);
        return started_ && child_ && armed_ && !state_.startupError && !state_.setupCleanupError &&
            !stop_.load() && !state_.finished && !collector_->finished() &&
            collector_->failure()==Pro40::WorkerResultCollector::Fault::none;
    }
    // Borrowed for final admission. Observe the command stop immediately even
    // before its queued relay closes the worker lifeline.
    Pro40::StopQuery stopQuery()noexcept{
        return {[](void *p)noexcept{return static_cast<WorkerStartupOwner*>(p)->stop_.load();},this};
    }
    void requestStop(){
        stop_.store(true);auto retained=shared_from_this();
        dispatch_async(queue_,^{if(retained->collection_)retained->collection_->requestStop();});
    }
    void pollAfterStartFailure(){dispatch_assert_queue(queue_);if(collection_)collection_->pollAfterStartFailure();}
    WorkerStartupReport snapshot()const{dispatch_assert_queue(queue_);auto copy=state_;copy.stopRequested=stop_.load();return copy;}
    std::shared_ptr<Pro40::WorkerChildOwner> retainedChild()const{dispatch_assert_queue(queue_);return child_;}
    std::shared_ptr<Pro40::WorkerResultCollector> collector()const{dispatch_assert_queue(queue_);return collector_;}
};
}
