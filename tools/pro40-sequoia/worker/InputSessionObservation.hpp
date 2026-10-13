// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../RecordedInputSession.hpp"
#include "../NativeFiniteInputSession.hpp"
#include "../NativeTimingEvidence.hpp"
#include "RecordingExport.hpp"
#include <memory>
#include <new>
namespace Pro40 {
// Optional serialized diagnostic sink. Allocate/touch storage before capture.
// No PCM is exported. Compare retained output against the recorder after teardown.
// Joint finite PCM/timestamp observation is the sole diagnostic path.
class InputSessionObservation {
    using Session=Pro40Audio::NativeFiniteInputSession<8192>;
    using Result=Session::Result;
    static constexpr size_t capacity=262144*8;
    const Pro40Packets::InputRecording &recording_;uint32_t numer_,denom_;
    std::unique_ptr<int32_t[]> samples_{new(std::nothrow) int32_t[capacity]()};
    size_t copied_=0;bool blocked_=false,finished_=false;
    std::optional<Session> session_;
    std::optional<Pro40Audio::RecordedInputSession<8192,Session>> adapter_;
    uint64_t stamps_=0,advanceCalls_=0,maximumStampLag_=0;bool lateStamp_=false;
    Pro40Audio::SourceFrameClock::Stamp lastStamp_{};
    uint64_t ticks(uint64_t ns)const noexcept{return uint64_t(__uint128_t(ns)*denom_/numer_);}
    void acceptStamp(const Pro40Audio::SourceFrameClock::Stamp &stamp,uint64_t now) noexcept {
        if(stamp.sample!=double(stamps_*Pro40Audio::SourceFrameClock::period)||stamp.sample>=double(copied_)||
           stamp.seed!=1||stamp.host>now||(stamps_&&stamp.host<=lastStamp_.host)){
            failed();return;
        }
        maximumStampLag_=std::max(maximumStampLag_,now-stamp.host);
        if(now-stamp.host>ticks(250000000)){lateStamp_=true;failed();return;}
        lastStamp_=stamp;++stamps_;
    }
    void advanceClock(uint64_t now) noexcept {
        if(blocked_||!adapter_)return;
        // Exactly one attempt per clock/poll/finalize call. No catch-up loop.
        ++advanceCalls_;Pro40Audio::SourceFrameClock::Stamp stamp;
        const auto result=adapter_->advance(now,stamp);
        using C=Session::ClockResult;
        if(result==C::published)acceptStamp(stamp,now);
        else if(result!=C::waiting&&result!=C::unavailable)failed();
    }
    void drain(uint64_t now,bool final) noexcept {
        // At most one8192-frame retained window per callback. Never spin on wait.
        for(unsigned n=0;n<16&&!blocked_;++n){
            const auto left=final?session_->produced()-copied_:512;
            if(final&&!left)break;
            const size_t count=std::min<uint64_t>(512,left);
            if(count>capacity-copied_){if(!final&&copied_==session_->produced())return;failed();return;}
            const auto result=adapter_->read({samples_.get()+copied_,count},now);
            if(result==Result::ok)copied_+=count;
            else if(!final&&result==Result::waiting)return;
            else {failed();return;}
        }
        if(final&&copied_!=session_->produced())failed();
    }
public:
    InputSessionObservation(const Pro40Packets::InputRecording &recording,uint32_t numer,uint32_t denom)
        :recording_(recording),numer_(numer),denom_(denom){
        if(!samples_||!numer||!denom||!recording.valid()||recording.fault()||!recording.pcm().empty())failed();
    }
    bool blocked()const noexcept{return blocked_;}
    bool finished()const noexcept{return finished_;}
    size_t copied()const noexcept{return copied_;}
    uint64_t stamps()const noexcept{return stamps_;}
    bool lateStamp()const noexcept{return lateStamp_;}
    void failed() noexcept {blocked_=true;if(adapter_)adapter_->failed();}
    void packet(size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &p,uint64_t now) noexcept {
        if(blocked_)return;
        if(finished_){failed();return;}
        if(!adapter_||!adapter_->record(ordinal,p,now))failed();
    }
    void clock(const NativeTimingEvidence::Clock &p,uint64_t now,bool fault) noexcept {
        if(blocked_)return;
        if(finished_){failed();return;}
        if(fault){failed();return;}
        if(!session_){
            const auto duration=ticks(4000000000);
            if(!p.beforeReceive||p.before>UINT64_MAX-duration){failed();return;}
            session_.emplace(Pro40Audio::SourceFrameClock::Identity{1,1,p.generation,numer_,denom_},
                Pro40Audio::SourceClockReadiness::Policy{4096,512,8192,ticks(50000000)},
                2880000,p.before,p.before+duration,ticks(50000000),ticks(100000000),
                Pro40Audio::NativeInputSession<8192>::TimingPolicy{ticks(1000000000),ticks(250000000)},ticks(1000),uint64_t(32));
            adapter_.emplace(*session_,recording_,1);
        }
        if(!adapter_->clock({p.before,p.uptime,p.after,p.generation,p.cycle,p.status,p.packetCount,p.beforeReceive},now)){failed();return;}
        if(session_->state()==Session::State::ready){
            Pro40Audio::SourceFrameClock::Stamp stamp;
            if(!adapter_->completeStartup(now,{samples_.get(),512},stamp)){failed();return;}
            copied_=512;
            acceptStamp(stamp,now);
        }
        if(session_->state()==Session::State::running)drain(now,false);
        advanceClock(now);
    }
    void poll(uint64_t now) noexcept {
        if(!blocked_&&adapter_){
            if(!adapter_->poll(now))failed();else advanceClock(now);
        }
    }
    void finish(uint64_t now,bool nativeAccepted) noexcept {
        if(blocked_||finished_)return;
        if(!nativeAccepted||!adapter_||!adapter_->finish(session_->produced(),now)){failed();return;}
        drain(now,true);
        advanceClock(now);
        finished_=!blocked_&&session_->state()==Session::State::finished;
        if(!finished_)failed();
    }
    bool matchesRecording()const noexcept {
        if(blocked_||!finished_||!recording_.valid()||recording_.fault()||recording_.pcm().size()!=copied_*3)return false;
        const auto pcm=recording_.pcm();
        for(size_t n=0;n<copied_;++n){const auto bits=uint32_t(samples_[n]);
            for(size_t j=0;j<3;++j)if(pcm[n*3+j]!=uint8_t(bits>>(8*j)))return false;}
        return true;
    }
    bool exportReport(const std::string &log,const NativeSessionResult &native){
        if(!finished_)failed();
        char text[800];const int n=std::snprintf(text,sizeof(text),
            "{\"schema\":2,\"diagnostic_only\":true,\"finite_clock\":true,\"native_accepted\":%s,\"blocked\":%s,\"finished\":%s,\"copied_frames\":%llu,\"samples_match\":%s,\"stamps\":%llu,\"last_frame\":%.0f,\"last_host\":%llu,\"advance_calls\":%llu,\"max_stamp_lag_ticks\":%llu,\"late_stamp\":%s,\"numer\":%u,\"denom\":%u}\n",
            native.accepted()?"true":"false",blocked_?"true":"false",finished_?"true":"false",
            (unsigned long long)copied_,matchesRecording()?"true":"false",(unsigned long long)stamps_,lastStamp_.sample,
            (unsigned long long)lastStamp_.host,(unsigned long long)advanceCalls_,(unsigned long long)maximumStampLag_,lateStamp_?"true":"false",numer_,denom_);
        return n>0&&size_t(n)<sizeof(text)&&publishRecordingFile(log+".input-session.json",{reinterpret_cast<const uint8_t *>(text),size_t(n)});
    }
};
}
