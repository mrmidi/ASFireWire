// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RetainedClockStart.hpp"
#include "GuardedClockPublisher.hpp"
namespace Pro40Audio {
// Offline single-owner startup transaction: initial PCM and sample-zero stamp
// are returned together, or neither is returned. Not a continuing HAL session.
// Publication storage must outlive this transaction; no access after owner teardown.
template<size_t Capacity> class RetainedClockStartup {
    using Reservation=RetainedClockStart<Capacity>;
public:
    enum class Result {ready,completed,notReady,clockRejected,blocked};
    RetainedClockStartup(SourceClockReadiness::Policy policy,uint64_t arrivalAge,
                         uint64_t useAge,uint64_t startedHost,uint64_t deadlineHost)
        :publisher_(0,arrivalAge,useAge),policy_(policy),lastTime_(startedHost),deadline_(deadlineHost),
         blocked_(!startedHost||deadlineHost<startedHost){}
    RetainedClockStartup(const RetainedClockStartup &)=delete;
    RetainedClockStartup &operator=(const RetainedClockStartup &)=delete;
    RetainedClockStartup(RetainedClockStartup &&)=delete;
    RetainedClockStartup &operator=(RetainedClockStartup &&)=delete;
    Result prepare(AudioWindowPublication<Capacity> &pcm,const SourceFrameClock &clock,
                   typename Reservation::Evidence evidence,uint64_t sequence,uint64_t now) noexcept {
        if(!time(now))return Result::blocked;
        if(reservation_)return fail(Result::blocked);
        typename Reservation::Result result;
        SourceClockReadiness::Result reason=SourceClockReadiness::Result::invalidEvidence;
        auto held=Reservation::acquire(pcm,clock,evidence,policy_,now,result,reason);
        if(!held)return fail(Result::notReady);
        if(publisher_.offer(clock,sequence,evidence.readyHost,now)!=GuardedClockPublisher::Result::ready)
            return fail(Result::clockRejected);
        reservation_=std::move(held);return Result::ready;
    }
    Result complete(uint64_t now,std::span<int32_t> output,SourceFrameClock::Stamp &stamp) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(!time(now))return Result::blocked;
        if(!reservation_)return fail(Result::notReady);
        if(reservation_->readInitial(now,output)!=Reservation::Result::retained)
            return fail(Result::notReady);
        SourceFrameClock::Stamp candidate;
        if(publisher_.advance(now,candidate)!=GuardedClockPublisher::Result::published){
            std::fill(output.begin(),output.end(),0);return fail(Result::clockRejected);
        }
        stamp=candidate;reservation_.reset();completed_=true;return Result::completed;
    }
    void stop() noexcept {if(!completed_)fail(Result::blocked);}
private:
    bool time(uint64_t now) noexcept {
        if(blocked_||completed_)return false;
        if(now<lastTime_||now>deadline_){fail(Result::blocked);return false;}
        lastTime_=now;return true;
    }
    Result fail(Result r) noexcept {blocked_=true;reservation_.reset();return r;}
    GuardedClockPublisher publisher_;SourceClockReadiness::Policy policy_;
    std::optional<Reservation> reservation_;
    uint64_t lastTime_,deadline_;bool blocked_=false,completed_=false;
};
}
