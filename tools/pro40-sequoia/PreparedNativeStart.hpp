// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartDescriptor.hpp"
#include <algorithm>
#include <array>
#include <span>
namespace Pro40Audio {
// Serialized, single-use owned initial PCM handoff. No threads, native owner
// pointer, HAL admission or hardware cleanup authority. Caller must keep polling
// the native session and stop it if this handoff fails/cancels/expires.
template<size_t Frames=512> class PreparedNativeStart {
    static_assert(Frames>0&&Frames%8==0);
public:
    enum class State {empty,prepared,accepted,failed};
    PreparedNativeStart()=default;
    PreparedNativeStart(const PreparedNativeStart &)=delete;
    PreparedNativeStart &operator=(const PreparedNativeStart &)=delete;
    State state()const noexcept{return state_;}
    template<class Owner> bool prepare(Owner &owner,uint64_t now) noexcept {
        if(state_!=State::empty)return reject();
        SourceFrameClock::Stamp stamp;NativeStartDescriptor candidate;
        if(!owner.completeStartup(now,samples_,stamp,&candidate)||!candidate.validAt(now)||
           candidate.initialFrames!=Frames||stamp.sample!=0||
           stamp.host!=candidate.clock.origin||stamp.seed!=candidate.clock.identity.seed)return reject();
        for(auto sample:samples_)if(sample< -8388608||sample>8388607)return reject();
        descriptor_=candidate;lastTime_=now;state_=State::prepared;return true;
    }
    bool proposal(uint64_t now,NativeStartDescriptor &out) noexcept {
        if(!poll(now))return false;
        out=descriptor_;return true;
    }
    bool accept(const NativeStartDescriptor &echo,uint64_t now,std::span<int32_t> output,
                NativeStartDescriptor &out) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(!poll(now)||echo!=descriptor_||output.size()!=Frames)return reject();
        std::copy(samples_.begin(),samples_.end(),output.begin());out=descriptor_;
        samples_.fill(0);state_=State::accepted;return true;
    }
    bool poll(uint64_t now) noexcept {
        if(state_!=State::prepared)return false;
        if(now<lastTime_||!descriptor_.validAt(now))return reject();
        lastTime_=now;return true;
    }
    void cancel() noexcept {if(state_!=State::accepted)reject();}
private:
    bool reject() noexcept {samples_.fill(0);if(state_!=State::accepted)state_=State::failed;return false;}
    State state_=State::empty;NativeStartDescriptor descriptor_{};
    std::array<int32_t,Frames> samples_{};uint64_t lastTime_=0;
};
}
