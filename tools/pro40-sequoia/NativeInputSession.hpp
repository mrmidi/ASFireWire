// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeAudioStartup.hpp"
namespace Pro40Audio {
// Offline finite serialized owner. No transport callbacks, HAL, ongoing timestamp
// publication or clock-quality approval. Publication happens only after a paired
// clock validates the entire consumed packet prefix.
template<size_t Capacity> class NativeInputSession {
public:
    using Result=typename NativeInputContinuation<Capacity>::Result;
    enum class State {collecting,ready,running,draining,finished,failed};
    enum class Fault {none,input,timing,startup,order,consumer,end,cancelled,expired,drainTimeout};
    struct TimingPolicy {uint64_t maximumEvidenceAgeTicks=0,drainTimeoutTicks=0;};
    NativeInputSession(SourceFrameClock::Identity id,SourceClockReadiness::Policy policy,
                       uint64_t horizon,uint64_t started,uint64_t deadline,uint64_t arrivalAge,uint64_t useAge,
                       TimingPolicy timing)
        :identity_(id),lastTime_(started),timing_(timing),validator_(id.generation,id.numer,id.denom){
        startup_.emplace(id,policy,horizon,started,deadline,arrivalAge,useAge);
        if(startup_->state()==NativeAudioStartup<Capacity>::State::failed)fail(Fault::startup);
        if(!timing.maximumEvidenceAgeTicks||!timing.drainTimeoutTicks)fail(Fault::timing);
    }
    NativeInputSession(const NativeInputSession &)=delete;
    NativeInputSession &operator=(const NativeInputSession &)=delete;
    State state() const noexcept {return state_;}
    Fault fault() const noexcept {return fault_;}
    uint64_t nextRead() const noexcept {return continuation_.nextRead();}
    uint64_t produced() const noexcept {return produced_;}
    bool packet(const PacketClockFit::Packet &p,const InputBlock *audio,uint64_t now) noexcept {
        if(!accepting())return fail(Fault::order);
        if(!poll(now))return false;
        if(p.firstFrame!=produced_||(p.blocks!=0&&p.blocks!=8)||
           (p.blocks==0&&audio)||(p.blocks==8&&(!audio||audio->epoch!=identity_.epoch||audio->firstFrame!=p.firstFrame)))return fail(Fault::input);
        if(!validator_.packet(p))return fail(Fault::timing);
        if(startup_){if(!startup_->packet(p,audio))return fail(Fault::startup);}
        else if(audio&&!continuation_.append(*audio))return fail(Fault::input);
        ++packets_;produced_+=p.blocks;return true;
    }
    bool clock(const ControllerClockFit::Pair &p,uint64_t now) noexcept {
        if(!accepting())return fail(Fault::order);
        // Check the previous evidence before admitting new evidence: a late
        // clock observation cannot revive an expired session without a poll.
        if(!poll(now))return false;
        if(p.after>now||now-p.after>timing_.maximumEvidenceAgeTicks||!validator_.clock(p))return fail(Fault::timing);
        lastEvidence_=p.after;
        if(startup_){
            if(!startup_->clock(p,now))return fail(Fault::startup);
            if(startup_->state()==NativeAudioStartup<Capacity>::State::prepared)state_=State::ready;
        }else if(!continuation_.publish())return fail(Fault::consumer);
        return true;
    }
    bool completeStartup(uint64_t now,std::span<int32_t> output,SourceFrameClock::Stamp &stamp,
                         NativeStartDescriptor *descriptor=nullptr) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(state_!=State::ready)return fail(Fault::order);
        if(!poll(now))return false;
        if(now<lastEvidence_||now-lastEvidence_>timing_.maximumEvidenceAgeTicks)return fail(Fault::expired);
        NativeClockFit::Snapshot snapshot;
        if(!validator_.snapshot(snapshot)||snapshot.controller.packets!=packets_)return fail(Fault::timing);
        SourceFrameClock::Stamp candidate;
        NativeStartDescriptor candidateDescriptor;
        if(!startup_->complete(now,output,candidate,&candidateDescriptor)||!startup_->handoff(continuation_)){
            std::fill(output.begin(),output.end(),0);return fail(Fault::startup);
        }
        startup_.reset();state_=State::running;stamp=candidate;
        if(descriptor)*descriptor=candidateDescriptor;
        return true;
    }
    Result read(std::span<int32_t> output,uint64_t now) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(state_==State::failed)return Result::failed;
        if(state_==State::finished)return Result::end;
        if(!poll(now))return Result::failed;
        if(startup_)return Result::waiting;
        const auto result=continuation_.read(output);
        if(result==Result::failed)fail(Fault::consumer);
        else if(continuation_.state()==NativeInputContinuation<Capacity>::State::finished)state_=State::finished;
        return result;
    }
    bool finish(uint64_t expectedFrames,uint64_t now) noexcept {
        if(state_!=State::running)return fail(Fault::order);
        if(!poll(now))return false;
        if(now>UINT64_MAX-timing_.drainTimeoutTicks)return fail(Fault::timing);
        NativeClockFit::Snapshot snapshot;
        if(expectedFrames!=produced_||!validator_.snapshot(snapshot)||snapshot.controller.packets!=packets_)return fail(Fault::end);
        if(!continuation_.finish())return fail(Fault::consumer);
        state_=continuation_.state()==NativeInputContinuation<Capacity>::State::finished?State::finished:State::draining;
        drainDeadline_=now+timing_.drainTimeoutTicks;
        return true;
    }
    // Call while idle as well as on data operations. No background timer is
    // installed here. Deadlines are inclusive; terminal states never revive.
    bool poll(uint64_t now) noexcept {
        if(state_==State::failed)return false;
        if(state_==State::finished)return true;
        if(now<lastTime_)return fail(Fault::timing);
        lastTime_=now;
        if(startup_){if(!startup_->poll(now))return fail(Fault::startup);}
        else if(state_==State::running){
            if(now<lastEvidence_||now-lastEvidence_>timing_.maximumEvidenceAgeTicks)return fail(Fault::expired);
        }else if(state_==State::draining&&now>drainDeadline_)return fail(Fault::drainTimeout);
        return true;
    }
    void stop() noexcept {if(state_!=State::finished)fail(Fault::cancelled);}
private:
    bool accepting() const noexcept {return state_==State::collecting||state_==State::ready||state_==State::running;}
    bool fail(Fault fault) noexcept {
        if(fault_==Fault::none)fault_=fault;
        if(startup_)startup_->stop();startup_.reset();continuation_.stop();state_=State::failed;return false;
    }
    SourceFrameClock::Identity identity_;uint64_t lastTime_,packets_=0,produced_=0;
    TimingPolicy timing_;uint64_t lastEvidence_=0,drainDeadline_=0;
    State state_=State::collecting;Fault fault_=Fault::none;
    NativeClockFit validator_;
    std::optional<NativeAudioStartup<Capacity>> startup_;
    NativeInputContinuation<Capacity> continuation_;
};
}
