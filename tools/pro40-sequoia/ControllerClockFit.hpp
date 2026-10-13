// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RunningAffineFit.hpp"
namespace Pro40Audio {
// Content-side diagnostic consumer of local controller/host observations.
// No controller access or wire writes. Caller supplies actual consumed count.
struct ControllerClockPair {uint64_t before=0,uptime=0,after=0;uint32_t generation=0,cycle=0,status=0,packets=0;bool beforeReceive=false;};
struct ControllerClockPosition {uint64_t originHost=0,firstBusTicks=0,elapsedBusTicks=0,lastHost=0;uint32_t packets=0;};
template<class Accumulator=RunningAffineFit,uint64_t Limit=4096> class BasicControllerClockFit {
    static_assert(Limit>0 && Limit<=691200000);
public:
    using Pair=ControllerClockPair;
    using Position=ControllerClockPosition;
    BasicControllerClockFit(uint32_t generation,uint32_t numer,uint32_t denom)
        :generation_(generation),numer_(numer),denom_(denom),fit_(Limit){}
    bool add(const Pair &p,uint32_t consumed) noexcept {
        if(fault_)return false;
        auto fail=[this](){fault_=true;return false;};
        if(!numer_||!denom_||p.status||p.generation!=generation_||!p.uptime||
           p.before>p.uptime||p.uptime>p.after||p.packets!=consumed||
           (count_?p.beforeReceive:(!p.beforeReceive||consumed))||consumed<packets_)return fail();
        const uint32_t cycle=(p.cycle>>12)&8191,offset=p.cycle&4095;
        if(cycle>=8000||offset>=3072)return fail();
        // Behavioral field layout checked against references/linux-dice/ohci.c:
        // 1659-1667. Its 1670-1683 read-quirk discussion means this arithmetic
        // cannot certify controller accuracy. No reference implementation copied.
        const uint64_t ticks=uint64_t(p.cycle>>25)*ticksPerSecond+uint64_t(cycle)*3072+offset;
        uint64_t elapsed=elapsed_;
        if(count_){
            if(p.uptime<=lastHost_)return fail();
            const uint64_t seconds=count_==1?4:1; // Existing causal analyzer's bounded-gap policy.
            if(__uint128_t(p.uptime-lastHost_)*numer_>=__uint128_t(seconds)*1000000000*denom_)return fail();
            const auto step=(ticks+period-lastTicks_)%period;
            if(!step||step>=seconds*ticksPerSecond||elapsed>UINT64_MAX-step)return fail();
            elapsed+=step;
        }
        const auto origin=count_?origin_:p.uptime;
        const long double x=static_cast<long double>(p.uptime-origin)*numer_/denom_/1000000000.L;
        const long double y=static_cast<long double>(elapsed)/ticksPerSecond;
        if(!fit_.add(x,y))return fail();
        if(!count_)firstTicks_=ticks;
        origin_=origin;lastHost_=p.uptime;lastTicks_=ticks;elapsed_=elapsed;packets_=consumed;++count_;
        return true;
    }
    bool fit(RunningAffineFit::Fit &out) const noexcept {return !fault_&&fit_.fit(out);}
    bool position(Position &out) const noexcept {
        if(fault_||!count_)return false;
        out={origin_,firstTicks_,elapsed_,lastHost_,packets_};return true;
    }
private:
    static constexpr uint64_t ticksPerSecond=24576000,period=128*ticksPerSecond;
    uint32_t generation_,numer_,denom_,packets_=0;uint64_t count_=0,origin_=0,lastHost_=0,lastTicks_=0,elapsed_=0,firstTicks_=0;
    Accumulator fit_;bool fault_=false;
};
using ControllerClockFit=BasicControllerClockFit<>;
}
