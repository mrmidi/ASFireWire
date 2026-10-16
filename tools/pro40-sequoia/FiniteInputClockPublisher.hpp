// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "GuardedClockPublisher.hpp"
namespace Pro40Audio {
// Offline finite-recording policy, NOT the clock of a running HAL device.
// One serialized owner must report only a contiguous, successfully delivered
// PCM prefix [0,end). A count is not a buffer lease or proof of sample content.
// END is exclusive: never fabricate a final stamp or round END up to a period.
class FiniteInputClockPublisher {
public:
    using Clock=SourceFrameClock;using Stamp=Clock::Stamp;
    using Result=GuardedClockPublisher::Result;
    enum class State {running,sealed,ended,failed};
    FiniteInputClockPublisher(Clock::Identity identity,uint64_t correction,
                             uint64_t arrivalAge,uint64_t useAge)
        :identity_(identity),publisher_(correction,arrivalAge,useAge){
        if(!identity.epoch||!identity.seed||!identity.numer||!identity.denom||
           !arrivalAge||useAge<arrivalAge)fail(Result::invalid);
    }
    State state()const noexcept{return state_;}
    Result fault()const noexcept{return fault_;}
    uint64_t deliveredFrames()const noexcept{return delivered_;}
    bool delivered(Clock::Identity identity,uint64_t end,uint64_t now) noexcept {
        if(!time(now))return false;
        if(state_!=State::running||identity!=identity_||end<delivered_||end>Clock::maxExactFrame){
            fail(identity!=identity_?Result::identity:Result::invalid);return false;
        }
        delivered_=end;return true;
    }
    Result offer(const Clock &clock,uint64_t sequence,uint64_t observed,uint64_t now) noexcept {
        if(!time(now))return Result::blocked;
        if(state_!=State::running)return fail(Result::invalid);
        if(clock.identity!=identity_)return fail(Result::identity);
        const auto result=publisher_.offer(clock,sequence,observed,now);
        return result==Result::ready?result:fail(result);
    }
    bool finish(uint64_t expectedEnd,uint64_t now) noexcept {
        if(!time(now))return false;
        if(state_!=State::running||expectedEnd!=delivered_){fail(Result::invalid);return false;}
        state_=next_>=delivered_?State::ended:State::sealed;return true;
    }
    Result advance(uint64_t now,Stamp &out) noexcept {
        if(!time(now))return Result::blocked;
        // Ended is a historical result, not authorization to use an old clock.
        if(state_==State::ended)return Result::unavailable;
        // Trial the small value-owned publisher. Waiting for PCM must not consume
        // a clock boundary; expiry/correction/ordering faults must still surface.
        auto trial=publisher_;Stamp candidate;
        const auto result=trial.advance(now,candidate);
        if(result==Result::waiting||result==Result::unavailable)return result;
        if(result!=Result::published)return fail(result);
        if(candidate.sample!=double(next_)||candidate.seed!=identity_.seed)return fail(Result::invalid);
        if(next_>=delivered_)return Result::waiting;
        publisher_=trial;out=candidate;next_+=Clock::period;
        if(state_==State::sealed&&next_>=delivered_)state_=State::ended;
        return Result::published;
    }
    void stop()noexcept{fail(Result::blocked);}
    // Diagnostic history only, including after failure or END.
    bool lastCommitted(Stamp &out)const noexcept{return publisher_.lastCommitted(out);}
private:
    bool time(uint64_t now)noexcept{
        if(state_==State::failed)return false;
        if(now<lastTime_){fail(Result::outOfOrderEvidence);return false;}
        lastTime_=now;return true;
    }
    Result fail(Result reason)noexcept{
        if(state_!=State::failed)fault_=reason;
        state_=State::failed;return reason;
    }
    Clock::Identity identity_;GuardedClockPublisher publisher_;
    uint64_t delivered_=0,next_=0,lastTime_=0;
    State state_=State::running;Result fault_=Result::ready;
};
}
