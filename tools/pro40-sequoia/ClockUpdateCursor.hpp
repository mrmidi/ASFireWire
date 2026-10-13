// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SourceFrameClock.hpp"
namespace Pro40Audio {
// Offline single-owner publication policy model. Not wired to HAL. Each call
// publishes exactly the next period boundary, never revises a committed stamp.
// Clock quality and the caller's correction budget require separate admission.
class ClockUpdateCursor {
public:
    using Clock=SourceFrameClock;
    using Stamp=Clock::Stamp;
    enum class Result {published,waiting,invalid,identity,expired,correctionTooLarge,backward};
    explicit ClockUpdateCursor(const Clock &initial):active_(initial){}
    Result start(uint64_t now,Stamp &out) noexcept {
        if(started_ || !active_.valid())return Result::invalid;
        uint64_t end=0;active_.hostTime(active_.identity,active_.horizonFrames,end);
        if(now>end)return Result::expired;
        if(now<active_.origin)return Result::waiting;
        last_={0,active_.origin,active_.identity.seed};started_=true;out=last_;
        return Result::published;
    }
    Result next(const Clock &proposal,uint64_t now,uint64_t maxCorrectionTicks,Stamp &out) noexcept {
        if(!started_ || !proposal.valid())return Result::invalid;
        if(proposal.identity!=active_.identity)return Result::identity;
        if(now<last_.host)return Result::backward;
        if(frame_>Clock::maxExactFrame-Clock::period)return Result::expired;
        const uint64_t nextFrame=frame_+Clock::period;
        uint64_t oldHost=0,newHost=0,oldEnd=0,newEnd=0;
        if(!active_.hostTime(active_.identity,nextFrame,oldHost) ||
           !proposal.hostTime(proposal.identity,nextFrame,newHost) ||
           !active_.hostTime(active_.identity,active_.horizonFrames,oldEnd) ||
           !proposal.hostTime(proposal.identity,proposal.horizonFrames,newEnd) ||
           now>oldEnd || now>newEnd)return Result::expired;
        const uint64_t correction=newHost>oldHost?newHost-oldHost:oldHost-newHost;
        if(correction>maxCorrectionTicks)return Result::correctionTooLarge;
        if(newHost<=last_.host)return Result::backward;
        if(newHost>now)return Result::waiting;
        frame_=nextFrame;active_=proposal;
        last_={double(frame_),newHost,proposal.identity.seed};out=last_;
        return Result::published;
    }
    bool last(Stamp &out) const noexcept {
        if(!started_)return false;
        out=last_;return true;
    }
private:
    Clock active_;Stamp last_{};uint64_t frame_=0;bool started_=false;
};
}
