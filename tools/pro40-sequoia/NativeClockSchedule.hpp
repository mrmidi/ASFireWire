// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40Audio {
// Single-owner proposal selection over validated prefixes, not a timer or a
// quality gate. Caller supplies counts from NativeClockFit snapshots only.
class NativeClockSchedule {
public:
    struct Policy {uint64_t minimumFrames=0,pairsPerUpdate=0;};
    enum class Result {waiting,selected,invalid};
    explicit NativeClockSchedule(Policy p):policy_(p){}
    Result observe(uint64_t pairs,uint64_t frames) noexcept {
        if(fault_)return Result::invalid;
        if(!policy_.minimumFrames||!policy_.pairsPerUpdate||!pairs||
           pairs<lastPairs_||frames<lastFrames_||
           (pairs==lastPairs_&&frames!=lastFrames_)){
            fault_=true;return Result::invalid;
        }
        if(pairs==lastPairs_)return Result::waiting;
        lastPairs_=pairs;lastFrames_=frames;
        if(frames<policy_.minimumFrames ||
           (selected_&&pairs-selected_<policy_.pairsPerUpdate))return Result::waiting;
        selected_=pairs;return Result::selected;
    }
private:
    Policy policy_;uint64_t lastPairs_=0,lastFrames_=0,selected_=0;bool fault_=false;
};
}
