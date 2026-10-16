// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeInputSession.hpp"
#include "FiniteInputClockPublisher.hpp"
#include "NativeClockSchedule.hpp"
#include <optional>
namespace Pro40Audio {
// Offline serialized owner of decoded PCM reads and finite timestamps. No HAL,
// IO, buffer borrowing or quality admission. A successful read copies samples
// into caller storage; only that success advances the timestamp coverage count.
template<size_t Capacity> class NativeFiniteInputSession {
public:
    using Input=NativeInputSession<Capacity>;
    using State=typename Input::State;using ReadResult=typename Input::Result;
    using Result=ReadResult; // Common recording-adapter read contract.
    using Clock=SourceFrameClock;using ClockResult=FiniteInputClockPublisher::Result;
    struct ClockSelection {uint64_t pairs=0,observedHost=0;};
    // Historical metadata of the last successfully offered model. Not a clock
    // lease or proof of freshness; copied on the serialized source owner only.
    std::optional<ClockSelection> selectedClockEvidence()const noexcept{return selectedEvidence_;}
    NativeFiniteInputSession(Clock::Identity id,SourceClockReadiness::Policy policy,
            uint64_t horizon,uint64_t start,uint64_t deadline,uint64_t arrivalAge,
            uint64_t startupUseAge,typename Input::TimingPolicy timing,
            uint64_t correction,uint64_t pairsPerUpdate)
        :input_(id,policy,horizon,start,deadline,arrivalAge,startupUseAge,timing),
         fit_(id.generation,id.numer,id.denom),clock_(id,correction,arrivalAge,timing.maximumEvidenceAgeTicks),
         schedule_({policy.minimumPrefixFrames,pairsPerUpdate}),identity_(id),horizon_(horizon),
         drainTicks_(timing.drainTimeoutTicks){
        if(!pairsPerUpdate||input_.state()==State::failed||clock_.state()==FiniteInputClockPublisher::State::failed)fail();
    }
    NativeFiniteInputSession(const NativeFiniteInputSession &)=delete;
    NativeFiniteInputSession &operator=(const NativeFiniteInputSession &)=delete;
    State state()const noexcept{
        if(blocked_)return State::failed;
        if(sealed_)return input_.state()==State::finished&&clock_.state()==FiniteInputClockPublisher::State::ended?State::finished:State::draining;
        return input_.state();
    }
    uint64_t produced()const noexcept{return input_.produced();}
    uint64_t nextRead()const noexcept{return input_.nextRead();}
    bool poll(uint64_t now)noexcept{
        if(blocked_)return false;
        if(now<lastTime_)return fail();lastTime_=now;
        if(state()==State::finished)return true;
        if(sealed_&&now>drainDeadline_)return fail();
        return input_.poll(now)?true:fail();
    }
    bool packet(const PacketClockFit::Packet &p,const InputBlock *audio,uint64_t now)noexcept{
        if(!poll(now))return false;
        return input_.packet(p,audio,now)&&fit_.packet(p)?true:fail();
    }
    bool clock(const ControllerClockFit::Pair &p,uint64_t now)noexcept{
        if(!poll(now))return false;
        if(!input_.clock(p,now)||!fit_.clock(p))return fail();
        // Freeze the first proposal at the same ready transition used by the
        // input startup transaction. Later pairs must not replace sample zero.
        const bool preparing=input_.state()==State::ready&&!prepared_;
        if(!preparing&&input_.state()!=State::running)return true;
        NativeClockFit::Snapshot snapshot;
        if(!fit_.snapshot(snapshot))return fail();
        const auto selected=schedule_.observe(snapshot.bus.count,snapshot.packet.count*8);
        if(selected==NativeClockSchedule::Result::invalid)return fail();
        if(selected==NativeClockSchedule::Result::waiting)return !preparing?true:fail();
        Clock proposal;
        if(!nativeClockProposal(snapshot,identity_,horizon_,proposal)||
           clock_.offer(proposal,snapshot.bus.count,p.after,now)!=ClockResult::ready)return fail();
        selectedEvidence_=ClockSelection{snapshot.bus.count,p.after};
        prepared_=true;return true;
    }
    bool completeStartup(uint64_t now,std::span<int32_t> output,Clock::Stamp &out,
                         NativeStartDescriptor *descriptor=nullptr)noexcept{
        std::fill(output.begin(),output.end(),0);
        if(!poll(now)||!prepared_)return fail();
        Clock::Stamp inputStamp,clockStamp;
        NativeStartDescriptor candidateDescriptor;
        if(!input_.completeStartup(now,output,inputStamp,&candidateDescriptor)||
           !clock_.delivered(identity_,input_.nextRead(),now)||
           clock_.advance(now,clockStamp)!=ClockResult::published||
           inputStamp.sample!=clockStamp.sample||inputStamp.host!=clockStamp.host||inputStamp.seed!=clockStamp.seed){
            std::fill(output.begin(),output.end(),0);return fail();
        }
        out=inputStamp;if(descriptor)*descriptor=candidateDescriptor;
        return true; // Expose sample zero exactly once.
    }
    ReadResult read(std::span<int32_t> output,uint64_t now)noexcept{
        std::fill(output.begin(),output.end(),0);
        if(!poll(now))return ReadResult::failed;
        const auto result=input_.read(output,now);
        if(result==ReadResult::failed){fail();return result;}
        if(result==ReadResult::ok&&(!clock_.delivered(identity_,input_.nextRead(),now)||!sealClock(now))){
            std::fill(output.begin(),output.end(),0);fail();return ReadResult::failed;
        }
        return result;
    }
    ClockResult advance(uint64_t now,Clock::Stamp &out)noexcept{
        if(!poll(now))return ClockResult::blocked;
        if(input_.state()==State::collecting||input_.state()==State::ready)return ClockResult::waiting;
        const auto result=clock_.advance(now,out);
        if(result!=ClockResult::published&&result!=ClockResult::waiting&&result!=ClockResult::unavailable)fail();
        return result;
    }
    bool finish(uint64_t expectedEnd,uint64_t now)noexcept{
        if(!poll(now))return false;
        if(now>UINT64_MAX-drainTicks_||!input_.finish(expectedEnd,now))return fail();
        sealed_=true;drainDeadline_=now+drainTicks_;
        return sealClock(now)?true:fail();
    }
    void stop()noexcept{fail();}
private:
    bool sealClock(uint64_t now)noexcept{
        if(!sealed_||clockSealed_||input_.nextRead()!=input_.produced())return true;
        if(!clock_.finish(input_.nextRead(),now))return false;
        clockSealed_=true;return true;
    }
    bool fail()noexcept{blocked_=true;input_.stop();clock_.stop();return false;}
    Input input_;NativeClockFit fit_;FiniteInputClockPublisher clock_;NativeClockSchedule schedule_;
    Clock::Identity identity_;uint64_t horizon_,drainTicks_,drainDeadline_=0,lastTime_=0;
    std::optional<ClockSelection> selectedEvidence_;
    bool blocked_=false,prepared_=false,sealed_=false,clockSealed_=false;
};
}
