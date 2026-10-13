// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioSyntheticAdmission.hpp"
#include "AudioCallbackSession.hpp"
#include "AudioFeedMachTransport.hpp"
#include <pthread.h>
namespace Pro40Audio {
enum class SyntheticPrepare {ok, alreadyAttempted, invalidStart, endpointFailure, admissionFailure, threadFailure, missedStart};
struct SyntheticPrepareResult {SyntheticPrepare result;bool cleanupOK;};
// One control owner prepares/stops. Expose to callback readers only AFTER prepare
// returns. Worker allocation never changes afterward, even after failed prepare.
// stopAndJoin blocks new reads and joins worker; callers must separately quiesce
// ALL readInput/report calls before destruction. Not yet wired into HAL lifecycle.
class AudioSyntheticReceiver {
    using Worker=AudioFeedWorker<8192,AudioFeedMachTransport,AudioCallbackSession<8192>>;
    std::optional<Worker> worker_;
    std::atomic<bool> entered_{false},armed_{false};
    pthread_t thread_{};bool attempted_=false,started_=false;
    static void *entry(void *raw) {
        auto &self=*static_cast<AudioSyntheticReceiver *>(raw);
        self.entered_.store(true,std::memory_order_release);self.worker_->run();return nullptr;
    }
    SyntheticPrepareResult rollback(SyntheticPrepare reason) {
        return {reason,stopAndJoin()};
    }
public:
    using ThreadCreate=int (*)(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
    ~AudioSyntheticReceiver(){stopAndJoin();}
    SyntheticPrepareResult prepare(mach_port_t control,const AudioSyntheticStart &start,
                                    ThreadCreate create=pthread_create) {
        if(attempted_)return {SyntheticPrepare::alreadyAttempted,false};
        attempted_=true;
        if(!SyntheticAdmission::ready(start))return {SyntheticPrepare::invalidStart,true};
        mach_port_t receive=MACH_PORT_NULL;
        if(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&receive)!=KERN_SUCCESS)
            return {SyntheticPrepare::endpointFailure,true};
        // Own receive right immediately. Six-second initial idle allowance covers
        // the admission protocol's maximum five-second future origin.
        worker_.emplace(AudioFeedMachTransport(receive),AudioCallbackTimeline{start.epoch,start.seed,0,0},0,
                        Worker::Limits{std::chrono::milliseconds(5),std::chrono::milliseconds(6000),std::chrono::milliseconds(100)});
        if(SyntheticAdmission::subscribe(control,receive,start)!=KERN_SUCCESS)
            return rollback(SyntheticPrepare::admissionFailure);
        if(!create || create(&thread_,nullptr,entry,this)!=0)return rollback(SyntheticPrepare::threadFailure);
        started_=true;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        while(!entered_.load(std::memory_order_acquire)) {
            if(std::chrono::steady_clock::now()>=deadline || !SyntheticAdmission::ready(start))
                return rollback(SyntheticPrepare::missedStart);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!SyntheticAdmission::ready(start) || worker_->report())return rollback(SyntheticPrepare::missedStart);
        armed_.store(true,std::memory_order_release);return {SyntheticPrepare::ok,true};
    }
    bool stopAndJoin() {
        armed_.store(false,std::memory_order_release);
        if(!worker_)return true;
        worker_->requestStop();
        if(started_){pthread_join(thread_,nullptr);started_=false;}
        else if(!worker_->report())worker_->run(); // Pre-thread rollback closes via same worker path.
        const auto report=worker_->report();return report && report->transportClosed;
    }
    auto report() const noexcept {return worker_?worker_->report():std::optional<WorkerReport>{};}
    CallbackSessionResult readInput(uint64_t epoch,uint64_t seed,const AudioServerPlugInIOCycleInfo &cycle,
                                   UInt32 frames,std::span<float> output) noexcept {
        if(!armed_.load(std::memory_order_acquire)) {
            std::fill(output.begin(),output.end(),0);return {};
        }
        return worker_->readInput(epoch,seed,cycle,frames,output);
    }
};
}
