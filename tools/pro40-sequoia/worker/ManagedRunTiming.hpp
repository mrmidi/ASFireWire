// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <optional>
namespace Pro40 {
// Pure owner-queue policy, not resource ownership, authorization or proof of
// cleanup. The caller supplies monotonic time and actual joined cleanup evidence.
// UntilStopped is selected by the installed session command, never by a worker
// or HAL request. It removes only the diagnostic run/collection time limit.
class ManagedRunTiming final {
public:
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
    enum class Lifetime {bounded,untilStopped};
    static constexpr auto admission=std::chrono::seconds(30);
    // Only the explicitly packaged endurance trial admits ten minutes of
    // callbacks. Admission, cleanup and native stall deadlines are unchanged.
#if defined(PRO40_ENDURANCE_TRIAL)
#if !defined(PRO40_ENABLE_DUPLEX_HARDWARE) || !defined(PRO40_ENABLE_DUPLEX_DELEGATED) || defined(PRO40_APPLICATION_TRIAL)
#error Endurance trial requires the separate delegated hardware profile.
#endif
    static constexpr auto run=std::chrono::seconds(660);
#elif defined(PRO40_ENABLE_DUPLEX_HARDWARE)
    static constexpr auto run=std::chrono::seconds(120);
#else
    static constexpr auto run=std::chrono::seconds(30);
#endif
    static constexpr auto cleanup=std::chrono::seconds(15);
    static constexpr auto collection=admission+run+cleanup;
    static constexpr auto command=collection+cleanup;
    static_assert(collection<command);
    static constexpr std::optional<Time> collectionDeadline(Time began,Lifetime lifetime) {
        return lifetime==Lifetime::bounded?std::optional<Time>(began+collection):std::nullopt;
    }
    static constexpr std::optional<std::chrono::milliseconds> commandLimit(Lifetime lifetime) {
        return lifetime==Lifetime::bounded?std::optional<std::chrono::milliseconds>(command):std::nullopt;
    }
    enum class Cause {none,cancelled,admissionExpired,runExpired,collectionExpired,clockRegression};
    struct Observation {
        bool requestStop=false; // Edge only: caller closes lifeline once.
        bool retainOwnership=true;
        bool cleanupOverdue=false;
        bool commandOverdue=false;
        bool complete=false;
        bool resultEligible=false; // Still requires actual successful native/export results.
        Cause cause=Cause::none;
    };
private:
    Time began_,last_,releasedAt_{},stopAt_{};
    const Lifetime lifetime_;
    bool released_=false,stopping_=false,complete_=false;
    bool cleanupOverdue_=false,commandOverdue_=false;
    Cause cause_=Cause::none;
    bool stop(Cause cause,Time now){
        if(stopping_)return false;
        stopping_=true;stopAt_=now;cause_=cause;return true;
    }
    Observation snapshot(bool requestStop=false)const{
        return {requestStop,!complete_,cleanupOverdue_,commandOverdue_,complete_,
            complete_ && released_ && cause_==Cause::none,cause_};
    }
public:
    explicit ManagedRunTiming(Time began,Lifetime lifetime=Lifetime::bounded):began_(began),last_(began),lifetime_(lifetime){}
    // Observe expiration BEFORE cleanup: a late successful result cannot erase
    // an expired/cancelled attempt. Cleanup remains a separate owner observation.
    [[nodiscard]] Observation observe(Time now,bool cancellation,bool resourcesComplete){
        if(complete_)return snapshot();
        bool edge=false;
        if(now<last_){edge=stop(Cause::clockRegression,last_);now=last_;}
        last_=now;
        if(cancellation)edge=stop(Cause::cancelled,now)||edge;
        if(!stopping_){
            if(lifetime_==Lifetime::bounded && now-began_>=collection)edge=stop(Cause::collectionExpired,now)||edge;
            else if(!released_ && now-began_>=admission)edge=stop(Cause::admissionExpired,now)||edge;
            else if(lifetime_==Lifetime::bounded && released_ && now-releasedAt_>=run)edge=stop(Cause::runExpired,now)||edge;
        }
        if(stopping_ && now-stopAt_>=cleanup)cleanupOverdue_=true;
        // A missed owner wake-up cannot extend the overall observation window.
        if(lifetime_==Lifetime::bounded) {
            if(now-began_>=collection)cleanupOverdue_=true;
            if(now-began_>=command)commandOverdue_=true;
        }else if(stopping_ && now-stopAt_>=cleanup+cleanup)commandOverdue_=true;
        if(resourcesComplete)complete_=true;
        return snapshot(edge);
    }
    // Pure timing precheck only. Observe() must first process cancellation/time.
    bool releasePermitted()const noexcept{return !released_ && !stopping_ && !complete_;}
    // Called once immediately after successful release of the retained child.
    // If the precheck became stale, refuse and request stop through observe().
    [[nodiscard]] bool didRelease(Time now){
        // Preserve the stop edge for the caller: do not consume it here.
        if(!releasePermitted() || now<last_ || now-began_>=admission)return false;
        released_=true;releasedAt_=now;last_=now;return true;
    }
};
}
