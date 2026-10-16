// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>
#include <cstdint>
#include <algorithm>
#include <cerrno>

namespace Pro40 {
// One dedicated native owner, one streaming interval, no borrowed HAL thread.
// SDK mach/thread_policy.h:115-166 uses absolute ticks, not nanoseconds.
// AudioWorkIntervalCreate cannot run here: a cold process calls back into
// CoreAudio while StartIO synchronously waits for this worker. The isolated
// start-dependency host experiment reproduces that wait without FireWire.
// Use only Mach policy and local CF observers in this one-session worker.
// Audio workgroups require a different provisioning lifetime, before StartIO.
// These are soft scheduling requests; DMA lead and observation guards still
// decide whether work may proceed. No packet/buffer geometry changes here.
class NativeAudioScheduling {
public:
    // Nominal64-packet notification period; a provisional2ms CPU allowance per
    // arrival with4ms completion constraint. Actual CPU/overruns are retained.
    static constexpr uint64_t periodNS=8000000,computationNS=2000000,constraintNS=4000000;
    struct Evidence {
        int before=-1,set=-1,after=-1;
        int finalRead=-1,restore=-1,restoredRead=-1,restoredQoSRead=-1;
        bool beforeDefault=false,afterDefault=true,finalDefault=true,restoredDefault=false;
        unsigned restoredQoS=0;
        uint64_t starts=0,finishes=0,overruns=0,maxWallNS=0,maxCpuNS=0;
        bool closed=false;
    } evidence;
private:
    mach_timebase_info_data_t timebase_{};
    thread_time_constraint_policy_data_t requested_{};
    CFRunLoopObserverRef observer_=nullptr;
    const pthread_t owner_=pthread_self();
    qos_class_t oldQoS_=QOS_CLASS_UNSPECIFIED;
    int oldRelative_=0;
    bool prepared_=false,entered_=false,verified_=false,interval_=false,attempted_=false;
    bool closedClean_=false;
    uint64_t begin_=0,cpuBegin_=0;
    static int readPolicy(thread_time_constraint_policy_data_t &policy,bool &isDefault) noexcept {
        mach_msg_type_number_t count=THREAD_TIME_CONSTRAINT_POLICY_COUNT;boolean_t value=false;
        const auto result=thread_policy_get(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),&count,&value);
        isDefault=value;
        return result?result:count==THREAD_TIME_CONSTRAINT_POLICY_COUNT?0:KERN_FAILURE;
    }
    bool matches(const thread_time_constraint_policy_data_t &policy) const noexcept {
        return policy.period==requested_.period && policy.computation==requested_.computation &&
            policy.constraint==requested_.constraint && policy.preemptible==requested_.preemptible;
    }
    void beginInterval() noexcept {
        if(!entered_ || interval_)return;
        const auto now=mach_absolute_time();
        begin_=now;cpuBegin_=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        interval_=true;++evidence.starts;
    }
    void finishInterval() noexcept {
        if(!interval_)return;
        const auto end=mach_absolute_time(),cpu=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        const auto wall=uint64_t(__uint128_t(end-begin_)*timebase_.numer/timebase_.denom);
        evidence.maxWallNS=std::max(evidence.maxWallNS,wall);
        evidence.maxCpuNS=std::max(evidence.maxCpuNS,cpu-cpuBegin_);
        evidence.overruns+=wall>constraintNS;
        interval_=false;++evidence.finishes;
    }
    static void observe(CFRunLoopObserverRef,CFRunLoopActivity activity,void *context) noexcept {
        auto &self=*static_cast<NativeAudioScheduling *>(context);
        if(!self.entered_)return;
        if(activity==kCFRunLoopBeforeWaiting)self.finishInterval();
        else if(activity==kCFRunLoopAfterWaiting)self.beginInterval();
    }
public:
    NativeAudioScheduling() {
        if(mach_timebase_info(&timebase_) || !timebase_.numer || !timebase_.denom)return;
        const auto ticks=[&](uint64_t ns){return __uint128_t(ns)*timebase_.denom/timebase_.numer;};
        if(!ticks(computationNS) || ticks(periodNS)>UINT32_MAX)return;
        requested_={uint32_t(ticks(periodNS)),uint32_t(ticks(computationNS)),uint32_t(ticks(constraintNS)),true};
        CFRunLoopObserverContext context{};context.info=this;
        observer_=CFRunLoopObserverCreate(nullptr,kCFRunLoopBeforeWaiting|kCFRunLoopAfterWaiting,true,0,observe,&context);
        if(!observer_)return;
        CFRunLoopAddObserver(CFRunLoopGetCurrent(),observer_,kCFRunLoopDefaultMode);
        prepared_=true;
    }
    NativeAudioScheduling(const NativeAudioScheduling &)=delete;
    NativeAudioScheduling &operator=(const NativeAudioScheduling &)=delete;
    [[nodiscard]] bool prepared() const noexcept {return prepared_;}
    [[nodiscard]] bool healthy() const noexcept {
        return entered_ && verified_ && !evidence.closed;
    }
    [[nodiscard]] bool enter() noexcept {
        if(!pthread_equal(owner_,pthread_self()) || !prepared_ || attempted_ || evidence.closed)return false;
        attempted_=true;
        if(pthread_get_qos_class_np(pthread_self(),&oldQoS_,&oldRelative_) || oldQoS_==QOS_CLASS_UNSPECIFIED)return false;
        thread_time_constraint_policy_data_t before{},after{};
        evidence.before=readPolicy(before,evidence.beforeDefault);
        if(evidence.before || !evidence.beforeDefault)return false; // Do not borrow an existing realtime thread.
        evidence.set=thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&requested_),THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if(evidence.set)return false;
        entered_=true;
        evidence.after=readPolicy(after,evidence.afterDefault);
        if(evidence.after || evidence.afterDefault || !matches(after))return false;
        verified_=true;
        beginInterval();return healthy();
    }
    // Call on the same owner after native Stop, before logging/restoration IO.
    // Restore standard scheduling. Mach opts this thread out of QoS permanently
    // (Sequoia returns EPERM when trying to rejoin). It must be a one-session
    // dedicated owner which exits after cleanup, never a reusable client thread.
    [[nodiscard]] bool close() noexcept {
        if(!pthread_equal(owner_,pthread_self()))return false;
        if(evidence.closed)return closedClean_;
        finishInterval();
        if(entered_) {
            thread_time_constraint_policy_data_t final{},restored{};
            evidence.finalRead=readPolicy(final,evidence.finalDefault);
            if(!evidence.finalRead && (!matches(final) || evidence.finalDefault))evidence.finalRead=KERN_FAILURE;
            evidence.restore=thread_policy_set(pthread_mach_thread_np(pthread_self()),THREAD_STANDARD_POLICY,nullptr,THREAD_STANDARD_POLICY_COUNT);
            evidence.restoredRead=readPolicy(restored,evidence.restoredDefault);
            qos_class_t qos{};int relative=0;
            evidence.restoredQoSRead=pthread_get_qos_class_np(pthread_self(),&qos,&relative);
            evidence.restoredQoS=unsigned(qos);
            closedClean_=!evidence.restore && !evidence.restoredRead && evidence.restoredDefault && !evidence.restoredQoSRead;
            entered_=false;
        } else closedClean_=true; // No policy change to undo; keep unattempted evidence unset.
        evidence.closed=true;
        return closedClean_;
    }
    ~NativeAudioScheduling() {
        (void)close();
        if(observer_){CFRunLoopObserverInvalidate(observer_);CFRelease(observer_);}
    }
};
}
