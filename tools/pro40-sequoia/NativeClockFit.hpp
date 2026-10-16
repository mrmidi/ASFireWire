// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControllerClockFit.hpp"
#include "PacketClockFit.hpp"
namespace Pro40Audio {
// Single-owner evidence composition with interchangeable bounded accumulators. Receive packets must be
// supplied before the clock observation reporting that exact consumed prefix.
// Policy matches analyze-native-timing.py: anchored_epoch. No hardware access.
struct NativeClockSnapshot {
        RunningAffineFit::Fit packet,bus;
        ControllerClockFit::Position controller;
        uint64_t packetEpochShiftTicks=0,maxPacketAgeTicks=0,observations=0;
        uint32_t generation=0,numer=0,denom=0;
    };
template<class PacketFit=PacketClockFit,class ControllerFit=ControllerClockFit> class BasicNativeClockFit {
public:
    using Snapshot=NativeClockSnapshot;
    static constexpr uint64_t maximumPacketAgeTicks=1024*3072;
    BasicNativeClockFit(uint32_t generation,uint32_t numer,uint32_t denom)
        :packets_(generation),clocks_(generation,numer,denom),generation_(generation),numer_(numer),denom_(denom){}
    bool packet(const PacketClockFit::Packet &p) noexcept {
        if(fault_)return false;
        if(!anchored_||!packets_.add(p)){fault_=true;return false;}
        if(!count_)arrival0_=p.arrivalCycle;
        ++count_;return true;
    }
    bool clock(const ControllerClockFit::Pair &p) noexcept {
        if(fault_)return false;
        auto fail=[this](){fault_=true;return false;};
        if(!clocks_.add(p,count_))return fail();
        ControllerClockFit::Position pos;
        if(!clocks_.position(pos))return fail();
        if(!anchored_){anchored_=true;return true;}
        if(!count_)return true;
        if(!observations_){
            if(__uint128_t(pos.lastHost-pos.originHost)*numer_>=__uint128_t(8)*1000000000*denom_ ||
               !pos.elapsedBusTicks || pos.elapsedBusTicks>=period)return fail();
            const uint64_t start=arrival0_*3072;
            const uint64_t lower=pos.firstBusTicks/3072*3072;
            // Earliest nonnegative 8-second epoch putting the first arrival at
            // or after the pre-receive anchor's cycle. Negative epochs cannot
            // satisfy this bound: arrival0 is less than one full period.
            shift_=lower>start?((lower-start+period-1)/period)*period:0;
        }
        // A fresh consumed-prefix upper bound must select that same epoch.
        // 128 ms is diagnostic policy, not a hardware latency guarantee.
        const uint64_t last=(arrival0_+count_-1)*3072+shift_;
        // Packet and controller accumulators bound their count and tick arithmetic.
        const uint64_t bus=pos.firstBusTicks+pos.elapsedBusTicks;
        if(bus<last || bus-last>maximumPacketAgeTicks)return fail();
        const auto age=bus-last;if(age>maxAge_)maxAge_=age;
        ++observations_;return true;
    }
    bool snapshot(Snapshot &out) const noexcept {
        Snapshot candidate;
        if(fault_||!observations_||!clocks_.position(candidate.controller)||
           candidate.controller.packets!=count_||!packets_.fit(candidate.packet)||
           !clocks_.fit(candidate.bus))return false;
        candidate.packetEpochShiftTicks=shift_;candidate.maxPacketAgeTicks=maxAge_;
        candidate.observations=observations_;candidate.generation=generation_;
        candidate.numer=numer_;candidate.denom=denom_;out=candidate;return true;
    }
private:
    static constexpr uint64_t period=8*24576000ULL;
    PacketFit packets_;ControllerFit clocks_;
    uint32_t generation_,numer_,denom_,count_=0;
    uint64_t arrival0_=0,shift_=0,maxAge_=0,observations_=0;
    bool anchored_=false,fault_=false;
};
using NativeClockFit=BasicNativeClockFit<>;
}
