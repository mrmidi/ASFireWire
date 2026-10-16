// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SustainedInputReceiver.hpp"
#include <chrono>
#include <pthread.h>
#include <thread>

namespace Pro40Audio {
// Diagnostic composition, allocated/touched before native entry. Thread work
// is bounded by receiver.poll; no callbacks into native code. A client must not
// destroy this while native code can still call handoff. Reader leases must be
// released before destruction. Main/native owner calls start and finish once.
class SustainedInputBridge {
    pthread_t thread_{};
    std::atomic<bool> done_{false};
    bool started_=false,joined_=false;
    static void *consume(void *context) noexcept {
        auto &self=*static_cast<SustainedInputBridge *>(context);
        while(self.receiver.poll() && !self.receiver.ended())
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        self.done_.store(true,std::memory_order_release);return nullptr;
    }
public:
    SustainedInputHandoff handoff;
    SustainedInputReceiver<> receiver;
    SustainedInputBridge(uint32_t numer,uint32_t denom,uint64_t epoch)
        :handoff(numer,denom),receiver(handoff,epoch){}
    SustainedInputBridge(const SustainedInputBridge &)=delete;
    ~SustainedInputBridge() {
        if(started_ && !joined_){handoff.fail(SustainedInputFault::consumer);pthread_join(thread_,nullptr);}
    }
    bool start() noexcept {
        if(started_ || !handoff.fresh())return false;
        if(pthread_create(&thread_,nullptr,consume,this)!=0)return handoff.fail(SustainedInputFault::consumer);
        started_=true;return true;
    }
    // Caller has COMPLETED native teardown. Drain at most 500ms; a blocked
    // reader or failed native source cancels publication and still joins.
    bool finish(bool accepted) noexcept {
        if(!started_ || joined_)return false;
        if(accepted)handoff.finish();else handoff.fail(SustainedInputFault::consumer);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        while(!done_.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        if(!done_.load(std::memory_order_acquire))handoff.fail(SustainedInputFault::consumer);
        if(pthread_join(thread_,nullptr)!=0)return false;
        joined_=true;
        return accepted && handoff.fault()==SustainedInputFault::none && receiver.ended() &&
            receiver.frames()==handoff.frames() && receiver.publishedNext()==handoff.frames() &&
            receiver.snapshots()!=0;
    }
};
}
