// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeClockProposal.hpp"
#include "RetainedClockStartup.hpp"
#include "NativeInputContinuation.hpp"
#include "NativeStartDescriptor.hpp"
namespace Pro40Audio {
// Offline finite startup owner. Decoded input1 and timing enter together; no
// wire parsing, hardware IO, ongoing playback or HAL publication is added.
template<size_t Capacity> class NativeAudioStartup {
public:
    enum class State {collecting,prepared,completed,failed};
    NativeAudioStartup(SourceFrameClock::Identity identity,SourceClockReadiness::Policy policy,
                       uint64_t horizon,uint64_t started,uint64_t deadline,uint64_t arrivalAge,uint64_t useAge)
        :identity_(identity),policy_(policy),horizon_(horizon),lastTime_(started),deadline_(deadline),
         fit_(identity.generation,identity.numer,identity.denom),window_(identity.epoch,0),publication_(window_),
         transaction_(policy,arrivalAge,useAge,started,deadline),useAge_(useAge){
        if(!identity.epoch||!identity.seed||!identity.numer||!identity.denom||!started||deadline<started||
           policy.capacityFrames!=Capacity||!policy.minimumPrefixFrames||!policy.initialReadFrames||
           policy.minimumPrefixFrames>Capacity||policy.initialReadFrames>Capacity||!policy.maximumEvidenceAgeTicks)fail();
    }
    bool packet(const PacketClockFit::Packet &p,const InputBlock *audio) noexcept {
        if(!active())return false;
        // Explicit frame identity prevents unrelated valid audio being silently
        // paired with timing. A blocking empty packet must have no audio block.
        if((p.blocks==8 && (!audio||audio->epoch!=identity_.epoch||audio->firstFrame!=p.firstFrame))||
           (p.blocks==0&&audio)||p.firstFrame!=window_.next())return fail();
        if(!fit_.packet(p))return fail();
        if(audio&&window_.append(*audio)!=WindowResult::ok)return fail();
        return true;
    }
    bool clock(const ControllerClockFit::Pair &p,uint64_t now) noexcept {
        if(!poll(now)||p.after>now||!fit_.clock(p))return fail();
        if(state_==State::prepared)return true;
        NativeClockFit::Snapshot s;
        if(!fit_.snapshot(s)||window_.next()<policy_.minimumPrefixFrames||window_.next()<policy_.initialReadFrames)return true;
        if(s.packet.count*8!=window_.next())return fail();
        SourceFrameClock clock;
        if(!nativeClockProposal(s,identity_,horizon_,clock)||publication_.tryPublish(window_)!=PublicationResult::ok)return fail();
        if(transaction_.prepare(publication_,clock,{identity_,window_.next(),p.after},s.bus.count,now)!=
           RetainedClockStartup<Capacity>::Result::ready)return fail();
        uint64_t horizonHost=0;
        if(!clock.hostTime(identity_,horizon_,horizonHost))return fail();
        const auto age=std::min(useAge_,policy_.maximumEvidenceAgeTicks);
        const auto evidenceExpiry=p.after>UINT64_MAX-age?UINT64_MAX:p.after+age;
        descriptor_={clock,s.bus.count,window_.next(),p.after,now,
                     std::min({deadline_,horizonHost,evidenceExpiry}),policy_.initialReadFrames};
        if(!descriptor_.validAt(now))return fail();
        state_=State::prepared;return true;
    }
    bool poll(uint64_t now) noexcept {
        if(!active())return false;
        if(now<lastTime_||now>deadline_)return fail();
        lastTime_=now;return true;
    }
    bool complete(uint64_t now,std::span<int32_t> output,SourceFrameClock::Stamp &stamp,
                  NativeStartDescriptor *descriptor=nullptr) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(!poll(now)||state_!=State::prepared||!descriptor_.validAt(now))return fail();
        if(transaction_.complete(now,output,stamp)!=RetainedClockStartup<Capacity>::Result::completed)return fail();
        if(descriptor)*descriptor=descriptor_;
        state_=State::completed;return true;
    }
    void stop() noexcept {if(active())fail();}
    State state() const noexcept {return state_;}
    // Caller must have handled the initial read. Copy the entire current window,
    // including unread prefix frames, into an independent owner exactly once.
    bool handoff(NativeInputContinuation<Capacity> &destination) noexcept {
        if(state_!=State::completed||handedOff_)return false;
        if(!destination.adopt(window_,identity_,policy_.initialReadFrames))return false;
        handedOff_=true;return true;
    }
private:
    bool active() const noexcept {return state_==State::collecting||state_==State::prepared;}
    bool fail() noexcept {
        if(state_!=State::completed){state_=State::failed;transaction_.stop();}return false;
    }
    SourceFrameClock::Identity identity_;SourceClockReadiness::Policy policy_;
    uint64_t horizon_,lastTime_,deadline_;State state_=State::collecting;bool handedOff_=false;
    NativeClockFit fit_;AudioInputWindow<Capacity> window_;AudioWindowPublication<Capacity> publication_;
    // Destroy reservation first, before its publication storage.
    RetainedClockStartup<Capacity> transaction_;
    uint64_t useAge_;NativeStartDescriptor descriptor_{};
};
}
