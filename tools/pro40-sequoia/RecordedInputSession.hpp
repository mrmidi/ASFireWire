// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "InputRecording.hpp"
#include "InputTimingEvidence.hpp"
#include "NativeInputSession.hpp"
namespace Pro40Audio {
// Serialized content adapter. Recorder appends first, then exactly one timing
// callback (including empty packets). Session and recorder outlive this adapter.
// Installs no callbacks; the live owner must forward failures and idle polling.
// SessionOwner can be the input-only owner (default, existing worker) or the
// joint finite owner. Both use this same decoding and recorder-validation path.
template<size_t Capacity,class SessionOwner=NativeInputSession<Capacity>> class RecordedInputSession {
public:
    using Session=SessionOwner;
    using Result=typename Session::Result;
    RecordedInputSession(Session &session,const Pro40Packets::InputRecording &recording,uint64_t epoch)
        :session_(session),recording_(recording),epoch_(epoch){if(!epoch||!unchanged())failed();}
    RecordedInputSession(const RecordedInputSession &)=delete;
    RecordedInputSession &operator=(const RecordedInputSession &)=delete;
    bool blocked() const noexcept {return blocked_;}
    bool record(size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &r,uint64_t now) noexcept {
        if(blocked_||!recording_.valid()||recording_.fault()||ordinal!=packets_||r.firstFrame!=frames_||
           (r.blocks!=0&&r.blocks!=8))return failed();
        const auto pcm=recording_.pcm();
        if(pcm.size()%3||pcm.size()/3!=frames_+r.blocks)return failed();
        InputBlock block{epoch_,frames_,{}};
        if(r.blocks){
            const auto tail=pcm.last(24);
            for(size_t n=0;n<8;++n){
                const uint32_t bits=uint32_t(tail[n*3])|(uint32_t(tail[n*3+1])<<8)|(uint32_t(tail[n*3+2])<<16);
                block.inputOne[n]=static_cast<int32_t>(bits)-((bits&0x800000)?0x1000000:0);
            }
        }
        if(!session_.packet({ordinal,r.firstFrame,r.generation,r.arrivalCycle,r.syt,r.blocks,r.dbc},r.blocks?&block:nullptr,now))return failed();
        ++packets_;frames_+=r.blocks;return true;
    }
    bool clock(const ControllerClockFit::Pair &p,uint64_t now) noexcept {
        return unchanged()&&session_.clock(p,now)?true:failed();
    }
    bool poll(uint64_t now) noexcept {return unchanged()&&session_.poll(now)?true:failed();}
    bool completeStartup(uint64_t now,std::span<int32_t> output,SourceFrameClock::Stamp &stamp,
                         NativeStartDescriptor *descriptor=nullptr) noexcept {
        std::fill(output.begin(),output.end(),0);
        return unchanged()&&session_.completeStartup(now,output,stamp,descriptor)?true:failed();
    }
    Result read(std::span<int32_t> output,uint64_t now) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(!unchanged()){failed();return Result::failed;}
        const auto result=session_.read(output,now);if(result==Result::failed)failed();return result;
    }
    // Available only for a joint audio/clock owner. Timestamp access must check
    // recorder health/count too, even when no read or packet callback occurs.
    auto advance(uint64_t now,SourceFrameClock::Stamp &out) noexcept
        requires requires(Session &s,SourceFrameClock::Stamp &stamp){s.advance(uint64_t{},stamp);}
    {
        using ClockResult=decltype(session_.advance(now,out));
        if(!unchanged()){failed();return ClockResult::blocked;}
        const auto result=session_.advance(now,out);
        if(result!=ClockResult::published&&result!=ClockResult::waiting&&result!=ClockResult::unavailable)failed();
        return result;
    }
    bool finish(uint64_t expectedFrames,uint64_t now) noexcept {
        return unchanged()&&expectedFrames==frames_&&session_.finish(expectedFrames,now)?true:failed();
    }
    bool failed() noexcept {blocked_=true;session_.stop();return false;}
private:
    bool unchanged() const noexcept {
        return !blocked_&&recording_.valid()&&!recording_.fault()&&recording_.pcm().size()%3==0&&recording_.pcm().size()/3==frames_;
    }
    Session &session_;const Pro40Packets::InputRecording &recording_;
    uint64_t epoch_,frames_=0,packets_=0;bool blocked_=false;
};
}
