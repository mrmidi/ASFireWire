// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedProducerAdmission.hpp"
#include "AudioFeedMachTransport.hpp"
#include <servers/bootstrap.h>
#include <pthread.h>
#include <os/log.h>
#include <unistd.h>
#include <cstdlib>

namespace Pro40Audio {
// Opt-in finite synthetic lab only. No IO callback references this object.
// Initialize/control owner starts once; destruction stops and joins all users
// before worker storage is destroyed. No captured Driver/host pointers.
class HalFeedDiagnostic {
    using Worker=AudioFeedWorker<1024,AudioFeedMachTransport>;
    std::optional<Worker> worker_;
    uint64_t epoch_=0;
    pthread_t owner_{},reader_{};
    bool ownerStarted_=false,attempted_=false;
    bool expectSourceLoss_=false;
    std::atomic<bool> done_{false},valid_{true};
    std::atomic<uint64_t> checks_{0};
    std::atomic<unsigned> outcome_{0}; // 0 running, 1 validated, 2 failed/stopped.
    static void *readEntry(void *context) {
        auto &self=*static_cast<HalFeedDiagnostic *>(context);
        while(!self.done_.load()) {
            {
                auto lease=self.worker_->tryAcquire();
                if(lease && lease->next()>=512) {
                    std::array<int32_t,512> audio{};const auto first=lease->next()-512;
                    if(lease->read(self.epoch_,first,audio)!=WindowResult::ok)self.valid_=false;
                    for(size_t n=0;n<512;++n)if(audio[n]!=int32_t((first+n)%16777216)-8388608)self.valid_=false;
                    ++self.checks_;
                }
            } // Release before sleeping; no long-lived callback lease.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return nullptr;
    }
    static void *runEntry(void *context) {
        auto &self=*static_cast<HalFeedDiagnostic *>(context);
        self.worker_->run();self.done_=true;
        pthread_join(self.reader_,nullptr);
        const auto report=*self.worker_->report();
        const auto expectedEnd=self.expectSourceLoss_?WorkerEnd::idleTimeout:WorkerEnd::disconnected;
        const uint64_t expectedFrames=self.expectSourceLoss_?51200:256000;
        const bool passed=report.end==expectedEnd && report.transportClosed && self.valid_ &&
            self.checks_>0 && report.acceptedNext==expectedFrames && report.publishedNext==expectedFrames;
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab feed: pid=%{public}d passed=%{public}u end=%{public}u frames=%{public}llu published=%{public}llu checks=%{public}llu closed=%{public}u",
            getpid(),unsigned(passed),unsigned(report.end),(unsigned long long)report.acceptedNext,
            (unsigned long long)report.publishedNext,(unsigned long long)self.checks_.load(),unsigned(report.transportClosed));
        self.outcome_.store(passed?1u:2u,std::memory_order_release);
        return nullptr;
    }
public:
    unsigned outcome() const {return outcome_.load(std::memory_order_acquire);}
    ~HalFeedDiagnostic() {
        if(ownerStarted_) {worker_->requestStop();pthread_join(owner_,nullptr);}
        // runEntry joined reader and all leases are gone before worker_ teardown.
    }
    kern_return_t start(const char *name="local.pro40.hal-feed-lab",bool expectSourceLoss=false) {
        if(attempted_)return KERN_FAILURE;attempted_=true;
        expectSourceLoss_=expectSourceLoss;
        mach_port_t control=MACH_PORT_NULL,data=MACH_PORT_NULL;
        auto result=bootstrap_look_up(bootstrap_port,name,&control);
        if(result!=KERN_SUCCESS)return result;
        result=mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&data);
        if(result!=KERN_SUCCESS){mach_port_deallocate(mach_task_self(),control);return result;}
        do {arc4random_buf(&epoch_,sizeof(epoch_));}while(!epoch_);
        worker_.emplace(AudioFeedMachTransport(data),epoch_,0);
        result=ProducerAdmission::subscribe(control,data,epoch_,epoch_);
        mach_port_deallocate(mach_task_self(),control);
        if(result!=KERN_SUCCESS){worker_.reset();return result;}
        if(pthread_create(&reader_,nullptr,readEntry,this)!=0){worker_.reset();return KERN_RESOURCE_SHORTAGE;}
        if(pthread_create(&owner_,nullptr,runEntry,this)!=0) {
            done_=true;pthread_join(reader_,nullptr);worker_.reset();return KERN_RESOURCE_SHORTAGE;
        }
        ownerStarted_=true;return KERN_SUCCESS;
    }
};
}
