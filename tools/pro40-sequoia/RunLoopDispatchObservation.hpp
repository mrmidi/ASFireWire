// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <chrono>
#include <cstdint>
#include <time.h>

namespace Pro40 {
struct DispatchClockSample {uint64_t wall=0,cpu=0;};
struct DispatchWaitSample {
    uint64_t wall=0,cpu=0,waitWall=0,waitCPU=0;
    unsigned waits=0;
    bool valid=true;
};

// Phase accounting only: an observed CF wait includes time until the thread
// executes AfterWaiting. It cannot separate kernel waiting from descheduling.
class DispatchWaitAccounting {
    DispatchClockSample start_{},waitStart_{};
    DispatchWaitSample sample_{};
    bool active_=false,waiting_=false;
public:
    void begin(DispatchClockSample now) noexcept {
        const bool overlap=active_;
        start_=now;sample_={};sample_.valid=!overlap;active_=true;waiting_=false;
    }
    void beforeWait(DispatchClockSample now) noexcept {
        if(!active_)return;
        if(waiting_ || now.wall<start_.wall || now.cpu<start_.cpu){sample_.valid=false;return;}
        waiting_=true;waitStart_=now;++sample_.waits;
    }
    void afterWait(DispatchClockSample now) noexcept {
        if(!active_)return;
        if(!waiting_ || now.wall<waitStart_.wall || now.cpu<waitStart_.cpu){sample_.valid=false;return;}
        sample_.waitWall+=now.wall-waitStart_.wall;
        sample_.waitCPU+=now.cpu-waitStart_.cpu;waiting_=false;
    }
    [[nodiscard]] DispatchWaitSample finish(DispatchClockSample now) noexcept {
        if(!active_ || waiting_ || now.wall<start_.wall || now.cpu<start_.cpu)sample_.valid=false;
        if(now.wall>=start_.wall)sample_.wall=now.wall-start_.wall;
        if(now.cpu>=start_.cpu)sample_.cpu=now.cpu-start_.cpu;
        if(sample_.waitWall>sample_.wall || sample_.waitCPU>sample_.cpu)sample_.valid=false;
        active_=waiting_=false;
        return sample_;
    }
    [[nodiscard]] bool active()const noexcept{return active_;}
};

// Install/retire on the dedicated native thread, outside streaming. The observer
// has no DMA pointers and never changes source delivery, wait duration or QoS.
// SDK CFRunLoop.h:43-50,136-158 defines activities and context ownership.
// Sequence: Apple's Threading Programming Guide, Run Loop Observers:
// https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/Multithreading/RunLoopManagement/RunLoopManagement.html
class RunLoopDispatchObservation {
    CFRunLoopObserverRef observer_=nullptr;
    DispatchWaitAccounting accounting_;
    static DispatchClockSample now() noexcept {
        return {uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()),
            clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)};
    }
    static void observe(CFRunLoopObserverRef,CFRunLoopActivity activity,void *context) noexcept {
        auto &self=*static_cast<RunLoopDispatchObservation *>(context);
        if(!self.accounting_.active())return;
        if(activity==kCFRunLoopBeforeWaiting)self.accounting_.beforeWait(now());
        else if(activity==kCFRunLoopAfterWaiting)self.accounting_.afterWait(now());
    }
public:
    RunLoopDispatchObservation() {
        CFRunLoopObserverContext context{};context.info=this;
        observer_=CFRunLoopObserverCreate(nullptr,kCFRunLoopBeforeWaiting|kCFRunLoopAfterWaiting,
            true,0,observe,&context);
        if(observer_)CFRunLoopAddObserver(CFRunLoopGetCurrent(),observer_,kCFRunLoopDefaultMode);
    }
    RunLoopDispatchObservation(const RunLoopDispatchObservation &)=delete;
    RunLoopDispatchObservation &operator=(const RunLoopDispatchObservation &)=delete;
    ~RunLoopDispatchObservation() {
        if(observer_){CFRunLoopObserverInvalidate(observer_);CFRelease(observer_);}
    }
    [[nodiscard]] bool valid()const noexcept{return observer_!=nullptr;}
    void begin()noexcept{accounting_.begin(now());}
    [[nodiscard]] DispatchWaitSample finish()noexcept{return accounting_.finish(now());}
};
}
