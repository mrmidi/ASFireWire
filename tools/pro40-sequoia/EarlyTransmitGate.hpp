// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace NativePackets {
struct EarlyTransmitGate {
    enum class Result {unexamined,ready,invalidWindow,guardFailure,readFailure,
                       timerRegression,deadline,stillEarly};
    unsigned reads=0;
    int32_t nativeStatus=0;
    bool ready=false,revalidated=false;
    Result result=Result::unexamined;
    // Only the hardware-observed one/two-cycle-early cases are deferrable. No buffer
    // mutation in this gate. Bounds do not guarantee native-call latency.
    template<class Guard,class Read,class Elapsed,class Pause>
    void wait(unsigned &lead,Guard guard,Read read,Elapsed elapsed,Pause pause,unsigned maximum=127) {
        if(maximum!=127 && maximum!=255){result=Result::invalidWindow;return;}
        const auto early=[&] {return lead==maximum+1 || lead==maximum+2;};
        if(!early()) {
            ready=lead>=64 && lead<=maximum;
            result=ready?Result::ready:Result::invalidWindow;
            return;
        }
        // A scheduling pause can expire the polling budget while `lead` still
        // holds an earlier observation. The budget limits polling, not ownership
        // of the completed bank. Permit one fresh, guarded timer snapshot, never
        // another retry or a descriptor mutation. Leave at least64 cycles before
        // the live terminal, with one/two extra cycles of conservative slack.
        const uint64_t deadline=uint64_t(maximum-64)*125;
        const auto snapshot=[&] {
            if(elapsed()>=deadline){result=Result::deadline;return false;}
            nativeStatus=guard();
            if(nativeStatus){result=Result::guardFailure;return false;}
            const unsigned previous=lead;
            ++reads;nativeStatus=read(lead);
            if(nativeStatus){result=Result::readFailure;return false;}
            nativeStatus=guard();
            if(nativeStatus){result=Result::guardFailure;return false;}
            if(elapsed()>=deadline){result=Result::deadline;return false;}
            if(lead>previous){result=Result::timerRegression;return false;}
            return true;
        };
        while(early()) {
            if(reads==16 || elapsed()>=1000)break;
            if(!snapshot())return;
            if(elapsed()>=1000)break;
        }
        if((reads==16 && early()) || elapsed()>=1000) {
            revalidated=true;
            // Sixteen fast RPCs can finish within one cycle. Yield one cycle
            // only in that case; after a host delay, reread immediately instead.
            if(elapsed()<1000) {
                nativeStatus=guard();
                if(nativeStatus){result=Result::guardFailure;return;}
                pause();
            }
            if(!snapshot())return;
        }
        ready=lead>=64 && lead<=maximum;
        result=ready?Result::ready:early()?Result::stillEarly:Result::invalidWindow;
    }
};
}
