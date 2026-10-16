// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../RecordedAudioStartup.hpp"
#include "../NativeTimingEvidence.hpp"
#include "../StartupSampleComparison.hpp"
#include "RecordingExport.hpp"
#include <optional>
namespace Pro40 {
// Finite, serialized diagnostic. Construct storage before native setup; initialize
// the owner at the pre-receive anchor. Completion is historical startup evidence,
// not permission to publish audio or evidence that the later session succeeded.
class StartupObservation {
public:
    enum class State {waiting,collecting,completed,failed};
    StartupObservation(const Pro40Packets::InputRecording &recording,uint32_t numer,uint32_t denom)
        :recording_(recording),numer_(numer),denom_(denom){}
    void failed() noexcept {
        if(state_==State::completed)return;
        if(adapter_)adapter_->failed();state_=State::failed;
    }
    void packet(size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &p) noexcept {
        if(terminal())return;
        if(!adapter_||!adapter_->record(ordinal,p)){failed();return;}
        ++packets_;frames_+=p.blocks;
    }
    void clock(const NativeTimingEvidence::Clock &p,uint64_t now,bool fault) noexcept {
        if(terminal())return;
        if(fault){failed();return;}
        if(state_==State::waiting){
            if(!p.beforeReceive||!numer_||!denom_){failed();return;}
            const uint64_t deadline=ticks(4000000000),age=ticks(50000000);
            if(!deadline||!age||p.before>UINT64_MAX-deadline){failed();return;}
            owner_.emplace(Pro40Audio::SourceFrameClock::Identity{1,1,p.generation,numer_,denom_},
                Pro40Audio::SourceClockReadiness::Policy{4096,512,8192,age},2880000,
                p.before,p.before+deadline,age,ticks(100000000));
            adapter_.emplace(*owner_,recording_,1);state_=State::collecting;
        }
        if(!adapter_->clock({p.before,p.uptime,p.after,p.generation,p.cycle,p.status,p.packetCount,p.beforeReceive},now)){
            failed();return;
        }
        if(owner_->state()==Pro40Audio::NativeAudioStartup<8192>::State::prepared){
            if(!adapter_->complete(now,samples_,stamp_)){failed();return;}
            state_=State::completed;
        }
    }
    void poll(uint64_t now) noexcept {
        if(!terminal()&&adapter_&&!adapter_->poll(now))failed();
    }
    State state() const noexcept {return state_;}
    std::span<const int32_t> samples() const noexcept {return samples_;}
    Pro40Audio::StartupSampleComparison sampleComparison() const noexcept {
        if(state_!=State::completed)return Pro40Audio::StartupSampleComparison::unavailable;
        return Pro40Audio::compareStartupSamples(samples_,recording_.pcm(),recording_.valid()&&!recording_.fault());
    }
    bool exportReport(const std::string &log,const NativeSessionResult &native){
        // A run that ended before startup cannot leave a reservation pending.
        if(!terminal())failed();
        char text[700];
        const int n=std::snprintf(text,sizeof(text),
            "{\"schema\":2,\"diagnostic_only\":true,\"native_accepted\":%s,\"state\":%d,\"packets\":%llu,\"frames\":%llu,\"initial_frames\":%u,\"first_frame\":%.0f,\"first_host\":%llu,\"seed\":%llu,\"sample_comparison\":%d}\n",
            native.accepted()?"true":"false",int(state_),(unsigned long long)packets_,(unsigned long long)frames_,
            state_==State::completed?512u:0u,stamp_.sample,(unsigned long long)stamp_.host,(unsigned long long)stamp_.seed,int(sampleComparison()));
        return n>0&&size_t(n)<sizeof(text)&&publishRecordingFile(log+".audio-startup.json",{reinterpret_cast<const uint8_t *>(text),size_t(n)});
    }
private:
    bool terminal() const noexcept {return state_==State::completed||state_==State::failed;}
    uint64_t ticks(uint64_t ns) const noexcept {return uint64_t(__uint128_t(ns)*denom_/numer_);}
    const Pro40Packets::InputRecording &recording_;uint32_t numer_,denom_;
    State state_=State::waiting;uint64_t packets_=0,frames_=0;
    std::array<int32_t,512> samples_{};Pro40Audio::SourceFrameClock::Stamp stamp_{};
    std::optional<Pro40Audio::NativeAudioStartup<8192>> owner_;
    // Adapter dies first; owner releases its reservation before its own storage.
    std::optional<Pro40Audio::RecordedAudioStartup<8192>> adapter_;
};
}
