// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeSessionRunner.hpp"
#include <IOKit/IOReturn.h>
#include <chrono>
#include <thread>
namespace Pro40Audio {
// Source/supervisor thread only, after capture callbacks have quiesced. Consumer
// owns copies, not native mappings. Pump/ended/cancel are bounded and nonblocking.
// END receipt means sender acceptance, not remote HAL playback/drain confirmation;
// remote confirmation remains part of peer cleanup. No endpoint ownership here.
template<class Consumer> class NativeSourceCompletion {
    using Wall=std::chrono::steady_clock;
    Consumer &consumer_;Wall::duration budget_;bool finished_=false;int32_t result_=0;
    int32_t fail(int32_t status){consumer_.cancel();return result_=status;}
public:
    NativeSourceCompletion(Consumer &consumer,Wall::duration budget):consumer_(consumer),budget_(budget){}
    NativeSourceCompletion(const NativeSourceCompletion &)=delete;
    int32_t finish(bool accepted,Pro40::StopQuery stop) noexcept {
        if(finished_)return result_;
        finished_=true;
        if(!accepted){consumer_.cancel();return 0;} // Existing source error remains authoritative.
        if(stop.requested())return fail(kIOReturnAborted);
        if(budget_<=Wall::duration::zero())return fail(kIOReturnBadArgument);
        const auto deadline=Wall::now()+budget_;
        if(!consumer_.finish(true))return fail(kIOReturnError);
        for(;;){
            if(stop.requested())return fail(kIOReturnAborted);
            if(Wall::now()>=deadline)return fail(kIOReturnTimeout);
            if(!consumer_.poll())return fail(kIOReturnError);
            if(consumer_.ended())return 0;
            const auto next=Wall::now()+std::chrono::microseconds(100);
            std::this_thread::sleep_until(next<deadline?next:deadline);
        }
    }
    Pro40::SourceCompletion hook() noexcept {
        return {[](void *p,bool accepted,Pro40::StopQuery stop) noexcept{
            return static_cast<NativeSourceCompletion *>(p)->finish(accepted,stop);
        },this};
    }
};
}
