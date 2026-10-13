// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40 {
// Diagnostic only. No IO or sleep: caller polls cancellation on control thread.
// Times share native cleanup origin. Never extends that cleanup deadline.
class PreCloseQuietHold {
public:
    enum class State {pending,complete,cancelled,insufficientBudget,invalidTime,expired};
    static constexpr uint64_t duration=3000000;
    static constexpr uint64_t cleanupLimit=10000000;
    static constexpr uint64_t closeReserve=2000000;
private:
    uint64_t last_,due_=0;
    State state_=State::pending;
public:
    explicit PreCloseQuietHold(uint64_t elapsed):last_(elapsed) {
        // Subtraction after bounds check avoids unsigned overflow.
        if(elapsed>=cleanupLimit-closeReserve-duration)
            state_=State::insufficientBudget;
        else due_=elapsed+duration;
    }
    State advance(uint64_t elapsed,bool cancelled) {
        if(state_!=State::pending)return state_;
        if(elapsed<last_)return state_=State::invalidTime;
        last_=elapsed;
        if(cancelled)return state_=State::cancelled;
        // Scheduler delays must not be presented as an on-time observation.
        if(elapsed>=cleanupLimit-closeReserve)return state_=State::expired;
        if(elapsed>=due_)state_=State::complete;
        return state_;
    }
    State state()const{return state_;}
};
}
