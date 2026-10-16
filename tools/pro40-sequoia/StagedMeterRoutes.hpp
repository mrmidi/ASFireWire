// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "MeterRouteSnapshot.hpp"
namespace Pro40 {
// Read-only low-rate current routing, same layout as MeterRouteSnapshot.
// References: ALSA tcat/extension.rs:98, current_config_section.rs:29-86,
// focusrite/spro40.rs:153. One transaction per admitted advance, no retries.
// ENABLE brackets do not make the multi-read observation atomic.
class StagedMeterRoutes {
public:
    enum class State { waiting, pending, complete, failed, cancelled };
    enum class Error { none, generation, progress, age, io, bounds, count, enable };
    MeterRouteSnapshot snapshot{};
    unsigned started=0,completed=0,attempted=0;
private:
    const uint32_t generation_;
    const uint64_t global_;
    const unsigned trigger_,budget_;
    uint64_t offset_=0,size_=0;
    unsigned next_=0,previous_=0;
    State state_=State::waiting;
    Error error_=Error::none;
    State fail(Error e) {error_=e;return state_=State::failed;}
public:
    StagedMeterRoutes(uint32_t generation,uint64_t global,unsigned trigger,unsigned budget)
        :generation_(generation),global_(global),trigger_(trigger),budget_(budget) {}
    State state() const {return state_;}
    Error error() const {return error_;}
    template<class Read>
    State advance(unsigned progress,uint32_t generation,bool stop,bool deferred,Read read) {
        if(state_!=State::waiting && state_!=State::pending)return state_;
        if(stop)return state_=State::cancelled;
        if(generation!=generation_)return fail(Error::generation);
        if(progress<previous_ || progress>=budget_)return fail(Error::progress);
        previous_=progress;
        if(state_==State::pending && progress-started>1024)return fail(Error::age);
        if(progress<trigger_ || deferred)return state_;
        if(state_==State::waiting){state_=State::pending;started=progress;}
        constexpr uint64_t base=0xffffe0200000ULL,window=0x200000;
        std::array<uint8_t,36> raw{};
        const auto be=[](const uint8_t *p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];};
        const uint64_t address=next_==0?base+48:next_==2?base+offset_:global_+0x50;
        const unsigned bytes=next_==0?8:next_==2?36:4;
        attempted|=1u<<next_;
        snapshot.ioStatus=read(address,bytes,raw.data());
        if(snapshot.ioStatus)return fail(Error::io);
        switch(next_) {
        case 0:
            offset_=uint64_t(be(raw.data()))*4;size_=uint64_t(be(raw.data()+4))*4;
            if(offset_<72 || offset_>window || size_<36 || size_>window-offset_)return fail(Error::bounds);
            break;
        case 1:
            snapshot.enableBefore=be(raw.data());
            if(snapshot.enableBefore!=1)return fail(Error::enable);
            break;
        case 2:
            snapshot.count=be(raw.data());
            if(snapshot.count<8 || snapshot.count>1023 || 4+uint64_t(snapshot.count)*4>size_)return fail(Error::count);
            for(unsigned i=0;i<8;++i)snapshot.words[i]=be(raw.data()+4+4*i);
            break;
        case 3:
            snapshot.enableAfter=be(raw.data());
            if(snapshot.enableAfter!=1)return fail(Error::enable);
            snapshot.valid=true;completed=progress;state_=State::complete;
            break;
        }
        ++next_;return state_;
    }
};
}
