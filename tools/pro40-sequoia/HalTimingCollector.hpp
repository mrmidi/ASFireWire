// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalCallbackTiming.hpp"
#include <pthread.h>
#include <chrono>
#include <mutex>
#include <thread>
namespace Pro40Audio {
// One serialized non-RT consumer. Sink runs on the collector or withDrained
// control caller, never on an audio callback. Recorder and
// sink context must outlive collector destruction; join before destroying them.
template<size_t Capacity> class HalTimingCollector {
    HalCallbackTiming<Capacity> &recorder_;
    void (*sink_)(const HalCallbackTimingRecord &,void *);
    void *context_;
    std::mutex consumerMutex_;
    std::atomic<bool> stop_{false};pthread_t thread_{};bool started_=false,attempted_=false;
    static void *entry(void *raw) {
        auto &self=*static_cast<HalTimingCollector *>(raw);
        do {
            self.withDrained([]{});
            if(self.stop_.load(std::memory_order_acquire))break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while(true);
        return nullptr;
    }
public:
    HalTimingCollector(HalCallbackTiming<Capacity> &recorder,
        void (*sink)(const HalCallbackTimingRecord &,void *),void *context):
        recorder_(recorder),sink_(sink),context_(context) {}
    bool start() {
        if(attempted_ || !sink_)return false;attempted_=true;
        started_=pthread_create(&thread_,nullptr,entry,this)==0;return started_;
    }
    // Quiesce producers before requesting a final snapshot. The lock is only
    // for the two non-RT consumer entry points; record() never takes it.
    template<class Consumer> void withDrained(Consumer &&consume) {
        std::lock_guard lock(consumerMutex_);
        if(sink_)recorder_.drain([&](const auto &r){sink_(r,context_);});
        consume();
    }
    void stopAndJoin() {
        stop_.store(true,std::memory_order_release);
        if(started_){pthread_join(thread_,nullptr);started_=false;}
    }
    ~HalTimingCollector(){stopAndJoin();}
};
}
