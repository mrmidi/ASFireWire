// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControlInputPeak.hpp"
#include <array>
#include <cstdint>
namespace Pro40 {
// Finite diagnostic policy, no native ownership/clock/stream operations.
// Native backend must check unowned/disabled/generation around each peak read.
// Timing is a host observation bracket, not the physical meter/LED transition.
class PostReleasePeakWindow {
public:
    struct Sample {uint64_t begin=0,end=0;ControlInputPeak peak{};};
    enum class State {pending,complete,cancelled,expired,invalidTime,invalidSample};
    static constexpr std::array<uint64_t,6> offsets{0,250000,500000,1000000,2000000,3000000};
    static constexpr uint64_t limit=3500000;
    static constexpr uint64_t lateness=250000;
private:
    std::array<Sample,6> samples_{};
    unsigned used_=0;
    uint64_t last_=0;
    bool initialized_=false;
    State state_=State::pending;
public:
    // Call on the control thread; elapsed is relative to this window's origin.
    // No waiting in this policy. Caller services cancellation between short waits.
    template<class Capture,class Now>
    State advance(uint64_t elapsed,bool cancelled,Capture capture,Now now){
        if(state_!=State::pending)return state_;
        if(initialized_&&elapsed<last_)return state_=State::invalidTime;
        initialized_=true;last_=elapsed;
        if(cancelled)return state_=State::cancelled;
        if(elapsed>=limit)return state_=State::expired;
        if(elapsed<offsets[used_])return state_;
        // Do not bunch missed observations together and present them as a
        // three-second time series. A late run is incomplete, never silence.
        if(elapsed-offsets[used_]>=lateness)return state_=State::expired;
        auto &sample=samples_[used_++];sample.begin=elapsed;
        capture(sample.peak);sample.end=now();
        if(sample.end<sample.begin)return state_=State::invalidTime;
        last_=sample.end;
        if(!sample.peak.valid)return state_=State::invalidSample;
        if(sample.end>=limit)return state_=State::expired;
        if(used_==samples_.size())state_=State::complete;
        return state_;
    }
    unsigned size()const{return used_;}
    State state()const{return state_;}
    const Sample &operator[](unsigned index)const{return samples_[index];}
};
}
