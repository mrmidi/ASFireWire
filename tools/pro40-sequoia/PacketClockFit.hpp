// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RunningAffineFit.hpp"
namespace Pro40Audio {
// Content-side consumer of decoded 48 kHz blocking-mode timing records.
// Fixed storage, explicit capture limit; no IO, transport parsing or quality admission.
// The fit uses the offline analyzer's packet-first-frame SYT convention. It does
// not certify the SYT-to-ADC relationship or align timestamps to a controller epoch.
struct PacketClockRecord {
        uint64_t ordinal=0,firstFrame=0;
        uint32_t generation=0,arrivalCycle=0,syt=0,blocks=0,dbc=0;
    };
template<class Accumulator=RunningAffineFit,uint64_t Limit=262144> class BasicPacketClockFit {
public:
    using Packet=PacketClockRecord;
    explicit BasicPacketClockFit(uint32_t generation):generation_(generation),fit_(limit){}
    bool add(const Packet &p) noexcept {
        if(fault_)return false;
        auto fail=[this](){fault_=true;return false;};
        if(count_>=limit || p.ordinal!=count_ || p.firstFrame!=frames_ ||
           p.generation!=generation_ || p.arrivalCycle>=64000 ||
           (count_ && p.arrivalCycle!=(arrival0_+count_)%64000) ||
           (p.blocks!=0 && p.blocks!=8) || p.dbc>255 ||
           (count_ && p.dbc!=nextDbc_))return fail();
        const auto first=count_?arrival0_:p.arrivalCycle;
        // Cross-checked behavior: references/linux-dice/amdtp-stream.c:138-147,
        // 353-366 (48 kHz blocking shape), 469-488 (forward SYT expansion).
        // Retain raw SYT; do not subtract Linux's transmission transfer delay.
        if(p.blocks){
            if(p.syt>=65535 || (p.syt&4095)>=3072)return fail();
            const uint64_t phase=(((p.syt>>12)+16-(p.arrivalCycle%16))%16)*3072+(p.syt&4095);
            const uint64_t ticks=(first+count_)*3072+phase;
            if(haveSyt_ && (ticks<=lastSyt_ || ticks-lastSyt_>=49152))return fail();
            if(!fit_.add(static_cast<long double>(frames_),static_cast<long double>(ticks)))return fail();
            lastSyt_=ticks;haveSyt_=true;
        }else if(p.syt!=65535)return fail();
        arrival0_=first;nextDbc_=(p.dbc+p.blocks)%256;frames_+=p.blocks;++count_;
        return true;
    }
    bool fit(RunningAffineFit::Fit &out) const noexcept {return !fault_&&fit_.fit(out);}
private:
    // Default matches finite evidence; sustained instantiations retain the same validation.
    // All admitted limits bound products and fit within controller packet counts.
    static constexpr uint64_t limit=Limit;
    static_assert(limit>0 && limit<=691200000); // Sustained native 24-hour counter guard.
    uint32_t generation_,nextDbc_=0;
    uint64_t count_=0,frames_=0,arrival0_=0,lastSyt_=0;
    Accumulator fit_;
    bool haveSyt_=false,fault_=false;
};
using PacketClockFit=BasicPacketClockFit<>;
}
